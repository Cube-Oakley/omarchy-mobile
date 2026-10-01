// SPDX-License-Identifier: GPL-2.0-only
/* S2MPG12/S2MPG13 regulator opmodes as the stock kernel sets them.
 *
 * Each PMIC rail has an enable field: 0 off, 1 on while PWREN (off in
 * SYS_SLEEP), 2 on while PWREN_MIF, 3 always on (single-bit fields: 1 on).
 * Google's s2mpg12/s2mpg13 regulator drivers write every rail's field at
 * boot from the stock DT's regulator-initial-mode. This kernel has no such
 * driver, so the bootloader's fields stay, and pixel-sleep-audit found them
 * differing from stock (kernel/suspend/README.md):
 * - retention: four S2MPG13 bucks, among them VDD2H (the DRAM core supply),
 *   follow PWREN_MIF where stock keeps them always on. A memory-interface
 *   power-down in sleep would then drop DRAM power. `retention=1` sets them
 *   to 3, which only keeps them on longer.
 * - suspend: rails stock lets PWREN switch off in SYS_SLEEP (the CPU, INT,
 *   MIF, camera, TCXO and PLL rails among them) are always on here, so sleep
 *   could not turn them off. `suspend=NAME,...` sets the named ones to 1,
 *   one at a time, each read back. While the SoC runs PWREN is high, so they
 *   stay on; stock runs with exactly these settings.
 * Test-only rails (`test`) are never part of "all" and must be named: the
 * touch (L25M, L26M) and panel (L27M VCI, L28M VDDD) supplies, always on
 * in stock, where the drivers switch them. Following PWREN they drop only
 * inside SYS_SLEEP; neither the touch IC nor the panel is re-initialized
 * after it, so they are for measurements that end in a reboot.
 * Nothing else is written: not the NFC/eSIM rail (L14S), not the rails this
 * port keeps off on purpose (GNSS, UWB, fingerprint), not voltages. The
 * fields stay as set when the module is unloaded; `restore=1` at unload puts
 * back what it found.
 * Register offsets and enable masks: Google's s2mpg12-regulator.c:233-294,
 * s2mpg13-regulator.c:217-300; opmodes from the stock DT (regulator-initial-
 * mode), checked by kernel/suspend/sleep-audit-diff.py.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/string.h>

#define ACPM_PMIC_CHANNEL	2
#define PM_BANK			0x01

struct rail {
	const char *name;
	u8 pmic, reg, mask;
	u8 want;		/* stock field value, unshifted */
	bool retention;		/* in the retention set (raised to always on) */
	bool test;		/* measurement only: never in "all" */
};

/* Retention: stock mode 3 where the bootloader leaves 2 (PWREN_MIF). */
/* Suspend: stock mode 1 where the bootloader leaves 3 (always on). */
static const struct rail rails[] = {
	{ "BUCK3S", 1, 0x13, 0xc0, 3, true },	/* S3S_LLDO1 */
	{ "BUCK4S", 1, 0x15, 0xc0, 3, true },	/* S4S_VDD2H_MEM */
	{ "BUCK7S", 1, 0x1b, 0xc0, 3, true },	/* S7S_MLDO */
	{ "BUCK9S", 1, 0x1f, 0xc0, 3, true },	/* S9S_VDD_AOC */
	{ "LDO13S", 1, 0x38, 0xc0, 1 },		/* L13S_DPAUX */
	{ "LDO18S", 1, 0x3d, 0xc0, 1 },		/* L18S_PCIE1 */
	{ "LDO3S", 1, 0x2e, 0xc0, 1 },		/* L3S_PCIE1 */
	{ "LDO2S", 1, 0x2d, 0xc0, 1 },		/* L2S_PLL_MIPI_UFS */
	{ "LDO1S", 1, 0x48, 0x03, 1 },		/* L1S_VDD_G3D_M */
	{ "BUCK1S", 1, 0x0f, 0xc0, 1 },		/* S1S_VDD_CAM */
	{ "BUCK8S", 1, 0x1d, 0xc0, 1 },		/* S8S_VDD_G3D_L2 */
	{ "BUCK5S", 1, 0x17, 0xc0, 1 },		/* S5S_VDDQ_MEM */
	{ "LDO4M", 0, 0x2f, 0xc0, 1 },		/* L4M_HSI */
	{ "LDO5M", 0, 0x30, 0xc0, 1 },		/* L5M_TCXO */
	{ "LDO6M", 0, 0x31, 0xc0, 1 },		/* L6M_PLL */
	{ "LDO14M", 0, 0x39, 0xc0, 1 },		/* L14M_TCXO */
	{ "LDO16M", 0, 0x3b, 0xc0, 1 },		/* L16M_PCIE0 */
	{ "LDO18M", 0, 0x3d, 0xc0, 1 },		/* L18M_PCIE0 */
	{ "LDO11M", 0, 0x48, 0x0c, 1 },		/* L11M_VDD_CPUCL1_M */
	{ "LDO12M", 0, 0x48, 0x30, 1 },		/* L12M_VDD_CPUCL0_M */
	{ "LDO17M", 0, 0x49, 0x0c, 1 },		/* L17M_VDD_CPUCL2_M */
	{ "BUCK7M", 0, 0x23, 0xc0, 1 },		/* S7M_VDD_INT_M */
	{ "BUCK2M", 0, 0x19, 0xc0, 1 },		/* S2M_VDD_CPUCL2 */
	{ "BUCK3M", 0, 0x1b, 0xc0, 1 },		/* S3M_VDD_CPUCL1 */
	{ "BUCK4M", 0, 0x1d, 0xc0, 1 },		/* S4M_VDD_CPUCL0 */
	{ "BUCK5M", 0, 0x1f, 0xc0, 1 },		/* S5M_VDD_INT */
	{ "BUCK1M", 0, 0x17, 0xc0, 1 },		/* S1M_VDD_MIF */
	{ "LDO25M", 0, 0x44, 0xc0, 1, false, true },	/* touch */
	{ "LDO26M", 0, 0x45, 0xc0, 1, false, true },	/* touch */
	{ "LDO27M", 0, 0x46, 0xc0, 1, false, true },	/* panel VCI */
	{ "LDO28M", 0, 0x47, 0xc0, 1, false, true },	/* panel VDDD */
};

static bool retention;
module_param(retention, bool, 0400);
MODULE_PARM_DESC(retention, "Set the four retention rails to always on (stock)");
static char *suspend;
module_param(suspend, charp, 0400);
MODULE_PARM_DESC(suspend, "Comma-separated rails to set to follow PWREN; \"all\" is the stock set");
static bool restore;
module_param(restore, bool, 0600);
MODULE_PARM_DESC(restore, "Put back the fields found at load when unloading");

static struct device *dev;
static struct acpm_handle *acpm;
static u8 found[ARRAY_SIZE(rails)];
static bool changed[ARRAY_SIZE(rails)];

static int field_set(unsigned int i, u8 field)
{
	const struct rail *r = &rails[i];
	u8 val, want, shift = __ffs(r->mask);
	int ret;

	ret = acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHANNEL, PM_BANK, r->reg, r->pmic, &val);
	if (ret)
		return ret;
	want = (val & ~r->mask) | ((field << shift) & r->mask);
	if (want == val)
		return 0;
	ret = acpm->ops->pmic.write_reg(acpm, ACPM_PMIC_CHANNEL, PM_BANK, r->reg, r->pmic, want);
	if (!ret)
		ret = acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHANNEL, PM_BANK, r->reg, r->pmic,
					       &val);
	if (!ret && val != want)
		ret = -EIO;
	pr_info("pixel-pmic-opmode: %s field %u -> %u (CTRL %#04x)%s\n", r->name,
		(found[i] & r->mask) >> shift, field, val, ret ? " FAILED" : "");
	return ret;
}

static bool named(const char *name)
{
	const char *p = suspend;
	size_t n = strlen(name);

	while (p && (p = strstr(p, name))) {
		if ((p == suspend || p[-1] == ',') && (p[n] == ',' || !p[n]))
			return true;
		p += n;
	}
	return false;
}

static bool selected(unsigned int i)
{
	if (rails[i].retention)
		return retention;
	if (named(rails[i].name))
		return true;
	return !rails[i].test && named("all");
}

static int __init opmode_init(void)
{
	struct device_node *np;
	unsigned int i;
	int ret = 0;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	dev = root_device_register("pixel-pmic-opmode");
	if (IS_ERR(dev))
		return PTR_ERR(dev);
	np = of_find_node_by_path("/power-management");
	acpm = np ? devm_acpm_get_by_node(dev, np) : ERR_PTR(-ENODEV);
	of_node_put(np);
	if (IS_ERR(acpm)) {
		root_device_unregister(dev);
		return PTR_ERR(acpm);
	}
	for (i = 0; i < ARRAY_SIZE(rails); i++) {
		ret = acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHANNEL, PM_BANK, rails[i].reg,
					       rails[i].pmic, &found[i]);
		if (ret)
			goto fail;
	}
	for (i = 0; i < ARRAY_SIZE(rails); i++) {
		const struct rail *r = &rails[i];
		u8 now = (found[i] & r->mask) >> __ffs(r->mask);

		if (!selected(i))
			continue;
		/* Only 3 -> 1 (suspend) and 2 -> 3 (retention): nothing is switched on or off. */
		if (!(r->retention ? now == 2 : now == 3)) {
			if (now != r->want)
				pr_info("pixel-pmic-opmode: %s field %u, not %s; left alone\n", r->name,
					now, r->retention ? "2" : "3");
			continue;
		}
		ret = field_set(i, r->want);
		if (ret)
			goto fail;
		changed[i] = true;
		usleep_range(1000, 2000);
	}
	return 0;
fail:
	pr_err("pixel-pmic-opmode: stopped (%d); rails set so far stay set\n", ret);
	return 0;
}
module_init(opmode_init);

static void __exit opmode_exit(void)
{
	unsigned int i = ARRAY_SIZE(rails);

	while (restore && i--)
		if (changed[i])
			field_set(i, (found[i] & rails[i].mask) >> __ffs(rails[i].mask));
	root_device_unregister(dev);
}
module_exit(opmode_exit);

MODULE_DESCRIPTION("GS201 PMIC regulator opmodes as the stock kernel sets them");
MODULE_LICENSE("GPL");
