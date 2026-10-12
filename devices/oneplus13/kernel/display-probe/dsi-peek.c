// SPDX-License-Identifier: GPL-2.0-only
/* One-shot read-only dump of the retained DSI0 controller registers.
 * Requires the display power owner (oneplus13-display-probe) to be bound and
 * runtime-active, so the MDSS AHB clock is held. Never writes a register.
 * Init always fails after logging, so the module never stays loaded.
 */
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/printk.h>

#define DSI0_BASE 0x0ae94000
#define DSI0_SIZE 0x400

static int __init dsi_peek_init(void)
{
	struct device_node *np;
	struct platform_device *owner;
	void __iomem *regs;
	u32 buf[DSI0_SIZE / 4];
	int i, ret = -ENODEV;

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
	for (i = 0; i < DSI0_SIZE / 4; i++)
		buf[i] = readl(regs + i * 4);
	iounmap(regs);
	for (i = 0; i < DSI0_SIZE / 4; i += 4)
		pr_info("dsi0 %03x: %08x %08x %08x %08x\n", i * 4,
			buf[i], buf[i + 1], buf[i + 2], buf[i + 3]);
	ret = -EAGAIN;
out:
	device_unlock(&owner->dev);
	put_device(&owner->dev);
	return ret;
}
module_init(dsi_peek_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OnePlus 13 retained DSI0 register dump (read-only, one-shot)");
