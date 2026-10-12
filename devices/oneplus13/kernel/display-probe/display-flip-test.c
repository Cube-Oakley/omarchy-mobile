// SPDX-License-Identifier: GPL-2.0-only
/* Timed RAM-only DMA experiment. Preserve the panel/DSI/DSC/SPR setup.
 * Pin any buffer that might remain active if restoration cannot be proven.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include "oneplus13-retained.h"

#define ORIGINAL 0xfc800000U
#define WIDTH 1440U
#define HEIGHT 3168U
#define PITCH (WIDTH * 4)
#define BYTES (PITCH * HEIGHT)

static bool run;
module_param(run, bool, 0444);
MODULE_PARM_DESC(run, "Perform one bounded DMA flip and restore the inherited buffer");
static bool manual;
module_param(manual, bool, 0444);
MODULE_PARM_DESC(manual, "Drain autorefresh and test on-demand transfers before restoring it");
static unsigned int flips = 1;
module_param(flips, uint, 0444);
MODULE_PARM_DESC(flips, "Bounded alternating transfer count, 1..240 (manual mode only above 1)");

struct flip_test {
	void __iomem *regs;
	wait_queue_head_t wait;
	atomic_t frames;
};


static irqreturn_t flip_irq(int irq, void *data)
{
	struct flip_test *test = data;
	if (!(readl(test->regs + 0x1014) & BIT(8)))
		return IRQ_NONE;
	writel(BIT(8), test->regs + 0x1018);
	atomic_inc(&test->frames);
	wake_up(&test->wait);
	return IRQ_HANDLED;
}

static int wait_two_frames(struct flip_test *test)
{
	int goal = atomic_read(&test->frames) + 2;
	return wait_event_timeout(test->wait, atomic_read(&test->frames) >= goal,
				msecs_to_jiffies(150)) ? 0 : -ETIMEDOUT;
}

static int transfer(struct flip_test *test, u32 address)
{
	int ret, goal = atomic_read(&test->frames) + (manual ? 1 : 2);
	wmb();
	writel(address, test->regs + 0x5014);
	writel(address, test->regs + 0x7014);
	writel(3, test->regs + 0x16018); /* VIG0 + VIG1 flush, upstream CTL mapping */
	/* Stock SDE explicitly omits CTL_START while autorefresh is enabled. */
	wmb();
	if (manual)
		writel(1, test->regs + 0x1601c);
	ret = wait_event_timeout(test->wait, atomic_read(&test->frames) >= goal,
				msecs_to_jiffies(150)) ? 0 : -ETIMEDOUT;
	if (!ret && ((readl(test->regs + 0x16018) & 3) ||
	    readl(test->regs + 0x5014) != address || readl(test->regs + 0x7014) != address))
		ret = -EIO;
	return ret;
}

static int drain_autorefresh(struct flip_test *test)
{
	u32 sync = readl(test->regs + 0x36288), line;
	int ret;
	/* Upstream dpu_hw_intf_disable_autorefresh: disconnect external TE,
	 * disable auto mode, drain the in-flight transfer, reconnect TE. */
	writel(sync & ~BIT(20), test->regs + 0x36288);
	writel(0, test->regs + 0x362b4);
	ret = readl_poll_timeout(test->regs + 0x362b0, line,
				!(line & 0xffff) || (line & 0xffff) >= HEIGHT,
				100, 50000);
	writel(sync, test->regs + 0x36288);
	pr_info("oneplus13-flip: drain=%d line=%08x autorefresh=%08x\n",
		ret, line, readl(test->regs + 0x362b4));
	return ret;
}

static int __init flip_test_init(void)
{
	struct device_node *np;
	struct platform_device *owner, *dma_dev;
	struct flip_test test = {};
	dma_addr_t address;
	u32 *pixels;
	unsigned int x, y, n = 0;
	ktime_t started;
	int irq, ret, restore;

	if (!run || !flips || flips > 240 || (flips > 1 && !manual) ||
	    !of_machine_is_compatible("oneplus,cph2655-omarchy-probe"))
		return -ENODEV;
	ret = verify_bypass();
	if (ret)
		return ret;
	np = of_find_node_by_path("/soc@0/display-subsystem@ae00000");
	if (!np)
		return -ENODEV;
	owner = of_find_device_by_node(np);
	of_node_put(np);
	if (!owner)
		return -ENODEV;
	device_lock(&owner->dev);
	if (!owner->dev.driver || strcmp(owner->dev.driver->name, "oneplus13-display-probe") ||
	    !pm_runtime_active(&owner->dev)) {
		ret = -ENODEV;
		goto put_owner;
	}
	test.regs = ioremap(0xAE00000, 0x93800);
	if (!test.regs) {
		ret = -ENOMEM;
		goto put_owner;
	}
	if (readl(test.regs) != 0xc0000000 ||
	    readl(test.regs + 0x5014) != ORIGINAL || readl(test.regs + 0x7014) != ORIGINAL ||
	    readl(test.regs + 0x5000) != 0x0c6002d0 || readl(test.regs + 0x7000) != 0x0c6002d0 ||
	    readl(test.regs + 0x5008) || readl(test.regs + 0x7008) != 720 ||
	    readl(test.regs + 0x5024) != PITCH || readl(test.regs + 0x7024) != PITCH ||
	    readl(test.regs + 0x5034) != 0x03020001 || readl(test.regs + 0x7034) != 0x03020001 ||
	    readl(test.regs + 0x16014) != 0xf0020000 || readl(test.regs + 0x362b4) != 0x80000001 ||
	    readl(test.regs + 0x6a404) != 0x00281127 || readl(test.regs + 0x6b404) != 0x00281127 ||
	    readl(test.regs + 0x1010) || readl(test.regs + 0x36800)) {
		ret = -EINVAL;
		goto unmap;
	}
	dma_dev = platform_device_register_simple("oneplus13-flip-buffer", -1, NULL, 0);
	if (IS_ERR(dma_dev)) {
		ret = PTR_ERR(dma_dev);
		goto unmap;
	}
	dma_dev->dev.dma_mask = &dma_dev->dev.coherent_dma_mask;
	ret = dma_set_mask_and_coherent(&dma_dev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto unregister;
	pixels = dma_alloc_wc(&dma_dev->dev, BYTES, &address, GFP_KERNEL);
	if (!pixels) {
		ret = -ENOMEM;
		goto unregister;
	}
	if (upper_32_bits(address + BYTES - 1)) {
		ret = -ERANGE;
		goto free_buffer;
	}
	for (y = 0; y < HEIGHT; y++)
		for (x = 0; x < WIDTH; x++)
			pixels[y * WIDTH + x] = y < HEIGHT / 2 ?
				(x < WIDTH / 2 ? 0xffbb55aa : 0xff55bbbb) :
				(x < WIDTH / 2 ? 0xff339966 : 0xffeeeeee);
	init_waitqueue_head(&test.wait);
	irq = platform_get_irq(owner, 0);
	if (irq < 0) {
		ret = irq;
		goto free_buffer;
	}
	writel(BIT(8), test.regs + 0x1018);
	ret = request_irq(irq, flip_irq, 0, "oneplus13-display-flip-test", &test);
	if (ret)
		goto free_buffer;
	writel(BIT(8), test.regs + 0x1010);
	ret = wait_two_frames(&test);
	if (!ret && manual)
		ret = drain_autorefresh(&test);
	if (!ret) {
		pr_info("oneplus13-flip: switching to DMA %pad size=%u\n", &address, BYTES);
		started = ktime_get();
		for (n = 0; n < flips; n++) {
			ret = transfer(&test, n & 1 ? ORIGINAL : lower_32_bits(address));
			if (ret)
				break;
		}
		pr_info("oneplus13-flip: completed=%u elapsed_us=%lld manual=%u\n",
			n, ktime_us_delta(ktime_get(), started), manual);
		if (!ret && flips == 1)
			msleep(2000);
		/* Always attempt original restoration, even after a failed flip. */
		restore = transfer(&test, ORIGINAL);
	} else {
		restore = 0; /* No buffer address was written. */
	}
	if (manual) {
		writel(0x80000001, test.regs + 0x362b4);
		if (wait_two_frames(&test))
			restore = -ETIMEDOUT;
	}
	writel(0, test.regs + 0x1010);
	readl(test.regs + 0x1010);
	free_irq(irq, &test);
	pr_info("oneplus13-flip: result=%d restore=%d frames=%d active=%08x/%08x flush=%08x\n",
		ret, restore, atomic_read(&test.frames), readl(test.regs + 0x5014),
		readl(test.regs + 0x7014), readl(test.regs + 0x16018));
	if (restore) {
		/* Hardware may still reference this buffer. Keep allocation/device
		 * alive and refuse module unload until the RAM boot ends. */
		__module_get(THIS_MODULE);
		pr_err("oneplus13-flip: uncertain restoration; DMA memory pinned until reboot\n");
		ret = 0;
		goto unmap;
	}
free_buffer:
	dma_free_wc(&dma_dev->dev, BYTES, pixels, address);
unregister:
	platform_device_unregister(dma_dev);
unmap:
	iounmap(test.regs);
put_owner:
	device_unlock(&owner->dev);
	put_device(&owner->dev);
	return ret;
}
static void __exit flip_test_exit(void) {}
module_init(flip_test_init);
module_exit(flip_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OnePlus 13 guarded DMA buffer flip and restoration test");
