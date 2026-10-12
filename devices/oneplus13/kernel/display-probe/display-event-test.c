// SPDX-License-Identifier: GPL-2.0-only
/* Bounded IRQ diagnostic. Temporarily enables documented completion sources,
 * restoring the masks afterward; never changes panel mode or buffer addresses.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

struct event_counts {
	void __iomem *regs;
	atomic_t te, pp0, pp1;
};
static bool kickstart;
module_param(kickstart, bool, 0444);
MODULE_PARM_DESC(kickstart, "Restart only the verified inherited CTL0 transfer");

static irqreturn_t event_irq(int irq, void *data)
{
	struct event_counts *c = data;
	u32 pp = readl(c->regs + 0x1014) & 0x300;
	u32 tear = readl(c->regs + 0x36804) & 4;
	if (!pp && !tear)
		return IRQ_NONE;
	if (pp) {
		if (pp & 0x100) atomic_inc(&c->pp0);
		if (pp & 0x200) atomic_inc(&c->pp1);
		writel(pp, c->regs + 0x1018);
	}
	if (tear) {
		atomic_inc(&c->te);
		writel(tear, c->regs + 0x36808);
	}
	return IRQ_HANDLED;
}

static int __init event_test_init(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	void __iomem *regs;
	u64 start, elapsed;
	struct event_counts counts = {};
	int irq;
	int ret = 0;

	if (!of_machine_is_compatible("oneplus,cph2655-omarchy-probe"))
		return -ENODEV;
	np = of_find_node_by_path("/soc@0/display-subsystem@ae00000");
	if (!np)
		return -ENODEV;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return -ENODEV;
	device_lock(&pdev->dev);
	if (!pdev->dev.driver || strcmp(pdev->dev.driver->name, "oneplus13-display-probe") ||
	    !pm_runtime_active(&pdev->dev)) {
		ret = -ENODEV;
		goto out;
	}
	regs = ioremap(0xAE00000, 0x93800);
	if (!regs) {
		ret = -ENOMEM;
		goto out;
	}
	/* Do not consume status belonging to an IRQ-driven owner. */
	if (readl(regs + 0x1010) || readl(regs + 0x36800)) {
		ret = -EBUSY;
		goto unmap;
	}
	if (kickstart && (readl(regs) != 0xc0000000 ||
	    readl(regs + 0x16014) != 0xf0020000 ||
	    readl(regs + 0x362b4) != 0x80000001 ||
	    readl(regs + 0x5014) != 0xfc800000 ||
	    readl(regs + 0x7014) != 0xfc800000)) {
		ret = -EINVAL;
		goto unmap;
	}
	writel(0x300, regs + 0x1018);
	writel(4, regs + 0x36808);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto unmap;
	}
	counts.regs = regs;
	ret = request_irq(irq, event_irq, 0, "oneplus13-display-event-test", &counts);
	if (ret)
		goto unmap;
	start = ktime_get_ns();
	writel(0x300, regs + 0x1010);
	writel(4, regs + 0x36800);
	if (kickstart)
		writel(1, regs + 0x1601c);
	msleep(2000);
	writel(0, regs + 0x1010);
	writel(0, regs + 0x36800);
	readl(regs + 0x36800);
	free_irq(irq, &counts);
	elapsed = ktime_get_ns() - start;
	pr_info("oneplus13-events: duration_us=%llu TE=%u PP0_done=%u PP1_done=%u autofresh=%08x\n",
		elapsed / 1000, atomic_read(&counts.te), atomic_read(&counts.pp0),
		atomic_read(&counts.pp1), readl(regs + 0x362B4));
unmap:
	iounmap(regs);
out:
	device_unlock(&pdev->dev);
	put_device(&pdev->dev);
	return ret;
}
static void __exit event_test_exit(void) {}
module_init(event_test_init);
module_exit(event_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OnePlus 13 bounded display event polling diagnostic");
