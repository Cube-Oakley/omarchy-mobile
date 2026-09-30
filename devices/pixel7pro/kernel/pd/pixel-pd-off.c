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
 * There is no power-on path: the domains come back on the next boot. The
 * module stays loaded only so the boot can tell it ran.
 *
 * Measured (image W, screen off, C2 working): TPU 0.08 W, then AUR, BO, MFC,
 * G2D and EH 0.12 W at the USB input; the camera domains made no difference
 * that the USB input could resolve.
 */
#include <linux/arm-smccc.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/string.h>

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
};

/* Stock DT pd-* nodes: reg, cmu_id, need_smc. Listed children first. */
static const struct domain domains[] = {
	{ "tpu",  0x2900, 0x1cc00000, 0x1cc10204 },
	{ "aur",  0x2980, 0x25a00000, 0x25a10204 },
	{ "bo",   0x2880, 0x1ca00000, 0x1ca10204 },
	{ "mfc",  0x2380, 0x1c800000, 0x1c810204 },
	{ "g2d",  0x2300, 0x1c600000, 0x1c610204 },
	{ "eh",   0x1c00, 0x17000000, 0x17010204 },
	{ "dns",  0x2500, 0x1b000000, 0x1b010204 },
	{ "itp",  0x2680, 0x1b400000, 0x1b410204 },
	{ "ipp",  0x2600, 0x1ac00000, 0x1ac10204 },
	{ "pdp",  0x2480, 0x1aa00000, 0x1aa10204 },
	{ "csis", 0x2400, 0x1a400000, 0x1a410204 },
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

static int domain_off(void __iomem *pmu, const struct domain *d)
{
	struct arm_smccc_res res;
	void __iomem *cmu;
	u32 val;
	int ret;

	if (!(readl(pmu + d->pmu + 4) & BIT(0))) {
		pr_info("pixel-pd-off: %s already off\n", d->name);
		return 0;
	}
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

static int __init pixel_pd_off_init(void)
{
	void __iomem *pmu;
	unsigned int i;
	int ret = 0;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	pmu = ioremap(GS201_PMU, 0x3000);
	if (!pmu)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(domains) && !ret; i++)
		if (wanted(domains[i].name))
			ret = domain_off(pmu, &domains[i]);
	iounmap(pmu);
	return ret;
}
module_init(pixel_pd_off_init);

static void __exit pixel_pd_off_exit(void)
{
}
module_exit(pixel_pd_off_exit);

MODULE_DESCRIPTION("Power off unused GS201 blocks");
MODULE_LICENSE("GPL");
