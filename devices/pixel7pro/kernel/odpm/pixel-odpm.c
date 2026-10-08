// SPDX-License-Identifier: GPL-2.0-only
/* Per-rail power from the S2MPG12/S2MPG13 on-device power meters (ODPM), and
 * the USB-C port's temperature from the S2MPG13 meter's NTC channels: one
 * owner for both, since they share the sub-PMIC's meter (its CTRL1 and its
 * reset).
 *
 * Power: each PMIC meters 12 channels. This module points them at the rails
 * that matter for idle power and runs the meters at 125 Hz.
 * - parameters/power: the low-pass filtered power of every channel, now.
 * - parameters/energy: each rail's energy (uJ) since the previous read, and
 *   the seconds the meter covered (its sample count at 125 Hz). The hardware
 *   accumulates while the AP sleeps. The first read takes the baseline.
 *   Every read starts a new interval, so there must be one reader (the sleep
 *   policy's battery history); intervals must stay under 2.3 hours (the
 *   20-bit sample count).
 * Register layout and per-rail resolutions from Google's s2mpg1x-meter.h,
 * s2mpg12-powermeter.c and s2mpg13-powermeter.c. External (VSYS shunt)
 * channels read power at VRAIL x VSHUNT x TRIM x 10 / shunt resolution
 * (odpm.c), with the stock DT shunts: 10 mOhm for mmWave, display and
 * WLAN/BT, 5 mOhm for the modem and RF front end.
 *
 * Port temperature (formerly pixel-port-temp): channel 3 is usb_pwr_therm,
 * at the port; 4 is usb_pwr_therm2; 0 is neutral_therm, the board. Stock
 * cuts the port above 42 C at the port, or 2.5 C above usb_pwr_therm2, or
 * 5 C above neutral_therm. pixel_port_temp_mc() (pixel-battery looks it up
 * by name) switches the three channels on at its first call with stock's
 * sequence, and they go off again OFF_AFTER without a call. Every energy
 * read resets the meter, which clears the NTC samples until the next one
 * (1.6 s at 0.625 Hz): a reading within RESET_GRACE of a reset is the last
 * good one, so the charger is not held at its unknown-temperature limit.
 *
 * A buck channel measures only with the buck's bit set in BUCKEN1/2: without
 * it the channel reads a constant (an off GPU buck read 49 mW). Stock's
 * odpm.c sets the bits of every metered buck (the DT's channel-en-byte-offset
 * and channel-en-index); so does this module, also through system sleep,
 * where stock clears the CPU, INT, TPU, camera and GPU bucks' bits.
 *
 * Only the meter bank (0x0A) and PMETER_MRST in the MT_TRIM bank are
 * written. Unloading puts back CTRL1/2/3, BUCKEN1/2 and the accumulator mode.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/workqueue.h>

#define ACPM_PMIC_CHANNEL	2
#define S2MPG13			1	/* the sub PMIC, as ACPM numbers them */
#define METER_BANK		0x0a
#define METER_CTRL1		0x08
#define METER_EN		BIT(0)
#define EXT_METER_EN		BIT(1)
#define INT_SAMP_RATE		GENMASK(4, 2)
#define INT_125HZ		4
#define NTC_RATE		GENMASK(7, 5)
#define NTC_0P625HZ		3
#define METER_CTRL2		0x09
#define EXT_CHANNELS_ALL	(0x7 << 3)
#define EXT_125HZ		4
#define METER_CTRL3		0x0a	/* NTC channel enables, one bit each */
#define METER_ACC_MODE		0x0b
#define METER_BUCKEN1		0x0f	/* +1: BUCKEN2 */
#define METER_CTRL6		0x0d	/* LPF mode, channels 0-7; +1 bits 3:0: 8-11 */
#define METER_MUXSEL0		0x11
#define METER_ACC_DATA		0x63	/* 6 bytes per channel, 41 bits */
#define METER_ACC_COUNT		0xab	/* 3 bytes, 20 bits */
#define METER_LPF_DATA		0xae	/* 3 bytes per channel, 21 bits */
#define METER_LPF_DATA_NTC	0xd4	/* 2 bytes per channel, 12 bits */
#define ASYNC_RD		BIT(7)
#define CHANNELS		12
#define SAMPLE_HZ		125

#define TRIM_BANK		0x0e
#define PMETER_MRST		BIT(7)
static const u8 trim_reg[2] = { 0x29, 0x34 };

#define NTC_NEUTRAL		0
#define NTC_USB			3
#define NTC_USB2		4
/* The rest of stock's skin estimate (VIRTUAL-SKIN in its thermal HAL config):
 * quiet_therm, qi_therm, disp_therm and gnss_tcxo_therm, for pixel-skin-guard
 * through parameters/temps. */
#define NTC_QUIET		1
#define NTC_QI			2
#define NTC_DISP		5
#define NTC_GNSS		7
#define NTC_USED		(BIT(NTC_NEUTRAL) | BIT(NTC_USB) | BIT(NTC_USB2) | BIT(NTC_QUIET) | \
				 BIT(NTC_QI) | BIT(NTC_DISP) | BIT(NTC_GNSS))
#define OFF_AFTER		(60 * HZ)
#define RESET_GRACE		(5 * HZ)

/* Resolutions in nW per LSB (s2mpg1x-register.h, GS201 values) */
#define CMS	6105
#define CMD	12210
#define CMT	18315
#define VM	12210
#define BB	10989
#define DVS_NLDO_150	458
#define DVS_NLDO_800	1221
#define NLDO_300	916
#define NLDO_450	1374
#define NLDO_800	2442
#define PLDO_150	916
#define PLDO_300	1832
#define PLDO_800	4884
#define SHUNT_10MOHM	13953
#define SHUNT_5MOHM	27906

struct rail {
	const char *name;
	u8 muxsel;
	u32 nw_per_lsb;
	u16 bucken;	/* BUCKEN1 | BUCKEN2 << 8; 0 for LDOs and shunts */
};

static const struct rail default_rails[2][CHANNELS] = {
	{	/* S2MPG12, main */
		{ "S1M_VDD_MIF", 0x01, CMS, 0x0001 },
		{ "S2M_VDD_CPUCL2", 0x02, CMT, 0x0002 },
		{ "S3M_VDD_CPUCL1", 0x03, CMD, 0x0004 },
		{ "S4M_VDD_CPUCL0", 0x04, CMS, 0x0008 },
		{ "S5M_VDD_INT", 0x05, CMT, 0x0010 },
		{ "S6M_LLDO1", 0x06, CMS, 0x0020 },
		{ "S7M_VDD_INT_M", 0x07, CMS, 0x0040 },
		{ "VSYS_MMWAVE", 0x5c, SHUNT_10MOHM },
		{ "S9M_LLDO3", 0x09, CMS, 0x0100 },
		{ "VSYS_MODEM", 0x5d, SHUNT_5MOHM },
		{ "L15M_VDD_SLC_M", 0x2f, DVS_NLDO_800 },
		{ "VSYS_RFFE", 0x5e, SHUNT_5MOHM },
	}, {	/* S2MPG13, sub */
		{ "S1S_VDD_CAM", 0x01, CMT, 0x0001 },
		{ "S2S_VDD_G3D", 0x02, CMT, 0x0002 },
		{ "S3S_LLDO1", 0x03, CMS, 0x0004 },
		{ "S4S_VDD2H_MEM", 0x04, CMS, 0x0008 },
		{ "S5S_VDDQ_MEM", 0x05, CMS, 0x0010 },
		{ "S6S_LLDO2", 0x06, CMS, 0x0020 },
		{ "S7S_MLDO", 0x07, VM, 0x0040 },
		{ "VSYS_DISPLAY", 0x5c, SHUNT_10MOHM },
		{ "VSYS_WLAN_BT", 0x5d, SHUNT_10MOHM },
		{ "S10S_LLDO3", 0x0a, CMS, 0x0200 },
		{ "SA", 0x0c, VM, 0x0800 },
		{ "SD", 0x0b, VM, 0x0400 },
	},
};

/* survey=1: the LDOs that stay on in SYS_SLEEP (always-on fields, or
 * single-bit enables), with the sub PMIC's MLDO, SC, SD and buck-boost
 * supplies for reference. survey=2: the LDO supply bucks and the other
 * LDOs. survey=3: every buck left on in sleep (the AoC's among them), the
 * largest LDOs and the shunts. Muxsel LDOn = 0x20 + n; resolutions from
 * s2mpg12/13-powermeter.c. Metering only selects what the meter measures:
 * no rail is switched.
 */
static const struct rail survey_rails[3][2][CHANNELS] = {
	{ {	/* survey 1, S2MPG12 */
		{ "L1M_ALIVE", 0x21, NLDO_300 },
		{ "L2M_ALIVE", 0x22, PLDO_800 },
		{ "L3M_VDD_AOC_RET", 0x23, NLDO_300 },
		{ "L7M_VDD_HSI", 0x27, DVS_NLDO_800 },
		{ "L8M_USB", 0x28, NLDO_300 },
		{ "L9M_USB", 0x29, PLDO_150 },
		{ "L10M_USB", 0x2a, PLDO_150 },
		{ "L16M_PCIE0", 0x30, NLDO_800 },
		{ "L18M_PCIE0", 0x32, PLDO_150 },
		{ "L20M_DMIC1", 0x34, PLDO_150 },
		{ "L21M_GSC", 0x35, PLDO_300 },
		{ "L26M_TS_AVDD", 0x3a, PLDO_300 },
	}, {	/* survey 1, S2MPG13 */
		{ "S7S_MLDO", 0x07, VM, 0x0040 },
		{ "SD", 0x0b, VM, 0x0400 },
		{ "SC", 0x0d, CMS, 0x0800 },
		{ "BB_HLDO", 0x10, BB, 0x1000 },
		{ "L3S_PCIE1", 0x23, NLDO_800 },
		{ "L5S_PROX", 0x25, PLDO_150 },
		{ "L7S_SENSORS", 0x27, PLDO_150 },
		{ "L15S_UDFPS_AVDD", 0x2f, PLDO_150 },
		{ "L18S_PCIE1", 0x32, PLDO_150 },
		{ "L19S_DMIC3", 0x33, PLDO_150 },
		{ "L20S_DMIC2", 0x34, PLDO_150 },
		{ "L21S_VDD2L_MEM", 0x35, NLDO_800 },
	} }, { {	/* survey 2, S2MPG12 */
		{ "S6M_LLDO1", 0x06, CMS, 0x0020 },
		{ "S8M_LLDO2", 0x08, CMS, 0x0080 },
		{ "S9M_LLDO3", 0x09, CMS, 0x0100 },
		{ "L4M_HSI", 0x24, PLDO_150 },
		{ "L5M_TCXO", 0x25, DVS_NLDO_150 },
		{ "L6M_PLL", 0x26, NLDO_450 },
		{ "L13M_VDD_TPU_M", 0x2d, DVS_NLDO_800 },
		{ "L14M_TCXO", 0x2e, PLDO_150 },
		{ "L15M_VDD_SLC_M", 0x2f, DVS_NLDO_800 },
		{ "L25M_TSP_DVDD", 0x39, PLDO_150 },
		{ "L27M_DISP_VCI", 0x3b, PLDO_150 },
		{ "L28M_DISP", 0x3c, NLDO_450 },
	}, {	/* survey 2, S2MPG13 */
		{ "S7S_MLDO", 0x07, VM, 0x0040 },
		{ "S3S_LLDO1", 0x03, CMS, 0x0004 },
		{ "S6S_LLDO2", 0x06, CMS, 0x0020 },
		{ "S10S_LLDO3", 0x0a, CMS, 0x0200 },
		{ "L1S_VDD_G3D_M", 0x21, DVS_NLDO_800 },
		{ "L2S_PLL_MIPI_UFS", 0x22, NLDO_800 },
		{ "L9S_GNSS_CORE", 0x29, DVS_NLDO_150 },
		{ "L10S_GNSS_RF", 0x2a, PLDO_150 },
		{ "L11S_GNSS_AUX", 0x2b, PLDO_150 },
		{ "L12S_CAMIO", 0x2c, PLDO_300 },
		{ "L13S_DPAUX", 0x2d, PLDO_150 },
		{ "L14S_ALIVE", 0x2e, PLDO_300 },
	} }, { {	/* survey 3, S2MPG12 */
		{ "S1M_VDD_MIF", 0x01, CMS, 0x0001 },
		{ "S6M_LLDO1", 0x06, CMS, 0x0020 },
		{ "S8M_LLDO2", 0x08, CMS, 0x0080 },
		{ "S9M_LLDO3", 0x09, CMS, 0x0100 },
		{ "S10M_VDD_TPU", 0x0a, CMT, 0x0200 },
		{ "L1M_ALIVE", 0x21, NLDO_300 },
		{ "L3M_VDD_AOC_RET", 0x23, NLDO_300 },
		{ "VSYS_MMWAVE", 0x5c, SHUNT_10MOHM },
		{ "L8M_USB", 0x28, NLDO_300 },
		{ "VSYS_MODEM", 0x5d, SHUNT_5MOHM },
		{ "L9M_USB", 0x29, PLDO_150 },
		{ "VSYS_RFFE", 0x5e, SHUNT_5MOHM },
	}, {	/* survey 3, S2MPG13 */
		{ "S9S_VDD_AOC", 0x09, CMS, 0x0100 },
		{ "S8S_VDD_G3D_L2", 0x08, CMS, 0x0080 },
		{ "S3S_LLDO1", 0x03, CMS, 0x0004 },
		{ "S4S_VDD2H_MEM", 0x04, CMS, 0x0008 },
		{ "S5S_VDDQ_MEM", 0x05, CMS, 0x0010 },
		{ "S6S_LLDO2", 0x06, CMS, 0x0020 },
		{ "S7S_MLDO", 0x07, VM, 0x0040 },
		{ "VSYS_DISPLAY", 0x5c, SHUNT_10MOHM },
		{ "VSYS_WLAN_BT", 0x5d, SHUNT_10MOHM },
		{ "S10S_LLDO3", 0x0a, CMS, 0x0200 },
		{ "SA", 0x0c, VM, 0x0800 },
		{ "SC", 0x0d, CMS, 0x0800 },
	} },
};

static unsigned int survey;
module_param(survey, uint, 0444);
MODULE_PARM_DESC(survey, "1 to 3: meter a survey set of LDOs and supplies instead of the usual rails");
static const struct rail (*rails)[CHANNELS] = default_rails;

/* Google's NTC voltage code to milli-degrees C, descending in voltage. */
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

static struct device *odpm_dev;
static struct acpm_handle *acpm;
static DEFINE_MUTEX(odpm_lock);
static u8 saved_ctrl1[2], saved_ctrl2[2], saved_ctrl3, saved_bucken[2][2];
static bool energy_started[2];
static bool acc_mode_saved[2];
static u8 saved_acc_mode[2][2];
/* The NTC channels: on, their CTRL1 rate before, the last reset of the sub
 * PMIC's meter and the last good reading. */
static bool ntc_on;
static u8 saved_ntc_rate;
static unsigned long last_reset;
static int last_mc[3];
static unsigned long last_mc_at;
static bool last_mc_valid;
static struct delayed_work off_work;

static int meter_read(unsigned int pmic, u8 reg, u8 *val)
{
	return acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHANNEL, METER_BANK, reg, pmic, val);
}

static int meter_write(unsigned int pmic, u8 reg, u8 val)
{
	return acpm->ops->pmic.write_reg(acpm, ACPM_PMIC_CHANNEL, METER_BANK, reg, pmic, val);
}

/* CTRL1 with only the bits in mask changed. */
static int ctrl1_update(unsigned int pmic, u8 mask, u8 bits)
{
	u8 val;
	int ret = meter_read(pmic, METER_CTRL1, &val);

	return ret ?: meter_write(pmic, METER_CTRL1, (val & ~mask) | bits);
}

static int meter_setup(unsigned int pmic)
{
	unsigned int ch;
	u16 bucken = 0;
	u8 val;
	int ret;

	ret = meter_read(pmic, METER_CTRL1, &saved_ctrl1[pmic]);
	ret = ret ?: meter_read(pmic, METER_CTRL2, &saved_ctrl2[pmic]);
	ret = ret ?: meter_read(pmic, METER_BUCKEN1, &saved_bucken[pmic][0]);
	ret = ret ?: meter_read(pmic, METER_BUCKEN1 + 1, &saved_bucken[pmic][1]);
	for (ch = 0; ch < CHANNELS && !ret; ch++) {
		ret = meter_write(pmic, METER_MUXSEL0 + ch, rails[pmic][ch].muxsel);
		bucken |= rails[pmic][ch].bucken;
	}
	ret = ret ?: meter_write(pmic, METER_BUCKEN1, bucken & 0xff);
	ret = ret ?: meter_write(pmic, METER_BUCKEN1 + 1, bucken >> 8);
	/* LPF in power mode for all 12 channels */
	ret = ret ?: meter_write(pmic, METER_CTRL6, 0x00);
	ret = ret ?: meter_read(pmic, METER_CTRL6 + 1, &val);
	ret = ret ?: meter_write(pmic, METER_CTRL6 + 1, val & ~0x0f);
	ret = ret ?: meter_write(pmic, METER_CTRL2, EXT_CHANNELS_ALL | EXT_125HZ);
	ret = ret ?: ctrl1_update(pmic, INT_SAMP_RATE | METER_EN | EXT_METER_EN,
				  FIELD_PREP(INT_SAMP_RATE, INT_125HZ) | METER_EN | EXT_METER_EN);
	return ret;
}

/* Stock s2mpg1x_meter_sw_reset (GS201): pulse PMETER_MRST, bit 7 of the
 * MT_TRIM bank's common register (the other bits include the NTC enable and
 * stay as found), then METER_EN. It clears the accumulators and their
 * sample count (without it they read back frozen: count pinned at 0xfffff),
 * and on the sub PMIC the NTC samples too.
 */
static int meter_sw_reset(unsigned int pmic)
{
	u8 val;
	int ret;

	ret = acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHANNEL, TRIM_BANK, trim_reg[pmic], pmic, &val);
	ret = ret ?: acpm->ops->pmic.write_reg(acpm, ACPM_PMIC_CHANNEL, TRIM_BANK, trim_reg[pmic],
					       pmic, val & ~PMETER_MRST);
	ret = ret ?: acpm->ops->pmic.write_reg(acpm, ACPM_PMIC_CHANNEL, TRIM_BANK, trim_reg[pmic],
					       pmic, val | PMETER_MRST);
	if (ret)
		return ret;
	usleep_range(100, 1000);
	if (pmic == S2MPG13)
		last_reset = jiffies;
	return ctrl1_update(pmic, METER_EN, METER_EN);
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

/* Stock s2mpg1x_meter_measure_acc: ASYNC_RD copies the accumulators to the
 * readable registers. */
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

/* Energy per rail since the last read, one line each ("NAME uJ"), after a
 * line per PMIC with the seconds its meter covered ("seconds PMIC S"): a
 * reset in between (the NTC channels coming on) leaves only the time since.
 */
static int energy_get(char *buf, const struct kernel_param *kp)
{
	ssize_t count = 0;
	unsigned int pmic, ch;
	int ret = 0;

	mutex_lock(&odpm_lock);
	for (pmic = 0; pmic < 2; pmic++) {
		u8 b[6];
		u32 samples;

		if (!energy_started[pmic]) {
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
			energy_started[pmic] = true;
			count += sysfs_emit_at(buf, count, "baseline %u\n", pmic);
			continue;
		}
		ret = meter_snapshot(pmic);
		ret = ret ?: acpm->ops->pmic.bulk_read(acpm, ACPM_PMIC_CHANNEL,
			METER_BANK, METER_ACC_COUNT, pmic, 3, b);
		if (ret)
			goto out;
		samples = (b[0] | b[1] << 8 | b[2] << 16) & GENMASK(19, 0);
		count += sysfs_emit_at(buf, count, "seconds %u %u.%03u\n", pmic, samples / SAMPLE_HZ,
				       (samples % SAMPLE_HZ) * 1000 / SAMPLE_HZ);
		for (ch = 0; ch < CHANNELS && samples; ch++) {
			u64 raw;

			ret = acpm->ops->pmic.bulk_read(acpm, ACPM_PMIC_CHANNEL,
				METER_BANK, METER_ACC_DATA + 6 * ch, pmic, 6, b);
			if (ret)
				goto out;
			raw = (u64)b[0] | (u64)b[1] << 8 | (u64)b[2] << 16 |
			      (u64)b[3] << 24 | (u64)b[4] << 32 | (u64)(b[5] & 1) << 40;
			/* Each sample is power (LSB x nW), 1/125 s long. */
			count += sysfs_emit_at(buf, count, "%s %llu\n", rails[pmic][ch].name,
				div_u64(raw * rails[pmic][ch].nw_per_lsb, SAMPLE_HZ * 1000));
		}
		ret = meter_sw_reset(pmic);		/* the next interval */
		if (ret)
			goto out;
	}
out:
	mutex_unlock(&odpm_lock);
	return ret ?: count;
}

static const struct kernel_param_ops energy_ops = { .get = energy_get };
module_param_cb(energy, &energy_ops, NULL, 0444);
MODULE_PARM_DESC(energy, "Energy per rail (uJ) since the previous read; one reader, intervals under 2.3 h");

/* Port temperature */

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

/* Stock's channel update (b/200582715): the channel set only takes across a
 * meter reset, twice. Then the NTCs at 0.625 Hz, the meter on. The resets
 * cut the sub PMIC's energy interval short (energy says what it covered). */
static int ntc_enable(void)
{
	u8 ctrl1;
	int ret;

	ret = meter_read(S2MPG13, METER_CTRL1, &ctrl1);
	if (ret)
		return ret;
	saved_ntc_rate = ctrl1 & NTC_RATE;
	ret = meter_sw_reset(S2MPG13);
	ret = ret ?: meter_write(S2MPG13, METER_CTRL3, 0);
	usleep_range(100, 10000);
	ret = ret ?: meter_sw_reset(S2MPG13);
	ret = ret ?: meter_write(S2MPG13, METER_CTRL3, NTC_USED);
	ret = ret ?: ctrl1_update(S2MPG13, NTC_RATE | METER_EN,
				  FIELD_PREP(NTC_RATE, NTC_0P625HZ) | METER_EN);
	if (!ret)
		ntc_on = true;
	dev_info(odpm_dev, "NTC channels %s\n", ret ? "failed" : "on");
	return ret;
}

static void ntc_disable(void)
{
	if (!ntc_on)
		return;
	meter_write(S2MPG13, METER_CTRL3, 0);
	/* Only the NTC rate goes back: the power meter stays on. */
	ctrl1_update(S2MPG13, NTC_RATE, saved_ntc_rate);
	ntc_on = false;
	last_mc_valid = false;
	dev_info(odpm_dev, "NTC channels off\n");
}

static void off_work_fn(struct work_struct *work)
{
	mutex_lock(&odpm_lock);
	ntc_disable();
	mutex_unlock(&odpm_lock);
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

/* The port, the second port sensor and the board, in milli-degrees C. Just
 * after a meter reset (an energy read), the last good reading. */
int pixel_port_temp_mc(int *usb, int *usb2, int *neutral);	/* looked up by name */
int pixel_port_temp_mc(int *usb, int *usb2, int *neutral)
{
	u8 enabled;
	int ret = 0, mc[3];

	if (!acpm)
		return -ENODEV;
	mutex_lock(&odpm_lock);
	if (!ntc_on)
		ret = ntc_enable();
	mod_delayed_work(system_wq, &off_work, OFF_AFTER);
	ret = ret ?: meter_read(S2MPG13, METER_CTRL3, &enabled);
	ret = ret ?: channel_mc(NTC_USB, enabled, &mc[0]);
	ret = ret ?: channel_mc(NTC_USB2, enabled, &mc[1]);
	ret = ret ?: channel_mc(NTC_NEUTRAL, enabled, &mc[2]);
	if (!ret) {
		memcpy(last_mc, mc, sizeof(mc));
		last_mc_at = jiffies;
		last_mc_valid = true;
	} else if (ret == -EBUSY && last_mc_valid && time_before(jiffies, last_reset + RESET_GRACE) &&
		   time_before(jiffies, last_mc_at + OFF_AFTER)) {
		memcpy(mc, last_mc, sizeof(mc));
		ret = 0;
	}
	if (!ret) {
		*usb = mc[0];
		*usb2 = mc[1];
		*neutral = mc[2];
	}
	mutex_unlock(&odpm_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(pixel_port_temp_mc);

static int temps_get(char *buf, const struct kernel_param *kp)
{
	u8 ctrl1, ctrl3, b[2];
	int len = 0, ch, ret, mc;

	/* As a reader would: the channels on, for OFF_AFTER. */
	pixel_port_temp_mc(&mc, &mc, &mc);
	mutex_lock(&odpm_lock);
	ret = meter_read(S2MPG13, METER_CTRL1, &ctrl1);
	ret = ret ?: meter_read(S2MPG13, METER_CTRL3, &ctrl3);
	if (ret)
		goto out;
	len = sysfs_emit(buf, "ctrl1 %#04x ctrl3 %#04x\n", ctrl1, ctrl3);
	for (ch = 0; ch < 8; ch++) {
		ret = acpm->ops->pmic.bulk_read(acpm, ACPM_PMIC_CHANNEL, METER_BANK,
						METER_LPF_DATA_NTC + 2 * ch, S2MPG13, 2, b);
		if (ret)
			goto out;
		ret = channel_mc(ch, ctrl3, &mc);
		len += sysfs_emit_at(buf, len, "ntc%d raw %#05x %s", ch, b[0] | (b[1] & 0x0f) << 8,
				     ch == NTC_USB ? "usb " : ch == NTC_USB2 ? "usb2 " :
				     ch == NTC_NEUTRAL ? "neutral " : ch == NTC_QUIET ? "quiet " :
				     ch == NTC_QI ? "qi " : ch == NTC_DISP ? "disp " :
				     ch == NTC_GNSS ? "gnss " : "");
		len += ret ? sysfs_emit_at(buf, len, "(%d)\n", ret) :
			     sysfs_emit_at(buf, len, "%d.%d C\n", mc / 1000, abs(mc % 1000) / 100);
	}
	ret = 0;
out:
	mutex_unlock(&odpm_lock);
	return ret ?: len;
}
static const struct kernel_param_ops temps_ops = { .get = temps_get };
module_param_cb(temps, &temps_ops, NULL, 0444);
MODULE_PARM_DESC(temps, "The S2MPG13 NTC channels: raw codes and degrees C");

static int __init pixel_odpm_init(void)
{
	struct device_node *np;
	struct acpm_handle *h;
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	if (survey > ARRAY_SIZE(survey_rails))
		return -EINVAL;
	odpm_dev = root_device_register("pixel-odpm");
	if (IS_ERR(odpm_dev))
		return PTR_ERR(odpm_dev);
	np = of_find_node_by_path("/power-management");
	h = np ? devm_acpm_get_by_node(odpm_dev, np) : ERR_PTR(-ENODEV);
	of_node_put(np);
	ret = PTR_ERR_OR_ZERO(h);
	if (ret) {
		root_device_unregister(odpm_dev);
		return ret;
	}
	acpm = h;
	if (survey)
		rails = survey_rails[survey - 1];
	INIT_DELAYED_WORK(&off_work, off_work_fn);
	mutex_lock(&odpm_lock);
	ret = meter_read(S2MPG13, METER_CTRL3, &saved_ctrl3);
	ret = ret ?: meter_setup(0);
	ret = ret ?: meter_setup(1);
	mutex_unlock(&odpm_lock);
	if (ret) {
		acpm = NULL;
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

	cancel_delayed_work_sync(&off_work);
	mutex_lock(&odpm_lock);
	ntc_disable();
	for (i = 0; i < 2; i++) {
		if (acc_mode_saved[i]) {
			meter_write(i, METER_ACC_MODE, saved_acc_mode[i][0]);
			meter_write(i, METER_ACC_MODE + 1, saved_acc_mode[i][1]);
		}
		meter_write(i, METER_CTRL1, saved_ctrl1[i]);
		meter_write(i, METER_CTRL2, saved_ctrl2[i]);
		meter_write(i, METER_BUCKEN1, saved_bucken[i][0]);
		meter_write(i, METER_BUCKEN1 + 1, saved_bucken[i][1]);
	}
	meter_write(S2MPG13, METER_CTRL3, saved_ctrl3);
	acpm = NULL;
	mutex_unlock(&odpm_lock);
	root_device_unregister(odpm_dev);
}
module_exit(pixel_odpm_exit);

MODULE_DESCRIPTION("GS201 S2MPG12/S2MPG13 per-rail power meters and USB-C port temperature");
MODULE_LICENSE("GPL");
