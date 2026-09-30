// SPDX-License-Identifier: GPL-2.0-only
/* Suspend support for the bring-up AP watchdog feeder. Userspace is frozen
 * before suspend_noirq and cannot kick its watchdog while the AP sleeps.
 * Preserve ABL's divider/prescaler and the feeder's timeout. Stop only for
 * the noirq/sleep interval, then reload and rearm before userspace resumes.
 * Loading/unloading does not arm, disarm or take over the live watchdog.
 *
 * Deep sleep (mem_sleep "deep": PSCI SYSTEM_SUSPEND) powers MISC, and with it
 * both watchdogs, off, so a running watchdog cannot be a deadman there, and
 * keep_running is ignored: the watchdogs are stopped and restored as usual.
 * pm_suspend_target_state tells the two apart: suspend_devices_and_enter()
 * sets it before any device callback, PM_SUSPEND_MEM for deep (PSCI offers no
 * standby) and PM_SUSPEND_TO_IDLE for s2idle. It stays PM_SUSPEND_MEM under
 * pm_test, which is harmless. After deep, a watchdog that was disabled gets
 * its saved WTCON back if power loss changed it.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/suspend.h>

#define WTCON 0
#define WTDAT 4
#define WTCNT 8
#define ENABLE BIT(5)
#define RESET BIT(0)

static struct {
	void __iomem *regs;
	u32 control, ticks;
} wdts[2];
static struct platform_device *pdev;
static unsigned int resumes;
module_param(resumes, uint, 0444);
static bool keep_running;
module_param(keep_running, bool, 0644);
MODULE_PARM_DESC(keep_running, "Diagnostic only: reload but do not pause enabled watchdogs in s2idle");
static unsigned int deep_suspends;
module_param(deep_suspends, uint, 0444);
MODULE_PARM_DESC(deep_suspends, "Deep (mem) suspends, where keep_running does not apply");
static bool deep;

static int pixel_wdt_resume(struct device *dev)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(wdts); i++) {
		if (!(wdts[i].control & ENABLE)) {
			/* Only a power loss can have changed it. */
			if (deep && readl(wdts[i].regs + WTCON) != wdts[i].control) {
				writel(wdts[i].control, wdts[i].regs + WTCON);
				if (readl(wdts[i].regs + WTCON) != wdts[i].control)
					dev_err(dev, "WDT%u failed to stay disabled\n", i);
			}
			continue;
		}
		writel(wdts[i].ticks, wdts[i].regs + WTDAT);
		writel(wdts[i].ticks, wdts[i].regs + WTCNT);
		writel(wdts[i].control, wdts[i].regs + WTCON);
		if (readl(wdts[i].regs + WTCON) != wdts[i].control)
			dev_err(dev, "WDT%u failed to rearm\n", i);
	}
	resumes++;
	deep = false;
	return 0;
}

static int pixel_wdt_suspend(struct device *dev)
{
	unsigned int i;

	deep = pm_suspend_target_state == PM_SUSPEND_MEM;
	if (deep)
		deep_suspends++;
	/* Validate both before changing either. No feeder runs in noirq. */
	for (i = 0; i < ARRAY_SIZE(wdts); i++) {
		wdts[i].control = readl(wdts[i].regs + WTCON);
		wdts[i].ticks = readl(wdts[i].regs + WTDAT);
		if ((wdts[i].control & ENABLE) &&
		    (!wdts[i].ticks || wdts[i].ticks > 0xffff))
			return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(wdts); i++) {
		if (!(wdts[i].control & ENABLE))
			continue;
		if (READ_ONCE(keep_running) && !deep) {
			/* A short RTC experiment may retain the existing reset
			 * watchdog as fallback. Never arm a previously stopped WDT
			 * or extend its timeout. Recovery depends on its clock.
			 */
			writel(wdts[i].ticks, wdts[i].regs + WTCNT);
			continue;
		}
		writel(wdts[i].control & ~(ENABLE | RESET), wdts[i].regs + WTCON);
		if (readl(wdts[i].regs + WTCON) & (ENABLE | RESET)) {
			pixel_wdt_resume(dev);
			return -EIO;
		}
	}
	return 0;
}

static const struct dev_pm_ops pixel_wdt_pm = {
	.suspend_noirq = pixel_wdt_suspend,
	.resume_noirq = pixel_wdt_resume,
};

static int pixel_wdt_probe(struct platform_device *dev)
{
	static const char * const compat[] = {
		"google,gs201-cl0-wdt", "google,gs201-cl1-wdt",
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(wdts); i++) {
		struct device_node *np;
		struct resource res;
		int ret;

		np = of_find_compatible_node(NULL, NULL, compat[i]);
		if (!np)
			return -ENODEV;
		ret = of_address_to_resource(np, 0, &res);
		of_node_put(np);
		if (ret || res.start != 0x10060000 + i * 0x10000 ||
		    resource_size(&res) != 0x100)
			return -EINVAL;
		wdts[i].regs = devm_ioremap(&dev->dev, res.start, resource_size(&res));
		if (!wdts[i].regs)
			return -ENOMEM;
	}
	return 0;
}

static struct platform_driver driver = {
	.probe = pixel_wdt_probe,
	.driver = { .name = "pixel-wdt-pm", .pm = &pixel_wdt_pm },
};

static int __init pixel_wdt_init(void)
{
	int ret;

	if (!of_machine_is_compatible("google,GS201 CHEETAH"))
		return -ENODEV;
	ret = platform_driver_register(&driver);
	if (ret)
		return ret;
	pdev = platform_device_register_simple("pixel-wdt-pm", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		platform_driver_unregister(&driver);
		return ret;
	}
	if (!pdev->dev.driver) {
		platform_device_unregister(pdev);
		platform_driver_unregister(&driver);
		return -ENODEV;
	}
	return 0;
}

static void __exit pixel_wdt_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&driver);
}
module_init(pixel_wdt_init);
module_exit(pixel_wdt_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pixel AP watchdog pause/rearm across system suspend");
