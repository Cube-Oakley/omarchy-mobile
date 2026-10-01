// SPDX-License-Identifier: GPL-2.0-only
/* Pixel 7 Pro cameras for V4L2 and libcamera: sensor subdevices and CSIS
 * capture nodes on one media device ("pixel-camera").
 *
 * Milestone (d) of docs/camera-plan-20260930.md. Everything here was proven
 * first with the bring-up tools in this directory (README.md):
 *  - sensor power through pixel-camera-power (pixel_camera_power_set);
 *  - the sensor's register tables, loaded as firmware
 *    (pixel-camera/<camera>.bin, generated from the user's own vendor image
 *    by make-camera-firmware.py and never committed);
 *  - the DC-PHY (D-PHY or C-PHY) and CSIS link set up as the stock HAL does
 *    (csis-probe.py), with the link's interleave mode 3;
 *  - SYSREG_CSIS routing, the CAM DVFS vote and the DC-PHY isolation bypass
 *    (pixel-csis-capture.c, pixel-csis-iso.c);
 *  - CSIS write-DMA context 0, one buffer address slot, re-pointed at every
 *    frame end from the context's interrupt (GIC SPI 197).
 * One camera streams at a time. pd_csis and pd_pdp must be on (the
 * camera-dev boot flag); there is no power-domain control yet.
 *
 * libcamera's simple pipeline finds each camera as sensor -> video node and
 * runs the software ISP on it; the sensor subdevices carry the controls it
 * needs (exposure, analogue gain, blanking, pixel rate, link frequency,
 * orientation, rotation) and the crop selections.
 */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/unaligned.h>
#include <media/media-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-dma-contig.h>

#include "pixel-camera-power.h"
#include "../pd/pixel-pd.h"

#define DRIVER_NAME		"pixel-camera"

/* Blocks (stock DT lwis_csi@1A440000; PMU from flexpmu_cal_local_gs201.h) */
#define CSIS_LINK(n)		(0x1a440000 + (n) * 0x10000)
#define CSIS_EBUF		0x1a4c0000
#define CSIS_DMA		0x1a4d0000
#define CSIS_PHY		0x1a4f0000
#define SYSREG_CSIS		0x1a420000
#define PMU_CSIS_STATUS		0x18062404
#define PMU_PDP_STATUS		0x18062484
#define PMU_DCPHY_ISO		0x18063ebc
#define GS201_SMC_PRIV_REG	0x82000504
#define PMUREG_WRITE		1
#define CSIS_DMA0_SPI		197

/* CSIS link (v5.4) */
#define LINK_CMN_CTRL		0x004
#define  CMN_CSI_EN		BIT(0)
#define  CMN_SW_RESET		BIT(1)
#define  CMN_LANE_NUMBER	GENMASK(9, 8)
#define  CMN_INTERLEAVE		GENMASK(11, 10)
#define  CMN_DESKEW_LEVEL	GENMASK(15, 13)
#define  CMN_PHY_SEL		BIT(21)
#define LINK_UPD_SDW		0x00c
#define LINK_INT_SRC0		0x014
#define LINK_INT_SRC1		0x01c
#define LINK_FE_INT_SRC		0x02c
#define LINK_ISP_CONFIG(ch)	(0x040 + (ch) * 0x10)
#define  CFG_VC			GENMASK(20, 16)
#define  CFG_PIXEL_MODE		GENMASK(13, 12)
#define  CFG_DT			GENMASK(7, 2)
#define LINK_ISP_RESOL(ch)	(0x044 + (ch) * 0x10)
#define LINK_LRTE_CONFIG	0x600		/* EPD_EN 31, SP/LP spacers 30:16/14:0 */
#define  LRTE_EPD_EN		BIT(31)
#define  LRTE_RESET		0x7fff7fff
#define LINK_DBG_OPTION_SUITE	0x690
#define LINK_PHY_CMN_CTRL	0x704
#define DT_RAW10		0x2b
#define PIXEL_MODE_QUAD		2

/* DC-PHY (stock HAL m1_dphy_sNc_gnr_con0 offsets; bias block at 0x1000) */
static const u16 phy_off[8] = { 0x1300, 0x1b00, 0x2300, 0x2800, 0x2b00, 0x3000, 0x3300, 0x3800 };
#define SYSREG_PHY_RESET	0x500

/* Write-DMA (Exynos 2100 common layout, GS201 interrupt bits) */
#define CMN_DMA_CTRL		0x0008
#define  DMA_SW_RESET		BIT(0)
#define  DMA_IP_PROCESSING	BIT(1)
#define CMN_DMA_CLK_CTRL	0x0014
#define DMA_CTX0		0x7000
#define CTX_DATA_CTRL		0x400
#define CTX_INT_ENABLE		0x404
#define CTX_INT_SRC		0x408
#define CH_CTRL			0x00
#define  CH_DMA_ENABLE		BIT(0)
#define  CH_UPDT_PTR_EN		BIT(1)
#define CH_FMT			0x04
#define CH_ADDR1		0x10
#define CH_RESOL		0xa4
#define CH_STRIDE		0xa8
#define CH_FCNTSEQ		0xb0
/* WDMA format 7 with pixel align writes MIPI CSI-2 RAW10 (4 pixels in 5
 * bytes, the LSB byte last), measured on GS201; format 4, the Pixel 6's,
 * writes a little-endian 10-bit bit stream here instead. */
#define FMT_RAW10_MIPI		0x7
#define INT_FRAME_START0	BIT(4)
#define INT_FRAME_END0		BIT(8)
#define INT_OVERLAP		BIT(20)
#define INT_ERRORS		(BIT(12) | BIT(14) | BIT(15) | INT_OVERLAP)
#define EBUF_CTRL		0x0000
#define  EBUF_BYPASS		BIT(0)
#define SYSREG_SC_CON(n)	(0x408 + (n) * 4)
#define SYSREG_WDMA_LINK	0x430

/* Sensor registers (MIPI CCS addresses; Sony takes them as byte pairs) */
#define REG_MODE_SELECT		0x0100
#define REG_GROUP_HOLD		0x0104
#define REG_EXPOSURE		0x0202
#define REG_ANALOGUE_GAIN	0x0204
#define REG_FRAME_LENGTH	0x0340
#define FW_DELAY		0xffff	/* firmware entry: wait <value> ms */

/* AK737x lens (mainline ak7375.c): 12-bit position << 4 at 0x00, mode at 0x02 */
#define LENS_POSITION		0x00
#define LENS_MODE		0x02
#define LENS_ACTIVE		0x00
#define LENS_STANDBY		0x40
#define LENS_MAX		4095
#define LENS_DEFAULT		1400	/* a desk at 0.5-1 m (README.md) */

/* D-PHY settle values for rate 4500 - 10 * i Mbps (stock HAL via the Pixel 6 port) */
static const u8 dphy_settle[] = {
	25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 24, 24, 24, 24, 24, 24,
	24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 23, 23, 23, 23, 23, 23,
	23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 22, 22, 22, 22, 22,
	22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 21, 21, 21, 21,
	21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 20, 20, 20, 20,
	20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 19, 19, 19,
	19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 18, 18, 18,
	18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 17, 17,
	17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 16,
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 15,
	15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
	14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
	14, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
	13, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
	12, 12, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
	11, 11, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
	10, 10, 10, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
	8, 8, 8, 8, 7, 7, 7, 7, 7, 7, 7, 7, 7, 73, 73, 72,
	72, 71, 71, 70, 70, 69, 69, 68, 68, 67, 67, 66, 66, 65, 65, 64,
	64, 64, 63, 63, 62, 62, 61, 61, 60, 60, 59, 59, 58, 58, 57, 57,
	56, 56, 55, 55, 54, 54, 53, 53, 52, 52, 52, 51, 51, 50, 50, 49,
	49, 48, 48, 47, 47, 46, 46, 45, 45, 44, 44, 43, 43, 42, 42, 41,
	41, 40, 40, 39, 39, 39, 38, 38, 37, 37, 36, 36, 35, 35, 34, 34,
	33, 33, 32, 32, 31, 31, 30, 30, 29, 29, 28, 28, 27, 27, 27, 26,
	26, 25, 25, 24, 24, 23, 23, 22, 22, 21, 21, 20, 20, 19, 19, 18,
	18, 17, 17, 16, 16, 15, 15, 14, 14, 14, 13, 13, 12, 12, 11, 11,
	10, 10, 9, 9, 8, 8, 7, 7, 6, 6,
};

struct pc_desc {
	const char *name;		/* pixel-camera-power name, firmware file */
	const char *model;		/* entity name prefix: libcamera's sensor model */
	const char *adapter;
	u8 addr, val_bytes;
	u16 id_reg, id;
	u8 link, phy, lanes;
	bool cphy;
	bool epd;			/* C-PHY LRTE packet delimiters (EPD) on the link */
	u32 rate;			/* Mb/s per lane, or Ms/s per trio */
	u32 width, height, code, pixfmt;
	u32 array_w, array_h;
	struct v4l2_rect crop;		/* the mode's area of the pixel array */
	u32 hts, vts, fps;		/* line and frame length, frame rate at vts */
	u32 exp_margin;
	u32 gain_min, gain_max, gain_def;
	u32 orientation, rotation;
	u8 lens_addr;			/* AK737x AF actuator on the same bus, or 0 */
	u32 cam_khz;			/* CAM DVFS while streaming */
};

static const struct pc_desc descs[] = {
	{
		.name = "uw", .model = "imx386", .adapter = "Pixel hsi2c_3", .addr = 0x1a,
		.val_bytes = 1, .id_reg = 0x0016, .id = 0x0386,
		.link = 2, .phy = 2, .lanes = 4, .rate = 1517,
		.width = 2016, .height = 1508, .code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.pixfmt = V4L2_PIX_FMT_SRGGB10P, .array_w = 4032, .array_h = 3016,
		.crop = { 0, 0, 4032, 3016 },
		.hts = 4296, .vts = 1799, .fps = 60, .exp_margin = 20,
		.gain_min = 0, .gain_max = 960, .gain_def = 0,
		.orientation = V4L2_CAMERA_ORIENTATION_BACK, .rotation = 90,
		.lens_addr = 0x0f, .cam_khz = 400000,
	},
	{
		.name = "front", .model = "s5k3j1", .adapter = "Pixel hsi2c_2", .addr = 0x10,
		.val_bytes = 2, .id_reg = 0x0000, .id = 0x30a1,
		.link = 0, .phy = 0, .lanes = 4, .rate = 1665,
		.width = 1920, .height = 1368, .code = MEDIA_BUS_FMT_SGRBG10_1X10,
		.pixfmt = V4L2_PIX_FMT_SGRBG10P, .array_w = 3840, .array_h = 2736,
		.crop = { 0, 0, 3840, 2736 },
		.hts = 5712, .vts = 2362, .fps = 60, .exp_margin = 16,
		.gain_min = 0x20, .gain_max = 0x200, .gain_def = 0x20,
		.orientation = V4L2_CAMERA_ORIENTATION_FRONT, .rotation = 270,
		.cam_khz = 400000,
	},
	{
		.name = "main", .model = "s5kgn1", .adapter = "Pixel hsi2c_1", .addr = 0x3d,
		.val_bytes = 2, .id_reg = 0x0000, .id = 0x08e1,
		.link = 1, .phy = 1, .lanes = 3, .cphy = true, .rate = 1596,
		.width = 2016, .height = 1136, .code = MEDIA_BUS_FMT_SGRBG10_1X10,
		.pixfmt = V4L2_PIX_FMT_SGRBG10P, .array_w = 8160, .array_h = 6144,
		.crop = { 0, 768, 8064, 4544 },
		.hts = 3000, .vts = 3632, .fps = 120, .exp_margin = 16,
		.gain_min = 0x20, .gain_max = 0x200, .gain_def = 0x20,
		.orientation = V4L2_CAMERA_ORIENTATION_BACK, .rotation = 90,
		.lens_addr = 0x0c,
		.cam_khz = 533000,	/* at 400 MHz the link overflows (ERR_OVER) */
	},
	{
		.name = "tele", .model = "s5kgm5", .adapter = "Pixel hsi2c_4", .addr = 0x2d,
		.val_bytes = 2, .id_reg = 0x0000, .id = 0x08d5,
		/* Two trios (its tables set 0x0114 = 0x0101), about 1060 Msps from
		 * its OP PLL against the GN1's, and LRTE packet delimiters: with
		 * EPD off the link decodes frame starts and nothing else. */
		.link = 4, .phy = 4, .lanes = 2, .cphy = true, .epd = true, .rate = 1060,
		.width = 2016, .height = 1512, .code = MEDIA_BUS_FMT_SGRBG10_1X10,
		.pixfmt = V4L2_PIX_FMT_SGRBG10P, .array_w = 4032, .array_h = 3024,
		.crop = { 0, 0, 4032, 3024 },
		.hts = 2784, .vts = 5456, .fps = 60, .exp_margin = 16,
		.gain_min = 0x20, .gain_max = 0x200, .gain_def = 0x20,
		.orientation = V4L2_CAMERA_ORIENTATION_BACK, .rotation = 90,
		.cam_khz = 533000,
	},
};
#define NCAMS ARRAY_SIZE(descs)

static char *cameras = "uw,front,main,tele";
module_param(cameras, charp, 0444);
MODULE_PARM_DESC(cameras, "cameras to register: uw, front, main, tele (comma list)");
static bool pixel_align = true;
module_param(pixel_align, bool, 0644);
MODULE_PARM_DESC(pixel_align, "link pixel alignment towards the WDMA (format 6 needs it)");
static uint wdma_fmt = FMT_RAW10_MIPI;
module_param(wdma_fmt, uint, 0644);
MODULE_PARM_DESC(wdma_fmt, "WDMA channel format (bring-up)");
static uint cam_khz;
module_param(cam_khz, uint, 0644);
MODULE_PARM_DESC(cam_khz, "ACPM CAM DVFS rate voted while streaming (0: the camera's own)");
/* At the default 200 MHz the WDMA's writes stall when libcamera's GPU debayer
 * starts up next to a 120 fps stream: a frame misses its end, the context
 * flags OVERLAP and stops for good (8 of 10 starts). 400 MHz: none in 10. */
static uint int_khz = 400000;
module_param(int_khz, uint, 0644);
MODULE_PARM_DESC(int_khz, "ACPM INT (bus) DVFS rate voted while streaming (0: leave)");

struct pc_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct pc_dev;

struct pc_cam {
	const struct pc_desc *d;
	struct pc_dev *pc;
	bool present;
	struct i2c_adapter *adap;
	struct v4l2_subdev sd;
	struct media_pad sd_pad;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure, *gain, *vblank;
	bool powered;
	struct v4l2_subdev lens;
	struct v4l2_ctrl_handler lens_ctrls;
	struct v4l2_ctrl *focus;
	bool has_lens;
	struct video_device vdev;
	struct media_pad vpad;
	struct vb2_queue queue;
	struct mutex vlock;
	struct list_head bufs;
	u32 sequence;
};

struct pc_dev {
	struct device *dev;
	struct media_device mdev;
	struct v4l2_device v4l2;
	void __iomem *link, *phy, *sys, *ebuf, *dma;
	int irq;
	spinlock_t slock;
	struct mutex lock;		/* one stream at a time */
	struct pc_cam *active;
	struct pc_buf *cur;
	u32 sys_saved[4];
	struct clk *cam_clk, *int_clk;
	unsigned long cam_old, int_old;
	bool iso_changed;
	bool pd_owned;			/* this driver powered CSIS and PDP on */
	u32 iso_found;
	struct pc_cam cams[NCAMS];
};

/* Packed RAW10 lines, padded to 64 bytes: the WDMA needs 16, and Mali's
 * Mesa refuses to import a dma-buf for libcamera's GPU debayer at less
 * than 64. Packing keeps four frames inside the 128 MB CMA area below
 * 4 GiB, which is all the 32-bit DMA address can reach. */
static u32 desc_stride(const struct pc_desc *d)
{
	return ALIGN(d->width * 5 / 4, 64);
}

static u32 desc_size(const struct pc_desc *d)
{
	return desc_stride(d) * d->height;
}

/* ---------------- sensor I2C ---------------- */

static int sensor_write(struct pc_cam *c, u16 reg, u32 val, unsigned int len)
{
	u8 buf[6] = { reg >> 8, reg & 0xff };
	struct i2c_msg msg = { .addr = c->d->addr, .buf = buf, .len = 2 + len };
	unsigned int i;
	int ret;

	for (i = 0; i < len; i++)
		buf[2 + i] = val >> (8 * (len - 1 - i));
	ret = i2c_transfer(c->adap, &msg, 1);
	return ret == 1 ? 0 : ret < 0 ? ret : -EIO;
}

static int sensor_read(struct pc_cam *c, u16 reg, u32 *val, unsigned int len)
{
	u8 wbuf[2] = { reg >> 8, reg & 0xff }, rbuf[4];
	struct i2c_msg msgs[2] = {
		{ .addr = c->d->addr, .buf = wbuf, .len = 2 },
		{ .addr = c->d->addr, .flags = I2C_M_RD, .buf = rbuf, .len = len },
	};
	unsigned int i;
	int ret;

	ret = i2c_transfer(c->adap, msgs, 2);
	if (ret != 2)
		return ret < 0 ? ret : -EIO;
	for (*val = 0, i = 0; i < len; i++)
		*val = *val << 8 | rbuf[i];
	return 0;
}

/* Firmware: little-endian {u16 register, u16 value} pairs; register 0xffff
 * waits <value> ms. Value width is the sensor's. */
static int sensor_load_tables(struct pc_cam *c)
{
	const struct firmware *fw;
	char name[64];
	size_t i;
	int ret;

	snprintf(name, sizeof(name), "pixel-camera/%s.bin", c->d->name);
	ret = request_firmware(&fw, name, c->pc->dev);
	if (ret) {
		dev_err(c->pc->dev, "%s: no %s (make-camera-firmware.py): %d\n",
			c->d->name, name, ret);
		return ret;
	}
	for (i = 0; i + 4 <= fw->size && !ret; i += 4) {
		u16 reg = get_unaligned_le16(fw->data + i);
		u16 val = get_unaligned_le16(fw->data + i + 2);

		if (reg == FW_DELAY)
			msleep(val);
		else
			ret = sensor_write(c, reg, val, c->d->val_bytes);
	}
	if (ret)
		dev_err(c->pc->dev, "%s: table write %zu failed: %d\n", c->d->name, i / 4, ret);
	else
		dev_dbg(c->pc->dev, "%s: %zu table writes\n", c->d->name, fw->size / 4);
	release_firmware(fw);
	return ret;
}

/* ---------------- sensor subdevice ---------------- */

static struct pc_cam *sd_to_cam(struct v4l2_subdev *sd)
{
	return container_of(sd, struct pc_cam, sd);
}

static void cam_fill_fmt(const struct pc_desc *d, struct v4l2_mbus_framefmt *f)
{
	memset(f, 0, sizeof(*f));
	f->width = d->width;
	f->height = d->height;
	f->code = d->code;
	f->field = V4L2_FIELD_NONE;
	f->colorspace = V4L2_COLORSPACE_RAW;
	f->xfer_func = V4L2_XFER_FUNC_NONE;
	f->ycbcr_enc = V4L2_YCBCR_ENC_601;
	f->quantization = V4L2_QUANTIZATION_FULL_RANGE;
}

static int cam_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	cam_fill_fmt(sd_to_cam(sd)->d, v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static int cam_enum_mbus_code(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			      struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = sd_to_cam(sd)->d->code;
	return 0;
}

static int cam_enum_frame_size(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			       struct v4l2_subdev_frame_size_enum *fse)
{
	const struct pc_desc *d = sd_to_cam(sd)->d;

	if (fse->index || fse->code != d->code)
		return -EINVAL;
	fse->min_width = fse->max_width = d->width;
	fse->min_height = fse->max_height = d->height;
	return 0;
}

static int cam_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
		       struct v4l2_subdev_format *fmt)
{
	cam_fill_fmt(sd_to_cam(sd)->d, &fmt->format);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	return 0;
}

static int cam_get_selection(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			     struct v4l2_subdev_selection *sel)
{
	const struct pc_desc *d = sd_to_cam(sd)->d;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = d->crop;
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = (struct v4l2_rect){ 0, 0, d->array_w, d->array_h };
		return 0;
	}
	return -EINVAL;
}

static const struct v4l2_subdev_pad_ops cam_pad_ops = {
	.enum_mbus_code = cam_enum_mbus_code,
	.enum_frame_size = cam_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = cam_set_fmt,
	.get_selection = cam_get_selection,
};

static const struct v4l2_subdev_ops cam_subdev_ops = {
	.pad = &cam_pad_ops,
};

static const struct v4l2_subdev_internal_ops cam_internal_ops = {
	.init_state = cam_init_state,
};

static int cam_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct pc_cam *c = container_of(ctrl->handler, struct pc_cam, ctrls);
	const struct pc_desc *d = c->d;
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		u32 max = d->height + ctrl->val - d->exp_margin;

		__v4l2_ctrl_modify_range(c->exposure, c->exposure->minimum, max,
					 c->exposure->step, min(c->exposure->default_value, (s64)max));
	}
	if (!c->powered)
		return 0;
	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = sensor_write(c, REG_EXPOSURE, ctrl->val, 2);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = sensor_write(c, REG_ANALOGUE_GAIN, ctrl->val, 2);
		break;
	case V4L2_CID_VBLANK:
		ret = sensor_write(c, REG_FRAME_LENGTH, d->height + ctrl->val, 2);
		break;
	}
	return ret;
}

static const struct v4l2_ctrl_ops cam_ctrl_ops = {
	.s_ctrl = cam_s_ctrl,
};

static int cam_init_controls(struct pc_cam *c)
{
	const struct pc_desc *d = c->d;
	struct v4l2_ctrl_handler *h = &c->ctrls;
	u64 pixel_rate = (u64)d->hts * d->vts * d->fps;
	u32 vblank = d->vts - d->height, hblank = d->hts - d->width;
	static s64 link_freq[1];
	struct v4l2_ctrl *ctrl;

	link_freq[0] = d->cphy ? (s64)d->rate * 1000000 / 2 : (s64)d->rate * 1000000 / 2;
	v4l2_ctrl_handler_init(h, 10);
	c->exposure = v4l2_ctrl_new_std(h, &cam_ctrl_ops, V4L2_CID_EXPOSURE, 1,
					d->vts - d->exp_margin, 1, d->vts / 2);
	c->gain = v4l2_ctrl_new_std(h, &cam_ctrl_ops, V4L2_CID_ANALOGUE_GAIN, d->gain_min,
				    d->gain_max, 1, d->gain_def);
	c->vblank = v4l2_ctrl_new_std(h, &cam_ctrl_ops, V4L2_CID_VBLANK, vblank,
				      0xffff - d->height, 1, vblank);
	ctrl = v4l2_ctrl_new_std(h, &cam_ctrl_ops, V4L2_CID_HBLANK, hblank, hblank, 1, hblank);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(h, &cam_ctrl_ops, V4L2_CID_PIXEL_RATE, pixel_rate, pixel_rate, 1,
			  pixel_rate);
	ctrl = v4l2_ctrl_new_int_menu(h, &cam_ctrl_ops, V4L2_CID_LINK_FREQ, 0, 0, link_freq);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std_menu(h, &cam_ctrl_ops, V4L2_CID_CAMERA_ORIENTATION,
			       V4L2_CAMERA_ORIENTATION_EXTERNAL, 0, d->orientation);
	v4l2_ctrl_new_std(h, &cam_ctrl_ops, V4L2_CID_CAMERA_SENSOR_ROTATION, d->rotation,
			  d->rotation, 1, d->rotation);
	if (h->error)
		return h->error;
	c->sd.ctrl_handler = h;
	return 0;
}

/* ---------------- lens ---------------- */

static int lens_write(struct pc_cam *c, u8 reg, const u8 *val, unsigned int len)
{
	u8 buf[3] = { reg };
	struct i2c_msg msg = { .addr = c->d->lens_addr, .buf = buf, .len = 1 + len };
	int ret;

	memcpy(buf + 1, val, len);
	ret = i2c_transfer(c->adap, &msg, 1);
	return ret == 1 ? 0 : ret < 0 ? ret : -EIO;
}

static int lens_move(struct pc_cam *c, u32 pos)
{
	u8 v[2] = { pos >> 4, (pos & 0xf) << 4 };

	return lens_write(c, LENS_POSITION, v, 2);
}

static int lens_power(struct pc_cam *c, bool on)
{
	u8 mode = on ? LENS_ACTIVE : LENS_STANDBY;
	int ret = lens_write(c, LENS_MODE, &mode, 1);

	if (!ret && on) {
		usleep_range(10000, 11000);	/* the first write after waking NACKs */
		ret = lens_move(c, c->focus->val);
	}
	return ret;
}

static int lens_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct pc_cam *c = container_of(ctrl->handler, struct pc_cam, lens_ctrls);

	return c->powered ? lens_move(c, ctrl->val) : 0;
}

static const struct v4l2_ctrl_ops lens_ctrl_ops = {
	.s_ctrl = lens_s_ctrl,
};

static const struct v4l2_subdev_ops lens_subdev_ops = {};

static int lens_register(struct pc_dev *pc, struct pc_cam *c)
{
	int ret;

	v4l2_ctrl_handler_init(&c->lens_ctrls, 1);
	c->focus = v4l2_ctrl_new_std(&c->lens_ctrls, &lens_ctrl_ops, V4L2_CID_FOCUS_ABSOLUTE, 0,
				     LENS_MAX, 1, LENS_DEFAULT);
	if (c->lens_ctrls.error) {
		ret = c->lens_ctrls.error;
		goto free;
	}
	v4l2_subdev_init(&c->lens, &lens_subdev_ops);
	c->lens.owner = THIS_MODULE;
	c->lens.dev = pc->dev;
	c->lens.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	c->lens.ctrl_handler = &c->lens_ctrls;
	c->lens.entity.function = MEDIA_ENT_F_LENS;
	snprintf(c->lens.name, sizeof(c->lens.name), "ak7375 %d-%04x", c->adap->nr, c->d->lens_addr);
	ret = media_entity_pads_init(&c->lens.entity, 0, NULL);
	ret = ret ?: v4l2_device_register_subdev(&pc->v4l2, &c->lens);
	if (ret)
		goto free;
	if (IS_ERR(media_create_ancillary_link(&c->sd.entity, &c->lens.entity))) {
		v4l2_device_unregister_subdev(&c->lens);
		ret = -ENOMEM;
		goto free;
	}
	c->has_lens = true;
	return 0;
free:
	v4l2_ctrl_handler_free(&c->lens_ctrls);
	return ret;
}

/* ---------------- PHY, link, DMA ---------------- */

static u32 settle_for(u32 rate)
{
	u32 key = DIV_ROUND_UP(rate, 10) * 10;

	if (key > 4500)
		return 25;
	if (key < 90)
		return 5;
	return dphy_settle[(4500 - key) / 10];
}

static void phy_wr(struct pc_dev *pc, u32 off, u32 val)
{
	writel(val, pc->phy + off);
}

static void dphy_set(struct pc_dev *pc, u32 off, unsigned int lanes, u32 rate)
{
	u32 settle = settle_for(rate), clk_sel = rate < 1500 ? BIT(8) : 0;
	u32 skew = rate >= 1500 && rate < 2000 ? 0x300 : rate >= 2000 && rate < 3000 ? 0x200 : 0;
	static const u16 bias[][2] = { { 0x0, 0x10 }, { 0x4, 0x110 }, { 0x8, 0x3223 }, { 0xc, 0 },
				       { 0x10, 0x200 } };
	static const u16 clk[][2] = { { 0x4, 0x1450 }, { 0x8, 0x9 }, { 0xc, 0xea40 }, { 0x10, 0x2 },
				      { 0x14, 0x8600 }, { 0x18, 0x4000 }, { 0x1c, 0 },
				      { 0x30, 0x301 }, { 0x40, 0x1 } };
	unsigned int i, j;

	phy_wr(pc, off, 0);
	for (i = 0; i < lanes; i++)
		phy_wr(pc, off + 0x100 + i * 0x100, 0);
	for (j = 0; j < ARRAY_SIZE(bias); j++)
		phy_wr(pc, 0x1000 + bias[j][0], bias[j][1]);
	for (j = 0; j < ARRAY_SIZE(clk); j++)
		phy_wr(pc, off + clk[j][0], clk[j][1]);
	phy_wr(pc, off, 1);
	for (i = 0; i < lanes; i++) {
		u32 sd = off + 0x100 + i * 0x100;
		static const u16 dat[][2] = { { 0x4, 0x1450 }, { 0x8, 0x9 }, { 0xc, 0xea40 },
					      { 0x14, 0x8600 }, { 0x18, 0x4000 }, { 0x1c, 0 },
					      { 0x20, 0 }, { 0x24, 0x40 } };

		for (j = 0; j < ARRAY_SIZE(dat); j++)
			phy_wr(pc, sd + dat[j][0], dat[j][1]);
		phy_wr(pc, sd + 0x10, 0x2 | skew);
		phy_wr(pc, sd + 0x30, (readl(pc->phy + sd + 0x30) & ~0x1ffU) | settle | clk_sel);
		phy_wr(pc, sd + 0x34, 0x3);
		phy_wr(pc, sd + 0x40, readl(pc->phy + sd + 0x40) | 1);
		phy_wr(pc, sd + 0x50, 0x81a);
		phy_wr(pc, sd, 1);
	}
}

static void cphy_set(struct pc_dev *pc, u32 off, unsigned int trios, u32 rate)
{
	u32 settle = rate >= 1000 ? 7 : 9, clk_sel = rate < 500 ? BIT(8) : 0;
	static const u16 bias[][2] = { { 0x0, 0x10 }, { 0x4, 0x110 }, { 0x8, 0x3223 }, { 0xc, 0 },
				       { 0x10, 0x40 } };
	static const u16 dat[][2] = { { 0x4, 0x1450 }, { 0x8, 0x9 }, { 0xc, 0x82b8 }, { 0x10, 0x1 },
				      { 0x14, 0x8600 }, { 0x18, 0x4000 }, { 0x1c, 0x200 },
				      { 0x20, 0x638 }, { 0x24, 0x40 } };
	unsigned int i, j;

	for (j = 0; j < ARRAY_SIZE(bias); j++)
		phy_wr(pc, 0x1000 + bias[j][0], bias[j][1]);
	for (i = 0; i < trios; i++) {
		u32 sd = off + 0x100 + i * 0x100;

		phy_wr(pc, sd, 1);
		for (j = 0; j < ARRAY_SIZE(dat); j++)
			phy_wr(pc, sd + dat[j][0], dat[j][1]);
		phy_wr(pc, sd + 0x30, (readl(pc->phy + sd + 0x30) & ~0xffU) | settle | clk_sel);
		phy_wr(pc, sd + 0x34, 0x32);
		phy_wr(pc, sd + 0x64, 0x1503);
		phy_wr(pc, sd + 0x68, 0x32);
	}
}

static u32 phy_mask(const struct pc_desc *d)
{
	return d->phy >= 2 && d->lanes > 2 && !d->cphy ? 3 << d->phy : BIT(d->phy);
}

static void link_setup(struct pc_dev *pc, const struct pc_desc *d)
{
	void __iomem *l = pc->link;
	u32 v;
	int ch;

	writel(readl(pc->sys + SYSREG_PHY_RESET) | phy_mask(d), pc->sys + SYSREG_PHY_RESET);
	if (d->cphy)
		cphy_set(pc, phy_off[d->phy], d->lanes, d->rate);
	else
		dphy_set(pc, phy_off[d->phy], d->lanes, d->rate);

	writel(readl(l + LINK_CMN_CTRL) | CMN_SW_RESET, l + LINK_CMN_CTRL);
	usleep_range(1000, 1500);
	v = readl(l + LINK_CMN_CTRL) & ~(CMN_LANE_NUMBER | CMN_INTERLEAVE | CMN_DESKEW_LEVEL |
					  CMN_PHY_SEL | CMN_CSI_EN);
	v |= FIELD_PREP(CMN_LANE_NUMBER, d->lanes - 1) | FIELD_PREP(CMN_INTERLEAVE, 3) |
	     FIELD_PREP(CMN_DESKEW_LEVEL, 2) | (d->cphy ? CMN_PHY_SEL : 0);
	writel(v, l + LINK_CMN_CTRL);
	writel(0x1e, l + LINK_PHY_CMN_CTRL);		/* data lanes, clock below */
	/* Channels 1-3 parked with DT 0x3f, as the HAL does, but on VC 15
	 * rather than the HAL's VC 1-3: the GN1 sends a second stream on VC1,
	 * and a channel on VC1 cuts channel 0's output to the WDMA to 8 bits
	 * a pixel. */
	for (ch = 3; ch >= 0; ch--) {
		v = readl(l + LINK_ISP_CONFIG(ch)) & ~(CFG_VC | CFG_PIXEL_MODE | CFG_DT);
		v |= FIELD_PREP(CFG_VC, ch ? 15 : 0) | FIELD_PREP(CFG_PIXEL_MODE, PIXEL_MODE_QUAD) |
		     FIELD_PREP(CFG_DT, ch ? 0x3f : DT_RAW10);
		writel(v, l + LINK_ISP_CONFIG(ch));
	}
	writel(d->height << 16 | d->width, l + LINK_ISP_RESOL(0));
	/* The GS201 HAL writes 0 here, but WDMA format 6 then gets two of every
	 * four pixels (the Pixel 6 port's finding, seen again on GS201). */
	writel(pixel_align ? BIT(23) : 0, l + LINK_DBG_OPTION_SUITE);
	writel(d->epd ? LRTE_RESET | LRTE_EPD_EN : LRTE_RESET, l + LINK_LRTE_CONFIG);
	writel(0xf, l + LINK_UPD_SDW);
	writel(0x1f, l + LINK_PHY_CMN_CTRL);
	writel(readl(l + LINK_CMN_CTRL) | CMN_CSI_EN, l + LINK_CMN_CTRL);
}

static void link_stop(struct pc_dev *pc, const struct pc_desc *d)
{
	writel(readl(pc->link + LINK_CMN_CTRL) & ~CMN_CSI_EN, pc->link + LINK_CMN_CTRL);
	writel(0, pc->link + LINK_PHY_CMN_CTRL);
	writel(readl(pc->sys + SYSREG_PHY_RESET) & ~phy_mask(d), pc->sys + SYSREG_PHY_RESET);
}

static void dma_setup(struct pc_dev *pc, const struct pc_desc *d, dma_addr_t addr)
{
	void __iomem *ch = pc->dma + DMA_CTX0;
	int i;

	writel(readl(pc->dma + CMN_DMA_CTRL) | DMA_SW_RESET, pc->dma + CMN_DMA_CTRL);
	for (i = 0; i < 10 && (readl(pc->dma + CMN_DMA_CTRL) & DMA_SW_RESET); i++)
		udelay(10);
	writel((readl(pc->dma + CMN_DMA_CTRL) & ~DMA_SW_RESET) | DMA_IP_PROCESSING,
	       pc->dma + CMN_DMA_CTRL);
	writel(readl(pc->dma + CMN_DMA_CLK_CTRL) | BIT(0), pc->dma + CMN_DMA_CLK_CTRL);
	writel(readl(pc->ebuf + EBUF_CTRL) | EBUF_BYPASS, pc->ebuf + EBUF_CTRL);

	for (i = 0; i < 3; i++)
		pc->sys_saved[i] = readl(pc->sys + SYSREG_SC_CON(i));
	pc->sys_saved[3] = readl(pc->sys + SYSREG_WDMA_LINK);
	for (i = 0; i < 3; i++)
		writel(1, pc->sys + SYSREG_SC_CON(i));
	writel(d->link, pc->sys + SYSREG_WDMA_LINK);

	writel(readl(ch + CTX_DATA_CTRL) & ~BIT(0), ch + CTX_DATA_CTRL);
	writel(wdma_fmt, ch + CH_FMT);
	writel(d->height << 16 | d->width, ch + CH_RESOL);
	writel(desc_stride(d), ch + CH_STRIDE);
	writel(lower_32_bits(addr), ch + CH_ADDR1);
	writel(BIT(0), ch + CH_FCNTSEQ);
	writel(readl(ch + CTX_INT_SRC), ch + CTX_INT_SRC);
	writel(INT_FRAME_START0 | INT_FRAME_END0 | INT_ERRORS, ch + CTX_INT_ENABLE);
}

static void dma_enable(struct pc_dev *pc, bool on)
{
	void __iomem *ch = pc->dma + DMA_CTX0;

	writel(on ? CH_UPDT_PTR_EN | CH_DMA_ENABLE : 0, ch + CH_CTRL);
	if (!on)
		writel(0, ch + CTX_INT_ENABLE);
}

static void dma_restore_routing(struct pc_dev *pc)
{
	int i;

	for (i = 0; i < 3; i++)
		writel(pc->sys_saved[i], pc->sys + SYSREG_SC_CON(i));
	writel(pc->sys_saved[3], pc->sys + SYSREG_WDMA_LINK);
}

/* ---------------- power around a stream ---------------- */

static bool domains_on(void)
{
	void __iomem *pmu = ioremap(PMU_CSIS_STATUS & PAGE_MASK, PAGE_SIZE);
	bool on;

	if (!pmu)
		return false;
	on = (readl(pmu + (PMU_CSIS_STATUS & ~PAGE_MASK)) & 1) &&
	     (readl(pmu + (PMU_PDP_STATUS & ~PAGE_MASK)) & 1);
	iounmap(pmu);
	return on;
}

static int iso_write(u32 val)
{
	struct arm_smccc_res res;

	arm_smccc_smc(GS201_SMC_PRIV_REG, PMU_DCPHY_ISO, PMUREG_WRITE, val, 0, 0, 0, 0, &res);
	return res.a0 ? -EIO : 0;
}

/* CSIS, then PDP, on; PDP, then CSIS, off (PDP is the stock DT's child).
 * pixel-pd-off powers them off at boot and keeps their save lists. Looked up
 * at run time, so a camera-dev boot (domains left on) needs no pixel-pd-off. */
static int camera_domains(struct pc_dev *pc, bool on)
{
	int (*power)(const char *name, bool on) = symbol_get(pixel_pd_power);
	int ret;

	if (!power) {
		dev_err(pc->dev, "pd_csis/pd_pdp are off and pixel-pd-off cannot power them on\n");
		return -ENODEV;
	}
	if (on) {
		ret = power("csis", true);
		if (!ret) {
			ret = power("pdp", true);
			if (ret)
				power("csis", false);
		}
	} else {
		ret = power("pdp", false);
		ret = power("csis", false) ?: ret;
	}
	symbol_put(pixel_pd_power);
	return ret;
}

static int csis_power_on(struct pc_dev *pc, const struct pc_desc *d)
{
	unsigned long rate = (unsigned long)(cam_khz ?: d->cam_khz) * 1000;
	void __iomem *pmu;
	int ret = 0;

	pc->pd_owned = false;
	if (!domains_on()) {
		ret = camera_domains(pc, true);
		if (ret)
			return ret;
		pc->pd_owned = true;
	}
	pmu = ioremap(PMU_DCPHY_ISO & PAGE_MASK, PAGE_SIZE);
	if (!pmu) {
		ret = -ENOMEM;
		goto domains_off;
	}
	pc->iso_found = readl(pmu + (PMU_DCPHY_ISO & ~PAGE_MASK));
	iounmap(pmu);
	pc->iso_changed = false;
	if (!(pc->iso_found & 1)) {
		ret = iso_write(pc->iso_found | 1);
		pc->iso_changed = !ret;
	}
	pc->cam_old = 0;
	if (!ret && pc->cam_clk) {
		pc->cam_old = clk_get_rate(pc->cam_clk);
		clk_set_rate(pc->cam_clk, rate);
	}
	pc->int_old = 0;
	if (!ret && pc->int_clk && int_khz) {
		pc->int_old = clk_get_rate(pc->int_clk);
		clk_set_rate(pc->int_clk, (unsigned long)int_khz * 1000);
	}
	if (!ret)
		return 0;
domains_off:
	if (pc->pd_owned)
		camera_domains(pc, false);
	pc->pd_owned = false;
	return ret;
}

static void csis_power_off(struct pc_dev *pc)
{
	if (pc->cam_clk && pc->cam_old)
		clk_set_rate(pc->cam_clk, pc->cam_old);
	if (pc->int_clk && pc->int_old)
		clk_set_rate(pc->int_clk, pc->int_old);
	if (pc->iso_changed)
		iso_write(pc->iso_found);
	if (pc->pd_owned)
		camera_domains(pc, false);
	pc->pd_owned = false;
}

/* ---------------- vb2 and the capture node ---------------- */

static struct pc_buf *to_pc_buf(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct pc_buf, vb);
}

static int pc_queue_setup(struct vb2_queue *q, unsigned int *nbuf, unsigned int *nplanes,
			  unsigned int sizes[], struct device *alloc_devs[])
{
	struct pc_cam *c = vb2_get_drv_priv(q);
	unsigned int size = desc_size(c->d);

	/* Cached buffers, synced on queue and dequeue: libcamera's software
	 * ISP reads every frame with the CPU for its statistics, which takes
	 * 48 ms through an uncached mapping. REQBUFS resets this from the
	 * caller's flags just before calling here, and libcamera sets none. */
	q->non_coherent_mem = 1;
	if (*nplanes)
		return *nplanes != 1 || sizes[0] < size ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = size;
	return 0;
}

static int pc_buf_prepare(struct vb2_buffer *vb)
{
	struct pc_cam *c = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long size = desc_size(c->d);

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;
	if (upper_32_bits(vb2_dma_contig_plane_dma_addr(vb, 0)))
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, size);
	return 0;
}

static void pc_buf_queue(struct vb2_buffer *vb)
{
	struct pc_cam *c = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long flags;

	spin_lock_irqsave(&c->pc->slock, flags);
	list_add_tail(&to_pc_buf(vb)->list, &c->bufs);
	spin_unlock_irqrestore(&c->pc->slock, flags);
}

static void pc_return_all(struct pc_cam *c, enum vb2_buffer_state state)
{
	struct pc_dev *pc = c->pc;
	struct pc_buf *b, *n;
	unsigned long flags;

	spin_lock_irqsave(&pc->slock, flags);
	if (pc->cur && pc->active == c) {
		vb2_buffer_done(&pc->cur->vb.vb2_buf, state);
		pc->cur = NULL;
	}
	list_for_each_entry_safe(b, n, &c->bufs, list) {
		list_del(&b->list);
		vb2_buffer_done(&b->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&pc->slock, flags);
}

static int pc_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct pc_cam *c = vb2_get_drv_priv(q);
	struct pc_dev *pc = c->pc;
	const struct pc_desc *d = c->d;
	unsigned long flags;
	u32 id;
	int ret, i;

	mutex_lock(&pc->lock);
	ret = -EBUSY;
	if (pc->active)
		goto unlock;
	pc->link = ioremap(CSIS_LINK(d->link), 0x1000);
	ret = -ENOMEM;
	if (!pc->link)
		goto unlock;
	ret = csis_power_on(pc, d);
	if (ret)
		goto unmap;
	ret = pixel_camera_power_set(d->name, true);
	if (ret)
		goto csis_off;
	ret = sensor_read(c, d->id_reg, &id, 2);
	if (!ret && id != d->id)
		ret = -ENODEV;
	if (ret) {
		dev_err(pc->dev, "%s: chip ID %#x: %d\n", d->name, id, ret);
		goto power_off;
	}
	ret = sensor_load_tables(c);
	if (ret)
		goto power_off;
	c->powered = true;
	ret = __v4l2_ctrl_handler_setup(&c->ctrls);
	if (ret)
		goto power_off;
	if (c->has_lens && lens_power(c, true))
		dev_warn(pc->dev, "%s: lens did not answer\n", d->name);

	spin_lock_irqsave(&pc->slock, flags);
	pc->active = c;
	pc->cur = list_first_entry_or_null(&c->bufs, struct pc_buf, list);
	if (pc->cur)
		list_del(&pc->cur->list);
	spin_unlock_irqrestore(&pc->slock, flags);
	c->sequence = 0;

	link_setup(pc, d);
	ret = -ENOBUFS;
	if (!pc->cur)
		goto stop_link;
	dma_setup(pc, d, vb2_dma_contig_plane_dma_addr(&pc->cur->vb.vb2_buf, 0));
	ret = sensor_write(c, REG_MODE_SELECT, 1, 1);
	if (ret)
		goto stop_link;
	/* Enable the DMA on a frame boundary (the Pixel 6 port's lesson). */
	writel(readl(pc->link + LINK_FE_INT_SRC), pc->link + LINK_FE_INT_SRC);
	for (i = 0; i < 100 && !(readl(pc->link + LINK_FE_INT_SRC) & 1); i++)
		usleep_range(1000, 1500);
	dma_enable(pc, true);
	enable_irq(pc->irq);
	mutex_unlock(&pc->lock);
	dev_info(pc->dev, "%s: streaming %ux%u\n", d->name, d->width, d->height);
	return 0;

stop_link:
	dma_enable(pc, false);
	dma_restore_routing(pc);
	link_stop(pc, d);
	pc->active = NULL;
power_off:
	c->powered = false;
	pixel_camera_power_set(d->name, false);
csis_off:
	csis_power_off(pc);
unmap:
	iounmap(pc->link);
	pc->link = NULL;
unlock:
	mutex_unlock(&pc->lock);
	pc_return_all(c, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void pc_stop_streaming(struct vb2_queue *q)
{
	struct pc_cam *c = vb2_get_drv_priv(q);
	struct pc_dev *pc = c->pc;
	const struct pc_desc *d = c->d;

	mutex_lock(&pc->lock);
	if (pc->active == c) {
		disable_irq(pc->irq);
		dma_enable(pc, false);
		sensor_write(c, REG_MODE_SELECT, 0, 1);
		if (c->has_lens)
			lens_power(c, false);
		msleep(40);
		dma_restore_routing(pc);
		link_stop(pc, d);
		c->powered = false;
		pixel_camera_power_set(d->name, false);
		csis_power_off(pc);
		iounmap(pc->link);
		pc->link = NULL;
	}
	mutex_unlock(&pc->lock);
	pc_return_all(c, VB2_BUF_STATE_ERROR);
	mutex_lock(&pc->lock);
	pc->active = NULL;
	mutex_unlock(&pc->lock);
	dev_info(pc->dev, "%s: stopped after %u frames\n", d->name, c->sequence);
}

static const struct vb2_ops pc_vb2_ops = {
	.queue_setup = pc_queue_setup,
	.buf_prepare = pc_buf_prepare,
	.buf_queue = pc_buf_queue,
	.start_streaming = pc_start_streaming,
	.stop_streaming = pc_stop_streaming,
};

static irqreturn_t pc_irq(int irq, void *data)
{
	struct pc_dev *pc = data;
	void __iomem *ch = pc->dma + DMA_CTX0;
	struct pc_buf *done, *next;
	struct pc_cam *c;
	u32 src;

	src = readl(ch + CTX_INT_SRC);
	if (!src)
		return IRQ_NONE;
	writel(src, ch + CTX_INT_SRC);
	if (src & INT_OVERLAP)
		dev_warn_ratelimited(pc->dev, "WDMA overlap (%#x): a frame missed its end\n", src);
	if (!(src & INT_FRAME_END0))
		return IRQ_HANDLED;
	spin_lock(&pc->slock);
	c = pc->active;
	if (!c) {
		spin_unlock(&pc->slock);
		return IRQ_HANDLED;
	}
	/* With no buffer queued, the next frame overwrites the current one,
	 * which is held back: a dropped frame instead of a scratch buffer. */
	next = list_first_entry_or_null(&c->bufs, struct pc_buf, list);
	done = next ? pc->cur : NULL;
	if (next) {
		list_del(&next->list);
		writel(lower_32_bits(vb2_dma_contig_plane_dma_addr(&next->vb.vb2_buf, 0)),
		       ch + CH_ADDR1);
		pc->cur = next;
	}
	if (done) {
		done->vb.vb2_buf.timestamp = ktime_get_ns();
		done->vb.sequence = c->sequence;
		done->vb.field = V4L2_FIELD_NONE;
		vb2_buffer_done(&done->vb.vb2_buf,
				src & INT_ERRORS ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	}
	c->sequence++;
	spin_unlock(&pc->slock);
	return IRQ_HANDLED;
}

static struct pc_cam *vdev_to_cam(struct file *file)
{
	return container_of(video_devdata(file), struct pc_cam, vdev);
}

static void pc_fill_pix(const struct pc_desc *d, struct v4l2_pix_format *p)
{
	memset(p, 0, sizeof(*p));
	p->width = d->width;
	p->height = d->height;
	p->pixelformat = d->pixfmt;
	p->field = V4L2_FIELD_NONE;
	p->bytesperline = desc_stride(d);
	p->sizeimage = desc_size(d);
	p->colorspace = V4L2_COLORSPACE_RAW;
}

static int pc_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	struct pc_cam *c = vdev_to_cam(file);

	strscpy(cap->driver, DRIVER_NAME, sizeof(cap->driver));
	snprintf(cap->card, sizeof(cap->card), "Pixel 7 Pro %s camera", c->d->name);
	return 0;
}

static int pc_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	const struct pc_desc *d = vdev_to_cam(file)->d;

	if (f->index || (f->mbus_code && f->mbus_code != d->code))
		return -EINVAL;
	f->pixelformat = d->pixfmt;
	return 0;
}

static int pc_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	pc_fill_pix(vdev_to_cam(file)->d, &f->fmt.pix);
	return 0;
}

static int pc_enum_framesizes(struct file *file, void *fh, struct v4l2_frmsizeenum *fs)
{
	const struct pc_desc *d = vdev_to_cam(file)->d;

	if (fs->index || fs->pixel_format != d->pixfmt)
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fs->discrete.width = d->width;
	fs->discrete.height = d->height;
	return 0;
}

static const struct v4l2_ioctl_ops pc_ioctl_ops = {
	.vidioc_querycap = pc_querycap,
	.vidioc_enum_fmt_vid_cap = pc_enum_fmt,
	.vidioc_g_fmt_vid_cap = pc_g_fmt,
	.vidioc_s_fmt_vid_cap = pc_g_fmt,
	.vidioc_try_fmt_vid_cap = pc_g_fmt,
	.vidioc_enum_framesizes = pc_enum_framesizes,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations pc_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
};

/* ---------------- registration ---------------- */

static int adap_match(struct device *dev, const void *name)
{
	struct i2c_adapter *a = i2c_verify_adapter(dev);

	return a && !strcmp(a->name, name);
}

static bool wanted(const char *name)
{
	const char *p = cameras;
	size_t n = strlen(name);

	while (p && (p = strstr(p, name))) {
		if ((p == cameras || p[-1] == ',') && (p[n] == ',' || !p[n]))
			return true;
		p += n;
	}
	return false;
}

static int cam_register(struct pc_dev *pc, struct pc_cam *c)
{
	const struct pc_desc *d = c->d;
	struct device *adev;
	int ret;

	adev = bus_find_device(&i2c_bus_type, NULL, d->adapter, adap_match);
	if (!adev)
		return dev_err_probe(pc->dev, -EPROBE_DEFER, "%s: no %s (pixel-hsi2c-cam)\n",
				     d->name, d->adapter);
	c->adap = i2c_verify_adapter(adev);

	v4l2_subdev_init(&c->sd, &cam_subdev_ops);
	c->sd.internal_ops = &cam_internal_ops;
	c->sd.owner = THIS_MODULE;
	c->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	c->sd.dev = pc->dev;
	snprintf(c->sd.name, sizeof(c->sd.name), "%s %d-%04x", d->model, c->adap->nr, d->addr);
	ret = cam_init_controls(c);
	if (ret)
		goto put;
	c->sd_pad.flags = MEDIA_PAD_FL_SOURCE;
	c->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&c->sd.entity, 1, &c->sd_pad);
	ret = ret ?: v4l2_subdev_init_finalize(&c->sd);
	ret = ret ?: v4l2_device_register_subdev(&pc->v4l2, &c->sd);
	if (ret)
		goto ctrls;

	INIT_LIST_HEAD(&c->bufs);
	mutex_init(&c->vlock);
	c->queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	c->queue.io_modes = VB2_MMAP | VB2_DMABUF;
	c->queue.drv_priv = c;
	c->queue.buf_struct_size = sizeof(struct pc_buf);
	c->queue.ops = &pc_vb2_ops;
	c->queue.mem_ops = &vb2_dma_contig_memops;
	c->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	c->queue.min_queued_buffers = 1;
	c->queue.lock = &c->vlock;
	c->queue.dev = pc->dev;
	ret = vb2_queue_init(&c->queue);
	if (ret)
		goto subdev;

	snprintf(c->vdev.name, sizeof(c->vdev.name), "pixel-camera %s", d->name);
	c->vdev.fops = &pc_fops;
	c->vdev.ioctl_ops = &pc_ioctl_ops;
	c->vdev.release = video_device_release_empty;
	c->vdev.v4l2_dev = &pc->v4l2;
	c->vdev.queue = &c->queue;
	c->vdev.lock = &c->vlock;
	c->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
	c->vpad.flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&c->vdev.entity, 1, &c->vpad);
	ret = ret ?: video_register_device(&c->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto subdev;
	ret = media_create_pad_link(&c->sd.entity, 0, &c->vdev.entity, 0,
				    MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (ret)
		goto video;
	if (d->lens_addr) {
		ret = lens_register(pc, c);
		if (ret)
			goto video;
	}
	c->present = true;
	dev_info(pc->dev, "%s: %s on %s, /dev/video%d%s\n", d->name, c->sd.name, d->adapter,
		 c->vdev.num, c->has_lens ? ", with a lens" : "");
	return 0;
video:
	video_unregister_device(&c->vdev);
subdev:
	v4l2_device_unregister_subdev(&c->sd);
	v4l2_subdev_cleanup(&c->sd);
ctrls:
	v4l2_ctrl_handler_free(&c->ctrls);
put:
	put_device(adev);
	return ret;
}

static void cam_unregister(struct pc_cam *c)
{
	if (!c->present)
		return;
	if (c->has_lens) {
		v4l2_device_unregister_subdev(&c->lens);
		v4l2_ctrl_handler_free(&c->lens_ctrls);
		c->has_lens = false;
	}
	video_unregister_device(&c->vdev);
	v4l2_device_unregister_subdev(&c->sd);
	v4l2_subdev_cleanup(&c->sd);
	v4l2_ctrl_handler_free(&c->ctrls);
	put_device(&c->adap->dev);
	c->present = false;
}

static struct clk *acpm_clk(int index, const char *name)
{
	struct device_node *np = of_find_node_by_path("/power-management");
	struct of_phandle_args args = { .np = np, .args_count = 1, .args = { index } };
	struct clk *c;

	if (!np)
		return NULL;
	c = of_clk_get_from_provider(&args);
	of_node_put(np);
	if (IS_ERR(c))
		return NULL;
	if (strcmp(__clk_get_name(c), name)) {
		clk_put(c);
		return NULL;
	}
	return c;
}

static int pc_map_irq(void)
{
	struct device_node *gic = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
	struct irq_fwspec spec = {
		.param_count = 3,
		.param = { 0, CSIS_DMA0_SPI, IRQ_TYPE_LEVEL_HIGH },
	};
	int irq;

	if (!gic)
		return -ENODEV;
	spec.fwnode = of_fwnode_handle(gic);
	irq = irq_create_fwspec_mapping(&spec);
	of_node_put(gic);
	return irq ?: -EINVAL;
}

static int pc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pc_dev *pc;
	unsigned int i, n = 0;
	int ret;

	pc = devm_kzalloc(dev, sizeof(*pc), GFP_KERNEL);
	if (!pc)
		return -ENOMEM;
	pc->dev = dev;
	spin_lock_init(&pc->slock);
	mutex_init(&pc->lock);
	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	pc->phy = devm_ioremap(dev, CSIS_PHY, 0x10000);
	pc->sys = devm_ioremap(dev, SYSREG_CSIS, 0x1000);
	pc->ebuf = devm_ioremap(dev, CSIS_EBUF, 0x1000);
	pc->dma = devm_ioremap(dev, CSIS_DMA, 0x10000);
	if (!pc->phy || !pc->sys || !pc->ebuf || !pc->dma)
		return -ENOMEM;
	pc->cam_clk = acpm_clk(10, "cam");
	pc->int_clk = acpm_clk(1, "int");

	pc->irq = pc_map_irq();
	if (pc->irq < 0)
		return dev_err_probe(dev, pc->irq, "CSIS DMA interrupt\n");
	irq_set_status_flags(pc->irq, IRQ_NOAUTOEN);
	ret = devm_request_irq(dev, pc->irq, pc_irq, 0, DRIVER_NAME, pc);
	if (ret)
		return ret;

	pc->mdev.dev = dev;
	strscpy(pc->mdev.model, "Pixel 7 Pro cameras", sizeof(pc->mdev.model));
	strscpy(pc->mdev.bus_info, "platform:" DRIVER_NAME, sizeof(pc->mdev.bus_info));
	media_device_init(&pc->mdev);
	pc->v4l2.mdev = &pc->mdev;
	ret = v4l2_device_register(dev, &pc->v4l2);
	if (ret)
		goto mdev;
	for (i = 0; i < NCAMS; i++) {
		pc->cams[i].d = &descs[i];
		pc->cams[i].pc = pc;
		if (!wanted(descs[i].name))
			continue;
		ret = cam_register(pc, &pc->cams[i]);
		if (ret)
			goto cams;
		n++;
	}
	ret = v4l2_device_register_subdev_nodes(&pc->v4l2);
	ret = ret ?: media_device_register(&pc->mdev);
	if (ret)
		goto cams;
	platform_set_drvdata(pdev, pc);
	dev_info(dev, "%u cameras on /dev/media%d\n", n, pc->mdev.devnode->minor);
	return 0;
cams:
	for (i = 0; i < NCAMS; i++)
		cam_unregister(&pc->cams[i]);
	v4l2_device_unregister(&pc->v4l2);
mdev:
	media_device_cleanup(&pc->mdev);
	if (pc->cam_clk)
		clk_put(pc->cam_clk);
	if (pc->int_clk)
		clk_put(pc->int_clk);
	return ret;
}

static void pc_remove(struct platform_device *pdev)
{
	struct pc_dev *pc = platform_get_drvdata(pdev);
	unsigned int i;

	media_device_unregister(&pc->mdev);
	for (i = 0; i < NCAMS; i++)
		cam_unregister(&pc->cams[i]);
	v4l2_device_unregister(&pc->v4l2);
	media_device_cleanup(&pc->mdev);
	if (pc->cam_clk)
		clk_put(pc->cam_clk);
	if (pc->int_clk)
		clk_put(pc->int_clk);
}

static struct platform_driver pc_driver = {
	.probe = pc_probe,
	.remove = pc_remove,
	.driver = { .name = DRIVER_NAME },
};

static struct platform_device *pc_pdev;

static int __init pc_init(void)
{
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	ret = platform_driver_register(&pc_driver);
	if (ret)
		return ret;
	pc_pdev = platform_device_register_simple(DRIVER_NAME, -1, NULL, 0);
	if (IS_ERR(pc_pdev)) {
		platform_driver_unregister(&pc_driver);
		return PTR_ERR(pc_pdev);
	}
	return 0;
}

static void __exit pc_exit(void)
{
	platform_device_unregister(pc_pdev);
	platform_driver_unregister(&pc_driver);
}

module_init(pc_init);
module_exit(pc_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pixel 7 Pro (GS201) cameras for V4L2 and libcamera");
MODULE_SOFTDEP("pre: pixel-camera-power");
