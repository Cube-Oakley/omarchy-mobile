// SPDX-License-Identifier: GPL-2.0-only
/* Read-only SICD prerequisite audit. Never changes CPU hints or PMU state.
 * Sample the stock GS201 status registers only in the system noirq window
 * when the last little CPU enters CPU_PM. Counts are attempts, not residency.
 * ACPM drainage is checked when its helper is available. Timer deadlines
 * and the full idle-IP list are NOT covered;
 * a candidate does not mean it is safe to issue a SICD hint.
 */
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>

static void __iomem *pmu, *ufs, *wifi_elbi;
static struct platform_device *pdev;
static struct acpm_handle *acpm;
static bool (*acpm_idle)(struct acpm_handle *handle);
static DEFINE_RAW_SPINLOCK(audit_lock);
static bool noirq_window;
static u8 idle_cpus;
static unsigned int counts[11];
module_param_array(counts, uint, NULL, 0444);
MODULE_PARM_DESC(counts, "attempts, CPU pending, cluster on, UFS on, debug busy, AUR on, modem PCIe on, Wi-Fi PCIe on, ACPM unavailable, ACPM busy, partial candidate");
static unsigned int debug_status, aur_status, ufs_clkstop;
module_param(debug_status, uint, 0444);
module_param(aur_status, uint, 0444);
module_param(ufs_clkstop, uint, 0444);
static unsigned int cp_isolation, wifi_isolation, wifi_ltssm;
module_param(cp_isolation, uint, 0444);
module_param(wifi_isolation, uint, 0444);
module_param(wifi_ltssm, uint, 0444);

static const u16 cpu_status[] = {
	0x1004, 0x1084, 0x1104, 0x1184, 0x1304, 0x1384, 0x1504, 0x1584,
};

static void audit(unsigned int cpu)
{
	unsigned int other;
	bool blocked = false;

	if (!READ_ONCE(noirq_window) || pm_suspend_target_state != PM_SUSPEND_TO_IDLE ||
	    cpu >= 4 || idle_cpus != 0xff || num_online_cpus() != 8)
		return;
	counts[0]++;
	for (other = 0; other < 8; other++) {
		if (other != cpu && (readl_relaxed(pmu + cpu_status[other]) & BIT(0))) {
			counts[1]++;
			return;
		}
	}
	if ((readl_relaxed(pmu + 0x1404) | readl_relaxed(pmu + 0x1604)) & BIT(0)) {
		counts[2]++;
		blocked = true;
	}
	/* HCI APB stays powered in the retained Hibern8 implementation. */
	ufs_clkstop = readl_relaxed(ufs + 0xb0);
	if ((ufs_clkstop & 0x17) != 0x17) {
		counts[3]++;
		blocked = true;
	}
	/* Stock external idle-IP: zero means busy. */
	debug_status = readl_relaxed(pmu + 0x3e0);
	if (!debug_status) {
		counts[4]++;
		blocked = true;
	}
	aur_status = readl_relaxed(pmu + 0x2984);
	if (aur_status & BIT(0)) {
		counts[5]++;
		blocked = true;
	}
	/* Both stock host nodes use-sicd=true: a powered link blocks SICD.
	 * CPIF isolates channel 0 after poweroff. Never read its ELBI here.
	 */
	cp_isolation = readl_relaxed(pmu + 0x3ec0);
	if (cp_isolation & BIT(0)) {
		counts[6]++;
		blocked = true;
	}
	wifi_isolation = readl_relaxed(pmu + 0x3ec4);
	wifi_ltssm = ~0U;
	/* Even a read of isolated ELBI hangs the SoC. Native channel 1 keeps
	 * isolation released after unloading but disables LTSSM and the PHY.
	 */
	if (wifi_isolation & BIT(0)) {
		wifi_ltssm = readl_relaxed(wifi_elbi + 0x2c8) & 0x3f;
		if (wifi_ltssm) {
			counts[7]++;
			blocked = true;
		}
	}
	if (!blocked) {
		if (!acpm_idle)
			counts[8]++;
		else if (!acpm_idle(acpm))
			counts[9]++;
		else
			counts[10]++;
	}
}

static int audit_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	unsigned int cpu = smp_processor_id();

	if (cpu >= 8)
		return NOTIFY_DONE;
	raw_spin_lock(&audit_lock);
	if (action == CPU_PM_ENTER) {
		idle_cpus |= BIT(cpu);
		audit(cpu);
	} else if (action == CPU_PM_EXIT || action == CPU_PM_ENTER_FAILED) {
		idle_cpus &= ~BIT(cpu);
	}
	raw_spin_unlock(&audit_lock);
	return NOTIFY_OK;
}

static struct notifier_block audit_nb = {
	.notifier_call = audit_notify,
	.priority = -10,
};

static int audit_suspend(struct device *dev)
{
	WRITE_ONCE(noirq_window, true);
	return 0;
}

static int audit_resume(struct device *dev)
{
	WRITE_ONCE(noirq_window, false);
	return 0;
}

static const struct dev_pm_ops audit_pm = {
	.suspend_noirq = audit_suspend,
	.resume_noirq = audit_resume,
};

static int audit_probe(struct platform_device *dev)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "google,gs201-acpm-ipc");
	if (!np)
		return -ENODEV;
	acpm = devm_acpm_get_by_node(&dev->dev, np);
	of_node_put(np);
	return PTR_ERR_OR_ZERO(acpm);
}

static struct platform_driver audit_driver = {
	.probe = audit_probe,
	.driver = { .name = "pixel-sicd-audit", .pm = &audit_pm },
};

static int __init audit_init(void)
{
	int ret;

	if (!of_machine_is_compatible("google,GS201 CHEETAH") ||
	    !IS_ENABLED(CONFIG_PM_SLEEP))
		return -ENODEV;
	pmu = ioremap(0x18060000, 0x4000);
	ufs = ioremap(0x14701100, 0x100);
	wifi_elbi = ioremap(0x14520000, 0x1000);
	if (!pmu || !ufs || !wifi_elbi) {
		ret = -ENOMEM;
		goto unmap;
	}
	ret = platform_driver_register(&audit_driver);
	if (ret)
		goto unmap;
	pdev = platform_device_register_simple("pixel-sicd-audit", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		goto unregister_driver;
	}
	if (!pdev->dev.driver) {
		ret = -ENODEV;
		goto unregister_device;
	}
	acpm_idle = symbol_get(exynos_acpm_is_idle);
	ret = cpu_pm_register_notifier(&audit_nb);
	if (!ret)
		return 0;
	if (acpm_idle)
		symbol_put(exynos_acpm_is_idle);
unregister_device:
	platform_device_unregister(pdev);
unregister_driver:
	platform_driver_unregister(&audit_driver);
unmap:
	if (pmu)
		iounmap(pmu);
	if (ufs)
		iounmap(ufs);
	if (wifi_elbi)
		iounmap(wifi_elbi);
	return ret;
}

static void __exit audit_exit(void)
{
	cpu_pm_unregister_notifier(&audit_nb);
	if (acpm_idle)
		symbol_put(exynos_acpm_is_idle);
	platform_device_unregister(pdev);
	platform_driver_unregister(&audit_driver);
	iounmap(ufs);
	iounmap(wifi_elbi);
	iounmap(pmu);
}
module_init(audit_init);
module_exit(audit_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only GS201 system idle prerequisite counters");
