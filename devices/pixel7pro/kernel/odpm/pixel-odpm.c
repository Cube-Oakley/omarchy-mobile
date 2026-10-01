// SPDX-License-Identifier: GPL-2.0-only
/* Per-rail power from the S2MPG12/S2MPG13 on-device power meters (ODPM).
 *
 * Each PMIC meters 12 channels. This module points them at the rails that
 * matter for idle power, runs the meters at 125 Hz, and reports the low-pass
 * filtered power of every channel when /sys/module/pixel_odpm/parameters/power
 * is read. Register layout and per-rail resolutions from Google's
 * s2mpg1x-meter.h, s2mpg12-powermeter.c and s2mpg13-powermeter.c. Only the
 * meter bank (0x0A) is written: CTRL1 (enable and internal sample rate; the
 * NTC rate bits are kept), CTRL2 (external channels and their sample rate),
 * MUXSELn and the LPF mode (CTRL6 and the next register's low nibble).
 * Interval reads additionally use accumulator mode (CTRL4/5) and ASYNC_RD
 * in CTRL2. Unloading restores CTRL1/2 and the saved accumulator mode.
 *
 * External (VSYS shunt) channels read power at VRAIL x VSHUNT x TRIM x 10 /
 * shunt resolution (odpm.c), with the stock DT shunts: 10 mOhm for mmWave,
 * display and WLAN/BT, 5 mOhm for the modem and RF front end.
 */
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/bitfield.h>
#include <linux/err.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/module.h>
#include <linux/of.h>

#define ACPM_PMIC_CHANNEL	2
#define METER_BANK		0x0a
#define METER_CTRL1		0x08
#define METER_EN		BIT(0)
#define EXT_METER_EN		BIT(1)
#define INT_SAMP_RATE		GENMASK(4, 2)
#define INT_125HZ		4
#define METER_CTRL2		0x09
#define EXT_CHANNELS_ALL	(0x7 << 3)
#define EXT_125HZ		4
#define METER_CTRL6		0x0d	/* LPF mode, channels 0-7; +1 bits 3:0: 8-11 */
#define METER_MUXSEL0		0x11
#define METER_LPF_DATA		0xae	/* 3 bytes per channel, 21 bits */
#define METER_ACC_MODE		0x0b
#define METER_ACC_DATA		0x63	/* 6 bytes per channel, 41 bits */
#define METER_ACC_COUNT		0xab	/* 3 bytes, 20 bits */
#define ASYNC_RD		BIT(7)
#define CHANNELS		12

/* Resolutions in nW per LSB (s2mpg1x-register.h, GS201 values) */
#define CMS	6105
#define CMD	12210
#define CMT	18315
#define VM	12210
#define DVS_NLDO_800	1221
#define PLDO_800	4884
#define SHUNT_10MOHM	13953
#define SHUNT_5MOHM	27906

struct rail {
	const char *name;
	u8 muxsel;
	u32 nw_per_lsb;
};

static const struct rail rails[2][CHANNELS] = {
	{	/* S2MPG12, main */
		{ "S1M_VDD_MIF", 0x01, CMS },
		{ "S2M_VDD_CPUCL2", 0x02, CMT },
		{ "S3M_VDD_CPUCL1", 0x03, CMD },
		{ "S4M_VDD_CPUCL0", 0x04, CMS },
		{ "S5M_VDD_INT", 0x05, CMT },
		{ "S6M_LLDO1", 0x06, CMS },
		{ "S7M_VDD_INT_M", 0x07, CMS },
		{ "VSYS_MMWAVE", 0x5c, SHUNT_10MOHM },
		{ "S9M_LLDO3", 0x09, CMS },
		{ "VSYS_MODEM", 0x5d, SHUNT_5MOHM },
		{ "L15M_VDD_SLC_M", 0x2f, DVS_NLDO_800 },
		{ "VSYS_RFFE", 0x5e, SHUNT_5MOHM },
	}, {	/* S2MPG13, sub */
		{ "S1S_VDD_CAM", 0x01, CMT },
		{ "S2S_VDD_G3D", 0x02, CMT },
		{ "S3S_LLDO1", 0x03, CMS },
		{ "S4S_VDD2H_MEM", 0x04, CMS },
		{ "S5S_VDDQ_MEM", 0x05, CMS },
		{ "S6S_LLDO2", 0x06, CMS },
		{ "S7S_MLDO", 0x07, VM },
		{ "VSYS_DISPLAY", 0x5c, SHUNT_10MOHM },
		{ "VSYS_WLAN_BT", 0x5d, SHUNT_10MOHM },
		{ "S10S_LLDO3", 0x0a, CMS },
		{ "SA", 0x0c, VM },
		{ "SD", 0x0b, VM },
	},
};

static struct device *odpm_dev;
static struct acpm_handle *acpm;
static u8 saved_ctrl1[2], saved_ctrl2[2];
static DEFINE_MUTEX(odpm_lock);
static bool interval_initialized[2];
static bool acc_mode_saved[2];
static u8 saved_acc_mode[2][2];

static int meter_read(unsigned int pmic, u8 reg, u8 *val)
{
	return acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHANNEL, METER_BANK, reg, pmic, val);
}

static int meter_write(unsigned int pmic, u8 reg, u8 val)
{
	return acpm->ops->pmic.write_reg(acpm, ACPM_PMIC_CHANNEL, METER_BANK, reg, pmic, val);
}

static int meter_setup(unsigned int pmic)
{
	unsigned int ch;
	u8 val;
	int ret;

	ret = meter_read(pmic, METER_CTRL1, &saved_ctrl1[pmic]);
	ret = ret ?: meter_read(pmic, METER_CTRL2, &saved_ctrl2[pmic]);
	for (ch = 0; ch < CHANNELS && !ret; ch++)
		ret = meter_write(pmic, METER_MUXSEL0 + ch, rails[pmic][ch].muxsel);
	/* LPF in power mode for all 12 channels */
	ret = ret ?: meter_write(pmic, METER_CTRL6, 0x00);
	ret = ret ?: meter_read(pmic, METER_CTRL6 + 1, &val);
	ret = ret ?: meter_write(pmic, METER_CTRL6 + 1, val & ~0x0f);
	ret = ret ?: meter_write(pmic, METER_CTRL2, EXT_CHANNELS_ALL | EXT_125HZ);
	val = (saved_ctrl1[pmic] & ~INT_SAMP_RATE) | FIELD_PREP(INT_SAMP_RATE, INT_125HZ) |
	      METER_EN | EXT_METER_EN;
	ret = ret ?: meter_write(pmic, METER_CTRL1, val);
	return ret;
}

static int power_get(char *buf, const struct kernel_param *kp)
{
	unsigned int pmic, ch;
	ssize_t count = 0;
	u64 total = 0;

	mutex_lock(&odpm_lock);
	for (pmic = 0; pmic < 2; pmic++) {
		for (ch = 0; ch < CHANNELS; ch++) {
			const struct rail *r = &rails[pmic][ch];
			u8 b[3];
			u32 raw;
			u64 uw;
			int ret;

			ret = acpm->ops->pmic.bulk_read(acpm, ACPM_PMIC_CHANNEL, METER_BANK,
							METER_LPF_DATA + 3 * ch, pmic, 3, b);
			if (ret) {
				count += sysfs_emit_at(buf, count, "%-16s read error %d\n",
						       r->name, ret);
				continue;
			}
			raw = b[0] | b[1] << 8 | (b[2] & 0x1f) << 16;
			uw = div_u64((u64)raw * r->nw_per_lsb, 1000);
			total += uw;
			count += sysfs_emit_at(buf, count, "%-16s %7llu.%01llu mW\n", r->name,
					       uw / 1000, (uw % 1000) / 100);
		}
	}
	count += sysfs_emit_at(buf, count, "%-16s %7llu.%01llu mW\n", "total",
			       total / 1000, (total % 1000) / 100);
	mutex_unlock(&odpm_lock);
	return count;
}

static const struct kernel_param_ops power_ops = { .get = power_get };
module_param_cb(power, &power_ops, NULL, 0444);
MODULE_PARM_DESC(power, "Low-pass filtered power per metered rail");

/* Stock s2mpg1x_meter_sw_reset (GS201 only): pulse PMETER_MRST, bit 7 of the
 * MT_TRIM bank's common register (S2MPG12 0x29, S2MPG13 0x34, where the
 * other bits include the NTC enable and stay as found), then set METER_EN.
 * Without it the accumulators and their 20-bit sample count read back
 * frozen (count pinned at 0xfffff, the same sums every interval): ASYNC_RD
 * alone, or turning the meters off and on, does not clear them.
 */
#define TRIM_BANK		0x0e
#define PMETER_MRST		BIT(7)
static const u8 trim_reg[2] = { 0x29, 0x34 };

static int meter_sw_reset(unsigned int pmic)
{
	u8 val, ctrl1;
	int ret;

	ret = acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHANNEL, TRIM_BANK, trim_reg[pmic], pmic, &val);
	ret = ret ?: acpm->ops->pmic.write_reg(acpm, ACPM_PMIC_CHANNEL, TRIM_BANK, trim_reg[pmic],
					       pmic, val & ~PMETER_MRST);
	ret = ret ?: acpm->ops->pmic.write_reg(acpm, ACPM_PMIC_CHANNEL, TRIM_BANK, trim_reg[pmic],
					       pmic, val | PMETER_MRST);
	if (ret)
		return ret;
	usleep_range(10, 100);
	ret = meter_read(pmic, METER_CTRL1, &ctrl1);
	return ret ?: meter_write(pmic, METER_CTRL1, ctrl1 | METER_EN);
}

/* Stock s2mpg1x_meter_measure_acc: ASYNC_RD copies the accumulators to the
 * readable registers. Each interval starts with meter_sw_reset(), so the
 * sums and the sample count cover exactly the interval. Hardware continues
 * accumulating while the AP is asleep.
 */
static int meter_snapshot(unsigned int pmic)
{
	u8 val;
	int ret, i;

	ret = meter_read(pmic, METER_CTRL2, &val);
	ret = ret ?: meter_write(pmic, METER_CTRL2, val | ASYNC_RD);
	for (i = 0; !ret && i < 20; i++) {
		ret = meter_read(pmic, METER_CTRL2, &val);
		if (!ret && !(val & ASYNC_RD))
			return 0;
		usleep_range(1000, 1500);
	}
	return ret ?: -ETIMEDOUT;
}

static int interval_power_get(char *buf, const struct kernel_param *kp)
{
	ssize_t count = 0;
	u64 total = 0;
	unsigned int pmic, ch;
	int ret = 0;

	mutex_lock(&odpm_lock);
	for (pmic = 0; pmic < 2; pmic++) {
		u8 b[6];
		u32 samples;

		if (!interval_initialized[pmic]) {
			if (!acc_mode_saved[pmic]) {
				ret = meter_read(pmic, METER_ACC_MODE, &saved_acc_mode[pmic][0]);
				ret = ret ?: meter_read(pmic, METER_ACC_MODE + 1, &saved_acc_mode[pmic][1]);
				if (ret)
					goto out;
				acc_mode_saved[pmic] = true;
			}
			/* Power mode for all 12 channels; preserve the upper nibble. */
			ret = meter_write(pmic, METER_ACC_MODE, 0);
			ret = ret ?: meter_write(pmic, METER_ACC_MODE + 1, saved_acc_mode[pmic][1] & 0xf0);
			ret = ret ?: meter_sw_reset(pmic);
			if (ret)
				goto out;
			interval_initialized[pmic] = true;
			count += sysfs_emit_at(buf, count, "PMIC %u: baseline taken\n", pmic);
			continue;
		}
		ret = meter_snapshot(pmic);
		ret = ret ?: acpm->ops->pmic.bulk_read(acpm, ACPM_PMIC_CHANNEL,
			METER_BANK, METER_ACC_COUNT, pmic, 3, b);
		if (ret)
			goto out;
		samples = (b[0] | b[1] << 8 | b[2] << 16) & GENMASK(19, 0);
		count += sysfs_emit_at(buf, count, "PMIC %u: %u samples\n", pmic, samples);
		if (!samples)
			continue;
		for (ch = 0; ch < CHANNELS; ch++) {
			u64 raw, uw;
			ret = acpm->ops->pmic.bulk_read(acpm, ACPM_PMIC_CHANNEL,
				METER_BANK, METER_ACC_DATA + 6 * ch, pmic, 6, b);
			if (ret)
				goto out;
			raw = (u64)b[0] | (u64)b[1] << 8 | (u64)b[2] << 16 |
			      (u64)b[3] << 24 | (u64)b[4] << 32 | (u64)(b[5] & 1) << 40;
			uw = div64_u64(raw * rails[pmic][ch].nw_per_lsb, (u64)samples * 1000);
			total += uw;
			count += sysfs_emit_at(buf, count, "%-16s %7llu.%01llu mW\n",
				rails[pmic][ch].name, uw / 1000, (uw % 1000) / 100);
		}
		ret = meter_sw_reset(pmic);		/* the next interval */
		if (ret)
			goto out;
	}
	count += sysfs_emit_at(buf, count, "total %llu.%01llu mW\n", total / 1000, (total % 1000) / 100);
out:
	mutex_unlock(&odpm_lock);
	return ret ?: count;
}

static const struct kernel_param_ops interval_ops = { .get = interval_power_get };
module_param_cb(interval_power, &interval_ops, NULL, 0400);
MODULE_PARM_DESC(interval_power, "Average rail power since previous read; first read starts measurement");

static int __init pixel_odpm_init(void)
{
	struct device_node *np;
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	odpm_dev = root_device_register("pixel-odpm");
	if (IS_ERR(odpm_dev))
		return PTR_ERR(odpm_dev);
	np = of_find_node_by_path("/power-management");
	acpm = np ? devm_acpm_get_by_node(odpm_dev, np) : ERR_PTR(-ENODEV);
	of_node_put(np);
	ret = PTR_ERR_OR_ZERO(acpm);
	ret = ret ?: meter_setup(0);
	ret = ret ?: meter_setup(1);
	if (ret) {
		root_device_unregister(odpm_dev);
		return ret;
	}
	pr_info("pixel-odpm: meters on (CTRL1 was %#x / %#x)\n", saved_ctrl1[0], saved_ctrl1[1]);
	return 0;
}
module_init(pixel_odpm_init);

static void __exit pixel_odpm_exit(void)
{
	unsigned int i;
	for (i = 0; i < 2; i++) {
		if (acc_mode_saved[i]) {
			meter_write(i, METER_ACC_MODE, saved_acc_mode[i][0]);
			meter_write(i, METER_ACC_MODE + 1, saved_acc_mode[i][1]);
		}
	}
	meter_write(0, METER_CTRL1, saved_ctrl1[0]);
	meter_write(1, METER_CTRL1, saved_ctrl1[1]);
	meter_write(0, METER_CTRL2, saved_ctrl2[0]);
	meter_write(1, METER_CTRL2, saved_ctrl2[1]);
	root_device_unregister(odpm_dev);
}
module_exit(pixel_odpm_exit);

MODULE_DESCRIPTION("GS201 S2MPG12/S2MPG13 per-rail power meters");
MODULE_LICENSE("GPL");
