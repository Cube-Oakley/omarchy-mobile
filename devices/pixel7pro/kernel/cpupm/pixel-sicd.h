/* SPDX-License-Identifier: GPL-2.0-only */
/* Experimental suspend-only SICD, included under pixel-cpupm's raw lock.
 * Google's exynos-cpupm + flexpmu_cal_system_gs201 sequences. Default off;
 * needs an explicit load-time permit and a consumable attempt budget.
 */
#include <clocksource/arm_arch_timer.h>
#include <linux/delay.h>
#include <asm/arch_timer.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/platform_device.h>
#include <linux/suspend.h>

static bool permit_sicd;
module_param(permit_sicd, bool, 0400);
MODULE_PARM_DESC(permit_sicd, "Experimental: permit bounded suspend-only SICD trials");
static unsigned int sicd_budget;
module_param(sicd_budget, uint, 0644);
MODULE_PARM_DESC(sicd_budget, "Remaining SICD attempts; default zero, consumed before programming");
static unsigned int sicd_entries, sicd_exits, sicd_cancelled, sicd_errors;
static unsigned int sicd_timer_busy, sicd_hardware_busy, sicd_acpm_busy;
static unsigned int sicd_idle_returns, sicd_idle_errors;
static unsigned int sicd_last_cpu_state;
static bool sicd_defer, sicd_retry_delay;
module_param(sicd_defer, bool, 0400);
MODULE_PARM_DESC(sicd_defer, "Diagnostic: let CPU0 retry up to 64 frozen-idle entries until siblings sleep");
static unsigned int sicd_deferrals, sicd_defer_left, sicd_first_last_cpu = ~0U;
module_param(sicd_deferrals, uint, 0444);
module_param(sicd_first_last_cpu, uint, 0444);
static unsigned int sicd_blocked[8], sicd_last_pd;
module_param_array(sicd_blocked, uint, NULL, 0444);
MODULE_PARM_DESC(sicd_blocked, "First hardware blocker: software CPU, powered CPU, cluster, domain, debug, UFS, modem PCIe, Wi-Fi PCIe");
module_param(sicd_last_pd, uint, 0444);
static unsigned int sicd_cpu_seq[8];
static u64 sicd_started_cycles;
static u32 sicd_counter_rate;
static unsigned long long sicd_elapsed_ns, sicd_longest_ns;
module_param(sicd_elapsed_ns, ullong, 0444);
module_param(sicd_longest_ns, ullong, 0444);
MODULE_PARM_DESC(sicd_elapsed_ns, "Elapsed time between uncancelled SICD hint and first CPU wake; not firmware residency");
module_param(sicd_entries, uint, 0444);
module_param(sicd_exits, uint, 0444);
module_param(sicd_cancelled, uint, 0444);
module_param(sicd_errors, uint, 0444);
module_param(sicd_timer_busy, uint, 0444);
module_param(sicd_hardware_busy, uint, 0444);
module_param(sicd_acpm_busy, uint, 0444);
module_param(sicd_idle_returns, uint, 0444);
module_param(sicd_idle_errors, uint, 0444);
module_param(sicd_last_cpu_state, uint, 0444);

static struct platform_device *sicd_pdev;
static struct acpm_handle *sicd_acpm;
static bool (*sicd_acpm_idle)(struct acpm_handle *handle);
static void __iomem *sicd_ufs, *sicd_wifi, *sicd_nscode;
static bool sicd_window, sicd_active;
static u32 sicd_saved_masks[3];

static int sicd_pmu_write(u32 offset, u32 value)
{
	struct arm_smccc_res res;

	arm_smccc_smc(GS201_SMC_PRIV_REG, GS201_PMU + offset, PMUREG_WRITE,
		      value, 0, 0, 0, 0, &res);
	return res.a0 ? -EIO : 0;
}

static bool sicd_hardware_idle(unsigned int cpu)
{
	/* Domains with stock idle-IP checks; these are boot-disabled here. */
	static const u16 pd_status[] = {
		0x2904, 0x2984, 0x2884, 0x2384, 0x2304, 0x1c04,
		0x2504, 0x2684, 0x2604, 0x2484, 0x2404, 0x2584,
		0x2704, 0x2784, 0x2804,
	};
	unsigned int other, i;

	for (other = 0; other < 8; other++) {
		if (!cpu_idle[other] || !READ_ONCE(cpu_s2idle[other])) {
			sicd_blocked[0]++;
			return false;
		}
	}
	for (other = 0; other < 8; other++) {
		if (other != cpu && (readl_relaxed(pmu + PMU_CPU_STATUS(other)) & BIT(0))) {
			sicd_blocked[1]++;
			return false;
		}
	}
	if ((readl_relaxed(pmu + 0x1404) | readl_relaxed(pmu + 0x1604)) & BIT(0)) {
		sicd_blocked[2]++;
		return false;
	}
	for (i = 0; i < ARRAY_SIZE(pd_status); i++) {
		if (readl_relaxed(pmu + pd_status[i]) & BIT(0)) {
			sicd_blocked[3]++;
			sicd_last_pd = pd_status[i];
			return false;
		}
	}
	if (!readl_relaxed(pmu + 0x3e0)) {
		sicd_blocked[4]++;
		return false;
	}
	if ((readl_relaxed(sicd_ufs + 0xb0) & 0x17) != 0x17) {
		sicd_blocked[5]++;
		return false;
	}
	if (readl_relaxed(pmu + 0x3ec0) & BIT(0)) {
		sicd_blocked[6]++;
		return false;
	}
	/* Never access isolated Wi-Fi ELBI. A released but stopped host is
	 * accepted only for the manually unloaded, PHY-down experiment.
	 */
	if ((readl_relaxed(pmu + 0x3ec4) & BIT(0)) &&
	    (readl_relaxed(sicd_wifi + 0x2c8) & 0x3f)) {
		sicd_blocked[7]++;
		return false;
	}
	return true;
}

static void sicd_restore_masks(void)
{
	unsigned int i;

	for (i = 0; i < 3; i++)
		if (sicd_pmu_write(0x3a80 + 4 * i, sicd_saved_masks[i])) {
			sicd_errors++;
			WRITE_ONCE(sicd_budget, 0);
		}
}

static void sicd_finish(bool cancel)
{
	u64 elapsed;
	int error;

	if (!sicd_active)
		return;
	/* Stock 5.10 exynos-cpupm reads CANCEL_FLAG from this reserved
	 * non-secure firmware page. CPU_PM_EXIT alone does not distinguish
	 * a rejected PSCI request from an actual wakeup.
	 */
	sicd_last_cpu_state = readl_relaxed(sicd_nscode + 0x2c + 4 * smp_processor_id());
	cancel |= !!(sicd_last_cpu_state & BIT(5));
	elapsed = mul_u64_u32_div(arch_timer_read_counter() - sicd_started_cycles,
				 NSEC_PER_SEC, sicd_counter_rate);
	if (!cancel) {
		sicd_elapsed_ns += elapsed;
		sicd_longest_ns = max_t(u64, sicd_longest_ns, elapsed);
	}
	/* First waking CPU performs Google's exit_sicd / early_sicd. */
	error = sicd_pmu_write(0x3944, 0);
	error |= sicd_pmu_write(0x3964, 0);
	clear_pending(0x1b08, 0x1b0c, ~0U);
	clear_pending(0x1f08, 0x1f0c, ~0U);
	if (cancel) {
		error |= sicd_pmu_write(0x3a10, readl(pmu + 0x3a10) & ~BIT(14));
		clear_pending(0x408, 0x40c, BIT(0));
		sicd_cancelled++;
	}
	sicd_restore_masks();
	if (error) {
		sicd_errors++;
		WRITE_ONCE(sicd_budget, 0);
	}
	sicd_active = false;
	sicd_exits++;
}

static void sicd_try_enter(unsigned int cpu)
{
	ktime_t deadline;
	unsigned int other, i;
	u32 budget = READ_ONCE(sicd_budget);
	int error;

	if (!permit_sicd || !budget || sicd_active || !READ_ONCE(sicd_window) ||
	    pm_suspend_target_state != PM_SUSPEND_TO_IDLE || cpu >= 4 ||
	    num_online_cpus() != 8 || !sicd_acpm_idle || IS_ERR_OR_NULL(sicd_acpm))
		return;
	if (!sicd_hardware_idle(cpu)) {
		sicd_hardware_busy++;
		return;
	}
	deadline = ktime_add_us(ktime_get_mono_fast_ns(), 10000);
	for (other = 0; other < 8; other++) {
		if (ktime_before(READ_ONCE(next_wake[other]), deadline)) {
			sicd_timer_busy++;
			return;
		}
	}
	if (!sicd_acpm_idle(sicd_acpm)) {
		sicd_acpm_busy++;
		return;
	}
	/* Do not take over wake masks belonging to another PM implementation. */
	if (readl(pmu + 0x3944) || readl(pmu + 0x3964)) {
		sicd_hardware_busy++;
		return;
	}
	WRITE_ONCE(sicd_budget, budget - 1);
	for (i = 0; i < 3; i++)
		sicd_saved_masks[i] = readl(pmu + 0x3a80 + 4 * i);
	/* Stock SICD uses CPU nIRQOUT[0..7], preserving GIC wake delivery.
	 * EINT wake additionally permits power key 43 and modem 60/61.
	 */
	error = sicd_pmu_write(PMU_CPU_INFORM(cpu), 3);
	error |= sicd_pmu_write(0x3950, 0);
	error |= sicd_pmu_write(0x3970, 0);
	error |= sicd_pmu_write(0x3944, 0x0ff00000);
	error |= sicd_pmu_write(0x3964, 0);
	error |= sicd_pmu_write(0x3a80, ~0U);
	error |= sicd_pmu_write(0x3a84, ~(u32)(BIT(11) | BIT(28) | BIT(29)));
	error |= sicd_pmu_write(0x3a88, ~0U);
	if (error) {
		sicd_errors++;
		WRITE_ONCE(sicd_budget, 0);
		sicd_pmu_write(0x3944, 0);
		sicd_pmu_write(0x3964, 0);
		sicd_restore_masks();
		cpu_inform(cpu, cpu_hint[cpu]);
		return;
	}
	sicd_started_cycles = arch_timer_read_counter();
	sicd_active = true;
	sicd_entries++;
	sicd_cpu_seq[cpu]++;
}

/* CPU_PM_ENTER_FAILED is a normal cpuidle refusal. Retry CPU0 outside the
 * notifier lock, bounded per system sleep, so a big CPU does not always go
 * down last and leave no eligible little CPU to request SICD. No C2 hint or
 * GRP2 bookkeeping has been applied to this CPU at this point.
 */
static bool sicd_defer_cpu(unsigned int cpu)
{
	unsigned int other;

	if (!permit_sicd || !sicd_defer || !sicd_defer_left || cpu ||
	    !READ_ONCE(sicd_budget) || !READ_ONCE(sicd_window) || !cpu_s2idle[cpu])
		return false;
	for (other = 1; other < 8; other++) {
		if (!cpu_idle[other] || !READ_ONCE(cpu_s2idle[other]) ||
		    (readl_relaxed(pmu + PMU_CPU_STATUS(other)) & BIT(0))) {
			sicd_defer_left--;
			sicd_deferrals++;
			sicd_retry_delay = true;
			return true;
		}
	}
	return false;
}

static int sicd_suspend_noirq(struct device *dev)
{
	sicd_first_last_cpu = ~0U;
	sicd_defer_left = 64;
	WRITE_ONCE(sicd_window, true);
	return 0;
}

static int sicd_resume_noirq(struct device *dev)
{
	WRITE_ONCE(sicd_window, false);
	return 0;
}

static const struct dev_pm_ops sicd_pm = {
	.suspend_noirq = sicd_suspend_noirq,
	.resume_noirq = sicd_resume_noirq,
};

static int sicd_probe(struct platform_device *pdev)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "google,gs201-acpm-ipc");
	if (!np)
		return -ENODEV;
	sicd_acpm = devm_acpm_get_by_node(&pdev->dev, np);
	of_node_put(np);
	return PTR_ERR_OR_ZERO(sicd_acpm);
}

static struct platform_driver sicd_driver = {
	.probe = sicd_probe,
	.driver = { .name = "pixel-sicd", .pm = &sicd_pm },
};

static int sicd_init(void)
{
	int ret;

	if (!permit_sicd)
		return 0;
	if (!of_machine_is_compatible("google,GS201 CHEETAH"))
		return -ENODEV;
	sicd_counter_rate = arch_timer_get_cntfrq();
	if (!sicd_counter_rate)
		return -ENODEV;
	sicd_ufs = ioremap(0x14701100, 0x100);
	sicd_wifi = ioremap(0x14520000, 0x1000);
	sicd_nscode = ioremap(0xbffff000, 0x1000);
	if (!sicd_ufs || !sicd_wifi || !sicd_nscode) {
		ret = -ENOMEM;
		goto unmap;
	}
	ret = platform_driver_register(&sicd_driver);
	if (ret)
		goto unmap;
	sicd_pdev = platform_device_register_simple("pixel-sicd", -1, NULL, 0);
	if (IS_ERR(sicd_pdev)) {
		ret = PTR_ERR(sicd_pdev);
		platform_driver_unregister(&sicd_driver);
		goto unmap;
	}
	/* An older kernel retains C2/CPD support; SICD remains unavailable. */
	sicd_acpm_idle = symbol_get(exynos_acpm_is_idle);
	return 0;
unmap:
	if (sicd_nscode)
		iounmap(sicd_nscode);
	if (sicd_wifi)
		iounmap(sicd_wifi);
	if (sicd_ufs)
		iounmap(sicd_ufs);
	return ret;
}

static void sicd_exit(void)
{
	if (!sicd_pdev)
		return;
	if (sicd_acpm_idle)
		symbol_put(exynos_acpm_is_idle);
	platform_device_unregister(sicd_pdev);
	platform_driver_unregister(&sicd_driver);
	iounmap(sicd_wifi);
	iounmap(sicd_ufs);
	iounmap(sicd_nscode);
}
