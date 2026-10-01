// SPDX-License-Identifier: GPL-2.0-only
/* DC-PHY isolation bypass for the CSIS MIPI PHYs, for camera bring-up.
 *
 * All eight CSIS DC-PHYs share one PMU isolation control, offset 0x3ebc bit 0
 * (1 = isolation bypassed), the only thing Google's phy-exynos-mipi.c does
 * for them; the HAL programs the PHYs from the normal world. PMU registers
 * are written through the secure monitor, as exynos_pmu_update() does on
 * GS201 (set_priv_reg, SMC 0x82000504). The module requires pd_csis on
 * (PMU 0x2404 bit 0), bypasses isolation at load and puts back the value it
 * found at unload. The PHYs stay in reset (SYSREG_CSIS 0x500) until released.
 *
 * It also votes the ACPM DVFS rates of the camera domains, as stock LWIS does
 * when a camera opens: the CSIS link runs from CAM, which boots at 67 MHz,
 * and at that rate a 4-lane link overflows (Pixel 6 port: 400 MHz). The
 * clocks are the ACPM provider /power-management, indices 8 (intcam) and 10
 * (cam) in the firmware's DVFS order (drivers/clk/samsung/clk-acpm.c).
 */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>

#define GS201_SMC_PRIV_REG	0x82000504
#define PMUREG_WRITE		1
#define PMU_PAGE		0x18063000
#define PMU_CSIS_STATUS		0x18062404
#define PMU_DCPHY_ISO		0x18063ebc

static u32 found;
static bool changed;
static uint cam_khz = 400000, intcam_khz;
module_param(cam_khz, uint, 0444);
MODULE_PARM_DESC(cam_khz, "ACPM CAM DVFS rate to vote (0: leave)");
module_param(intcam_khz, uint, 0444);
MODULE_PARM_DESC(intcam_khz, "ACPM INTCAM DVFS rate to vote (0: leave)");

static struct {
	const char *name;
	int index;
	uint *khz;
	struct clk *clk;
	unsigned long old;
} dvfs[] = {
	{ "cam", 10, &cam_khz },
	{ "intcam", 8, &intcam_khz },
};

static void dvfs_vote(void)
{
	struct device_node *np = of_find_node_by_path("/power-management");
	unsigned int i;

	if (!np)
		return;
	for (i = 0; i < ARRAY_SIZE(dvfs); i++) {
		struct of_phandle_args args = { .np = np, .args_count = 1,
						.args = { dvfs[i].index } };
		struct clk *c;
		int ret;

		if (!*dvfs[i].khz)
			continue;
		c = of_clk_get_from_provider(&args);
		if (IS_ERR(c)) {
			pr_err("pixel-csis-iso: no ACPM %s clock\n", dvfs[i].name);
			continue;
		}
		if (strcmp(__clk_get_name(c), dvfs[i].name)) {
			pr_err("pixel-csis-iso: ACPM clock %d is %s, not %s\n", dvfs[i].index,
			       __clk_get_name(c), dvfs[i].name);
			clk_put(c);
			continue;
		}
		dvfs[i].old = clk_get_rate(c);
		ret = clk_set_rate(c, (unsigned long)*dvfs[i].khz * 1000);
		pr_info("pixel-csis-iso: %s %lu -> %lu Hz (%d)\n", dvfs[i].name, dvfs[i].old,
			clk_get_rate(c), ret);
		dvfs[i].clk = c;
	}
	of_node_put(np);
}

static void dvfs_restore(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dvfs); i++) {
		if (!dvfs[i].clk)
			continue;
		clk_set_rate(dvfs[i].clk, dvfs[i].old);
		clk_put(dvfs[i].clk);
		dvfs[i].clk = NULL;
	}
}

static int iso_write(void __iomem *reg, u32 val)
{
	struct arm_smccc_res res;

	arm_smccc_smc(GS201_SMC_PRIV_REG, PMU_DCPHY_ISO, PMUREG_WRITE, val, 0, 0, 0, 0, &res);
	if (res.a0 || readl(reg) != val) {
		pr_err("pixel-csis-iso: PMU 0x3ebc write %#x failed (%ld, reads %#x)\n",
		       val, (long)res.a0, readl(reg));
		return -EIO;
	}
	return 0;
}

static int __init pixel_csis_iso_init(void)
{
	void __iomem *pmu = ioremap(PMU_PAGE, 0x1000);
	void __iomem *status = ioremap(PMU_CSIS_STATUS & PAGE_MASK, PAGE_SIZE);
	int ret = -ENODEV;

	if (!pmu || !status) {
		ret = -ENOMEM;
		goto out;
	}
	if (!(readl(status + (PMU_CSIS_STATUS & ~PAGE_MASK)) & 1)) {
		pr_err("pixel-csis-iso: pd_csis is off\n");
		goto out;
	}
	found = readl(pmu + (PMU_DCPHY_ISO & 0xfff));
	ret = 0;
	if (!(found & 1)) {
		ret = iso_write(pmu + (PMU_DCPHY_ISO & 0xfff), found | 1);
		changed = !ret;
	}
	pr_info("pixel-csis-iso: DC-PHY isolation bypass %s (was %#x)\n",
		ret ? "failed" : "on", found);
	if (!ret)
		dvfs_vote();
out:
	if (pmu)
		iounmap(pmu);
	if (status)
		iounmap(status);
	return ret;
}

static void __exit pixel_csis_iso_exit(void)
{
	void __iomem *pmu = ioremap(PMU_PAGE, 0x1000);

	dvfs_restore();
	if (pmu && changed && !iso_write(pmu + (PMU_DCPHY_ISO & 0xfff), found))
		pr_info("pixel-csis-iso: DC-PHY isolation restored (%#x)\n", found);
	if (pmu)
		iounmap(pmu);
}

module_init(pixel_csis_iso_init);
module_exit(pixel_csis_iso_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GS201 CSIS DC-PHY isolation bypass for camera bring-up");
