// SPDX-License-Identifier: GPL-2.0-only
/* GS201 SYS_SLEEP Stage 0 audit: a read-only snapshot of every register the
 * stock deep-sleep path depends on (docs/deep-sleep-plan-20260930.md §6).
 *
 * This module contains no register write path at all (the shared header
 * has none, and nothing here calls an SMC, writel or a PMIC write). It
 * reads, on load:
 * - PMU_ALIVE: wake enables/status/EINT masks, SYSTEM_CTRL, TOP_OUT,
 *   CPU0_INT_EN, CPU_INFORM0-7, the lpm_init durations, both PCIe PHY
 *   isolation controls, and every PD STATUS named in the stock tables;
 * - PMU_INTR_GEN: GRP1/2/4/27/31 pending (and GRP2 enable);
 * - every pmucal_lpm_init target and every save_sleep entry, with stock
 *   save semantics (COND_* only when their condition holds) and only in
 *   blocks whose power domain reads on; CMU_CPUCL1/2 from a CPU of that
 *   cluster while its NONCPU STATUS is on, CPUCL0 from CPU 0;
 * - S2MPU_HSI1/HSI2 CTRL0 (offset 0 only) with HSI1/HSI2 on;
 * - the MCT G_TCON;
 * - S2MPG12/13 regulator CTRL registers and PCTRLSEL over ACPM PMIC reads
 *   (never L14S_CTRL: the eSIM/eSE rail is not touched in any way);
 * - the patch-0003 FLEXPMU counters when the running kernel has them.
 *
 * The report is /sys/kernel/debug/pixel-sleep-audit/report (one record per
 * line, see README). With oneshot=1 it is printed to the kernel log instead
 * and loading fails with -EAGAIN, leaving nothing behind.
 */
#include <linux/cpu.h>
#include <linux/debugfs.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/module.h>
#include <linux/seq_buf.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "pixel-sleep-common.h"

static bool oneshot;
module_param(oneshot, bool, 0444);
MODULE_PARM_DESC(oneshot, "Print the report to the kernel log and fail the load with -EAGAIN");
static bool domains = true;
module_param(domains, bool, 0444);
MODULE_PARM_DESC(domains, "Also read lpm_init/save targets in subsystem power domains when on (default 1)");
static bool g3d;
module_param(g3d, bool, 0444);
MODULE_PARM_DESC(g3d, "Also read G3D-gated targets (G3D toggles under GPU runtime PM; default 0)");
static bool pmic = true;
module_param(pmic, bool, 0444);
MODULE_PARM_DESC(pmic, "Read S2MPG12/13 regulator CTRL and PCTRLSEL registers over ACPM (default 1)");

#define REPORT_SIZE	(512 * 1024)

static struct seq_buf report;
static char *report_buf;
static struct dentry *dir;

static u32 lpm_val[PS_LPM_INIT_N], save_val[PS_SAVE_N];
static u8 lpm_st[PS_LPM_INIT_N], save_st[PS_SAVE_N];

static const char * const type_name[] = {
	[PS_READ] = "READ", [PS_WRITE] = "WRITE", [PS_COND_READ] = "COND_READ",
	[PS_COND_WRITE] = "COND_WRITE", [PS_SAVE_RESTORE] = "SAVE_RESTORE",
	[PS_COND_SAVE_RESTORE] = "COND_SAVE_RESTORE",
	[PS_SET_BIT_ATOMIC] = "SET_BIT_ATOMIC", [PS_CLEAR_PEND] = "CLEAR_PEND",
};

/* PMU_ALIVE registers, offsets from the stock sources cited. */
static const struct {
	u16 off;
	const char *name, *cite;
} pmu_regs[] = {
	{ 0x3944, "WAKEUP_INT_EN", "flexpmu_cal_system_gs201.h:697; gs201.dtsi:403" },
	{ 0x3964, "WAKEUP2_INT_EN", "flexpmu_cal_system_gs201.h:698; gs201.dtsi:403" },
	{ 0x3950, "WAKEUP_STAT", "gs201.dtsi:402" },
	{ 0x3970, "WAKEUP2_STAT", "gs201.dtsi:402" },
	{ 0x3a80, "EINT_WAKEUP_MASK", "gs201.dtsi:406" },
	{ 0x3a84, "EINT_WAKEUP_MASK2", "gs201.dtsi:406" },
	{ 0x3a88, "EINT_WAKEUP_MASK3", "gs201.dtsi:406" },
	{ 0x3a10, "SYSTEM_CTRL", "flexpmu_cal_system_gs201.h:743" },
	{ 0x3920, "TOP_OUT", "flexpmu_cal_system_gs201.h:727-732" },
	{ 0x1044, "CLUSTER0_CPU0_INT_EN", "flexpmu_cal_system_gs201.h:143" },
	{ 0x0860, "CPU_INFORM0", "flexpmu_cal_cpu_gs201.h:7" },
	{ 0x0864, "CPU_INFORM1", "flexpmu_cal_cpu_gs201.h:8" },
	{ 0x0868, "CPU_INFORM2", "flexpmu_cal_cpu_gs201.h:9" },
	{ 0x086c, "CPU_INFORM3", "flexpmu_cal_cpu_gs201.h:10" },
	{ 0x0870, "CPU_INFORM4", "flexpmu_cal_cpu_gs201.h:11" },
	{ 0x0874, "CPU_INFORM5", "flexpmu_cal_cpu_gs201.h:12" },
	{ 0x0878, "CPU_INFORM6", "flexpmu_cal_cpu_gs201.h:13" },
	{ 0x087c, "CPU_INFORM7", "flexpmu_cal_cpu_gs201.h:14" },
	{ 0x3cb0, "EXT_REGULATOR_MIF_DURATION", "flexpmu_cal_system_gs201.h:101" },
	{ 0x3cb4, "EXT_REGULATOR_TOP_DURATION", "flexpmu_cal_system_gs201.h:102" },
	{ 0x3cb8, "EXT_REGULATOR_CPUCL2_DURATION", "flexpmu_cal_system_gs201.h:103" },
	{ 0x3cbc, "EXT_REGULATOR_CPUCL1_DURATION", "flexpmu_cal_system_gs201.h:104" },
	{ 0x3cc0, "EXT_REGULATOR_G3D_DURATION", "flexpmu_cal_system_gs201.h:105" },
	{ 0x3cc4, "EXT_REGULATOR_TPU_DURATION", "flexpmu_cal_system_gs201.h:106" },
	{ 0x3cc8, "TCXO_DURATION", "flexpmu_cal_system_gs201.h:107" },
	{ 0x3ec0, "PCIE_PHY_CONTROL_HSI1", "gs201-pcie.dtsi:54 pmu-offset; pcie-exynos-rc.c:393-407" },
	{ 0x3ec4, "PCIE_PHY_CONTROL_HSI2", "gs201-pcie.dtsi:114 pmu-offset" },
};

/* PMU_INTR_GEN, the offsets used by the stock CPU and system lists. */
static const struct {
	u16 off;
	const char *name, *cite;
} intr_regs[] = {
	{ 0x0108, "GRP1_INTR_BID_UPEND", "flexpmu_cal_system_gs201.h:142" },
	{ 0x0200, "GRP2_INTR_BID_ENABLE", "flexpmu_cal_system_gs201.h:141" },
	{ 0x0208, "GRP2_INTR_BID_UPEND", "flexpmu_cal_system_gs201.h:702" },
	{ 0x0408, "GRP4_INTR_BID_UPEND", "flexpmu_cal_system_gs201.h:744" },
	{ 0x1b08, "GRP27_INTR_BID_UPEND", "flexpmu_cal_system_gs201.h:699" },
	{ 0x1f08, "GRP31_INTR_BID_UPEND", "flexpmu_cal_system_gs201.h:700" },
};

/* S2MPU CTRL0 is offset 0 (s2mpu-regs.h:3); bases gs201-s2mpu.dtsi:146-157.
 * Reading any other S2MPU register (VERSION) has reset the SoC before.
 */
static const struct {
	u32 pa;
	u16 pd_status;
	const char *name;
} s2mpu[] = {
	{ 0x11880000, 0x2104, "S2MPU_HSI1" },
	{ 0x145e0000, 0x2184, "S2MPU_HSI2" },
};
static void __iomem *s2mpu_va[ARRAY_SIZE(s2mpu)];
static u32 s2mpu_val[ARRAY_SIZE(s2mpu)];
static u8 s2mpu_st[ARRAY_SIZE(s2mpu)];

/* MCT in MISC: gs201.dtsi:142-144; G_TCON at 0x240 (mainline exynos_mct.c:31). */
#define MCT_PA		0x10050000
#define MCT_G_TCON	0x240
static void __iomem *mct_va;
static u32 mct_tcon;

/* PMIC registers, PM bank (type 1) over ACPM channel 2, chan 0 = S2MPG12,
 * chan 1 = S2MPG13 (pixel-aoc-power.c). Offsets: s2mpg12-register.h:67-118,
 * 151-164, 204-210; s2mpg13-register.h:55-113, 127-137, 161-163. Regulator
 * CTRL/enable registers and PCTRLSEL only: never INT*, STATUS or *SRC
 * (read-to-clear), never L14S_CTRL (0x39 on S2MPG13, the eSE/eSIM rail).
 */
struct pmic_reg {
	u8 reg;
	const char *name;
};

static const struct pmic_reg pmic_m[] = {
	{ 0x17, "B1M_CTRL" }, { 0x19, "B2M_CTRL" }, { 0x1b, "B3M_CTRL" },
	{ 0x1d, "B4M_CTRL" }, { 0x1f, "B5M_CTRL" }, { 0x21, "B6M_CTRL" },
	{ 0x23, "B7M_CTRL" }, { 0x25, "B8M_CTRL" }, { 0x27, "B9M_CTRL" },
	{ 0x29, "B10M_CTRL" },
	{ 0x2b, "L1M_CTRL" }, { 0x2c, "L2M_CTRL" }, { 0x2d, "L3M_CTRL" },
	{ 0x2e, "L3M_CTRL2" }, { 0x2f, "L4M_CTRL" }, { 0x30, "L5M_CTRL" },
	{ 0x31, "L6M_CTRL" }, { 0x32, "L7M_CTRL" }, { 0x33, "L8M_CTRL" },
	{ 0x34, "L9M_CTRL" }, { 0x35, "L10M_CTRL" }, { 0x36, "L11M_CTRL1" },
	{ 0x37, "L12M_CTRL1" }, { 0x38, "L13M_CTRL1" }, { 0x39, "L14M_CTRL" },
	{ 0x3a, "L15M_CTRL1" }, { 0x3b, "L16M_CTRL" }, { 0x3c, "L17M_CTRL" },
	{ 0x3d, "L18M_CTRL" }, { 0x3e, "L19M_CTRL" }, { 0x3f, "L20M_CTRL" },
	{ 0x40, "L21M_CTRL" }, { 0x41, "L22M_CTRL" }, { 0x42, "L23M_CTRL" },
	{ 0x43, "L24M_CTRL" }, { 0x44, "L25M_CTRL" }, { 0x45, "L26M_CTRL" },
	{ 0x46, "L27M_CTRL" }, { 0x47, "L28M_CTRL" },
	{ 0x48, "LDO_CTRL1" }, { 0x49, "LDO_CTRL2" }, { 0x4a, "LDO_CTRL3" },
	{ 0x9b, "PCTRLSEL1" }, { 0x9c, "PCTRLSEL2" }, { 0x9d, "PCTRLSEL3" },
	{ 0x9e, "PCTRLSEL4" }, { 0x9f, "PCTRLSEL5" }, { 0xa0, "PCTRLSEL6" },
	{ 0xa1, "PCTRLSEL7" }, { 0xa2, "PCTRLSEL8" }, { 0xa3, "PCTRLSEL9" },
	{ 0xa4, "PCTRLSEL10" }, { 0xa5, "PCTRLSEL11" }, { 0xa6, "PCTRLSEL12" },
	{ 0xa7, "PCTRLSEL13" }, { 0xa8, "PCTRLSEL14" },
	{ 0xd3, "L11M_CTRL2" }, { 0xd4, "L12M_CTRL2" }, { 0xd5, "L13M_CTRL2" },
	{ 0xd6, "L15M_CTRL2" }, { 0xd7, "L17M_CTRL2" }, { 0xd8, "L19M_CTRL2" },
	{ 0xd9, "L22M_CTRL2" },
};

static const struct pmic_reg pmic_s[] = {
	{ 0x0f, "B1S_CTRL" }, { 0x11, "B2S_CTRL" }, { 0x13, "B3S_CTRL" },
	{ 0x15, "B4S_CTRL" }, { 0x17, "B5S_CTRL" }, { 0x19, "B6S_CTRL" },
	{ 0x1b, "B7S_CTRL" }, { 0x1d, "B8S_CTRL" }, { 0x1f, "B9S_CTRL" },
	{ 0x22, "B10S_CTRL" }, { 0x24, "BUCKD_CTRL" }, { 0x26, "BUCKA_CTRL" },
	{ 0x28, "BUCKC_CTRL" }, { 0x2a, "BB_CTRL" },
	{ 0x2c, "L1S_CTRL" }, { 0x2d, "L2S_CTRL" }, { 0x2e, "L3S_CTRL" },
	{ 0x2f, "L4S_CTRL" }, { 0x30, "L5S_CTRL" }, { 0x31, "L6S_CTRL" },
	{ 0x32, "L7S_CTRL" }, { 0x33, "L8S_CTRL" }, { 0x34, "L9S_CTRL" },
	{ 0x35, "L10S_CTRL" }, { 0x36, "L11S_CTRL" }, { 0x37, "L12S_CTRL" },
	{ 0x38, "L13S_CTRL" }, /* 0x39 L14S_CTRL deliberately absent */
	{ 0x3a, "L15S_CTRL" }, { 0x3b, "L16S_CTRL" }, { 0x3c, "L17S_CTRL" },
	{ 0x3d, "L18S_CTRL" }, { 0x3e, "L19S_CTRL" }, { 0x3f, "L20S_CTRL" },
	{ 0x40, "L21S_CTRL" }, { 0x41, "L22S_CTRL" }, { 0x42, "L23S_CTRL" },
	{ 0x43, "L24S_CTRL" }, { 0x44, "L25S_CTRL" }, { 0x45, "L26S_CTRL" },
	{ 0x46, "L27S_CTRL" }, { 0x47, "L28S_CTRL" },
	{ 0x48, "LDO_CTRL1" }, { 0x49, "LDO_CTRL2" },
	{ 0x97, "PCTRLSEL1" }, { 0x98, "PCTRLSEL2" }, { 0x99, "PCTRLSEL3" },
	{ 0x9a, "PCTRLSEL4" }, { 0x9b, "PCTRLSEL5" }, { 0x9c, "PCTRLSEL6" },
	{ 0x9d, "PCTRLSEL7" }, { 0x9e, "PCTRLSEL8" }, { 0x9f, "PCTRLSEL9" },
	{ 0xa0, "PCTRLSEL10" }, { 0xa1, "PCTRLSEL11" },
	{ 0xbf, "L1S_CTRL2" }, { 0xc0, "L23S_CTRL2" }, { 0xc1, "L26S_CTRL2" },
};

#define ACPM_PMIC_CHANNEL	2
#define PMIC_PM_BANK		0x01

/* Blocks the parameters exclude, before any register access. */
static bool excluded(u8 blk)
{
	const struct ps_block *b = &ps_blocks[blk];

	return !domains && b->pd_status && b->cluster < 0;
}

static void read_entry(const struct ps_seq *e, bool lpm, u32 *val, u8 *st)
{
	unsigned long flags;
	int ret;

	if (excluded(e->blk)) {
		*st = PS_SKIP_EXCLUDED;
		return;
	}
	/* Gate and read back to back with interrupts off on this CPU. */
	local_irq_save(flags);
	if (lpm) {
		ret = ps_block_gate(e->blk);
		if (ret == PS_OK)
			*val = readl(ps_reg(e));
	} else {
		ret = ps_save_read(e, val);
	}
	local_irq_restore(flags);
	*st = ret;
}

/* CPUCL1/2 targets: runs on a CPU of that cluster (IPI, interrupts off). */
static void collect_cluster(void *arg)
{
	int cl = (long)arg;
	unsigned int i;

	for (i = 0; i < PS_LPM_INIT_N; i++)
		if (ps_blocks[ps_lpm_init[i].blk].cluster == cl)
			read_entry(&ps_lpm_init[i], true, &lpm_val[i], &lpm_st[i]);
	for (i = 0; i < PS_SAVE_N; i++)
		if (ps_blocks[ps_save[i].blk].cluster == cl)
			read_entry(&ps_save[i], false, &save_val[i], &save_st[i]);
}

/* Everything else: pinned to CPU 0 (cluster 0 is then necessarily on). */
static long collect_main(void *arg)
{
	unsigned int i;
	unsigned long flags;

	for (i = 0; i < PS_LPM_INIT_N; i++)
		if (ps_blocks[ps_lpm_init[i].blk].cluster <= 0)
			read_entry(&ps_lpm_init[i], true, &lpm_val[i], &lpm_st[i]);
	for (i = 0; i < PS_SAVE_N; i++)
		if (ps_blocks[ps_save[i].blk].cluster <= 0)
			read_entry(&ps_save[i], false, &save_val[i], &save_st[i]);
	for (i = 0; i < ARRAY_SIZE(s2mpu); i++) {
		local_irq_save(flags);
		if (ps_pmu_read(s2mpu[i].pd_status) & BIT(0)) {
			s2mpu_val[i] = readl(s2mpu_va[i]);
			s2mpu_st[i] = PS_OK;
		} else {
			s2mpu_st[i] = PS_SKIP_PD;
		}
		local_irq_restore(flags);
	}
	mct_tcon = readl(mct_va + MCT_G_TCON);
	return 0;
}

static void collect_clusters(void)
{
	static const u8 first[3] = { 0, 4, 6 };
	unsigned int i, cl, cpu;

	/* Entries of an empty cluster stay "cluster" skips. */
	for (i = 0; i < PS_LPM_INIT_N; i++)
		if (ps_blocks[ps_lpm_init[i].blk].cluster > 0)
			lpm_st[i] = PS_SKIP_CLUSTER;
	for (i = 0; i < PS_SAVE_N; i++)
		if (ps_blocks[ps_save[i].blk].cluster > 0)
			save_st[i] = PS_SKIP_CLUSTER;
	cpus_read_lock();
	for (cl = 1; cl < 3; cl++) {
		for (cpu = first[cl]; cpu < first[cl] + 2; cpu++) {
			if (cpu_online(cpu)) {
				smp_call_function_single(cpu, collect_cluster, (void *)(long)cl, 1);
				break;
			}
		}
	}
	cpus_read_unlock();
}

static void report_pmic(struct device *dev)
{
	struct acpm_handle *acpm;
	struct device_node *np;
	unsigned int i;
	u8 val;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "google,gs201-acpm-ipc");
	acpm = np ? devm_acpm_get_by_node(dev, np) : ERR_PTR(-ENODEV);
	of_node_put(np);
	if (IS_ERR(acpm)) {
		seq_buf_printf(&report, "pmic unavailable %ld\n", PTR_ERR(acpm));
		return;
	}
	for (i = 0; i < ARRAY_SIZE(pmic_m); i++) {
		ret = acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHANNEL, PMIC_PM_BANK,
					       pmic_m[i].reg, 0, &val);
		if (ret)
			seq_buf_printf(&report, "pmic m %#04x %s err:%d\n", pmic_m[i].reg,
				       pmic_m[i].name, ret);
		else
			seq_buf_printf(&report, "pmic m %#04x %s %#04x\n", pmic_m[i].reg,
				       pmic_m[i].name, val);
	}
	for (i = 0; i < ARRAY_SIZE(pmic_s); i++) {
		ret = acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHANNEL, PMIC_PM_BANK,
					       pmic_s[i].reg, 1, &val);
		if (ret)
			seq_buf_printf(&report, "pmic s %#04x %s err:%d\n", pmic_s[i].reg,
				       pmic_s[i].name, ret);
		else
			seq_buf_printf(&report, "pmic s %#04x %s %#04x\n", pmic_s[i].reg,
				       pmic_s[i].name, val);
	}
}

static void report_flexpmu(void)
{
	struct ps_flexpmu f;
	struct ps_counts c;
	int ret;

	ret = ps_flexpmu_open(&f);
	if (!ret)
		ret = ps_flexpmu_read(&f, &c);
	ps_flexpmu_close(&f);
	if (ret) {
		seq_buf_printf(&report, "flexpmu unavailable %d\n", ret);
		return;
	}
	seq_buf_printf(&report,
		       "flexpmu soc_early %u soc_down %u mif_down %u sleep_early %u sleep_soc_down %u sleep_mif_down %u sicd_early %u sicd_soc_down %u sicd_mif_down %u mif_always_on %u sw_flags %08x %08x\n",
		       c.soc_early, c.soc_down, c.mif_down, c.sleep_early,
		       c.sleep_soc_down, c.sleep_mif_down, c.sicd_early,
		       c.sicd_soc_down, c.sicd_mif_down, c.mif_always_on,
		       c.sw_flag0, c.sw_flag1);
}

static void report_lists(unsigned int *diff, unsigned int *skipped)
{
	unsigned int i;

	for (i = 0; i < PS_LPM_INIT_N; i++) {
		const struct ps_seq *e = &ps_lpm_init[i];

		if (lpm_st[i] != PS_OK) {
			seq_buf_printf(&report, "lpm %u %u %#010x %s %#010x %#010x - skip:%s\n",
				       i, e->line, ps_seq_pa(e), e->name, e->mask, e->value,
				       ps_skip_name[lpm_st[i]]);
			(*skipped)++;
			continue;
		}
		if ((lpm_val[i] & e->mask) != (e->value & e->mask))
			(*diff)++;
		seq_buf_printf(&report, "lpm %u %u %#010x %s %#010x %#010x %#010x %s\n",
			       i, e->line, ps_seq_pa(e), e->name, e->mask, e->value, lpm_val[i],
			       (lpm_val[i] & e->mask) == (e->value & e->mask) ? "ok" : "DIFF");
	}
	for (i = 0; i < PS_SAVE_N; i++) {
		const struct ps_seq *e = &ps_save[i];

		if (save_st[i] != PS_OK)
			seq_buf_printf(&report, "save %u %u %s %#010x %s - skip:%s\n", i,
				       e->line, type_name[e->type], ps_seq_pa(e), e->name,
				       ps_skip_name[save_st[i]]);
		else
			seq_buf_printf(&report, "save %u %u %s %#010x %s %#010x ok\n", i,
				       e->line, type_name[e->type], ps_seq_pa(e), e->name,
				       save_val[i]);
	}
}

static int build_report(struct device *dev)
{
	unsigned int i, lpm_diff = 0, lpm_skip = 0, save_skip = 0;
	long ret;

	seq_buf_printf(&report, "# pixel-sleep-audit 1 stock %s cpus %*pbl domains %d g3d %d\n",
		       PS_STOCK_COMMIT, cpumask_pr_args(cpu_online_mask), domains, g3d);
	for (i = 0; i < ARRAY_SIZE(pmu_regs); i++)
		seq_buf_printf(&report, "pmu %#06x %s %#010x %s\n", pmu_regs[i].off,
			       pmu_regs[i].name, ps_pmu_read(pmu_regs[i].off), pmu_regs[i].cite);
	for (i = 0; i < ARRAY_SIZE(ps_pd); i++) {
		u32 v = ps_pmu_read(ps_pd[i].offset);

		seq_buf_printf(&report, "pd %#06x %s %s %#010x %s:%u\n", ps_pd[i].offset,
			       ps_pd[i].name, v & BIT(0) ? "on" : "off", v, ps_pd[i].file,
			       ps_pd[i].line);
	}
	for (i = 0; i < ARRAY_SIZE(intr_regs); i++)
		seq_buf_printf(&report, "intr %#06x %s %#010x %s\n", intr_regs[i].off,
			       intr_regs[i].name, ps_intr_read(intr_regs[i].off), intr_regs[i].cite);

	ret = work_on_cpu(0, collect_main, NULL);
	if (ret)
		return ret;
	collect_clusters();
	report_lists(&lpm_diff, &lpm_skip);
	for (i = 0; i < PS_SAVE_N; i++)
		if (save_st[i] != PS_OK)
			save_skip++;
	for (i = 0; i < ARRAY_SIZE(s2mpu); i++) {
		if (s2mpu_st[i] == PS_OK)
			seq_buf_printf(&report, "s2mpu %s %#010x CTRL0 %#010x ok\n", s2mpu[i].name,
				       s2mpu[i].pa, s2mpu_val[i]);
		else
			seq_buf_printf(&report, "s2mpu %s %#010x CTRL0 - skip:pd-off\n",
				       s2mpu[i].name, s2mpu[i].pa);
	}
	seq_buf_printf(&report, "mct G_TCON %#010x %#010x\n", MCT_PA + MCT_G_TCON, mct_tcon);
	if (pmic)
		report_pmic(dev);
	report_flexpmu();
	seq_buf_printf(&report, "summary lpm %u diff %u skipped %u save %u skipped %u overflow %d\n",
		       PS_LPM_INIT_N, lpm_diff, lpm_skip, PS_SAVE_N, save_skip,
		       seq_buf_has_overflowed(&report));
	pr_info("pixel-sleep-audit: lpm_init %u entries, %u differ from stock, %u skipped; save_sleep %u entries, %u skipped\n",
		PS_LPM_INIT_N, lpm_diff, lpm_skip, PS_SAVE_N, save_skip);
	return 0;
}

static ssize_t report_read(struct file *file, char __user *buf, size_t len, loff_t *pos)
{
	return simple_read_from_buffer(buf, len, pos, report_buf, seq_buf_used(&report));
}

static const struct file_operations report_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = report_read,
	.llseek = default_llseek,
};

static void print_report(void)
{
	const char *p = report_buf, *end = report_buf + seq_buf_used(&report), *nl;

	while (p < end) {
		nl = memchr(p, '\n', end - p);
		if (!nl)
			nl = end;
		pr_info("pixel-sleep-audit: %.*s\n", (int)(nl - p), p);
		p = nl + 1;
	}
}

static void unmap_all(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(s2mpu); i++)
		if (s2mpu_va[i])
			iounmap(s2mpu_va[i]);
	if (mct_va)
		iounmap(mct_va);
	ps_unmap_blocks();
}

static int __init audit_init(void)
{
	struct device *dev;
	unsigned int i;
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	if (!g3d)
		ps_gate_flags |= PS_GATE_NO_G3D;
	ps_gate_flags |= PS_GATE_CPUCL_LOCAL;
	ret = ps_map_blocks();
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(s2mpu); i++)
		s2mpu_va[i] = ioremap(s2mpu[i].pa, 0x1000);
	mct_va = ioremap(MCT_PA, 0x1000);
	report_buf = vmalloc(REPORT_SIZE);
	if (!report_buf || !mct_va || !s2mpu_va[0] || !s2mpu_va[1]) {
		ret = -ENOMEM;
		goto out;
	}
	seq_buf_init(&report, report_buf, REPORT_SIZE);
	dev = root_device_register("pixel-sleep-audit");
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto out;
	}
	ret = build_report(dev);
	root_device_unregister(dev);
	if (ret)
		goto out;
	if (oneshot) {
		print_report();
		ret = -EAGAIN;
		goto out;
	}
	dir = debugfs_create_dir("pixel-sleep-audit", NULL);
	debugfs_create_file("report", 0400, dir, NULL, &report_fops);
	unmap_all();
	return 0;
out:
	vfree(report_buf);
	report_buf = NULL;
	unmap_all();
	return ret;
}
module_init(audit_init);

static void __exit audit_exit(void)
{
	debugfs_remove_recursive(dir);
	vfree(report_buf);
}
module_exit(audit_exit);

MODULE_DESCRIPTION("GS201 SYS_SLEEP read-only register audit");
MODULE_LICENSE("GPL");
