// SPDX-License-Identifier: GPL-2.0-only
/* One raw frame from a running CSIS link through CSIS write-DMA (bring-up).
 *
 * Milestone (c) of docs/camera-plan-20260930.md. Load it while the sensor
 * streams and its link runs (kernel/camera/csis-probe.py setup), with pd_csis
 * and pd_pdp on and the CAM DVFS raised (pixel-csis-iso). It allocates a
 * buffer below 4 GiB (the context's address register is 32-bit), resets the
 * shared WDMA block and turns it on, bypasses the elastic buffer, routes the
 * link to the WDMA through SYSREG_CSIS, programs channel 0 of one context
 * (2D, RAW10 unpacked to 16-bit words, resolution, stride, ADDR1 with only
 * slot 0 valid) and enables it, then waits for two frame ends. The frame is
 * at /sys/kernel/debug/pixel-csis-capture/frame (little-endian 16-bit, 10
 * significant bits), what happened in .../status. Unloading stops the DMA,
 * puts SYSREG_CSIS back and frees the buffer.
 *
 * Register map: the Pixel 6 port's gs101-mipi-csis (CSIS v5.4; the GS201
 * channel registers match, the common block follows the Exynos 2100 layout)
 * and its notes: EBUF bypass, pixel align, format 6, and FCNTSEQ = slot 0
 * only (its reset value enables all 32 address slots).
 *
 * GS201 routing, found on the phone (2026-09-30), differs from GS101:
 *  - SYSREG_CSIS 0x430 = the link number feeds the WDMA (every context sees
 *    it); without it no frame start reaches the WDMA.
 *  - 0x408/0x40c/0x410 are the HAL's CSIS_SC_CON0..2 ("PDP MUX", 3 bits
 *    each), not per-context DMA muxes. At their reset value 0 the WDMA gets
 *    frame and line syncs but no pixel data (DEBUG_INFO HDCNT data count 0,
 *    LASTDATA/LASTADDR errors, buffer untouched); any non-zero value in all
 *    three lets the data through.
 *  - The GS101 enable bits at 0x488 make no difference.
 * The WDMA's own test pattern generator (tpg=1) writes memory without any
 * of this, which is how the bus side was ruled out.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define PMU_CSIS_STATUS		0x18062404
#define CSIS_LINK(n)		(0x1a440000 + (n) * 0x10000)
#define CSIS_EBUF		0x1a4c0000
#define CSIS_DMA		0x1a4d0000
#define SYSREG_CSIS		0x1a420000

#define LINK_UPD_SDW		0x00c
#define LINK_DBG_OPTION_SUITE	0x690
#define DBG_PIXEL_ALIGN_EN	BIT(23)

#define CMN_DMA_CTRL		0x0008
#define CMN_SW_RESET		BIT(0)
#define CMN_IP_PROCESSING	BIT(1)
#define CMN_DMA_CLK_CTRL	0x0014
#define CMN_CLKGATE_OFF		BIT(0)
#define CMN_TP_ON		0x0020		/* TESTPATTERN 31, PPCMODE 30, VBLANK 29:0 */
#define CMN_TP_CTRL		0x0024		/* VTOHBLANK 23:16, HBLANK 15:8, HTOVBLANK 7:0 */
#define CMN_TP_SIZE		0x0028		/* VSIZE 29:16, HSIZE 13:0 */
#define CMN_TP_ENABLE		BIT(31)
#define CMN_TP_PPC_DUAL		BIT(30)
#define CMN_DMA_CFG_CSIS(n)	(0x0044 + (n) * 4)	/* HAL csis_cmn_dma_cfg_csis1 = 0x48 */
#define CMN_DEBUG_EN		0x0100
#define CMN_DBG_VCNT(n)		(0x0144 + (n) * 0x20)	/* CSIS_DMAn channel 0: frames */
#define CMN_DBG_HDCNT(n)	(0x0154 + (n) * 0x20)	/* lines 31:16, data 15:0 */
#define EBUF_CTRL		0x0000
#define EBUF_BYPASS		BIT(0)

#define DMA_CTX(n)		(0x7000 + (n) * 0x1000)
#define CTX_CTL			0x400		/* DATA_CTRL, INT_ENABLE, INT_SRC */
#define CTL_DATA_CTRL		0x0
#define CTL_INT_ENABLE		0x4
#define CTL_INT_SRC		0x8
#define CH_CTRL			0x00
#define CH_DMA_ENABLE		BIT(0)
#define CH_UPDT_PTR_EN		BIT(1)
#define CH_FMT			0x04
#define CH_ADDR1		0x10
#define CH_ACT_CTRL		0x90
#define CH_ACT_FRAMECNT_SEQ	0x94
#define CH_RESOL		0xa4
#define CH_STRIDE		0xa8
#define CH_FCNTSEQ		0xb0
#define FMT_U10BIT_UNPACK	0x6		/* 10 bits in 16, MSBs zero */
#define INT_FRAME_START0	BIT(4)
#define INT_FRAME_END0		BIT(8)
#define INT_ALL_BUT_LINE_END	0xfffffff0	/* GS201 layout: lwis gs201 csi.h */

#define SYSREG_SC_CON(n)	(0x408 + (n) * 4)
#define SYSREG_WDMA_LINK	0x430

static uint link = 2, ctx, width = 2016, height = 1508, timeout_ms = 1000, sc_con = 1;
static ushort fill = 0xa5a5;
static bool keep, tpg;
module_param(link, uint, 0444);
module_param(ctx, uint, 0444);
module_param(width, uint, 0444);
module_param(height, uint, 0444);
module_param(timeout_ms, uint, 0444);
module_param(sc_con, uint, 0444);
MODULE_PARM_DESC(sc_con, "value for CSIS_SC_CON0..2 (1..7; 0 blocks the WDMA data)");
module_param(fill, ushort, 0444);
MODULE_PARM_DESC(fill, "16-bit pattern the buffer holds before capture (shows untouched words)");
module_param(keep, bool, 0444);
MODULE_PARM_DESC(keep, "leave the channel enabled after the capture (until unload)");
module_param(tpg, bool, 0444);
MODULE_PARM_DESC(tpg, "feed the WDMA from its own test pattern generator instead of the link");

static const u16 sys_regs[] = { SYSREG_SC_CON(0), SYSREG_SC_CON(1), SYSREG_SC_CON(2), SYSREG_WDMA_LINK };
static u32 sys_saved[ARRAY_SIZE(sys_regs)];
static bool sys_changed;
static struct platform_device *pdev;
static void *cpu;
static dma_addr_t phys;
static size_t size;
static void __iomem *l, *eb, *dma, *sys;
static struct debugfs_blob_wrapper blob, status_blob;
static char status[768];
static struct dentry *dir;

static void unmap_all(void)
{
	if (l)
		iounmap(l);
	if (eb)
		iounmap(eb);
	if (dma)
		iounmap(dma);
	if (sys)
		iounmap(sys);
}

static void cleanup_regs(void)
{
	int i;

	if (dma && tpg)
		writel(readl(dma + CMN_TP_ON) & ~CMN_TP_ENABLE, dma + CMN_TP_ON);
	if (dma)
		writel(readl(dma + DMA_CTX(ctx) + CH_CTRL) & ~CH_DMA_ENABLE,
		       dma + DMA_CTX(ctx) + CH_CTRL);
	if (sys && sys_changed)
		for (i = 0; i < ARRAY_SIZE(sys_regs); i++)
			writel(sys_saved[i], sys + sys_regs[i]);
}

static int __init capture_init(void)
{
	void __iomem *pmu, *ch;
	u32 v, seen = 0, act, seq;
	unsigned long end, written = 0, words, k;
	int i, n = 0, ret, len;

	if (link > 7 || ctx > 3 || !sc_con || sc_con > 7 || !width || !height)
		return -EINVAL;
	pmu = ioremap(PMU_CSIS_STATUS & PAGE_MASK, PAGE_SIZE);
	if (!pmu)
		return -ENOMEM;
	v = readl(pmu + (PMU_CSIS_STATUS & ~PAGE_MASK));
	iounmap(pmu);
	if (!(v & 1)) {
		pr_err("pixel-csis-capture: pd_csis is off\n");
		return -ENODEV;
	}
	pdev = platform_device_register_simple("pixel-csis-capture", -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	ret = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err;
	size = PAGE_ALIGN((size_t)width * 2 * height);
	cpu = dma_alloc_coherent(&pdev->dev, size, &phys, GFP_KERNEL);
	ret = -ENOMEM;
	if (!cpu)
		goto err;
	words = (size_t)width * height;
	for (k = 0; k < words; k++)
		((u16 *)cpu)[k] = fill;
	l = ioremap(CSIS_LINK(link), 0x1000);
	eb = ioremap(CSIS_EBUF, 0x1000);
	dma = ioremap(CSIS_DMA, 0x10000);
	sys = ioremap(SYSREG_CSIS, 0x1000);
	if (!l || !eb || !dma || !sys)
		goto err;
	ch = dma + DMA_CTX(ctx);

	/* WDMA common: software reset, then IP_PROCESSING; clock gate off. */
	writel(readl(dma + CMN_DMA_CTRL) | CMN_SW_RESET, dma + CMN_DMA_CTRL);
	for (i = 0; i < 10 && (readl(dma + CMN_DMA_CTRL) & CMN_SW_RESET); i++)
		udelay(10);
	writel((readl(dma + CMN_DMA_CTRL) & ~CMN_SW_RESET) | CMN_IP_PROCESSING,
	       dma + CMN_DMA_CTRL);
	writel(readl(dma + CMN_DMA_CLK_CTRL) | CMN_CLKGATE_OFF, dma + CMN_DMA_CLK_CTRL);
	writel(1, dma + CMN_DEBUG_EN);
	writel(readl(eb + EBUF_CTRL) | EBUF_BYPASS, eb + EBUF_CTRL);

	/* Link: pixel alignment towards the WDMA, then latch the shadows. */
	writel(readl(l + LINK_DBG_OPTION_SUITE) | DBG_PIXEL_ALIGN_EN, l + LINK_DBG_OPTION_SUITE);
	writel(0xf, l + LINK_UPD_SDW);

	/* SYSREG_CSIS: the link feeds the WDMA, the PDP muxes off their reset value. */
	for (i = 0; i < ARRAY_SIZE(sys_regs); i++)
		sys_saved[i] = readl(sys + sys_regs[i]);
	sys_changed = true;
	for (i = 0; i < 3; i++)
		writel(sc_con, sys + SYSREG_SC_CON(i));
	writel(link, sys + SYSREG_WDMA_LINK);

	/* Context: OTF input, channel 0 = VC0. */
	writel(readl(ch + CTX_CTL + CTL_DATA_CTRL) & ~BIT(0), ch + CTX_CTL + CTL_DATA_CTRL);
	writel(FMT_U10BIT_UNPACK, ch + CH_FMT);			/* DIM 0: 2D */
	writel((height << 16) | width, ch + CH_RESOL);
	writel(width * 2, ch + CH_STRIDE);
	writel(lower_32_bits(phys), ch + CH_ADDR1);
	writel(BIT(0), ch + CH_FCNTSEQ);			/* slot 0 only */
	writel(INT_ALL_BUT_LINE_END, ch + CTX_CTL + CTL_INT_ENABLE);
	writel(readl(ch + CTX_CTL + CTL_INT_SRC), ch + CTX_CTL + CTL_INT_SRC);
	writel(CH_UPDT_PTR_EN | CH_DMA_ENABLE, ch + CH_CTRL);
	if (tpg) {
		/* pablo csi_hw_s_dma_common_pattern_enable at 400 MHz, 30 fps. */
		u32 vvalid = width * height / (400 * 2), vblank = 1000000 / 30 - vvalid;

		writel((0x80 << 16) | (70 << 8) | 0x40, dma + CMN_TP_CTRL);
		writel((height << 16) | width, dma + CMN_TP_SIZE);
		writel(CMN_TP_PPC_DUAL | (vblank * 400), dma + CMN_TP_ON);
		writel(readl(dma + CMN_TP_ON) | CMN_TP_ENABLE, dma + CMN_TP_ON);
	}

	/* Wait for a frame start, then for that frame's end. */
	end = jiffies + msecs_to_jiffies(timeout_ms);
	while (time_before(jiffies, end)) {
		v = readl(ch + CTX_CTL + CTL_INT_SRC);
		if (v) {
			writel(v, ch + CTX_CTL + CTL_INT_SRC);
			seen |= v;
			if ((v & INT_FRAME_END0) && (seen & INT_FRAME_START0) && ++n >= 2)
				break;
		}
		usleep_range(500, 1000);
	}
	act = readl(ch + CH_ACT_CTRL);
	seq = readl(ch + CH_ACT_FRAMECNT_SEQ);
	if (!keep)
		writel(readl(ch + CH_CTRL) & ~CH_DMA_ENABLE, ch + CH_CTRL);
	msleep(40);
	for (k = 0; k < words; k++)
		written += ((u16 *)cpu)[k] != fill;
	len = scnprintf(status, sizeof(status),
			"link %u ctx %u %ux%u buffer %pad (%zu bytes)\n"
			"frame ends %d, INT_SRC seen %#x, ACT_CTRL %#x ACT_FRAMECNT_SEQ %#x\n"
			"cmn CTRL %#x CFG_CSIS%u %#x debug VCNT %#x HDCNT %#x\n"
			"words changed from %#06x: %lu of %lu; first words %04x %04x %04x %04x\n",
			link, ctx, width, height, &phys, size, n, seen, act, seq,
			readl(dma + CMN_DMA_CTRL), ctx, readl(dma + CMN_DMA_CFG_CSIS(ctx)),
			readl(dma + CMN_DBG_VCNT(ctx)), readl(dma + CMN_DBG_HDCNT(ctx)),
			fill, written, words,
			((u16 *)cpu)[0], ((u16 *)cpu)[1], ((u16 *)cpu)[2], ((u16 *)cpu)[3]);
	pr_info("pixel-csis-capture: %s", status);
	blob.data = cpu;
	blob.size = (size_t)width * 2 * height;
	dir = debugfs_create_dir("pixel-csis-capture", NULL);
	debugfs_create_blob("frame", 0400, dir, &blob);
	status_blob.data = status;
	status_blob.size = len;
	debugfs_create_blob("status", 0444, dir, &status_blob);
	return 0;
err:
	cleanup_regs();
	unmap_all();
	if (cpu)
		dma_free_coherent(&pdev->dev, size, cpu, phys);
	platform_device_unregister(pdev);
	return ret;
}

static void __exit capture_exit(void)
{
	debugfs_remove_recursive(dir);
	cleanup_regs();
	dma_free_coherent(&pdev->dev, size, cpu, phys);
	unmap_all();
	platform_device_unregister(pdev);
}

module_init(capture_init);
module_exit(capture_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GS201 CSIS one-frame raw capture for camera bring-up");
