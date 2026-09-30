// SPDX-License-Identifier: GPL-2.0-only
/* Power for the peripherals the AoC runs: the built-in microphones and the
 * sensors.
 *
 * The stock DT gives these PMIC rails to the AoC:
 * - microphones: S2MPG12 LDO20M (DMIC1), S2MPG13 LDO19S (DMIC3) and LDO20S
 *   (DMIC4/5), always on at 1.6-1.95 V;
 * - sensors: S2MPG13 LDO7S (L7S_SENSORS, 1.6-1.95 V) and LDO5S (L5S_PROX,
 *   2.5-3.3 V), the AoC driver's sensor_1v8 and sensor_3v3 supplies, which it
 *   switches on in that order.
 * Nothing on this kernel drives the PMIC regulators and the bootloader leaves
 * all five off, so the AoC records silence and finds no sensor on its buses.
 * This switches each on through ACPM: opmode ON (bits 7:6) for the
 * microphone LDOs, the enable bit (7) for the sensor LDOs, as Google's
 * s2mpg12/s2mpg13 regulator drivers define them. The voltage (bits 5:0) is
 * the one the bootloader set; a rail whose voltage is outside the stock DT
 * range is left alone. The rails stay on when the module is unloaded.
 * Register offsets from Google's s2mpg12/s2mpg13-register.h (PM bank).
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/module.h>
#include <linux/of.h>

#define ACPM_PMIC_CHANNEL	2
#define PM_BANK			0x01
#define VSEL			0x3f

struct rail {
	const char *name;
	u8 pmic, reg;
	u8 enable;		/* enable bits (mask and value) */
	u32 min_uv, step_uv;	/* voltage encoding of this LDO group */
	u32 lo_uv, hi_uv;	/* stock DT range */
};

/* In the order they are switched on. */
static const struct rail rails[] = {
	{ "LDO20M (DMIC1)", 0, 0x3f, 0xc0, 700000, 25000, 1600000, 1950000 },
	{ "LDO19S (DMIC3)", 1, 0x3e, 0xc0, 700000, 25000, 1600000, 1950000 },
	{ "LDO20S (DMIC4/5)", 1, 0x3f, 0xc0, 700000, 25000, 1600000, 1950000 },
	{ "LDO7S (sensors 1.8 V)", 1, 0x32, 0x80, 700000, 25000, 1600000, 1950000 },
	{ "LDO5S (sensors 3.3 V)", 1, 0x30, 0x80, 1800000, 25000, 2500000, 3300000 },
};

static struct device *dev;

static int rail_on(struct acpm_handle *acpm, const struct rail *r)
{
	u32 uv;
	u8 val;
	int ret;

	ret = acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHANNEL, PM_BANK, r->reg, r->pmic, &val);
	if (ret)
		return ret;
	uv = r->min_uv + r->step_uv * (val & VSEL);
	if (uv < r->lo_uv || uv > r->hi_uv) {
		pr_err("pixel-aoc-power: %s at %u uV (CTRL %#04x), outside the stock range; left alone\n",
		       r->name, uv, val);
		return -ERANGE;
	}
	if ((val & r->enable) == r->enable)
		return 0;
	ret = acpm->ops->pmic.write_reg(acpm, ACPM_PMIC_CHANNEL, PM_BANK, r->reg, r->pmic,
					val | r->enable);
	if (!ret)
		pr_info("pixel-aoc-power: %s on at %u mV (CTRL was %#04x)\n", r->name, uv / 1000, val);
	usleep_range(1000, 2000);
	return ret;
}

static int __init aoc_power_init(void)
{
	struct acpm_handle *acpm;
	struct device_node *np;
	unsigned int i;
	int ret, err = 0;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	dev = root_device_register("pixel-aoc-power");
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
		ret = rail_on(acpm, &rails[i]);
		if (ret && !err)
			err = ret;
	}
	/* A rail left off is reported; the others stay on regardless. */
	return 0;
}
module_init(aoc_power_init);

static void __exit aoc_power_exit(void)
{
	root_device_unregister(dev);
}
module_exit(aoc_power_exit);

MODULE_DESCRIPTION("GS201 microphone and sensor supplies for the AoC");
MODULE_LICENSE("GPL");
