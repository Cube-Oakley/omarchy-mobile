// SPDX-License-Identifier: GPL-2.0-only
/* One-shot DSI0 command test for the retained OnePlus 13 panel.
 *
 * Sends DCS packets through the DSI controller's command DMA engine while the
 * retained scanout keeps the link configured. Completion is polled: the MDSS
 * interrupt line belongs to oneplus13-scanout, so no DSI interrupt mask is
 * ever enabled. Register offsets include the DSI 6G +4 shift.
 *
 *   read=0x0a        DCS read, 1 byte (power mode; 0x9c when the panel is on)
 *   read52=1         DCS read 0x52, 2 bytes (display brightness)
 *   brightness=N     DCS 0x51 with N (big-endian, panel range 1..4094)
 *   short_cmd=0x28   DCS short write without parameters (display off etc.)
 *
 * A command DMA timeout pins the buffer and keeps the module loaded.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include "aa569-on-120.h"

#define DSI0_BASE 0x0ae94000
#define DSI0_SIZE 0x400

#define R_CTRL		0x004
#define R_STATUS0	0x008
#define R_CMD_DMA_CTRL	0x03c
#define R_DMA_BASE	0x048
#define R_DMA_LEN	0x04c
#define R_ACK_ERR	0x068
#define R_RDBK(i)	(0x06c + 4 * (i))
#define R_TRIG_CTRL	0x084
#define R_TRIG_DMA	0x090
#define R_LANE_STATUS	0x0a8
#define R_TIMEOUT	0x0c0
#define R_INTR_CTRL	0x110
#define R_RDBK_CTRL	0x1d4

#define CMD_DMA_DONE	BIT(0)
#define DMA_LOW_POWER	BIT(26)

static int read = -1;
module_param(read, int, 0444);
static bool read52;
module_param(read52, bool, 0444);
static int brightness = -1;
module_param(brightness, int, 0444);
static int short_cmd = -1;
module_param(short_cmd, int, 0444);
static bool seq_on;
module_param(seq_on, bool, 0444);
MODULE_PARM_DESC(seq_on, "Replay the stock 120 Hz panel-on sequence (sleep out ... display on)");
static bool hs;
module_param(hs, bool, 0444);
MODULE_PARM_DESC(hs, "Send writes in high-speed mode (reads always use low power)");

static void __iomem *regs;
static u8 *buf;
static dma_addr_t buf_dma;
static struct platform_device *pdev;
static bool pinned;

static int dma_kick(int len, bool lp)
{
	u32 st, dma_ctrl = readl(regs + R_CMD_DMA_CTRL);
	int ret;

	if (readl(regs + R_STATUS0) & 0x7) {
		pr_err("dsi-cmd: engine busy %08x\n", readl(regs + R_STATUS0));
		return -EBUSY;
	}
	writel(lp ? dma_ctrl | DMA_LOW_POWER : dma_ctrl & ~DMA_LOW_POWER,
	       regs + R_CMD_DMA_CTRL);
	writel(CMD_DMA_DONE, regs + R_INTR_CTRL);	/* W1C status, masks stay 0 */
	wmb();
	writel(lower_32_bits(buf_dma), regs + R_DMA_BASE);
	writel(len, regs + R_DMA_LEN);
	writel(1, regs + R_TRIG_DMA);
	wmb();
	ret = readl_poll_timeout(regs + R_INTR_CTRL, st, st & CMD_DMA_DONE, 20, 200000);
	if (ret) {
		pinned = true;
		pr_err("dsi-cmd: DMA timeout intr=%08x status=%08x ack=%08x timeout=%08x lanes=%08x\n",
		       readl(regs + R_INTR_CTRL), readl(regs + R_STATUS0),
		       readl(regs + R_ACK_ERR), readl(regs + R_TIMEOUT),
		       readl(regs + R_LANE_STATUS));
		return ret;
	}
	writel(CMD_DMA_DONE, regs + R_INTR_CTRL);
	writel(dma_ctrl, regs + R_CMD_DMA_CTRL);
	return 0;
}

/* MSM command DMA layout: header bytes 1, 2, 0, then flags (last|long|read). */
static int pack(u8 type, const u8 *payload, int n, bool is_long, bool rx)
{
	int len;

	memset(buf, 0xff, 512);
	if (is_long) {
		buf[0] = n & 0xff;
		buf[1] = n >> 8;
		memcpy(buf + 4, payload, n);
		len = 4 + n;
	} else {
		buf[0] = n > 0 ? payload[0] : 0;
		buf[1] = n > 1 ? payload[1] : 0;
		len = 4;
	}
	buf[2] = type;
	buf[3] = BIT(7) | (is_long ? BIT(6) : 0) | (rx ? BIT(5) : 0);
	wmb();
	return (len + 3) & ~3;
}

static int dcs_read(u8 cmd, int size, u8 *out)
{
	u8 sz[2] = { size, 0 };
	u32 r[4];
	int i, ret;

	ret = dma_kick(pack(0x37, sz, 2, false, false), true);
	if (ret)
		return ret;
	writel(1, regs + R_RDBK_CTRL);
	wmb();
	writel(0, regs + R_RDBK_CTRL);
	wmb();
	ret = dma_kick(pack(0x06, &cmd, 1, false, true), true);
	if (ret)
		return ret;
	for (i = 0; i < 4; i++)
		r[i] = readl(regs + R_RDBK(i));
	pr_info("dsi-cmd: read %02x rdbk %08x %08x %08x %08x ack=%08x\n", cmd,
		r[0], r[1], r[2], r[3], readl(regs + R_ACK_ERR));
	/* Short response in RDBK0, big-endian: type, data0, data1, ECC. */
	for (i = 0; i < size && i < 2; i++)
		out[i] = (r[0] >> (16 - 8 * i)) & 0xff;
	return (r[0] >> 24) & 0xff;
}

static int __init dsi_cmd_init(void)
{
	struct device_node *np;
	struct platform_device *owner;
	u8 v[2];
	int ret = -ENODEV;

	if (!of_machine_is_compatible("oneplus,cph2655-omarchy-probe"))
		return -ENODEV;
	np = of_find_node_by_path("/soc@0/display-subsystem@ae00000");
	if (!np)
		return -ENODEV;
	owner = of_find_device_by_node(np);
	of_node_put(np);
	if (!owner)
		return -ENODEV;
	device_lock(&owner->dev);
	if (!owner->dev.driver || strcmp(owner->dev.driver->name, "oneplus13-display-probe") ||
	    !pm_runtime_active(&owner->dev))
		goto out;
	regs = ioremap(DSI0_BASE, DSI0_SIZE);
	if (!regs) {
		ret = -ENOMEM;
		goto out;
	}
	/* Exact retained command-mode profile: enabled, 4 lanes, idle, no IRQ masks. */
	if (readl(regs) != 0x20090000 || readl(regs + R_CTRL) != 0x1f5 ||
	    (readl(regs + R_CMD_DMA_CTRL) & ~DMA_LOW_POWER) != 0x10000000 ||
	    readl(regs + R_TRIG_CTRL) != 0x80000004 ||
	    (readl(regs + R_INTR_CTRL) & 0x02020202)) {
		pr_err("dsi-cmd: unexpected DSI state ctrl=%08x dma=%08x trig=%08x intr=%08x\n",
		       readl(regs + R_CTRL), readl(regs + R_CMD_DMA_CTRL),
		       readl(regs + R_TRIG_CTRL), readl(regs + R_INTR_CTRL));
		ret = -EINVAL;
		goto unmap;
	}
	pdev = platform_device_register_simple("oneplus13-dsi-cmd", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		goto unmap;
	}
	pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto unregister;
	buf = dma_alloc_coherent(&pdev->dev, PAGE_SIZE, &buf_dma, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto unregister;
	}

	if (read >= 0) {
		ret = dcs_read(read, 1, v);
		pr_info("dsi-cmd: dcs %02x -> type %02x value %02x\n", read, ret, v[0]);
	}
	if (!pinned && brightness >= 0 && brightness <= 4094) {
		u8 p[3] = { 0x51, brightness >> 8, brightness & 0xff };
		ret = dma_kick(pack(0x39, p, 3, true, false), !hs);
		pr_info("dsi-cmd: brightness %d -> %d\n", brightness, ret);
	}
	if (!pinned && short_cmd >= 0) {
		u8 c = short_cmd;
		ret = dma_kick(pack(0x05, &c, 1, false, false), !hs);
		pr_info("dsi-cmd: short %02x -> %d\n", c, ret);
	}
	if (!pinned && seq_on) {
		const u8 *r = aa569_on_120;
		int n = 0;

		while (!pinned && r < aa569_on_120 + sizeof(aa569_on_120)) {
			u8 type = r[0], delay = r[1], len = r[2];

			ret = dma_kick(pack(type, r + 3, len, type == 0x39, false), true);
			if (ret)
				break;
			if (delay)
				msleep(delay);
			r += 3 + len;
			n++;
		}
		pr_info("dsi-cmd: on sequence %d commands -> %d\n", n, ret);
	}
	if (!pinned && read52) {
		ret = dcs_read(0x52, 2, v);
		pr_info("dsi-cmd: brightness readback type %02x value %u\n", ret,
			(v[0] << 8) | v[1]);
	}
	if (pinned) {
		pr_err("dsi-cmd: command engine timed out; buffer and module pinned until reboot\n");
		device_unlock(&owner->dev);
		put_device(&owner->dev);
		return 0;
	}
	dma_free_coherent(&pdev->dev, PAGE_SIZE, buf, buf_dma);
	ret = -EAGAIN;
unregister:
	platform_device_unregister(pdev);
unmap:
	iounmap(regs);
out:
	device_unlock(&owner->dev);
	put_device(&owner->dev);
	return ret;
}
module_init(dsi_cmd_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OnePlus 13 retained-panel DSI command test (one-shot)");
