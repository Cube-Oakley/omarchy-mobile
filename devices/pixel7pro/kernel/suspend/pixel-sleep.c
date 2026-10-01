// SPDX-License-Identifier: GPL-2.0-only
/* GS201 SYS_SLEEP kernel side (Stage 3 of docs/deep-sleep-plan-20260930.md):
 * the equivalent of Google's exynos-pm syscore ops plus cal_pm_enter/exit/
 * earlywakeup for mode 8 (SYS_SLEEP), table-driven from the stock lists
 * generated into pixel-sleep-lists.h.
 *
 * Stock order (exynos-pm.c:233-314, pmucal_system.c:27-83), mirrored here:
 *   EINT wake masks; WAKEUP_STAT = 0, WAKEUP_INT_EN; WAKEUP2 likewise;
 *   CPU_INFORM[cpu0] = 4 (pmucal_powermode_hint); read save_sleep (545);
 *   write enter_sleep (3); record FLEXPMU counters.
 * Added: pixel_cpupm_system_sleep(true) before the hint so pixel-cpupm stops
 * writing CPU_INFORM; a read-back of every PMU and INTR_GEN register written
 * (a mismatch undoes everything and fails the suspend with -EIO); and a
 * last-in-chain CPU_PM notifier that re-reads CPU_INFORM[0] in
 * cpu_pm_suspend(), right before PSCI SYSTEM_SUSPEND, and fails it if
 * anything replaced the 4.
 *
 * Resume (exynos-pm.c:316-358, pmucal_system.c:93-220): early wakeup when
 * the SLEEP AP-down counter did not move, else exit; CPU_INFORM[0] = 0
 * (stock clears it from CPU_PM_EXIT, pmucal_cpu.c:32, before exynos-pm
 * resumes); exit_sleep (37) or early_sleep (9); restore the save list;
 * INT_EN and EINT masks back to their pre-suspend values; wake reason.
 *
 * Nothing is armed unless the target is deep, `budget` is nonzero (one unit
 * per attempt) and every precondition holds. dry_run (default) performs the
 * PMU/INTR_GEN writes and replaces every other write (CMU restore, DRCG,
 * DMC, UFS, TREX) with a logged comparison; it requires pm_test=core, as
 * does any run without real_sleep=1.
 */
#include <linux/arm-smccc.h>
#include <linux/cpu_pm.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <linux/workqueue.h>

#include "pixel-sleep-common.h"
#include "pixel-sleep-gpio.h"
#include "pixel-sleep-usi.h"

/* pixel-cpupm.c, EXPORT_SYMBOL_GPL: while on, pixel-cpupm writes no
 * CPU_INFORM (stock exynos-cpupm ignores CPU_PM_ENTER once suspended,
 * exynos-cpupm.c:974-976). Build with KBUILD_EXTRA_SYMBOLS pointing at
 * pixel-cpupm's Module.symvers; load pixel-cpupm first.
 */
extern void pixel_cpupm_system_sleep(bool on);

#define PS_S2MPUS	4	/* the s2mpu[] table below */

static unsigned int budget;
module_param(budget, uint, 0644);
MODULE_PARM_DESC(budget, "Deep-suspend attempts allowed; one consumed per attempt (default 0: never armed)");
static bool dry_run = true;
module_param(dry_run, bool, 0644);
MODULE_PARM_DESC(dry_run, "PMU/INTR_GEN writes only; log CMU/sysreg/DMC writes instead (default 1; needs pm_test=core)");
static bool real_sleep;
module_param(real_sleep, bool, 0644);
MODULE_PARM_DESC(real_sleep, "Permit firmware entry (pm_test other than core); needs dry_run=0 and the real-sleep checks");
static unsigned int eint_wake[16] = { 43, 44, 62, 60 };
static unsigned int eint_wake_n = 4;
module_param_array(eint_wake, uint, &eint_wake_n, 0644);
MODULE_PARM_DESC(eint_wake, "Stock EINT wake bits (default power 43, vol-down 44, vol-up 62, CP2AP 60)");
static bool gate_cpucl = true;
module_param(gate_cpucl, bool, 0644);
MODULE_PARM_DESC(gate_cpucl, "Touch CMU_CPUCL1/2 only while that cluster is on (non-stock guard; default 1)");
static bool lpm_init_pmu;
module_param(lpm_init_pmu, bool, 0444);
MODULE_PARM_DESC(lpm_init_pmu, "At load, write the stock lpm_init PMU durations that differ (default 0: report only)");
static bool pd_restore = true;
module_param(pd_restore, bool, 0644);
MODULE_PARM_DESC(pd_restore, "After an exit, write back the domain save-list values that changed (default 1; lists with PLL waits are only compared)");
static bool pd_restore_noc;
module_param(pd_restore_noc, bool, 0644);
MODULE_PARM_DESC(pd_restore_noc, "Also write back the interconnect (NOCL*) domain lists (default 0: compare only)");
static unsigned int hsi2_cycle;
module_param(hsi2_cycle, uint, 0644);
MODULE_PARM_DESC(hsi2_cycle, "Test: with pm_test=core and dry_run=0, power HSI2 off and on at arm as stock's domain driver does: 1 with the DTZPC save/restore calls, 2 without (default 0)");
static bool disp_off;
module_param(disp_off, bool, 0644);
MODULE_PARM_DESC(disp_off, "Test: power DPU and DISP off at arm and on at resume, as stock's genpd does before SYS_SLEEP (the display needs a reboot afterwards)");
static long hsi2_smc_ret;
module_param(hsi2_smc_ret, long, 0444);
static bool verbose;
module_param(verbose, bool, 0644);
MODULE_PARM_DESC(verbose, "Print every logged write after resume");

static unsigned int attempts, armed_count, refused, aborted, early_count, exit_count;
module_param(attempts, uint, 0444);
module_param(armed_count, uint, 0444);
module_param(refused, uint, 0444);
module_param(aborted, uint, 0444);
module_param(early_count, uint, 0444);
module_param(exit_count, uint, 0444);

#define GS201_SMC_PRIV_REG	0x82000504	/* set_priv_reg: exynos-pmu-if.c:56-59 */
#define PMUREG_WRITE		1		/* pixel-sicd.h, pixel-reboot */

/* PMU_ALIVE offsets. */
#define PMU_CPU_INFORM0		0x0860	/* flexpmu_cal_cpu_gs201.h:7 */
#define CPU_INFORM_SLEEP	4	/* flexpmu_cal_define_gs201.h:10, cal_data.c:47 */
#define PMU_CPU0_INT_EN		0x1044	/* flexpmu_cal_system_gs201.h:143 */
#define PMU_WAKEUP_INT_EN	0x3944	/* gs201.dtsi:403 */
#define PMU_WAKEUP_STAT		0x3950	/* gs201.dtsi:402 */
#define PMU_WAKEUP2_INT_EN	0x3964	/* gs201.dtsi:403 */
#define PMU_WAKEUP2_STAT	0x3970	/* gs201.dtsi:402 */
#define PMU_EINT_MASK(i)	(0x3a80 + 4 * (i))	/* gs201.dtsi:406 */
#define PMU_PCIE_PHY_HSI1	0x3ec0	/* gs201-pcie.dtsi:54: bit 0 = 0 isolated */
#define PMU_PCIE_PHY_HSI2	0x3ec4	/* gs201-pcie.dtsi:114 */
#define PMU_G3D_STATUS		0x1e04	/* flexpmu_cal_local_gs201.h:515 */
#define STOCK_INT_EN		0x1001f0bf	/* gs201.dtsi:404 */
/* Bits that read back as written, measured on this phone (2026-09-30):
 * EINT_WAKEUP_MASK3 implements three bits (it reads 0x7 after 0xffffffff),
 * and WAKEUP_INT_EN bit 7 (MAILBOX_AOC2AP) does not stick through the
 * monitor's write. Stock writes the same values and never reads them back;
 * the AoC still wakes the AP through WAKEUP2_INT_EN (0x1f0).
 */
static const u32 eint_mask_impl[3] = { 0xffffffff, 0xffffffff, 0x00000007 };
#define INT_EN_IMPL		(~BIT(7))
#define STOCK_INT2_EN		0x000001f0	/* gs201.dtsi:404 */

/* PMU_INTR_GEN. */
#define INTR_GRP2_ENABLE	0x0200	/* flexpmu_cal_system_gs201.h:141 */

/* CLUSTERx_CPUy_STATUS, flexpmu_cal_cpu_gs201.h:31-136. */
static const u16 cpu_status[8] = {
	0x1004, 0x1084, 0x1104, 0x1184, 0x1304, 0x1384, 0x1504, 0x1584,
};

/* Wake-reason EINT_PEND: exynos-pm.c:20 (base + 0xa00 + bank * 4), bases
 * gs201.dtsi:382-383, num-eint 64 / num-eint-far 32 (gs201.dtsi:392-393).
 */
#define GPIO_ALIVE_PA		0x180d0000
#define GPIO_FAR_ALIVE_PA	0x180e0000
#define EINT_PEND		0x0a00
static void __iomem *gpio_alive, *gpio_far;

/* Never a wake source (plan §4; the live DT): gpa0-6 PMIC IRQ, gpa5-0
 * SMPL_WARN, gpa9-1 NFC IRQ (st21nfc irq-gpios), gpa6-3 and gpa8-6 speaker
 * amplifiers, gpa7-0 touch. Bit = eint_num + pin + wake_mask_bit_offset
 * (pinctrl-exynos.c:428; banks pinctrl-gs201.c:73-90).
 */
static const u8 eint_deny[] = { 6, 28, 36, 48, 53, 63 };

static const char * const ws_name[32] = {	/* gs201.dtsi:414-447 */
	"RTC_ALARM", "RTC_TICK", "TRTC_ALARM", "TRTC_TICK", "EINT", "EINT_FAR",
	"MAILBOX_APM2AP", "MAILBOX_AOC2AP", "L1SUB_PCIE_GEN4A_0", "L1SUB_PCIE_GEN4B_0",
	"L1SUB_PCIE_GEN4A_1", "L1SUB_PCIE_GEN4B_1", "EXT_PCIE_GEN4A_0", "EXT_PCIE_GEN4B_0",
	"EXT_PCIE_GEN4A_1", "EXT_PCIE_GEN4B_1", "USB_REWA", "USBDP", "MMC_CARD", "TIMER",
	"CLUSTER0_CPU0_nIRQOUT", "CLUSTER0_CPU1_nIRQOUT", "CLUSTER0_CPU2_nIRQOUT",
	"CLUSTER0_CPU3_nIRQOUT", "CLUSTER1_CPU0_nIRQOUT", "CLUSTER1_CPU1_nIRQOUT",
	"CLUSTER2_CPU0_nIRQOUT", "CLUSTER2_CPU1_nIRQOUT", "INTREQ_PCIE_GEN4A_0",
	"INTREQ_PCIE_GEN4B_0", "INTREQ_PCIE_GEN4A_1", "INTREQ_PCIE_GEN4B_1",
};

static const char * const ws2_name[14] = {	/* gs201.dtsi:450-464 */
	"RESERVED", "RESERVED", "USB20_PHY_FS_VMINUS_WAKEUP", "USB20_PHY_FS_VPLUS_WAKEUP",
	"MAILBOX_APM2AP", "MAILBOX_AOCA322AP", "MAILBOX_AOCF12AP", "MAILBOX_AOCP62AP",
	"MAILBOX_DBGCORE2AP", "MAILBOX_AUR02AP", "MAILBOX_AUR12AP", "MAILBOX_AUR22AP",
	"MAILBOX_AUR32AP", "VGPIO2PMU_EINT",
};

/* Write log: every write this module performs or replaces. */
enum { L_ARM, L_ENTER, L_EXIT, L_EARLY, L_RESTORE, L_UNDO, L_RESUME, L_LPM, L_PD };
static const char * const list_name[] = {
	"arm", "enter", "exit", "early", "restore", "undo", "resume", "lpm_init", "pd",
};

enum { A_DONE, A_DRY, A_GATED, A_COND, A_ERR };
static const char * const act_name[] = { "done", "dry", "gated", "cond-false", "SMC-ERR" };

struct wlog {
	u32 pa, before, value, after;
	u16 line;
	u8 list, act;
};

#define WLOG_MAX 1024
static struct wlog wlog[WLOG_MAX];
static unsigned int wlog_n, wlog_lost;

static struct {
	bool valid, armed, early, dry, pm_test_core, clobbered, undone;
	const char *refusal, *path_src;
	u32 pre_mask[3], new_mask[3];
	u32 pre_int_en, pre_int2_en, pre_cpu0_int_en, pre_grp2, pre_inform;
	u32 stat_after_clear, stat2_after_clear, inform_at_cpu_pm;
	u32 wake_stat, wake_stat2, eint_pend[8], eint_far_pend[4];
	struct ps_counts c_pre, c_post;
	unsigned int readback_bad, verify_bad, smc_err;
	unsigned int save_ok, save_skip[5];
	unsigned int restore_n, restore_diff, restore_writes, restore_gated;
	unsigned int list_done, list_dry, list_gated;
	u32 pd_saved;				/* bit per ps_pd_lists[] entry */
	int hsi2_cycled;			/* 1 done, <0 the step that failed */
	int disp_state;			/* 1 off at arm, 2 back on; <0 failed step */
	bool s2mpu_saved[PS_S2MPUS];
	u32 s2mpu_val[PS_S2MPUS], s2mpu_found[PS_S2MPUS];
	unsigned int s2mpu_restored;
	u16 pd_diff[PS_PD_LISTS_N];
	unsigned int pd_writes, pd_off;
} rec;

static bool pm_test_core;
static bool cycle_attempt;	/* this suspend cycle consumed budget */
static bool armed;		/* read by the CPU_PM notifier */

static u32 save_val[PS_SAVE_N];
static bool need_restore[PS_SAVE_N];
static u32 pd_val[PS_PD_SAVE_N];

static struct ps_flexpmu flexpmu;
static bool flexpmu_ok;
static struct device *root;
static struct acpm_handle *acpm;
static bool (*acpm_idle)(struct acpm_handle *handle);
static struct dentry *dbg_dir;

static void log_write(u8 list, u16 line, u32 pa, u32 before, u32 value, u32 after, u8 act)
{
	if (wlog_n >= WLOG_MAX) {
		wlog_lost++;
		return;
	}
	wlog[wlog_n++] = (struct wlog){ pa, before, value, after, line, list, act };
}

/* The only PMU_ALIVE write path: the secure SMC, as set_priv_reg(). */
static int pmu_write(u8 list, u16 line, u32 off, u32 val)
{
	struct arm_smccc_res res;
	u32 before = ps_pmu_read(off);

	arm_smccc_smc(GS201_SMC_PRIV_REG, PS_PMU_PA + off, PMUREG_WRITE, val, 0, 0, 0, 0, &res);
	log_write(list, line, PS_PMU_PA + off, before, val, ps_pmu_read(off),
		  res.a0 ? A_ERR : A_DONE);
	if (res.a0) {
		rec.smc_err++;
		return -EIO;
	}
	return 0;
}

/* pmucal_set_bit_atomic(), pmucal_rae.c:167-173: the PMU_ALIVE set alias
 * offset | 0xc000 takes the bit number (exynos-pmu-if.c:30-34, 72-79).
 * The alias itself is never read; the bit is read back at its offset.
 */
static void pmu_set_bit_atomic(const struct ps_seq *e, u8 list)
{
	struct arm_smccc_res res;
	u32 before = ps_pmu_read(e->offset);

	arm_smccc_smc(GS201_SMC_PRIV_REG, PS_PMU_PA + (e->offset | 0xc000), PMUREG_WRITE,
		      e->value, 0, 0, 0, 0, &res);
	log_write(list, e->line, PS_PMU_PA + (e->offset | 0xc000), before, e->value,
		  ps_pmu_read(e->offset), res.a0 ? A_ERR : A_DONE);
	if (res.a0)
		rec.smc_err++;
	else
		rec.list_done++;
}

/* pmucal_rae_write(), pmucal_rae.c:150-162, through pmucal_write_reg(). */
static void ps_seq_write(const struct ps_seq *e, u8 list)
{
	const struct ps_block *b = &ps_blocks[e->blk];
	void __iomem *reg = ps_reg(e);
	u32 cur, val;
	int gate;

	if (b->class == PS_CLASS_PMU) {
		val = e->value;
		if (e->mask != U32_MAX)
			val = (ps_pmu_read(e->offset) & ~e->mask) | (e->value & e->mask);
		if (!pmu_write(list, e->line, e->offset, val))
			rec.list_done++;
		return;
	}
	gate = ps_block_gate(e->blk);
	if (gate != PS_OK) {
		log_write(list, e->line, ps_seq_pa(e), 0, e->value, 0, A_GATED);
		rec.list_gated++;
		return;
	}
	cur = readl(reg);
	val = e->mask == U32_MAX ? e->value : (cur & ~e->mask) | (e->value & e->mask);
	if (b->class == PS_CLASS_OTHER && rec.dry) {
		log_write(list, e->line, ps_seq_pa(e), cur, val, cur, A_DRY);
		rec.list_dry++;
		return;
	}
	writel(val, reg);
	log_write(list, e->line, ps_seq_pa(e), cur, val, readl(reg), A_DONE);
	rec.list_done++;
}

/* pmucal_clr_pend(), pmucal_rae.c:217-223. Destinations are INTR_GEN
 * write-one-to-clear registers (never read); the pending source is logged.
 */
static void ps_seq_clear_pend(const struct ps_seq *e, u8 list)
{
	u32 pend = readl(ps_va[e->cblk] + e->cond_offset) & e->cond_mask;

	if (ps_blocks[e->blk].class != PS_CLASS_INTR_GEN) {
		log_write(list, e->line, ps_seq_pa(e), pend, pend & e->mask, 0, A_GATED);
		rec.list_gated++;
		return;
	}
	writel(pend & e->mask, ps_reg(e));
	log_write(list, e->line, ps_seq_pa(e), pend, pend & e->mask,
		  readl(ps_va[e->cblk] + e->cond_offset) & e->cond_mask, A_DONE);
	rec.list_done++;
}

/* pmucal_rae_handle_seq(), pmucal_rae.c:332-428, for the types these lists
 * use (the generator refuses any other). No waits, no retries: bounded.
 */
static void run_seq(const struct ps_seq *s, unsigned int n, u8 list)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		const struct ps_seq *e = &s[i];

		switch (e->type) {
		case PS_COND_WRITE:
			if (!ps_cond(e)) {
				log_write(list, e->line, ps_seq_pa(e), 0, e->value, 0, A_COND);
				break;
			}
			fallthrough;
		case PS_WRITE:
			ps_seq_write(e, list);
			break;
		case PS_CLEAR_PEND:
			ps_seq_clear_pend(e, list);
			break;
		case PS_SET_BIT_ATOMIC:
			pmu_set_bit_atomic(e, list);
			break;
		default:
			log_write(list, e->line, ps_seq_pa(e), 0, 0, 0, A_GATED);
			rec.list_gated++;
			break;
		}
	}
}

/* pmucal_rae_save_seq(), pmucal_rae.c:225-258. need_restore is reset for
 * every entry first, so only what this save read can be restored.
 */
static void run_save(void)
{
	unsigned int i;
	int st;

	for (i = 0; i < PS_SAVE_N; i++) {
		const struct ps_seq *e = &ps_save[i];

		need_restore[i] = false;
		st = ps_save_read(e, &save_val[i]);
		rec.save_skip[st]++;
		if (st != PS_OK)
			continue;
		rec.save_ok++;
		if (e->type == PS_SAVE_RESTORE || e->type == PS_COND_SAVE_RESTORE)
			need_restore[i] = true;
	}
}

/* pmucal_rae_restore_seq(), pmucal_rae.c:260-321: COND entries re-check
 * their condition, SAVE_RESTORE writes the saved value (all save masks are
 * 0xffffffff; a partial mask would be merged as pmucal_rae_write does).
 * In dry run, the write is replaced by a comparison with the saved value.
 */
static void run_restore(void)
{
	unsigned int i;
	u32 cur, val;

	for (i = 0; i < PS_SAVE_N; i++) {
		const struct ps_seq *e = &ps_save[i];

		if (e->type != PS_SAVE_RESTORE && e->type != PS_COND_SAVE_RESTORE)
			continue;
		if (e->type == PS_COND_SAVE_RESTORE && !ps_cond(e))
			continue;
		if (!need_restore[i])
			continue;
		need_restore[i] = false;
		rec.restore_n++;
		if (ps_block_gate(e->blk) != PS_OK) {
			log_write(L_RESTORE, e->line, ps_seq_pa(e), 0, save_val[i], 0, A_GATED);
			rec.restore_gated++;
			continue;
		}
		cur = readl(ps_reg(e));
		val = e->mask == U32_MAX ? save_val[i] : (cur & ~e->mask) | (save_val[i] & e->mask);
		if (cur != val)
			rec.restore_diff++;
		if (rec.dry) {
			if (cur != val)
				log_write(L_RESTORE, e->line, ps_seq_pa(e), cur, val, cur, A_DRY);
			continue;
		}
		writel(val, ps_reg(e));
		rec.restore_writes++;
		if (cur != val || verbose)
			log_write(L_RESTORE, e->line, ps_seq_pa(e), cur, val, readl(ps_reg(e)), A_DONE);
	}
}

/* pmucal_local_disable()/enable() save and restore, pmucal_local.c:63,119,
 * for every domain that is on at arm: SYS_SLEEP powers them down without
 * the domain driver (stock powers HSI0, DPU, DISP, EH and BO off through
 * genpd before it, and has no domain driver for HSI2 or the NOCs). After an
 * exit, every changed value is logged; with pd_restore it is written back,
 * except in lists with PLL waits (compare only) and, without pd_restore_noc,
 * the interconnect lists.
 */
static void pd_save(void)
{
	unsigned int l, i;

	BUILD_BUG_ON(PS_PD_LISTS_N > 32);
	rec.pd_saved = 0;
	for (l = 0; l < PS_PD_LISTS_N; l++) {
		const struct ps_pd_list *d = &ps_pd_lists[l];

		if (!(ps_pmu_read(d->status) & BIT(0)))
			continue;
		for (i = d->first; i < d->first + d->n; i++) {
			if (ps_block_gate(ps_pd_save[i].blk) != PS_OK)
				break;
			pd_val[i] = readl(ps_reg(&ps_pd_save[i]));
		}
		if (i == d->first + d->n)
			rec.pd_saved |= BIT(l);
	}
}

/* S2MPU CTRL0 only (offset 0, s2mpu-regs.h:3,28-31; bases
 * gs201-s2mpu.dtsi:146-157; any other S2MPU register has reset the SoC).
 * The bootloader leaves both units disabled (0). A power-cycled domain
 * brings its S2MPU back at the reset value, which stops the domain's DMA
 * until configured; stock's pKVM restores it when the domain powers up.
 * Restored in the syscore resume, before any device resumes.
 * MISC's and CPUCL0's units are always on while the SoC runs (the stock DT's
 * "always-on"; pd_status 0), and SYS_SLEEP resets them too (CTRL0 1 after a
 * deep resume, October 1). GPU's is the GPU domain driver's: that domain is
 * off when the sleep arms.
 */
static const struct {
	u32 pa;
	u16 pd_status;		/* 0: always on */
	const char *name;
} s2mpu[] = {
	{ 0x11880000, 0x2104, "S2MPU_HSI1" },
	{ 0x145e0000, 0x2184, "S2MPU_HSI2" },
	{ 0x101e0000, 0, "S2MPU_MISC" },
	{ 0x20c70000, 0, "S2MPU_CPUCL0" },
};
static void __iomem *s2mpu_va[PS_S2MPUS];

static bool s2mpu_powered(unsigned int i)
{
	return !s2mpu[i].pd_status || (ps_pmu_read(s2mpu[i].pd_status) & BIT(0));
}

static void s2mpu_save(void)
{
	unsigned int i;

	for (i = 0; i < PS_S2MPUS; i++) {
		rec.s2mpu_saved[i] = s2mpu_va[i] && s2mpu_powered(i);
		if (rec.s2mpu_saved[i])
			rec.s2mpu_val[i] = readl(s2mpu_va[i]);
	}
}

static void s2mpu_restore(void)
{
	unsigned int i;

	for (i = 0; i < PS_S2MPUS; i++) {
		if (!rec.s2mpu_saved[i] || !s2mpu_powered(i))
			continue;
		rec.s2mpu_found[i] = readl(s2mpu_va[i]);
		if (rec.s2mpu_found[i] == rec.s2mpu_val[i] || rec.dry)
			continue;
		writel(rec.s2mpu_val[i], s2mpu_va[i]);
		rec.s2mpu_restored++;
	}
}

/* Whether the exit list writes this register: after an exit the list's
 * value stands (the NOC and HSI2 sysreg DRCG enables), as in stock, where
 * no domain driver restores those domains afterwards.
 */
static bool exit_writes(const struct ps_seq *e)
{
	unsigned int i;

	for (i = 0; i < PS_EXIT_N; i++)
		if (ps_seq_pa(&ps_exit[i]) == ps_seq_pa(e) &&
		    (ps_exit[i].type == PS_WRITE || ps_exit[i].type == PS_COND_WRITE))
			return true;
	return false;
}

/* Stock's genpd power-off and power-on of HSI2 (exynos-pd.c with
 * pmucal_local.c:40-130): save list (done by pd_save), DTZPC save through
 * the monitor (exynos-pd_el3.c:16-38; SMC_CMD_PREPARE_PD_ONOFF 0x82000410,
 * EXYNOS_GET_IN_PD_DOWN 0 / EXYNOS_WAKEUP_PD_DOWN 1, RUNTIME_PM_TZPC_GROUP
 * 2, exynos-el3_mon.h:16-19; pd_hsi2 need_smc 0x14410204,
 * gs201-pm-domains.dtsi:75), hsi2_off, hsi2_on (flexpmu_cal_local_gs201.h:
 * 600-603, 659-663), DTZPC restore, then the save list written back. A test
 * of what SYS_SLEEP does to HSI2, without entering the firmware.
 */
#define SMC_PD_ONOFF		0x82000410
#define HSI2_TZPC		0x14410204
#define PMU_HSI2_CONFIG		0x2180
#define PMU_HSI2_STATUS		0x2184
#define CMU_CONTROLLER_OPTION	0x0800

static bool hsi2_wait(u32 want)
{
	unsigned int i;

	for (i = 0; i < 10000; i++) {
		if ((ps_pmu_read(PMU_HSI2_STATUS) & BIT(0)) == want)
			return true;
		udelay(1);
	}
	return false;
}

static int hsi2_power_cycle(void)
{
	struct arm_smccc_res res;
	unsigned int l, i;
	void __iomem *opt = ps_va[PS_BLK_CMU_HSI2] + CMU_CONTROLLER_OPTION;

	for (l = 0; l < PS_PD_LISTS_N; l++)
		if (!strcmp(ps_pd_lists[l].name, "hsi2"))
			break;
	if (l == PS_PD_LISTS_N || !(rec.pd_saved & BIT(l)))
		return -1;
	if (hsi2_cycle == 1) {
		arm_smccc_smc(SMC_PD_ONOFF, 0, HSI2_TZPC, 2, 0, 0, 0, 0, &res);
		hsi2_smc_ret = res.a0;
		if (res.a0)
			return -2;
	}
	/* Step markers reach the log only with printk.console_suspend=N. */
	pr_emerg("pixel-sleep: HSI2 cycle: controller option %08x\n", readl(opt));
	writel(readl(opt) & ~BIT(24), opt);
	pr_emerg("pixel-sleep: HSI2 cycle: powering off (config %08x)\n",
		 ps_pmu_read(PMU_HSI2_CONFIG));
	if (pmu_write(L_ARM, 0, PMU_HSI2_CONFIG, ps_pmu_read(PMU_HSI2_CONFIG) & ~BIT(0)))
		return -3;
	if (!hsi2_wait(0))
		return -4;
	pr_emerg("pixel-sleep: HSI2 cycle: off (status %08x), powering on\n",
		 ps_pmu_read(PMU_HSI2_STATUS));
	if (pmu_write(L_ARM, 0, PMU_HSI2_CONFIG, ps_pmu_read(PMU_HSI2_CONFIG) | BIT(0)))
		return -5;
	if (!hsi2_wait(1))
		return -6;
	pr_emerg("pixel-sleep: HSI2 cycle: on (status %08x)\n", ps_pmu_read(PMU_HSI2_STATUS));
	if (hsi2_cycle == 1) {
		arm_smccc_smc(SMC_PD_ONOFF, 1, HSI2_TZPC, 2, 0, 0, 0, 0, &res);
		hsi2_smc_ret = res.a0;
		if (res.a0)
			return -7;
	}
	for (i = ps_pd_lists[l].first; i < ps_pd_lists[l].first + ps_pd_lists[l].n; i++) {
		if (i == ps_pd_lists[l].first)
			pr_emerg("pixel-sleep: HSI2 cycle: first restore %#010x\n",
				 ps_seq_pa(&ps_pd_save[i]));
		writel(pd_val[i], ps_reg(&ps_pd_save[i]));
	}
	pr_emerg("pixel-sleep: HSI2 cycle: CMU restored\n");
	return 1;
}

/* Stock genpd off/on for the display domains (DPU inside DISP):
 * exynos_pd_tz_save/restore through the monitor (gs201-pm-domains.dtsi
 * need_smc 0x1C010204, 0x1C210204), then dpu_off/disp_off or disp_on/dpu_on
 * (flexpmu_cal_local_gs201.h), and the domain save lists written back.
 */
static const struct {
	const char *name;
	u16 config;
	u8 cmu_blk;
	u32 tzpc;
} disp_pds[2] = {
	{ "dpu", 0x2200, PS_BLK_CMU_DPU, 0x1c010204 },
	{ "disp", 0x2280, PS_BLK_CMU_DISP, 0x1c210204 },
};

static bool pd_wait(u16 status, u32 want)
{
	unsigned int i;

	for (i = 0; i < 10000; i++) {
		if ((ps_pmu_read(status) & BIT(0)) == want)
			return true;
		udelay(1);
	}
	return false;
}

static int disp_power_off(void)
{
	struct arm_smccc_res res;
	unsigned int i;

	for (i = 0; i < 2; i++) {
		void __iomem *opt = ps_va[disp_pds[i].cmu_blk] + CMU_CONTROLLER_OPTION;

		arm_smccc_smc(SMC_PD_ONOFF, 0, disp_pds[i].tzpc, 2, 0, 0, 0, 0, &res);
		if (res.a0)
			return -1 - 10 * i;
		writel(readl(opt) & ~BIT(24), opt);
		if (pmu_write(L_ARM, 0, disp_pds[i].config, ps_pmu_read(disp_pds[i].config) & ~BIT(0)))
			return -2 - 10 * i;
		if (!pd_wait(disp_pds[i].config + 4, 0))
			return -3 - 10 * i;
	}
	return 1;
}

static int disp_power_on(void)
{
	struct arm_smccc_res res;
	unsigned int i, l, k;

	for (i = 2; i-- > 0;) {
		if (pmu_write(L_RESUME, 0, disp_pds[i].config, ps_pmu_read(disp_pds[i].config) | BIT(0)))
			return -4 - 10 * i;
		if (!pd_wait(disp_pds[i].config + 4, 1))
			return -5 - 10 * i;
		arm_smccc_smc(SMC_PD_ONOFF, 1, disp_pds[i].tzpc, 2, 0, 0, 0, 0, &res);
		if (res.a0)
			return -6 - 10 * i;
		for (l = 0; l < PS_PD_LISTS_N; l++) {
			if (strcmp(ps_pd_lists[l].name, disp_pds[i].name) || !(rec.pd_saved & BIT(l)))
				continue;
			for (k = ps_pd_lists[l].first; k < ps_pd_lists[l].first + ps_pd_lists[l].n; k++)
				writel(pd_val[k], ps_reg(&ps_pd_save[k]));
		}
	}
	return 2;
}

static void pd_restore_all(void)
{
	unsigned int l, i;
	u32 cur;

	for (l = 0; l < PS_PD_LISTS_N; l++) {
		const struct ps_pd_list *d = &ps_pd_lists[l];
		bool write = !rec.dry && !rec.early && READ_ONCE(pd_restore) &&
			     !(d->flags & PS_PD_WAITS) &&
			     (!(d->flags & PS_PD_NOC) || READ_ONCE(pd_restore_noc));

		if (!(rec.pd_saved & BIT(l)))
			continue;
		if (!(ps_pmu_read(d->status) & BIT(0))) {
			rec.pd_off++;
			continue;
		}
		for (i = d->first; i < d->first + d->n; i++) {
			const struct ps_seq *e = &ps_pd_save[i];

			if (ps_block_gate(e->blk) != PS_OK)
				break;
			cur = readl(ps_reg(e));
			if (cur == pd_val[i])
				continue;
			rec.pd_diff[l]++;
			if (!write || exit_writes(e)) {
				log_write(L_PD, e->line, ps_seq_pa(e), cur, pd_val[i], cur, A_DRY);
				continue;
			}
			writel(pd_val[i], ps_reg(e));
			rec.pd_writes++;
			log_write(L_PD, e->line, ps_seq_pa(e), cur, pd_val[i], readl(ps_reg(e)),
				  A_DONE);
		}
	}
}

static bool eint_denied(unsigned int bit)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(eint_deny); i++)
		if (bit == eint_deny[i])
			return true;
	return false;
}

/* exynos_wkup_irq_set_wake(): 1 = masked, one bit per wake EINT. */
static int build_masks(u32 m[3])
{
	unsigned int i, bit;

	m[0] = m[1] = m[2] = U32_MAX;
	for (i = 0; i < min_t(unsigned int, eint_wake_n, ARRAY_SIZE(eint_wake)); i++) {
		bit = eint_wake[i];
		if (bit >= 96 || eint_denied(bit))
			return -EINVAL;
		m[bit / 32] &= ~BIT(bit % 32);
	}
	return 0;
}

static bool lpm_pmu_durations_stock(void)
{
	unsigned int i;

	for (i = 0; i < PS_LPM_INIT_N; i++) {
		const struct ps_seq *e = &ps_lpm_init[i];

		if (e->blk == PS_PMU && (ps_pmu_read(e->offset) & e->mask) != (e->value & e->mask))
			return false;
	}
	return true;
}

/* Blocks the stock save list reads without a condition must be on for a
 * real sleep (stock would have hung otherwise); CPUCL1/2 are gated by
 * design. Exit-list blocks (HSI1/HSI2 sysregs) are only required after the
 * wake, when the firmware has powered the SoC back up; they are gated then.
 */
static const char *unpowered_list_block(void)
{
	unsigned int i;

	for (i = 0; i < PS_SAVE_N; i++) {
		const struct ps_seq *e = &ps_save[i];

		if ((e->type == PS_SAVE_RESTORE || e->type == PS_READ) &&
		    ps_blocks[e->blk].cluster <= 0 && ps_block_gate(e->blk) != PS_OK)
			return ps_blocks[e->blk].name;
	}
	return NULL;
}

/* Returns NULL when armable, else the reason. Reads PMU_ALIVE only. */
static const char *preconditions(u32 masks[3])
{
	unsigned int cpu;

	if (smp_processor_id() != 0 || num_online_cpus() != 1)
		return "not CPU 0 alone";
	if (!real_sleep && !pm_test_core)
		return "pm_test is not core (set real_sleep=1 to permit firmware entry)";
	if (real_sleep && dry_run)
		return "real_sleep needs dry_run=0";
	if (build_masks(masks))
		return "eint_wake has a bit >= 96 or a denied bit";
	/* Mode 13 (SLEEP_HSI1ON) is deferred: the modem host must be down. */
	if (ps_pmu_read(PMU_PCIE_PHY_HSI1) & BIT(0))
		return "modem PCIe PHY not isolated (link up)";
	if (ps_pmu_read(PMU_WAKEUP_INT_EN) || ps_pmu_read(PMU_WAKEUP2_INT_EN))
		return "WAKEUP_INT_EN already owned";
	if (ps_pmu_read(PMU_CPU_INFORM0))
		return "CPU_INFORM[0] not clear";
	if (!real_sleep)
		return NULL;
	if (!flexpmu_ok || !rec.c_pre.valid)
		return "FLEXPMU counters unavailable (early/exit undecidable)";
	for (cpu = 1; cpu < 8; cpu++)
		if (ps_pmu_read(cpu_status[cpu]) & BIT(0))
			return "a secondary CPU is still powered";
	if (ps_pmu_read(PMU_PCIE_PHY_HSI2) & BIT(0))
		return "Wi-Fi PCIe PHY not isolated";
	if (ps_pmu_read(PMU_G3D_STATUS) & BIT(0))
		return "G3D powered";
	if (!lpm_pmu_durations_stock())
		return "lpm_init PMU durations differ from stock (lpm_init_pmu=1)";
	if (!acpm_idle || !acpm_idle(acpm))
		return "ACPM queues busy or idle check unavailable";
	if (unpowered_list_block())
		return "a block the stock lists touch unconditionally is off";
	return NULL;
}

static void undo_arm(void)
{
	u32 v;

	v = readl(ps_va[PS_INTR] + INTR_GRP2_ENABLE);
	writel((v & ~BIT(0)) | (rec.pre_grp2 & BIT(0)), ps_va[PS_INTR] + INTR_GRP2_ENABLE);
	log_write(L_UNDO, 0, ps_blocks[PS_INTR].pa + INTR_GRP2_ENABLE, v,
		  (v & ~BIT(0)) | (rec.pre_grp2 & BIT(0)),
		  readl(ps_va[PS_INTR] + INTR_GRP2_ENABLE), A_DONE);
	v = ps_pmu_read(PMU_CPU0_INT_EN);
	pmu_write(L_UNDO, 0, PMU_CPU0_INT_EN, (v & ~BIT(3)) | (rec.pre_cpu0_int_en & BIT(3)));
	pmu_write(L_UNDO, 0, PMU_CPU_INFORM0, rec.pre_inform);
	pmu_write(L_UNDO, 0, PMU_WAKEUP_INT_EN, rec.pre_int_en);
	pmu_write(L_UNDO, 0, PMU_WAKEUP2_INT_EN, rec.pre_int2_en);
	for (v = 0; v < 3; v++)
		pmu_write(L_UNDO, 0, PMU_EINT_MASK(v), rec.pre_mask[v]);
	pixel_cpupm_system_sleep(false);
}

static int ps_suspend(void *data)
{
	unsigned int i, left;
	int err = 0;

	if (pm_suspend_target_state != PM_SUSPEND_MEM)
		return 0;
	left = READ_ONCE(budget);
	if (!left) {
		pr_info("pixel-sleep: deep suspend, budget 0: not armed\n");
		return 0;
	}
	WRITE_ONCE(budget, left - 1);
	attempts++;
	cycle_attempt = true;

	memset(&rec, 0, sizeof(rec));
	wlog_n = 0;
	wlog_lost = 0;
	rec.valid = true;
	rec.dry = dry_run;
	rec.pm_test_core = pm_test_core;
	if (flexpmu_ok)
		ps_flexpmu_read(&flexpmu, &rec.c_pre);
	rec.refusal = preconditions(rec.new_mask);
	if (rec.refusal) {
		refused++;
		pr_warn("pixel-sleep: deep suspend refused: %s\n", rec.refusal);
		return -EBUSY;
	}
	ps_gate_flags = gate_cpucl ? PS_GATE_CPUCL_STATUS : 0;

	for (i = 0; i < 3; i++)
		rec.pre_mask[i] = ps_pmu_read(PMU_EINT_MASK(i));
	rec.pre_int_en = ps_pmu_read(PMU_WAKEUP_INT_EN);
	rec.pre_int2_en = ps_pmu_read(PMU_WAKEUP2_INT_EN);
	rec.pre_cpu0_int_en = ps_pmu_read(PMU_CPU0_INT_EN);
	rec.pre_inform = ps_pmu_read(PMU_CPU_INFORM0);
	rec.pre_grp2 = ps_intr_read(INTR_GRP2_ENABLE);

	/* exynos_set_wakeupmask(), exynos-pm.c:233-250. */
	for (i = 0; i < 3; i++)
		err |= pmu_write(L_ARM, 0, PMU_EINT_MASK(i), rec.new_mask[i]);
	err |= pmu_write(L_ARM, 0, PMU_WAKEUP_STAT, 0);
	err |= pmu_write(L_ARM, 0, PMU_WAKEUP_INT_EN, STOCK_INT_EN);
	err |= pmu_write(L_ARM, 0, PMU_WAKEUP2_STAT, 0);
	err |= pmu_write(L_ARM, 0, PMU_WAKEUP2_INT_EN, STOCK_INT2_EN);
	rec.stat_after_clear = ps_pmu_read(PMU_WAKEUP_STAT);
	rec.stat2_after_clear = ps_pmu_read(PMU_WAKEUP2_STAT);
	if (err)
		goto undo;	/* the monitor refused a PMU write: stop here */

	/* pmucal_system_enter(), pmucal_system.c:57-62. */
	pixel_cpupm_system_sleep(true);
	err |= pmu_write(L_ARM, 0, PMU_CPU_INFORM0, CPU_INFORM_SLEEP);
	run_save();
	run_seq(ps_enter, PS_ENTER_N, L_ENTER);

	/* Read back every control register written. WAKEUP_STAT is a latch of
	 * live wake events, not a control: it is recorded, never compared.
	 */
	for (i = 0; i < 3; i++)
		rec.readback_bad += !!((ps_pmu_read(PMU_EINT_MASK(i)) ^ rec.new_mask[i]) &
				       eint_mask_impl[i]);
	rec.readback_bad += !!((ps_pmu_read(PMU_WAKEUP_INT_EN) ^ STOCK_INT_EN) & INT_EN_IMPL);
	rec.readback_bad += ps_pmu_read(PMU_WAKEUP2_INT_EN) != STOCK_INT2_EN;
	rec.readback_bad += ps_pmu_read(PMU_CPU_INFORM0) != CPU_INFORM_SLEEP;
	for (i = 0; i < PS_ENTER_N; i++) {
		const struct ps_seq *e = &ps_enter[i];

		if (e->type == PS_WRITE &&
		    (readl(ps_reg(e)) & e->mask) != (e->value & e->mask))
			rec.readback_bad++;
	}
	if (err || rec.readback_bad || rec.smc_err || rec.list_gated)
		goto undo;
	/* exynos_pm_syscore_suspend(), exynos-pm.c:303-311. */
	if (flexpmu_ok)
		ps_flexpmu_read(&flexpmu, &rec.c_pre);
	/* The consumers' stock pin power-down states, then
	 * samsung_pinctrl_suspend(): the banks that lose power in SYS_SLEEP.
	 */
	psg_apply_pdn(!rec.dry);
	psg_save();
	pd_save();
	s2mpu_save();
	psu_save();
	if (hsi2_cycle && rec.pm_test_core && !rec.dry && !real_sleep)
		rec.hsi2_cycled = hsi2_power_cycle();
	if (disp_off && !rec.dry)
		rec.disp_state = disp_power_off();
	rec.armed = true;
	WRITE_ONCE(armed, true);
	armed_count++;
	pr_info("pixel-sleep: armed SYS_SLEEP (%s), masks %08x %08x %08x, sleep_soc_down %u\n",
		rec.dry ? "dry run" : "live", rec.new_mask[0], rec.new_mask[1],
		rec.new_mask[2], rec.c_pre.sleep_soc_down);
	return 0;
undo:
	aborted++;
	rec.undone = true;
	undo_arm();
	pr_err("pixel-sleep: arm failed (read-back %u, SMC errors %u, gated %u); undone\n",
	       rec.readback_bad, rec.smc_err, rec.list_gated);
	return -EIO;
}

static void ps_resume(void *data)
{
	unsigned int i;

	if (!rec.armed)
		return;
	rec.armed = false;
	WRITE_ONCE(armed, false);
	if (rec.disp_state == 1)
		rec.disp_state = disp_power_on();

	/* samsung_pinctrl_resume() runs before exynos-pm's resume releases pad
	 * retention (the exit list's TOP_OUT writes). In a dry run it only counts.
	 */
	psg_restore(!rec.dry);
	s2mpu_restore();

	/* exynos_show_wakeup_reason(), exynos-pm.c:177-231. */
	rec.wake_stat = ps_pmu_read(PMU_WAKEUP_STAT);
	rec.wake_stat2 = ps_pmu_read(PMU_WAKEUP2_STAT);
	for (i = 0; i < 8; i++)
		rec.eint_pend[i] = readl(gpio_alive + EINT_PEND + 4 * i);
	for (i = 0; i < 4; i++)
		rec.eint_far_pend[i] = readl(gpio_far + EINT_PEND + 4 * i);

	/* exynos_pm_syscore_resume(), exynos-pm.c:318-332. */
	if (flexpmu_ok)
		ps_flexpmu_read(&flexpmu, &rec.c_post);
	if (rec.c_pre.valid && rec.c_post.valid) {
		rec.early = rec.c_post.sleep_soc_down == rec.c_pre.sleep_soc_down;
		rec.path_src = "SLEEP AP-down counter";
	} else if (!real_sleep) {
		/* Only pm_test=core runs arm without counters: no firmware entry. */
		rec.early = true;
		rec.path_src = "pm_test=core (counters unavailable)";
	} else {
		/* Armed with counters that later failed: releasing retention
		 * (exit) is the safer guess after a possible power-down.
		 */
		rec.early = false;
		rec.path_src = "counters unreadable after a real attempt: exit assumed";
	}

	/* Stock clears the hint in CPU_PM_EXIT (cal_cpu_enable,
	 * pmucal_cpu.c:32) before this point on both paths.
	 */
	pmu_write(L_RESUME, 0, PMU_CPU_INFORM0, 0);
	if (rec.early) {
		run_seq(ps_early, PS_EARLY_N, L_EARLY);	/* pmucal_system.c:190 */
		early_count++;
	} else {
		run_seq(ps_exit, PS_EXIT_N, L_EXIT);	/* pmucal_system.c:123 */
		exit_count++;
	}
	run_restore();					/* pmucal_system.c:132, 200 */
	pd_restore_all();		/* genpd power-on of each domain, resume_noirq */
	psu_restore(!rec.dry);		/* exynos-usi and the bus drivers' resume */

	if (ps_pmu_read(PMU_WAKEUP_INT_EN) != rec.pre_int_en)
		pmu_write(L_RESUME, 0, PMU_WAKEUP_INT_EN, rec.pre_int_en);
	if (ps_pmu_read(PMU_WAKEUP2_INT_EN) != rec.pre_int2_en)
		pmu_write(L_RESUME, 0, PMU_WAKEUP2_INT_EN, rec.pre_int2_en);
	for (i = 0; i < 3; i++)
		pmu_write(L_RESUME, 0, PMU_EINT_MASK(i), rec.pre_mask[i]);

	rec.verify_bad += ps_pmu_read(PMU_CPU_INFORM0) != 0;
	rec.verify_bad += ps_pmu_read(PMU_WAKEUP_INT_EN) != rec.pre_int_en;
	rec.verify_bad += ps_pmu_read(PMU_WAKEUP2_INT_EN) != rec.pre_int2_en;
	rec.verify_bad += !!(ps_pmu_read(PMU_CPU0_INT_EN) & BIT(3));
	for (i = 0; i < 3; i++)
		rec.verify_bad += ps_pmu_read(PMU_EINT_MASK(i)) != rec.pre_mask[i];
	pixel_cpupm_system_sleep(false);
	pr_info("pixel-sleep: resumed via %s (%s), verify %s\n",
		rec.early ? "early_sleep" : "exit_sleep", rec.path_src,
		rec.verify_bad ? "FAILED" : "ok");
}

static const struct syscore_ops ps_syscore_ops = {
	.suspend = ps_suspend,
	.resume = ps_resume,
};

static struct syscore ps_syscore = {
	.ops = &ps_syscore_ops,
};

/* Last CPU_PM_ENTER notifier: cpu_pm_suspend() runs after ps_suspend(), as
 * the final syscore op before PSCI SYSTEM_SUSPEND. A replaced hint aborts.
 */
static int ps_cpu_pm_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	if (action != CPU_PM_ENTER || !READ_ONCE(armed) || smp_processor_id())
		return NOTIFY_OK;
	rec.inform_at_cpu_pm = ps_pmu_read(PMU_CPU_INFORM0);
	if (rec.inform_at_cpu_pm != CPU_INFORM_SLEEP) {
		rec.clobbered = true;
		return NOTIFY_BAD;
	}
	return NOTIFY_OK;
}

static struct notifier_block ps_cpu_pm_nb = {
	.notifier_call = ps_cpu_pm_notify,
	.priority = INT_MIN,
};

static bool read_pm_test_core(void)
{
	char buf[128];
	struct file *f;
	loff_t pos = 0;
	ssize_t n;

	f = filp_open("/sys/power/pm_test", O_RDONLY, 0);
	if (IS_ERR(f))
		return false;
	n = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);
	if (n <= 0)
		return false;
	buf[n] = 0;
	return strstr(buf, "[core]") != NULL;
}

static void names(char *out, size_t len, u32 stat, const char * const *tbl, unsigned int n)
{
	unsigned int bit, pos = 0;

	out[0] = 0;
	for (bit = 0; bit < 32; bit++)
		if (stat & BIT(bit))
			pos += scnprintf(out + pos, len - pos, "%s%s", pos ? "," : "",
					 bit < n ? tbl[bit] : "?");
}

static void report_attempt(void)
{
	char ws[320], ws2[160];
	unsigned int i, shown = 0;

	if (rec.refusal)
		return;
	if (rec.undone) {
		pr_err("pixel-sleep: arm was undone (read-back %u, SMC errors %u); see debugfs pixel-sleep/last\n",
		       rec.readback_bad, rec.smc_err);
		return;
	}
	names(ws, sizeof(ws), rec.wake_stat, ws_name, ARRAY_SIZE(ws_name));
	names(ws2, sizeof(ws2), rec.wake_stat2, ws2_name, ARRAY_SIZE(ws2_name));
	pr_info("pixel-sleep: %s run, path %s; CPU_INFORM at CPU_PM_ENTER %u%s\n",
		rec.dry ? "dry" : "live", rec.early ? "early" : "exit", rec.inform_at_cpu_pm,
		rec.clobbered ? " (REPLACED: suspend aborted)" : "");
	pr_info("pixel-sleep: STAT after clear %08x/%08x; wake STAT %08x [%s] STAT2 %08x [%s]\n",
		rec.stat_after_clear, rec.stat2_after_clear, rec.wake_stat, ws, rec.wake_stat2, ws2);
	pr_info("pixel-sleep: EINT_PEND %02x %02x %02x %02x %02x %02x %02x %02x far %02x %02x %02x %02x\n",
		rec.eint_pend[0], rec.eint_pend[1], rec.eint_pend[2], rec.eint_pend[3],
		rec.eint_pend[4], rec.eint_pend[5], rec.eint_pend[6], rec.eint_pend[7],
		rec.eint_far_pend[0], rec.eint_far_pend[1], rec.eint_far_pend[2],
		rec.eint_far_pend[3]);
	if (rec.c_pre.valid && rec.c_post.valid)
		pr_info("pixel-sleep: sleep_soc_down %u->%u sleep_early %u->%u sleep_mif_down %u->%u mif_always_on %u MIF req %#x\n",
			rec.c_pre.sleep_soc_down, rec.c_post.sleep_soc_down,
			rec.c_pre.sleep_early, rec.c_post.sleep_early,
			rec.c_pre.sleep_mif_down, rec.c_post.sleep_mif_down,
			rec.c_post.mif_always_on, (rec.c_pre.sw_flag1 >> 16) & 0xff);
	pr_info("pixel-sleep: pin power-down states differing %u, written %u\n",
		psg.pdn_differed, psg.pdn_written);
	pr_info("pixel-sleep: GPIO banks saved %u, registers differing %u, restored %u, skipped %u\n",
		psg.saved_banks, psg.differed, psg.restored, psg.skipped);
	for (i = 0; i < PS_PD_LISTS_N; i++)
		if (rec.pd_saved & BIT(i))
			pr_info("pixel-sleep: domain %s: %u of %u saved registers changed\n",
				ps_pd_lists[i].name, rec.pd_diff[i], ps_pd_lists[i].n);
	pr_info("pixel-sleep: domain registers written %u, domains off at resume %u\n",
		rec.pd_writes, rec.pd_off);
	for (i = 0; i < PS_S2MPUS; i++)
		if (rec.s2mpu_saved[i])
			pr_info("pixel-sleep: %s CTRL0 %08x before, %08x after resume\n",
				s2mpu[i].name, rec.s2mpu_val[i], rec.s2mpu_found[i]);
	pr_info("pixel-sleep: S2MPU CTRL0 restored %u\n", rec.s2mpu_restored);
	pr_info("pixel-sleep: USIs saved %u, lost %u, restored %u\n",
		psu.saved_n, psu.lost, psu.written);
	if (rec.disp_state)
		pr_info("pixel-sleep: display domains off/on: %d\n", rec.disp_state);
	if (rec.hsi2_cycled)
		pr_info("pixel-sleep: HSI2 power cycle %s (%d)\n",
			rec.hsi2_cycled > 0 ? "done" : "FAILED", rec.hsi2_cycled);
	pr_info("pixel-sleep: save read %u, skipped cond %u pd %u cluster %u; restore %u, differed %u, written %u, gated %u\n",
		rec.save_ok, rec.save_skip[PS_SKIP_COND], rec.save_skip[PS_SKIP_PD],
		rec.save_skip[PS_SKIP_CLUSTER], rec.restore_n, rec.restore_diff,
		rec.restore_writes, rec.restore_gated);
	pr_info("pixel-sleep: list writes done %u, dry %u, gated %u, SMC errors %u, verify %u bad, log %u (+%u lost)\n",
		rec.list_done, rec.list_dry, rec.list_gated, rec.smc_err, rec.verify_bad,
		wlog_n, wlog_lost);
	/* Changed domain registers first: they identify what a power-down
	 * lost. Then, without verbose: every non-performed write, and performed
	 * restore, exit and undo writes (restores only when they differed).
	 */
	for (i = 0; i < wlog_n && shown < 24; i++) {
		const struct wlog *w = &wlog[i];

		if (w->list != L_PD)
			continue;
		shown++;
		pr_info("pixel-sleep: pd line %u %#010x %08x -> %08x (after %08x) %s\n",
			w->line, w->pa, w->before, w->value, w->after, act_name[w->act]);
	}
	shown = 0;
	for (i = 0; i < wlog_n; i++) {
		const struct wlog *w = &wlog[i];

		if (w->list == L_PD)
			continue;
		if (!verbose && w->act == A_DONE && w->list != L_RESTORE &&
		    w->list != L_EXIT && w->list != L_UNDO)
			continue;
		if (!verbose && ++shown > 32) {
			pr_info("pixel-sleep: more in /sys/kernel/debug/pixel-sleep/last\n");
			break;
		}
		pr_info("pixel-sleep: %s line %u %#010x %08x -> %08x (after %08x) %s\n",
			list_name[w->list], w->line, w->pa, w->before, w->value, w->after,
			act_name[w->act]);
	}
}

static int ps_pm_notify(struct notifier_block *nb, unsigned long event, void *unused)
{
	switch (event) {
	case PM_SUSPEND_PREPARE:
		pm_test_core = READ_ONCE(budget) ? read_pm_test_core() : false;
		cycle_attempt = false;
		break;
	case PM_POST_SUSPEND:
		if (cycle_attempt)
			report_attempt();
		cycle_attempt = false;
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block ps_pm_nb = {
	.notifier_call = ps_pm_notify,
};

static int last_show(struct seq_file *m, void *v)
{
	unsigned int i;

	seq_printf(m, "valid %d dry %d pm_test_core %d refusal %s undone %d\n", rec.valid,
		   rec.dry, rec.pm_test_core, rec.refusal ?: "-", rec.undone);
	if (!rec.valid || rec.refusal)
		return 0;
	seq_printf(m, "path %s (%s) inform_at_cpu_pm %u clobbered %d\n",
		   rec.early ? "early" : "exit", rec.path_src ?: "-", rec.inform_at_cpu_pm,
		   rec.clobbered);
	seq_printf(m, "pre masks %08x %08x %08x int_en %08x %08x cpu0_int_en %08x grp2 %08x inform %u\n",
		   rec.pre_mask[0], rec.pre_mask[1], rec.pre_mask[2], rec.pre_int_en,
		   rec.pre_int2_en, rec.pre_cpu0_int_en, rec.pre_grp2, rec.pre_inform);
	seq_printf(m, "new masks %08x %08x %08x stat_after_clear %08x %08x\n", rec.new_mask[0],
		   rec.new_mask[1], rec.new_mask[2], rec.stat_after_clear, rec.stat2_after_clear);
	seq_printf(m, "wake stat %08x stat2 %08x eint_pend", rec.wake_stat, rec.wake_stat2);
	for (i = 0; i < 8; i++)
		seq_printf(m, " %02x", rec.eint_pend[i]);
	seq_puts(m, " far");
	for (i = 0; i < 4; i++)
		seq_printf(m, " %02x", rec.eint_far_pend[i]);
	seq_putc(m, '\n');
	seq_printf(m, "counters pre valid %d sleep_soc_down %u sleep_early %u sleep_mif_down %u; post valid %d %u %u %u; mif_always_on %u\n",
		   rec.c_pre.valid, rec.c_pre.sleep_soc_down, rec.c_pre.sleep_early,
		   rec.c_pre.sleep_mif_down, rec.c_post.valid, rec.c_post.sleep_soc_down,
		   rec.c_post.sleep_early, rec.c_post.sleep_mif_down, rec.c_post.mif_always_on);
	seq_printf(m, "save ok %u skip cond %u pd %u cluster %u excluded %u\n", rec.save_ok,
		   rec.save_skip[PS_SKIP_COND], rec.save_skip[PS_SKIP_PD],
		   rec.save_skip[PS_SKIP_CLUSTER], rec.save_skip[PS_SKIP_EXCLUDED]);
	seq_printf(m, "restore %u differed %u written %u gated %u\n", rec.restore_n,
		   rec.restore_diff, rec.restore_writes, rec.restore_gated);
	for (i = 0; i < PS_PD_LISTS_N; i++)
		if (rec.pd_saved & BIT(i))
			seq_printf(m, "domain %s saved %u changed %u\n", ps_pd_lists[i].name,
				   ps_pd_lists[i].n, rec.pd_diff[i]);
	seq_printf(m, "domain writes %u off at resume %u\n", rec.pd_writes, rec.pd_off);
	seq_printf(m, "lists done %u dry %u gated %u smc_err %u readback_bad %u verify_bad %u\n",
		   rec.list_done, rec.list_dry, rec.list_gated, rec.smc_err, rec.readback_bad,
		   rec.verify_bad);
	seq_printf(m, "# list line pa before value after action (%u entries, %u lost)\n",
		   wlog_n, wlog_lost);
	for (i = 0; i < wlog_n; i++)
		seq_printf(m, "%s %u %#010x %08x %08x %08x %s\n", list_name[wlog[i].list],
			   wlog[i].line, wlog[i].pa, wlog[i].before, wlog[i].value, wlog[i].after,
			   act_name[wlog[i].act]);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(last);

/* M4: pmucal_lpm_init parity. The PMU durations (ALIVE) are compared and,
 * with lpm_init_pmu=1, the differing ones written through the SMC with the
 * stock masked semantics and read back. The ALIVE/TOP/MIF/CPUCL0 targets
 * are compared only; subsystem-domain and CPUCL1/2 targets are left to
 * pixel-sleep-audit.
 */
static long lpm_check(void *arg)
{
	unsigned int i, diff = 0, fixed = 0, skipped = 0;
	u32 v, want;

	for (i = 0; i < PS_LPM_INIT_N; i++) {
		const struct ps_seq *e = &ps_lpm_init[i];
		const struct ps_block *b = &ps_blocks[e->blk];

		if (b->pd_status || b->cluster > 0 || ps_block_gate(e->blk) != PS_OK) {
			skipped++;
			continue;
		}
		v = readl(ps_reg(e));
		if ((v & e->mask) == (e->value & e->mask))
			continue;
		diff++;
		want = (v & ~e->mask) | (e->value & e->mask);
		pr_info("pixel-sleep: lpm_init line %u %s %#010x = %08x, stock %08x (mask %08x)\n",
			e->line, e->name, ps_seq_pa(e), v, want, e->mask);
		if (lpm_init_pmu && b->class == PS_CLASS_PMU && !pmu_write(L_LPM, e->line, e->offset, want) &&
		    (ps_pmu_read(e->offset) & e->mask) == (e->value & e->mask))
			fixed++;
	}
	pr_info("pixel-sleep: lpm_init: %u differ, %u PMU durations written, %u not checked here\n",
		diff, fixed, skipped);
	return 0;
}

static void ps_cleanup(void)
{
	unsigned int i;

	debugfs_remove_recursive(dbg_dir);
	if (acpm_idle)
		symbol_put(exynos_acpm_is_idle);
	if (root)
		root_device_unregister(root);
	ps_flexpmu_close(&flexpmu);
	if (gpio_far)
		iounmap(gpio_far);
	if (gpio_alive)
		iounmap(gpio_alive);
	psg_unmap();
	psu_unmap();
	for (i = 0; i < PS_S2MPUS; i++)
		if (s2mpu_va[i])
			iounmap(s2mpu_va[i]);
	ps_unmap_blocks();
}

static int __init pixel_sleep_init(void)
{
	struct device_node *np;
	unsigned int i;
	int ret;

	BUILD_BUG_ON(ARRAY_SIZE(s2mpu) != PS_S2MPUS);
	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	ret = ps_map_blocks();
	if (ret)
		return ret;
	for (i = 0; i < PS_S2MPUS; i++)
		s2mpu_va[i] = ioremap(s2mpu[i].pa, 0x1000);
	gpio_alive = ioremap(GPIO_ALIVE_PA, 0x1000);
	gpio_far = ioremap(GPIO_FAR_ALIVE_PA, 0x1000);
	if (!gpio_alive || !gpio_far || psg_map() || psu_map()) {
		ret = -ENOMEM;
		goto err;
	}
	ret = ps_flexpmu_open(&flexpmu);
	flexpmu_ok = !ret;
	if (ret)
		pr_warn("pixel-sleep: FLEXPMU counters unavailable (%d): early/exit only for pm_test=core\n",
			ret);
	root = root_device_register("pixel-sleep");
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		root = NULL;
		goto err;
	}
	np = of_find_compatible_node(NULL, NULL, "google,gs201-acpm-ipc");
	acpm = np ? devm_acpm_get_by_node(root, np) : ERR_PTR(-ENODEV);
	of_node_put(np);
	if (!IS_ERR(acpm))
		acpm_idle = symbol_get(exynos_acpm_is_idle);
	ret = work_on_cpu(0, lpm_check, NULL);
	if (ret)
		goto err;
	dbg_dir = debugfs_create_dir("pixel-sleep", NULL);
	debugfs_create_file("last", 0400, dbg_dir, NULL, &last_fops);
	ret = register_pm_notifier(&ps_pm_nb);
	if (ret)
		goto err;
	ret = cpu_pm_register_notifier(&ps_cpu_pm_nb);
	if (ret) {
		unregister_pm_notifier(&ps_pm_nb);
		goto err;
	}
	/* Registered long after cpu_pm (core_initcall): this suspend runs
	 * before cpu_pm_suspend() and this resume after cpu_pm_resume(), as
	 * stock exynos-pm (arch_initcall) did.
	 */
	register_syscore(&ps_syscore);
	pr_info("pixel-sleep: loaded; budget %u dry_run %d real_sleep %d counters %s acpm-idle %s\n",
		budget, dry_run, real_sleep, flexpmu_ok ? "yes" : "no", acpm_idle ? "yes" : "no");
	return 0;
err:
	ps_cleanup();
	return ret;
}
module_init(pixel_sleep_init);

static void __exit pixel_sleep_exit(void)
{
	unregister_syscore(&ps_syscore);
	cpu_pm_unregister_notifier(&ps_cpu_pm_nb);
	unregister_pm_notifier(&ps_pm_nb);
	ps_cleanup();
}
module_exit(pixel_sleep_exit);

MODULE_DESCRIPTION("GS201 SYS_SLEEP syscore sequence (stock exynos-pm/PMUCAL lists)");
MODULE_LICENSE("GPL");
