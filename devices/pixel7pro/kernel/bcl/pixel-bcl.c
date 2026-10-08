// SPDX-License-Identifier: GPL-2.0
/*
 * Pixel 7 Pro (GS201) brownout protection: the PMIC's warnings and the CPU
 * clusters' hardware clock dividers, as stock sets them.
 *
 * Stock throttles the CPUs in hardware when the main PMIC warns of a buck
 * over-current (OCP_WARN) or a low system voltage (SMPL_WARN): the warnings
 * drive each CPU cluster's CMU clock divider (CLKDIVSTEP), with no code
 * running. Here the bootloader leaves every OCP warning off and the MID and
 * BIG dividers without their SMPL filter and light ratio, so nothing throttles
 * them. On 2026-10-05 an all-core load drew 3.9 A from the battery and the
 * phone reset with "UVLO Reboot(PMIC MAIN)".
 *
 * At load the module prints both sides beside the stock values: the S2MPG12/13
 * warning registers over ACPM (s2mpg12/13-register.h), the CMU_CPUCL0/1/2
 * CLKDIVSTEP registers (pmucal_lpm_init and the save_sleep list in
 * flexpmu_cal_system_gs201.h, read on a CPU of the cluster so it is powered),
 * and the PMIC power-on/off sources (cleared by the read), which name the
 * last reset on the first load after it.
 * Without apply=1 the load then fails with -EAGAIN.
 *
 * apply=1 sets stock's final values (the kernel's at boot, then those
 * init.gs201.rc writes once the brownout HAL is up) and stays loaded:
 * - S2MPG12: B2M (BIG) and B3M (MID) OCP warnings on at 12 A and 8 A, and the
 *   SMPL warning at 3.1 V;
 * - each cluster's CLKDIVSTEP registers, written on a CPU of that cluster as
 *   it comes online, so CPUs 4-7, offline while the screen is dark, get them
 *   when they return.
 * Unloading puts back the values found. Not changed: the BUCK_OCP_CTRL and
 * S2MPG13 OCP_CTRL registers (stock 0xbb, here 0x00; their fields are not
 * documented) and the G3D/TPU warnings, whose dividers nothing configures.
 * Research: .work/research/reports/Pixel 7 Pro brownout throttling (BCL).md
 */
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/smp.h>

#define ACPM_PMIC_CHANNEL	2
#define PM_BANK			0x01

static bool apply;
module_param(apply, bool, 0444);
MODULE_PARM_DESC(apply, "Set stock's warnings and dividers and stay loaded (default: report only)");

struct pmic_reg {
	const char *name;
	u8 pmic, reg;
	u8 stock;	/* stock's final value */
	bool set;	/* apply=1 writes it */
};

static const struct pmic_reg pmic_regs[] = {
	/* The driver writes 0x68 (2.9 V); the rc's smpl_lvl 3100 then makes it 0xa8. */
	{ "S2MPG12 SMPL_WARN_CTRL", 0, 0x16, 0xa8, true },
	{ "S2MPG12 BUCK_OCP_CTRL1", 0, 0xb6, 0xbb },
	{ "S2MPG12 BUCK_OCP_CTRL2", 0, 0xb7, 0xbb },
	{ "S2MPG12 BUCK_OCP_CTRL3", 0, 0xb8, 0xbb },
	{ "S2MPG12 BUCK_OCP_CTRL4", 0, 0xb9, 0xbb },
	{ "S2MPG12 BUCK_OCP_CTRL5", 0, 0xba, 0xbb },
	{ "S2MPG12 B2M_OCP_WARN (BIG)", 0, 0xc3, 0x80, true },	/* EN, level 0 = 12 A */
	{ "S2MPG12 B3M_OCP_WARN (MID)", 0, 0xc4, 0x80, true },	/* EN, level 0 = 8 A */
	{ "S2MPG12 B10M_OCP_WARN (TPU)", 0, 0xc5, 0x80 },
	{ "S2MPG12 B2M_SOFT_OCP_WARN", 0, 0xc6, 0x00 },	/* driver 0x14, rc 0x00 */
	{ "S2MPG12 B3M_SOFT_OCP_WARN", 0, 0xc7, 0x00 },
	{ "S2MPG12 B10M_SOFT_OCP_WARN", 0, 0xc8, 0x00 },
	{ "S2MPG13 OCP_CTRL1", 1, 0xab, 0xbb },
	{ "S2MPG13 OCP_CTRL2", 1, 0xac, 0xbb },
	{ "S2MPG13 OCP_CTRL3", 1, 0xad, 0xbb },
	{ "S2MPG13 OCP_CTRL4", 1, 0xae, 0xbb },
	{ "S2MPG13 OCP_CTRL5", 1, 0xaf, 0xbb },
	{ "S2MPG13 OCP_CTRL6", 1, 0xb0, 0xbb },
	{ "S2MPG13 OCP_CTRL7", 1, 0xb1, 0x0b },
	{ "S2MPG13 B2S_OCP_WARN (G3D)", 1, 0xb8, 0x80 },
	{ "S2MPG13 B2S_SOFT_OCP_WARN", 1, 0xb9, 0x00 },
};

/* Power-on and power-off sources: google_bcl reads them at boot. Reading
 * clears them, so only the first load after a reset sees the reset's.
 */
static const struct pmic_reg src_regs[] = {
	{ "S2MPG12 PWRONSRC", 0, 0x0c },
	{ "S2MPG12 OFFSRC1", 0, 0x0d },
	{ "S2MPG12 OFFSRC2", 0, 0x0e },
	{ "S2MPG13 OFFSRC", 1, 0x0a },
};

struct cmu_reg {
	const char *name;
	u16 offset;
	bool set;	/* stock writes it; apply=1 does too */
	u32 stock;
};

#define CMU_REGS	6

struct cluster {
	const char *name;
	unsigned int first_cpu, last_cpu;
	phys_addr_t base;
	struct cmu_reg regs[CMU_REGS];	/* written in this order, CLKDIVSTEP last */
	void __iomem *cmu;
	u32 val[CMU_REGS];		/* read at load (or first online) */
	bool found, changed, logged;
};

/* CPUCL0 only has the SMPL input; +0x838 is its CON register. Its CLKDIVSTEP
 * write sets bit 0 alone (stock masks the write).
 */
static struct cluster clusters[] = {
	{ "CPUCL0 (LITTLE)", 0, 3, 0x20c00000, {
		{ "CLKDIVSTEP_SMPL_FLT", 0x834, true, 0x8000ffff },
		{ "CLKDIVSTEP_CON", 0x838, true, 0x00f041c3 },
		{ "CLKDIVSTEP", 0x830, true, 0x1 },
	} },
	{ "CPUCL1 (MID)", 4, 5, 0x20c10000, {
		{ "CLKDIVSTEP_OCP_FLT", 0x834 },
		{ "CLKDIVSTEP_VDROOP_FLT", 0x838 },
		{ "CLKDIVSTEP_SMPL_FLT", 0x83c, true, 0x8000ffff },
		{ "CLKDIVSTEP_CON_HEAVY", 0x840, true, 0xfff041c0 },	/* rc cpu1_heavy */
		{ "CLKDIVSTEP_CON_LIGHT", 0x844, true, 0x00f041c3 },
		{ "CLKDIVSTEP", 0x830, true, 0x381 },			/* rc cpu1_clk_div */
	} },
	{ "CPUCL2 (BIG)", 6, 7, 0x20c20000, {
		{ "CLKDIVSTEP_OCP_FLT", 0x834 },
		{ "CLKDIVSTEP_VDROOP_FLT", 0x838 },
		{ "CLKDIVSTEP_SMPL_FLT", 0x83c, true, 0x8000ffff },
		{ "CLKDIVSTEP_CON_HEAVY", 0x840, true, 0xfff041c0 },	/* rc cpu2_heavy */
		{ "CLKDIVSTEP_CON_LIGHT", 0x844, true, 0xfff041c3 },	/* rc cpu2_light */
		{ "CLKDIVSTEP", 0x830, true, 0x1 },			/* rc cpu2_clk_div */
	} },
};

static struct device *dev;
static struct acpm_handle *acpm;
static u8 pmic_found[ARRAY_SIZE(pmic_regs)];
static bool pmic_changed[ARRAY_SIZE(pmic_regs)];
static enum cpuhp_state hp_state;

static struct cluster *cluster_of(unsigned int cpu)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(clusters); i++)
		if (cpu >= clusters[i].first_cpu && cpu <= clusters[i].last_cpu)
			return &clusters[i];
	return NULL;
}

static unsigned int nregs(const struct cluster *c)
{
	unsigned int n = 0;

	while (n < CMU_REGS && c->regs[n].name)
		n++;
	return n;
}

static void cluster_read(void *data)
{
	struct cluster *c = data;
	unsigned int i;

	for (i = 0; i < nregs(c); i++)
		c->val[i] = readl(c->cmu + c->regs[i].offset);
	c->found = true;
}

static u32 cluster_want(const struct cluster *c, unsigned int i, u32 now)
{
	const struct cmu_reg *r = &c->regs[i];

	if (c->first_cpu == 0 && r->offset == 0x830)
		return now | 0x1;
	return r->stock;
}

/* Runs on a CPU of the cluster, so the cluster and its CMU are powered. */
static void cluster_write(void *data)
{
	struct cluster *c = data;
	unsigned int i;

	for (i = 0; i < nregs(c); i++) {
		u32 now = readl(c->cmu + c->regs[i].offset);
		u32 want = cluster_want(c, i, now);

		if (c->regs[i].set && now != want) {
			writel(want, c->cmu + c->regs[i].offset);
			c->changed = true;
		}
	}
}

static void cluster_restore(void *data)
{
	struct cluster *c = data;
	int i;

	/* CLKDIVSTEP (the enable) first, the configuration after. */
	for (i = nregs(c) - 1; i >= 0; i--)
		if (c->regs[i].set)
			writel(c->val[i], c->cmu + c->regs[i].offset);
}

/* cpuhp startup callbacks run on the CPU coming online. */
static int bcl_cpu_online(unsigned int cpu)
{
	struct cluster *c = cluster_of(cpu);

	if (!c)
		return 0;
	if (!c->found) {
		cluster_read(c);
		pr_info("pixel-bcl: %s found CLKDIVSTEP %08x\n", c->name,
			c->val[nregs(c) - 1]);
	}
	cluster_write(c);
	if (c->changed && !c->logged) {
		unsigned int i;

		for (i = 0; i < nregs(c); i++)
			pr_info("pixel-bcl: %s %-22s %08x -> %08x\n", c->name, c->regs[i].name,
				c->val[i], readl(c->cmu + c->regs[i].offset));
		c->logged = true;
	}
	return 0;
}

static void report_cluster(struct cluster *c)
{
	unsigned int j;

	for (j = 0; j < nregs(c); j++) {
		const struct cmu_reg *r = &c->regs[j];
		u32 want = cluster_want(c, j, c->val[j]);

		if (r->set)
			pr_info("pixel-bcl: %s %-22s +%#05x = %08x, stock %08x%s\n", c->name,
				r->name, r->offset, c->val[j], want,
				c->val[j] == want ? "" : "  DIFF");
		else
			pr_info("pixel-bcl: %s %-22s +%#05x = %08x\n", c->name, r->name,
				r->offset, c->val[j]);
	}
}

static int pmic_read(const struct pmic_reg *r, u8 *val)
{
	int ret = acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHANNEL, PM_BANK, r->reg, r->pmic, val);

	if (ret)
		pr_err("pixel-bcl: %s read failed: %d\n", r->name, ret);
	return ret;
}

static int pmic_write(const struct pmic_reg *r, u8 want)
{
	u8 val;
	int ret = acpm->ops->pmic.write_reg(acpm, ACPM_PMIC_CHANNEL, PM_BANK, r->reg, r->pmic, want);

	if (!ret)
		ret = pmic_read(r, &val);
	if (!ret && val != want)
		ret = -EIO;
	pr_info("pixel-bcl: %s -> %#04x%s\n", r->name, want, ret ? " FAILED" : "");
	return ret;
}

static void pmic_restore(void)
{
	int i;

	for (i = ARRAY_SIZE(pmic_regs) - 1; i >= 0; i--)
		if (pmic_changed[i])
			pmic_write(&pmic_regs[i], pmic_found[i]);
}

static void cmu_unmap(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(clusters); i++)
		if (clusters[i].cmu)
			iounmap(clusters[i].cmu);
}

static void clusters_restore(void)
{
	unsigned int i, cpu;

	cpus_read_lock();
	for (i = 0; i < ARRAY_SIZE(clusters); i++) {
		struct cluster *c = &clusters[i];

		if (!c->changed)
			continue;
		for (cpu = c->first_cpu; cpu <= c->last_cpu && !cpu_online(cpu); cpu++)
			;
		if (cpu <= c->last_cpu)
			smp_call_function_single(cpu, cluster_restore, c, 1);
		else
			pr_warn("pixel-bcl: %s offline; its dividers keep stock values\n", c->name);
	}
	cpus_read_unlock();
}

static int __init bcl_init(void)
{
	struct device_node *np;
	unsigned int i;
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(clusters); i++) {
		clusters[i].cmu = ioremap(clusters[i].base, SZ_4K);
		if (!clusters[i].cmu) {
			ret = -ENOMEM;
			goto unmap;
		}
	}
	dev = root_device_register("pixel-bcl");
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto unmap;
	}
	np = of_find_node_by_path("/power-management");
	acpm = np ? devm_acpm_get_by_node(dev, np) : ERR_PTR(-ENODEV);
	of_node_put(np);
	if (IS_ERR(acpm)) {
		ret = PTR_ERR(acpm);
		goto out;
	}

	/* Report. A cluster whose CPUs are all offline is read when one returns. */
	cpus_read_lock();
	for (i = 0; i < ARRAY_SIZE(clusters); i++) {
		struct cluster *c = &clusters[i];
		unsigned int cpu;

		for (cpu = c->first_cpu; cpu <= c->last_cpu && !cpu_online(cpu); cpu++)
			;
		if (cpu > c->last_cpu) {
			pr_info("pixel-bcl: %s offline\n", c->name);
			continue;
		}
		smp_call_function_single(cpu, cluster_read, c, 1);
		report_cluster(c);
	}
	cpus_read_unlock();
	for (i = 0; i < ARRAY_SIZE(pmic_regs); i++) {
		const struct pmic_reg *r = &pmic_regs[i];

		ret = pmic_read(r, &pmic_found[i]);
		if (ret)
			goto out;
		pr_info("pixel-bcl: %-28s %#04x = %#04x, stock %#04x%s\n", r->name, r->reg,
			pmic_found[i], r->stock, pmic_found[i] == r->stock ? "" : "  DIFF");
	}
	for (i = 0; i < ARRAY_SIZE(src_regs); i++) {
		u8 val;

		if (!pmic_read(&src_regs[i], &val))
			pr_info("pixel-bcl: %-28s %#04x = %#04x\n", src_regs[i].name,
				src_regs[i].reg, val);
	}
	if (!apply) {
		ret = -EAGAIN;
		goto out;
	}

	/* The dividers first, so a warning has something to act on. */
	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "pixel-bcl:online", bcl_cpu_online, NULL);
	if (ret < 0)
		goto out;
	hp_state = ret;
	for (i = 0; i < ARRAY_SIZE(pmic_regs); i++) {
		const struct pmic_reg *r = &pmic_regs[i];

		if (!r->set || pmic_found[i] == r->stock)
			continue;
		ret = pmic_write(r, r->stock);
		if (ret)
			goto fail;
		pmic_changed[i] = true;
	}
	pr_info("pixel-bcl: applied; PMIC warnings on, dividers set as clusters come online\n");
	return 0;

fail:
	pmic_restore();
	cpuhp_remove_state_nocalls(hp_state);
	clusters_restore();
out:
	root_device_unregister(dev);
unmap:
	cmu_unmap();
	return ret;
}
module_init(bcl_init);

static void __exit bcl_exit(void)
{
	pmic_restore();
	cpuhp_remove_state_nocalls(hp_state);
	clusters_restore();
	root_device_unregister(dev);
	cmu_unmap();
	pr_info("pixel-bcl: restored\n");
}
module_exit(bcl_exit);

MODULE_DESCRIPTION("Pixel 7 Pro brownout protection: PMIC warnings and CMU clock dividers as stock");
MODULE_LICENSE("GPL");
