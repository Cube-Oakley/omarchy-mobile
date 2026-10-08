// SPDX-License-Identifier: GPL-2.0-only
/* The USB-C port's temperature, for charging at 3 A and more: the NTC
 * channels of the S2MPG13 sub-PMIC's meter bank, read through ACPM as
 * Google's s2mpg13_spmic_thermal does. Channel 3 is usb_pwr_therm, at the
 * port; 4 is usb_pwr_therm2; 0 is neutral_therm, the board. Stock cuts the
 * port above 42 C at the port, or 2.5 C above usb_pwr_therm2, or 5 C above
 * neutral_therm.
 *
 * The bootloader leaves the NTC engine off (METER_CTRL3 0, the meter off).
 * pixel_port_temp_mc() switches the three channels on at its first call,
 * with stock's sequence, and they go off again OFF_AFTER without a call:
 * pixel-battery asks only while USB power is there. The first samples take
 * a few seconds (-EBUSY until then). Reading temps does the same, and reports
 * all eight channels.
 * Loading pixel-odpm meanwhile is not supported: both drive METER_CTRL1 and
 * the meter reset.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/workqueue.h>

#define ACPM_PMIC_CHANNEL	2
#define S2MPG13			1	/* the sub PMIC, as ACPM numbers them */
#define METER_BANK		0x0a
#define METER_CTRL1		0x08	/* bits 7:5 NTC sample rate, bit 0 meter on */
#define METER_EN		BIT(0)
#define NTC_RATE		GENMASK(7, 5)
#define NTC_0P625HZ		3
#define METER_CTRL3		0x0a	/* NTC channel enables, one bit each */
#define METER_LPF_DATA_NTC	0xd4	/* 2 bytes per channel, 12 bits */
#define TRIM_BANK		0x0e
#define TRIM_COMMON2		0x34	/* bit 7: PMETER_MRST, the meter reset */
#define PMETER_MRST		BIT(7)
#define NTC_NEUTRAL		0
#define NTC_USB			3
#define NTC_USB2		4
#define NTC_USED		(BIT(NTC_NEUTRAL) | BIT(NTC_USB) | BIT(NTC_USB2))
#define OFF_AFTER		(60 * HZ)

static struct device *port_temp_dev;
static struct acpm_handle *acpm;
static DEFINE_MUTEX(ntc_lock);
static bool ntc_on;
static u8 saved_ctrl1;
static struct delayed_work off_work;

/* Google's voltage code to milli-degrees C, descending in voltage. */
static const struct { int volt, temp; } adc_map[] = {
	{ 0xF8D, -26428 }, { 0xF6A, -21922 }, { 0xF29, -15958 },
	{ 0xEE4, -11060 }, { 0xE9D, -6890 },  { 0xE3F, -2264 },
	{ 0xDBF, 2961 },   { 0xD33, 7818 },   { 0xC97, 12525 },
	{ 0xBF5, 16945 },  { 0xB3A, 21623 },  { 0xA42, 27431 },
	{ 0x7F1, 40631 },  { 0x734, 44960 },  { 0x66B, 49757 },
	{ 0x5A3, 54854 },  { 0x4EE, 59898 },  { 0x446, 65076 },
	{ 0x43A, 65779 },  { 0x430, 65856 },  { 0x3C3, 69654 },
	{ 0x3BD, 69873 },  { 0x33B, 74910 },  { 0x2BB, 80691 },
	{ 0x259, 85844 },  { 0x206, 90915 },  { 0x1CE, 94873 },
	{ 0x191, 99720 },  { 0x160, 104216 }, { 0x12E, 109531 },
	{ 0xF9, 116445 },  { 0xD7, 121600 },  { 0x9F, 131839 },
};

static int volt_to_mc(int volt)
{
	unsigned int i;

	if (volt >= adc_map[0].volt)
		return adc_map[0].temp;
	for (i = 1; i < ARRAY_SIZE(adc_map); i++)
		if (volt >= adc_map[i].volt)
			return adc_map[i].temp +
			       mult_frac(adc_map[i - 1].temp - adc_map[i].temp,
					 volt - adc_map[i].volt,
					 adc_map[i - 1].volt - adc_map[i].volt);
	return adc_map[ARRAY_SIZE(adc_map) - 1].temp;
}

static int meter_read(u8 reg, u8 *val)
{
	return acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHANNEL, METER_BANK, reg, S2MPG13, val);
}

static int meter_write(u8 reg, u8 val)
{
	return acpm->ops->pmic.write_reg(acpm, ACPM_PMIC_CHANNEL, METER_BANK, reg, S2MPG13, val);
}

static int trim_write(u8 val)
{
	return acpm->ops->pmic.write_reg(acpm, ACPM_PMIC_CHANNEL, TRIM_BANK, TRIM_COMMON2,
					 S2MPG13, val);
}

/* Stock's channel update (b/200582715): the channel set only takes across a
 * meter reset, twice. Then the meter on, sampling the NTCs at 0.625 Hz. */
static int ntc_enable(void)
{
	u8 trim;
	int ret;

	ret = acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHANNEL, TRIM_BANK, TRIM_COMMON2,
				       S2MPG13, &trim);
	ret = ret ?: meter_read(METER_CTRL1, &saved_ctrl1);
	ret = ret ?: trim_write(trim & ~PMETER_MRST);
	usleep_range(100, 10000);
	ret = ret ?: trim_write(trim | PMETER_MRST);
	usleep_range(100, 10000);
	ret = ret ?: meter_write(METER_CTRL3, 0);
	usleep_range(100, 10000);
	ret = ret ?: trim_write(trim & ~PMETER_MRST);
	usleep_range(100, 10000);
	ret = ret ?: trim_write(trim | PMETER_MRST);
	usleep_range(100, 10000);
	ret = ret ?: meter_write(METER_CTRL3, NTC_USED);
	ret = ret ?: meter_write(METER_CTRL1, (saved_ctrl1 & ~NTC_RATE) |
				 FIELD_PREP(NTC_RATE, NTC_0P625HZ) | METER_EN);
	if (!ret)
		ntc_on = true;
	dev_info(port_temp_dev, "NTC channels %s (meter was %#x)\n", ret ? "failed" : "on",
		 saved_ctrl1);
	return ret;
}

static void ntc_disable(void)
{
	if (!ntc_on)
		return;
	meter_write(METER_CTRL3, 0);
	meter_write(METER_CTRL1, saved_ctrl1);
	ntc_on = false;
	dev_info(port_temp_dev, "NTC channels off\n");
}

static void off_work_fn(struct work_struct *work)
{
	mutex_lock(&ntc_lock);
	ntc_disable();
	mutex_unlock(&ntc_lock);
}

/* One channel in milli-degrees C: -EIO while it is off, -EBUSY before its
 * first sample. */
static int channel_mc(unsigned int ch, u8 enabled, int *mc)
{
	u8 b[2];
	int ret, raw;

	if (!(enabled & BIT(ch)))
		return -EIO;
	ret = acpm->ops->pmic.bulk_read(acpm, ACPM_PMIC_CHANNEL, METER_BANK,
					METER_LPF_DATA_NTC + 2 * ch, S2MPG13, 2, b);
	if (ret)
		return ret;
	raw = b[0] | (b[1] & 0x0f) << 8;
	if (!raw)
		return -EBUSY;
	*mc = volt_to_mc(raw);
	return 0;
}

/* The port, the second port sensor and the board, in milli-degrees C. */
int pixel_port_temp_mc(int *usb, int *usb2, int *neutral);	/* looked up by name */
int pixel_port_temp_mc(int *usb, int *usb2, int *neutral)
{
	u8 enabled;
	int ret = 0;

	if (!acpm)
		return -ENODEV;
	mutex_lock(&ntc_lock);
	if (!ntc_on)
		ret = ntc_enable();
	mod_delayed_work(system_wq, &off_work, OFF_AFTER);
	ret = ret ?: meter_read(METER_CTRL3, &enabled);
	ret = ret ?: channel_mc(NTC_USB, enabled, usb);
	ret = ret ?: channel_mc(NTC_USB2, enabled, usb2);
	ret = ret ?: channel_mc(NTC_NEUTRAL, enabled, neutral);
	mutex_unlock(&ntc_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(pixel_port_temp_mc);

static int temps_get(char *buf, const struct kernel_param *kp)
{
	u8 ctrl1, ctrl3, b[2];
	int len, ch, ret, mc;

	/* As a reader would: the channels on, for OFF_AFTER. */
	pixel_port_temp_mc(&mc, &mc, &mc);
	ret = meter_read(METER_CTRL1, &ctrl1);
	ret = ret ?: meter_read(METER_CTRL3, &ctrl3);
	if (ret)
		return ret;
	len = sysfs_emit(buf, "ctrl1 %#04x ctrl3 %#04x\n", ctrl1, ctrl3);
	for (ch = 0; ch < 8; ch++) {
		ret = acpm->ops->pmic.bulk_read(acpm, ACPM_PMIC_CHANNEL, METER_BANK,
						METER_LPF_DATA_NTC + 2 * ch, S2MPG13, 2, b);
		if (ret)
			return ret;
		ret = channel_mc(ch, ctrl3, &mc);
		len += sysfs_emit_at(buf, len, "ntc%d raw %#05x %s", ch, b[0] | (b[1] & 0x0f) << 8,
				     ch == NTC_USB ? "usb " : ch == NTC_USB2 ? "usb2 " :
				     ch == NTC_NEUTRAL ? "neutral " : "");
		len += ret ? sysfs_emit_at(buf, len, "(%d)\n", ret) :
			     sysfs_emit_at(buf, len, "%d.%d C\n", mc / 1000, abs(mc % 1000) / 100);
	}
	return len;
}
static const struct kernel_param_ops temps_ops = { .get = temps_get };
module_param_cb(temps, &temps_ops, NULL, 0444);
MODULE_PARM_DESC(temps, "The S2MPG13 NTC channels: raw codes and degrees C");

static int __init pixel_port_temp_init(void)
{
	struct device_node *np;
	struct acpm_handle *h;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	port_temp_dev = root_device_register("pixel-port-temp");
	if (IS_ERR(port_temp_dev))
		return PTR_ERR(port_temp_dev);
	np = of_find_node_by_path("/power-management");
	h = np ? devm_acpm_get_by_node(port_temp_dev, np) : ERR_PTR(-ENODEV);
	of_node_put(np);
	if (IS_ERR(h)) {
		root_device_unregister(port_temp_dev);
		return PTR_ERR(h);
	}
	INIT_DELAYED_WORK(&off_work, off_work_fn);
	acpm = h;
	return 0;
}
module_init(pixel_port_temp_init);

static void __exit pixel_port_temp_exit(void)
{
	cancel_delayed_work_sync(&off_work);
	mutex_lock(&ntc_lock);
	ntc_disable();
	mutex_unlock(&ntc_lock);
	acpm = NULL;
	root_device_unregister(port_temp_dev);
}
module_exit(pixel_port_temp_exit);

MODULE_DESCRIPTION("Pixel 7 Pro USB-C port temperature (S2MPG13 NTC)");
MODULE_LICENSE("GPL");
