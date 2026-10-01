// SPDX-License-Identifier: GPL-2.0-only
/* Power off GS201 blocks that Linux does not use.
 *
 * The bootloader leaves almost every power domain on: the whole camera
 * pipeline, the TPU, both video codecs, G2D, EH and AUR. Nothing here drives
 * them, and they leak for as long as they stay powered. Each listed domain is
 * switched off with Google's flexpmu sequence (flexpmu_cal_local_gs201.h
 * "<block>_off"), preceded by the secure context save the vendor exynos-pd
 * driver does for domains with a TZPC ("need_smc" in the stock DT), as
 * gs201-g3d-pd does for the GPU:
 *   1. SMC 0x82000410 (save, TZPC);
 *   2. <block> CMU CONTROLLER_OPTION bit 24 (automatic clock gating) cleared;
 *   3. PMU <block>_CONFIGURATION bit 0 cleared through the secure register SMC;
 *   4. PMU <block>_STATUS bit 0 polled to 0.
 * Children go before their stock-DT parents (DNS before ITP, IPP before PDP).
 *
 * CSIS and PDP, the camera capture path, can come back on:
 * pixel_pd_power(name, on) (pixel-pd.h; pixel-camera calls it around a
 * stream) runs the stock `<block>_on` sequence and then, as exynos-pd and
 * gs201-g3d-pd do, the secure context restore (SMC 0x82000410, restore 1) and
 * the domain's save list (pixel-pd-lists.h, gen-pd-lists.py), which powering
 * it off read first. A domain that went off without its list being read (by
 * an older module) is not powered back on. The CSIS domain also holds the
 * write-DMA's two S2MPUs, which come back from a power cycle enabled and drop
 * every DMA write (frames arrive empty), as HSI1's and HSI2's do in system
 * sleep (kernel/suspend/pixel-sleep.c); their CTRL0, the only S2MPU register
 * read or written (others have reset the SoC), is saved before power-off and
 * written back after power-on, 0 (the bootloader's disabled state) if it was
 * never read. The `power` parameter does the same from userspace ("csis=1"),
 * for tests. The other domains have no on path and come back on the next
 * boot.
 *
 * Measured (image W, screen off, C2 working): TPU 0.08 W, then AUR, BO, MFC,
 * G2D and EH 0.12 W at the USB input; the camera domains made no difference
 * that the USB input could resolve.
 */
#include <linux/arm-smccc.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/string.h>

#include "pixel-pd.h"

struct pd_reg {
	u32 pa;
	const char *name;
};

#include "pixel-pd-lists.h"

#define GS201_PMU		0x18060000
#define GS201_SMC_PRIV_REG	0x82000504
#define GS201_SMC_PD_CONTEXT	0x82000410
#define CMU_CONTROLLER_OPTION	0x800
#define ENABLE_AUTOMATIC_CLKGATING BIT(24)

struct domain {
	const char *name;
	u32 pmu;	/* CONFIGURATION; STATUS is +4 */
	u32 cmu;
	u32 tzpc;
	const struct pd_reg *save;	/* the stock save list, if it can come back on */
	unsigned int nsave;
	u32 *saved;
	bool valid;			/* saved[] holds the list as read before power-off */
	u32 s2mpu[2];			/* S2MPUs in the domain (stock DT), CTRL0 at +0 */
	u32 s2mpu_ctrl0[2];
};

static u32 csis_vals[ARRAY_SIZE(csis_save)], pdp_vals[ARRAY_SIZE(pdp_save)];
#define SAVE(list, vals)	.save = list, .nsave = ARRAY_SIZE(list), .saved = vals

/* Stock DT pd-* nodes: reg, cmu_id, need_smc. Listed children first. */
static struct domain domains[] = {
	{ "tpu",  0x2900, 0x1cc00000, 0x1cc10204 },
	{ "aur",  0x2980, 0x25a00000, 0x25a10204 },
	{ "bo",   0x2880, 0x1ca00000, 0x1ca10204 },
	{ "mfc",  0x2380, 0x1c800000, 0x1c810204 },
	{ "g2d",  0x2300, 0x1c600000, 0x1c610204 },
	{ "eh",   0x1c00, 0x17000000, 0x17010204 },
	{ "dns",  0x2500, 0x1b000000, 0x1b010204 },
	{ "itp",  0x2680, 0x1b400000, 0x1b410204 },
	{ "ipp",  0x2600, 0x1ac00000, 0x1ac10204 },
	{ "pdp",  0x2480, 0x1aa00000, 0x1aa10204, SAVE(pdp_save, pdp_vals) },
	{ "csis", 0x2400, 0x1a400000, 0x1a410204, SAVE(csis_save, csis_vals),
	  .s2mpu = { 0x1a520000, 0x1a550000 } },
	{ "g3aa", 0x2580, 0x1a800000, 0x1a810204 },
	{ "mcsc", 0x2700, 0x1b700000, 0x1b710204 },
	{ "gdc",  0x2780, 0x1d000000, 0x1d010204 },
	{ "tnr",  0x2800, 0x1bc00000, 0x1bc10204 },
};

static char *off = "";
module_param(off, charp, 0400);
MODULE_PARM_DESC(off, "Comma-separated domains to power off (all: every listed one)");

static bool wanted(const char *name)
{
	const char *p = off;
	size_t n = strlen(name);

	if (!strcmp(off, "all"))
		return true;
	while (p && *p) {
		if (!strncmp(p, name, n) && (p[n] == ',' || !p[n]))
			return true;
		p = strchr(p, ',');
		if (p)
			p++;
	}
	return false;
}

static DEFINE_MUTEX(pd_lock);
static void __iomem *pmu_va;

/* One save-list register; the list stays inside the domain's CMU and SYSREG
 * (checked by gen-pd-lists.py), which are only touched while it is on. */
static u32 reg_rw(u32 pa, bool write, u32 val)
{
	void __iomem *r = ioremap(pa, 4);

	if (!r)
		return 0;
	if (write)
		writel(val, r);
	else
		val = readl(r);
	iounmap(r);
	return val;
}

static int domain_off(void __iomem *pmu, struct domain *d)
{
	struct arm_smccc_res res;
	void __iomem *cmu;
	unsigned int i;
	u32 val;
	int ret;

	if (!(readl(pmu + d->pmu + 4) & BIT(0))) {
		pr_info("pixel-pd-off: %s already off\n", d->name);
		return 0;
	}
	for (i = 0; i < d->nsave; i++)
		d->saved[i] = reg_rw(d->save[i].pa, false, 0);
	d->valid = d->nsave;
	for (i = 0; i < ARRAY_SIZE(d->s2mpu); i++)
		if (d->s2mpu[i])
			d->s2mpu_ctrl0[i] = reg_rw(d->s2mpu[i], false, 0);
	arm_smccc_smc(GS201_SMC_PD_CONTEXT, 0, d->tzpc, 2, 0, 0, 0, 0, &res);
	if (res.a0) {
		pr_err("pixel-pd-off: %s: secure context save failed: %#lx\n", d->name, res.a0);
		return -EIO;
	}
	cmu = ioremap(d->cmu + CMU_CONTROLLER_OPTION, 4);
	if (!cmu)
		return -ENOMEM;
	writel(readl(cmu) & ~ENABLE_AUTOMATIC_CLKGATING, cmu);
	iounmap(cmu);
	val = readl(pmu + d->pmu) & ~BIT(0);
	arm_smccc_smc(GS201_SMC_PRIV_REG, GS201_PMU + d->pmu, 1, val, 0, 0, 0, 0, &res);
	if (res.a0) {
		pr_err("pixel-pd-off: %s: PMU write failed: %#lx\n", d->name, res.a0);
		return -EIO;
	}
	ret = readl_poll_timeout(pmu + d->pmu + 4, val, !(val & BIT(0)), 10, 100000);
	pr_info("pixel-pd-off: %s %s\n", d->name, ret ? "did not power off" : "off");
	return ret;
}

/* Stock <block>_on, then the secure context restore and the save list, in
 * gs201-g3d-pd's order. */
static int domain_on(void __iomem *pmu, struct domain *d)
{
	struct arm_smccc_res res;
	unsigned int i;
	u32 val;
	int ret;

	if (readl(pmu + d->pmu + 4) & BIT(0))
		return 0;
	if (!d->valid) {
		pr_err("pixel-pd-off: %s went off without its save list; not powering it on\n",
		       d->name);
		return -ENODATA;
	}
	val = readl(pmu + d->pmu) | BIT(0);
	arm_smccc_smc(GS201_SMC_PRIV_REG, GS201_PMU + d->pmu, 1, val, 0, 0, 0, 0, &res);
	if (res.a0) {
		pr_err("pixel-pd-off: %s: PMU write failed: %#lx\n", d->name, res.a0);
		return -EIO;
	}
	ret = readl_poll_timeout(pmu + d->pmu + 4, val, val & BIT(0), 10, 100000);
	if (ret) {
		pr_err("pixel-pd-off: %s did not power on\n", d->name);
		return ret;
	}
	arm_smccc_smc(GS201_SMC_PD_CONTEXT, 1, d->tzpc, 2, 0, 0, 0, 0, &res);
	if (res.a0) {
		pr_err("pixel-pd-off: %s: secure context restore failed: %#lx\n", d->name, res.a0);
		return -EIO;
	}
	for (i = 0; i < d->nsave; i++)
		reg_rw(d->save[i].pa, true, d->saved[i]);
	for (i = 0; i < ARRAY_SIZE(d->s2mpu); i++) {
		if (!d->s2mpu[i])
			continue;
		val = reg_rw(d->s2mpu[i], false, 0);
		if (val != d->s2mpu_ctrl0[i])
			reg_rw(d->s2mpu[i], true, d->s2mpu_ctrl0[i]);
		pr_info("pixel-pd-off: %s S2MPU %#x CTRL0 %#x -> %#x\n", d->name, d->s2mpu[i], val,
			reg_rw(d->s2mpu[i], false, 0));
	}
	pr_info("pixel-pd-off: %s on, %u registers restored\n", d->name, d->nsave);
	return 0;
}

int pixel_pd_power(const char *name, bool on)
{
	unsigned int i;
	int ret = -ENOENT;

	for (i = 0; i < ARRAY_SIZE(domains); i++) {
		if (strcmp(domains[i].name, name))
			continue;
		if (!domains[i].nsave)
			return -EOPNOTSUPP;
		mutex_lock(&pd_lock);
		ret = on ? domain_on(pmu_va, &domains[i]) : domain_off(pmu_va, &domains[i]);
		mutex_unlock(&pd_lock);
		break;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(pixel_pd_power);

static int power_set(const char *val, const struct kernel_param *kp)
{
	const char *eq = strchr(val, '=');
	char name[8];
	bool on;

	if (!eq || eq == val || eq - val >= sizeof(name) || kstrtobool(eq + 1, &on))
		return -EINVAL;
	strscpy(name, val, eq - val + 1);
	return pixel_pd_power(name, on);
}
static const struct kernel_param_ops power_ops = { .set = power_set };
module_param_cb(power, &power_ops, NULL, 0200);
MODULE_PARM_DESC(power, "Test: \"csis=1\", \"pdp=0\" powers a restorable domain on or off");

static int __init pixel_pd_off_init(void)
{
	unsigned int i;
	int ret = 0;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	pmu_va = ioremap(GS201_PMU, 0x3000);
	if (!pmu_va)
		return -ENOMEM;
	mutex_lock(&pd_lock);
	for (i = 0; i < ARRAY_SIZE(domains) && !ret; i++)
		if (wanted(domains[i].name))
			ret = domain_off(pmu_va, &domains[i]);
	mutex_unlock(&pd_lock);
	if (ret)
		iounmap(pmu_va);
	return ret;
}
module_init(pixel_pd_off_init);

static void __exit pixel_pd_off_exit(void)
{
	iounmap(pmu_va);
}
module_exit(pixel_pd_off_exit);

MODULE_DESCRIPTION("Power off unused GS201 blocks");
MODULE_LICENSE("GPL");
