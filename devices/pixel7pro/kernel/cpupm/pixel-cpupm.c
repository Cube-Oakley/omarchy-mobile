// SPDX-License-Identifier: GPL-2.0-only
/* GS201 CPU power-down idle (C2): the PMU bookkeeping the el3mon/ACPM firmware
 * needs around PSCI CPU_SUSPEND, as mainline exynos-pmu does for GS101.
 *
 * Without it the firmware rejects every C2 entry, and the idle loop retries
 * immediately: the "idle" CPUs spin on SMCs instead of sleeping. GS201 uses the
 * same registers as GS101 (Google's flexpmu_cal_cpu_gs201.h):
 * - PMU CPU_INFORM hint at 0x18060000 + 0x860 + 4 * cpu, written through the
 *   secure register SMC (pixel-reboot.c's path): C2 before, clear after;
 * - pmu_intr_gen at 0x18070000: the CPU's GRP2 wake-up interrupt enabled while
 *   it is down, and its GRP1 (and GRP1 bit cpu + 8) pending bits cleared.
 *
 * The core's arch timer loses its comparator in C2, and the stock DT idle
 * states lack "local-timer-stop" (stock ran the Exynos MCT as a broadcast
 * timer). Without it a CPU woken from C2 never takes a timer interrupt again:
 * on the first test, every timer queued on that CPU stayed pending. Before any
 * hint is sent, the C2 states are flagged CPUIDLE_FLAG_TIMER_STOP and each CPU
 * joins the tick broadcast (the kernel's hrtimer broadcast device, run by
 * whichever CPU stays awake). Until the hints are active the firmware rejects
 * C2, so the order is safe.
 *
 * Cluster power-down (CPD) follows the vendor's exynos-cpupm: the last CPU of
 * a cluster to go idle hints CPD (2) instead of C2, and the firmware powers
 * the cluster down with it. It may do so only when every online sibling is
 * idle with its next timer at least the stock target residency (10 ms) away,
 * and no sibling has hinted CPD without having powered down yet (PMU
 * CLUSTERx_CPUy_STATUS). The stock DT allows CPD for the mid and big clusters
 * only, so `cpd` defaults to clusters 1 and 2. Each CPU's next timer is
 * dev->next_hrtimer, which cpuidle sets before calling the state's enter
 * function; stock read it through an Android vendor hook, so here each C2
 * state's enter callback is wrapped to record it. Every CPU has its own psci
 * driver, and the wrap is done on that CPU, so it never races its own idle.
 *
 * CPU hotplug is not handled: nothing here takes CPUs offline. Disable the C2
 * states in sysfs before unloading: without the hints, C2 entries are
 * rejected again.
 */
#include <linux/arm-smccc.h>
#include <linux/cpu_pm.h>
#include <linux/cpuidle.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/reboot.h>
#include <linux/spinlock.h>
#include <linux/tick.h>

#define GS201_PMU		0x18060000
#define PMU_CPU_INFORM(cpu)	(0x860 + ((cpu) & 7) * 4)
#define CPU_INFORM_CLEAR	0
#define CPU_INFORM_C2		1
#define CPU_INFORM_CPD		2
#define PMU_CPU_STATUS(cpu)	(cpu_status_off[(cpu) & 7])
#define CPU_STATUS_ON		BIT(0)
#define GS201_SMC_PRIV_REG	0x82000504
#define PMUREG_WRITE		1

#define GS201_PMU_INTR_GEN	0x18070000
#define GRP1_INTR_BID_UPEND	0x108
#define GRP1_INTR_BID_CLEAR	0x10c
#define GRP2_INTR_BID_ENABLE	0x200
#define GRP2_INTR_BID_UPEND	0x208
#define GRP2_INTR_BID_CLEAR	0x20c

#define NR_GS201_CPUS		8
#define NR_CLUSTERS		3

typedef int (*cpuidle_enter_fn)(struct cpuidle_device *dev, struct cpuidle_driver *drv,
				int index);

/* flexpmu_cal_cpu_gs201.h: CLUSTERx_CPUy_STATUS, from the PMU base. */
static const u16 cpu_status_off[NR_GS201_CPUS] = {
	0x1004, 0x1084, 0x1104, 0x1184, 0x1304, 0x1384, 0x1504, 0x1584,
};
static const u8 cpu_cluster[NR_GS201_CPUS] = { 0, 0, 0, 0, 1, 1, 2, 2 };
static const u8 cluster_cpus[NR_CLUSTERS] = { 0x0f, 0x30, 0xc0 };

static unsigned int cpd = BIT(1) | BIT(2);
module_param(cpd, uint, 0644);
MODULE_PARM_DESC(cpd, "Clusters allowed to power down (bit per cluster; stock: 1 and 2)");
static unsigned int cpd_residency_us = 10000;
module_param(cpd_residency_us, uint, 0644);
MODULE_PARM_DESC(cpd_residency_us, "Minimum time to every sibling's next timer for CPD");
static unsigned int cpd_entries[NR_CLUSTERS];
module_param_array(cpd_entries, uint, NULL, 0444);
MODULE_PARM_DESC(cpd_entries, "CPD hints given, per cluster");

static void __iomem *intr_gen, *pmu;
static DEFINE_RAW_SPINLOCK(cpupm_lock);
static bool rebooting;
static bool cpu_idle[NR_GS201_CPUS], cpu_s2idle[NR_GS201_CPUS];
static unsigned int s2idle_entries[NR_GS201_CPUS];
module_param_array(s2idle_entries, uint, NULL, 0444);
static unsigned int s2idle_last[NR_GS201_CPUS];
module_param_array(s2idle_last, uint, NULL, 0444);
static u32 cpu_hint[NR_GS201_CPUS];
static ktime_t next_wake[NR_GS201_CPUS];
static cpuidle_enter_fn psci_enter[NR_GS201_CPUS][CPUIDLE_STATE_MAX];
static cpuidle_enter_fn psci_s2idle[NR_GS201_CPUS][CPUIDLE_STATE_MAX];

static void cpu_inform(unsigned int cpu, u32 hint)
{
	struct arm_smccc_res res;

	arm_smccc_smc(GS201_SMC_PRIV_REG, GS201_PMU + PMU_CPU_INFORM(cpu), PMUREG_WRITE,
		      hint, 0, 0, 0, 0, &res);
}

static void clear_pending(u32 upend, u32 clear, u32 mask)
{
	writel(readl(intr_gen + upend) & mask, intr_gen + clear);
}

/* __gs101_cpu_pmu_offline() */
static void cpu_down_prepare(unsigned int cpu, u32 hint)
{
	cpu_inform(cpu, hint);
	writel(readl(intr_gen + GRP2_INTR_BID_ENABLE) | BIT(cpu),
	       intr_gen + GRP2_INTR_BID_ENABLE);
	clear_pending(GRP1_INTR_BID_UPEND, GRP1_INTR_BID_CLEAR, BIT(cpu));
	clear_pending(GRP1_INTR_BID_UPEND, GRP1_INTR_BID_CLEAR, BIT(cpu + 8));
}

/* __gs101_cpu_pmu_online() */
static void cpu_up_finish(unsigned int cpu)
{
	cpu_inform(cpu, CPU_INFORM_CLEAR);
	writel(readl(intr_gen + GRP2_INTR_BID_ENABLE) & ~BIT(cpu),
	       intr_gen + GRP2_INTR_BID_ENABLE);
	clear_pending(GRP2_INTR_BID_UPEND, GRP2_INTR_BID_CLEAR, BIT(cpu));
}

/* The vendor's entry_allow() for the cluster mode, with this CPU marked idle:
 * cpus_busy() and cpus_last_core_detecting().
 */
static bool cluster_may_power_down(unsigned int cpu)
{
	unsigned int cl = cpu_cluster[cpu], sib;
	ktime_t min_wake;

	if (!(READ_ONCE(cpd) & BIT(cl)))
		return false;
	min_wake = ktime_add_us(ktime_get_mono_fast_ns(), READ_ONCE(cpd_residency_us));
	for_each_online_cpu(sib) {
		if (sib >= NR_GS201_CPUS || !(cluster_cpus[cl] & BIT(sib)))
			continue;
		if (!cpu_idle[sib] || ktime_before(READ_ONCE(next_wake[sib]), min_wake))
			return false;
		if (sib != cpu && cpu_hint[sib] == CPU_INFORM_CPD &&
		    (readl_relaxed(pmu + PMU_CPU_STATUS(sib)) & CPU_STATUS_ON))
			return false;
	}
	return true;
}

#include "pixel-sicd.h"

/* Runs on the idling CPU with interrupts disabled. */
static int pixel_cpupm_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	unsigned int cpu = smp_processor_id();
	u32 hint;
	unsigned int other;
	bool last;

	raw_spin_lock(&cpupm_lock);
	switch (action) {
	case CPU_PM_ENTER:
		if (rebooting) {
			raw_spin_unlock(&cpupm_lock);
			return NOTIFY_BAD;
		}
		cpu_idle[cpu] = true;
		last = true;
		for (other = 0; other < NR_GS201_CPUS; other++)
			if (!cpu_idle[other] || !READ_ONCE(cpu_s2idle[other]))
				last = false;
		if (last) {
			s2idle_last[cpu]++;
			if (READ_ONCE(sicd_window) && sicd_first_last_cpu == ~0U)
				sicd_first_last_cpu = cpu;
		}
		if (sicd_defer_cpu(cpu)) {
			cpu_idle[cpu] = false;
			raw_spin_unlock(&cpupm_lock);
			return NOTIFY_BAD;
		}
		hint = cluster_may_power_down(cpu) ? CPU_INFORM_CPD : CPU_INFORM_C2;
		if (hint == CPU_INFORM_CPD)
			cpd_entries[cpu_cluster[cpu]]++;
		cpu_hint[cpu] = hint;
		cpu_down_prepare(cpu, hint);
		sicd_try_enter(cpu);
		break;
	case CPU_PM_EXIT:
	case CPU_PM_ENTER_FAILED:
		sicd_finish(action == CPU_PM_ENTER_FAILED);
		cpu_idle[cpu] = false;
		cpu_hint[cpu] = CPU_INFORM_CLEAR;
		cpu_up_finish(cpu);
		break;
	}
	raw_spin_unlock(&cpupm_lock);
	return NOTIFY_OK;
}

static struct notifier_block pixel_cpupm_nb = {
	.notifier_call = pixel_cpupm_notify,
};

/* As mainline: no more C2 entries once a restart has begun. */
static int pixel_cpupm_reboot(struct notifier_block *nb, unsigned long action, void *data)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&cpupm_lock, flags);
	rebooting = true;
	raw_spin_unlock_irqrestore(&cpupm_lock, flags);
	return NOTIFY_DONE;
}

static struct notifier_block pixel_cpupm_reboot_nb = {
	.notifier_call = pixel_cpupm_reboot,
};

/* The frozen-tick s2idle path has its own callback. Ordinary next_hrtimer
 * values are stale there: timekeeping and all local timers have stopped.
 * Record an unbounded timer deadline for that interval, just as the generic
 * s2idle core selects its deepest state without normal governor prediction.
 */
static int pixel_cpupm_call(struct cpuidle_device *dev, struct cpuidle_driver *drv,
			    int index, bool frozen)
{
	unsigned int seq = READ_ONCE(sicd_cpu_seq[dev->cpu]);
	unsigned long flags;
	int ret;

	WRITE_ONCE(next_wake[dev->cpu], frozen ? KTIME_MAX : dev->next_hrtimer);
	WRITE_ONCE(cpu_s2idle[dev->cpu], frozen);
	if (frozen) {
		s2idle_entries[dev->cpu]++;
		ret = psci_s2idle[dev->cpu][index](dev, drv, index);
	} else {
		ret = psci_enter[dev->cpu][index](dev, drv, index);
	}
	if (frozen && !dev->cpu && ret < 0 && sicd_retry_delay) {
		sicd_retry_delay = false;
		/* Outside both CPU_PM notifier locks; allow sibling progress.
		 * At most 64 refusals x 50 us per bounded diagnostic sleep.
		 */
		udelay(50);
	}
	WRITE_ONCE(cpu_s2idle[dev->cpu], false);
	if (READ_ONCE(sicd_cpu_seq[dev->cpu]) != seq) {
		raw_spin_lock_irqsave(&cpupm_lock, flags);
		if (ret < 0)
			sicd_idle_errors++;
		else
			sicd_idle_returns++;
		raw_spin_unlock_irqrestore(&cpupm_lock, flags);
	}
	return ret;
}

static int pixel_cpupm_enter(struct cpuidle_device *dev, struct cpuidle_driver *drv, int index)
{
	return pixel_cpupm_call(dev, drv, index, false);
}

static int pixel_cpupm_s2idle(struct cpuidle_device *dev, struct cpuidle_driver *drv, int index)
{
	return pixel_cpupm_call(dev, drv, index, true);
}

/* Runs on each CPU: its C2 states lose the local timer, it joins the
 * broadcast, and their enter callbacks are wrapped. States past WFI are the
 * PSCI power-down ones (the stock DT has one per cluster).
 */
static void timer_stop_setup(void *failed)
{
	struct cpuidle_driver *drv = cpuidle_get_driver();
	unsigned int cpu = smp_processor_id();
	int i;

	if (!drv || drv->state_count < 2 || cpu >= NR_GS201_CPUS) {
		*(bool *)failed = true;
		return;
	}
	for (i = 1; i < drv->state_count; i++) {
		drv->states[i].flags |= CPUIDLE_FLAG_TIMER_STOP;
		psci_enter[cpu][i] = drv->states[i].enter;
		drv->states[i].enter = pixel_cpupm_enter;
		psci_s2idle[cpu][i] = drv->states[i].enter_s2idle;
		if (psci_s2idle[cpu][i])
			drv->states[i].enter_s2idle = pixel_cpupm_s2idle;
	}
	tick_broadcast_enable();
}

static void enter_restore(void *unused)
{
	struct cpuidle_driver *drv = cpuidle_get_driver();
	unsigned int cpu = smp_processor_id();
	int i;

	for (i = 1; drv && i < drv->state_count; i++) {
		if (psci_enter[cpu][i])
			drv->states[i].enter = psci_enter[cpu][i];
		if (psci_s2idle[cpu][i])
			drv->states[i].enter_s2idle = psci_s2idle[cpu][i];
	}
}

static int __init pixel_cpupm_init(void)
{
	bool failed = false;
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	intr_gen = ioremap(GS201_PMU_INTR_GEN, 0x2000);
	pmu = ioremap(GS201_PMU, 0x4000);
	if (!intr_gen || !pmu) {
		ret = -ENOMEM;
		goto unmap;
	}
	ret = sicd_init();
	if (ret)
		goto unmap;
	on_each_cpu(timer_stop_setup, &failed, 1);
	if (failed) {
		pr_err("pixel-cpupm: a CPU has no PSCI idle states\n");
		ret = -ENODEV;
		goto restore;
	}
	ret = register_reboot_notifier(&pixel_cpupm_reboot_nb);
	if (!ret)
		ret = cpu_pm_register_notifier(&pixel_cpupm_nb);
	if (ret) {
		unregister_reboot_notifier(&pixel_cpupm_reboot_nb);
		goto restore;
	}
	pr_info("pixel-cpupm: C2 hints active, CPD clusters %#x (GRP2 enable %#x)\n",
		cpd, readl(intr_gen + GRP2_INTR_BID_ENABLE));
	return 0;
restore:
	on_each_cpu(enter_restore, NULL, 1);
	sicd_exit();
unmap:
	if (pmu)
		iounmap(pmu);
	if (intr_gen)
		iounmap(intr_gen);
	return ret;
}
module_init(pixel_cpupm_init);

/* The timer-stop flags and broadcast stay: they are right for C2 either way.
 * Each CPU restores its own enter callbacks, so none is inside the wrapper.
 */
static void __exit pixel_cpupm_exit(void)
{
	cpu_pm_unregister_notifier(&pixel_cpupm_nb);
	unregister_reboot_notifier(&pixel_cpupm_reboot_nb);
	on_each_cpu(enter_restore, NULL, 1);
	sicd_exit();
	iounmap(pmu);
	iounmap(intr_gen);
}
module_exit(pixel_cpupm_exit);

MODULE_DESCRIPTION("GS201 CPU power-down idle hints");
MODULE_LICENSE("GPL");
