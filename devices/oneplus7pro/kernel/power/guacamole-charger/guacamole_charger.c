// SPDX-License-Identifier: GPL-2.0-only
/*
 * OnePlus 7 Pro (guacamole) PM8150B charging with the stock charge profile.
 *
 * Bound to the PMIC charger by driver_override in place of the built-in
 * guarded bring-up policy (500 mA, 4.20 V), which it hands back on unbind.
 * The profile is the stock one from the phone's own device tree
 * (18857/sm8150-mtp.dtsi, oplus charging framework): 4.39 V float and up to
 * 3 A between 16 and 45 C, less current when cool, 4.13 V and 1.05 A when
 * warm, nothing from 50 C. Charging below 0 C, which stock allows at 320 mA,
 * is not done here. The input limit follows the charger type (BC1.2 APSD):
 * 500 mA from a computer port, 1.5 A from a charging port, 2 A from a
 * wall charger. No fast-charge protocol (Warp, PD, QC) is spoken.
 *
 * charge_control_end_threshold (50-100 %) holds charging at that level and
 * resumes 5 % below it, to keep the battery lower for its lifespan.
 *
 * Charging stops and latches on: battery voltage from 4.445 V (the stock
 * over-voltage limit), battery current well above the programmed current,
 * a register that does not read back as written, the charger's own battery
 * over-voltage flag, or 10 hours of continuous charging. A latch clears on
 * unplug, at most three times per boot.
 */
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define CHG_STATUS_1		0x06	/* charge state in bits 2:0 */
#define CHG_STATUS_2		0x07
#define CHG_STATUS_BAT_OV	BIT(1)
#define CHG_ENABLE_CMD		0x42
#define FCC_CFG			0x61	/* 50 mA per step */
#define FV_CFG			0x70	/* 3.60 V + 10 mV per step */
#define POWER_PATH_STATUS	0x10b
#define PATH_USBIN		BIT(4)
#define PATH_VALID		BIT(0)
#define APSD_STATUS		0x307
#define APSD_DONE		BIT(0)
#define APSD_RESULT		0x308
#define APSD_FLOAT		BIT(4)
#define APSD_DCP		BIT(3)
#define APSD_CDP		BIT(2)
#define APSD_OCP		BIT(1)
#define CMD_ICL_OVERRIDE	0x342
#define USBIN_ICL_OPTIONS	0x366
#define USBIN_MODE_HC		BIT(0)
#define USBIN_ICL_CFG		0x370	/* 50 mA per step */
#define USBIN_AICL_OPTIONS	0x380

/* The bring-up driver's limits, restored when this one lets go. */
#define GUARDED_FCC		10	/* 500 mA */
#define GUARDED_FV		60	/* 4.20 V */
#define GUARDED_ICL		10	/* 500 mA */

#define MAX_FV_MV		4390
#define LATCH_MV		4445
#define RECHARGE_DROP_MV	100
#define MAX_CHARGE_TIME		(10 * 3600 * HZ)
#define MAX_LATCHES		3

struct band {
	int from_dc;		/* battery temperature at or above, 0.1 C */
	int fv_mv;
	int fcc_ma;
};

/* Stock 18857 bands; below 0 C and from 50 C there is no charging. */
static const struct band bands[] = {
	{   0, 4390, 1450 },	/* little cold, 0-5 C */
	{  50, 4390, 2100 },	/* cool, 5-12 C */
	{ 120, 4390, 2100 },	/* little cool, 12-16 C */
	{ 160, 4390, 3000 },	/* normal, 16-45 C */
	{ 450, 4130, 1050 },	/* warm, 45-50 C */
};
#define HOT_DC			500
#define HYSTERESIS_DC		20

struct gchg {
	struct device *dev;
	struct regmap *regmap;
	u32 base;
	struct power_supply *psy, *gauge;
	struct delayed_work work;
	struct mutex lock;
	bool stopping;

	int band;		/* index into bands, -1 cold, -2 hot */
	bool enabled;		/* charging command as last written */
	bool online, full, limited, latched, timed_out;
	int latches;
	int usb_type;
	int fv_mv, fcc_ma, icl_ma;
	int end_threshold;	/* percent, 100 = off */
	unsigned int icl_options;	/* USBIN_ICL_OPTIONS as found, restored on release */
	unsigned long started;	/* jiffies when this charge began */
	int high_current;	/* consecutive over-current samples */
	const char *reason;
};

static int gchg_read(struct gchg *c, unsigned int reg, unsigned int *val)
{
	return regmap_read(c->regmap, c->base + reg, val);
}

static int gchg_write(struct gchg *c, unsigned int reg, unsigned int val)
{
	unsigned int actual;
	int ret = regmap_write(c->regmap, c->base + reg, val);

	if (!ret)
		ret = regmap_read(c->regmap, c->base + reg, &actual);
	if (!ret && actual != val)
		ret = -EIO;
	return ret;
}

static int gchg_gauge(struct gchg *c, enum power_supply_property prop, int *val)
{
	union power_supply_propval v;
	int ret = power_supply_get_property(c->gauge, prop, &v);

	if (!ret)
		*val = v.intval;
	return ret;
}

static int gchg_set_enabled(struct gchg *c, bool on)
{
	int ret = gchg_write(c, CHG_ENABLE_CMD, on ? 1 : 0);

	if (!ret)
		c->enabled = on;
	return ret;
}

static void gchg_latch(struct gchg *c, const char *why)
{
	c->latched = true;
	c->latches++;
	c->reason = why;
	gchg_set_enabled(c, false);
	dev_err(c->dev, "charging latched off: %s (%d this boot)\n", why, c->latches);
}

static int gchg_usb_type(struct gchg *c)
{
	unsigned int status, result;

	if (gchg_read(c, APSD_STATUS, &status) || !(status & APSD_DONE) ||
	    gchg_read(c, APSD_RESULT, &result))
		return POWER_SUPPLY_USB_TYPE_UNKNOWN;
	if (result & APSD_CDP)
		return POWER_SUPPLY_USB_TYPE_CDP;
	if (result & (APSD_DCP | APSD_OCP))
		return POWER_SUPPLY_USB_TYPE_DCP;
	if (result & APSD_FLOAT)
		return POWER_SUPPLY_USB_TYPE_UNKNOWN;	/* data lines floating: be cautious */
	return POWER_SUPPLY_USB_TYPE_SDP;
}

static int gchg_icl_ma(int type)
{
	switch (type) {
	case POWER_SUPPLY_USB_TYPE_DCP:
		return 2000;
	case POWER_SUPPLY_USB_TYPE_CDP:
		return 1500;
	default:
		return 500;
	}
}

/* How much a band charges: moving to less happens at once, to more only
 * once the temperature is 2 C past the boundary. -1 cold, -2 hot. */
static int gchg_rank(int band)
{
	static const int rank[] = { 1, 2, 2, 3, 1 };

	return band < 0 ? 0 : rank[band];
}

static int gchg_band_of(int temp_dc)
{
	int i;

	if (temp_dc >= HOT_DC)
		return -2;
	for (i = ARRAY_SIZE(bands) - 1; i >= 0; i--)
		if (temp_dc >= bands[i].from_dc)
			return i;
	return -1;
}

static int gchg_band(int previous, int temp_dc)
{
	int next = gchg_band_of(temp_dc);
	int boundary;

	if (next == previous || gchg_rank(next) < gchg_rank(previous))
		return next;
	/* The boundary crossed between the two bands. */
	if (previous == -2 || next == -2)
		boundary = HOT_DC;
	else if (previous == -1 || next == -1)
		boundary = bands[0].from_dc;
	else
		boundary = bands[max(previous, next)].from_dc;
	return abs(temp_dc - boundary) >= HYSTERESIS_DC ? next : previous;
}

/* Program FV, FCC and ICL; only registers that change are written. */
static int gchg_program(struct gchg *c, int fv_mv, int fcc_ma, int icl_ma)
{
	int ret = 0;

	fv_mv = min(fv_mv, MAX_FV_MV);
	if (fv_mv != c->fv_mv) {
		ret = gchg_write(c, FV_CFG, (fv_mv - 3600) / 10);
		if (!ret)
			c->fv_mv = fv_mv;
	}
	if (!ret && fcc_ma != c->fcc_ma) {
		ret = gchg_write(c, FCC_CFG, fcc_ma / 50);
		if (!ret)
			c->fcc_ma = fcc_ma;
	}
	if (!ret && icl_ma != c->icl_ma) {
		ret = gchg_write(c, USBIN_ICL_CFG, icl_ma / 50);
		if (!ret)
			c->icl_ma = icl_ma;
	}
	return ret;
}

/* The registers must still hold what was written. */
static bool gchg_verify(struct gchg *c)
{
	unsigned int fv, fcc, icl;

	if (gchg_read(c, FV_CFG, &fv) || gchg_read(c, FCC_CFG, &fcc) ||
	    gchg_read(c, USBIN_ICL_CFG, &icl))
		return false;
	return fv == (c->fv_mv - 3600) / 10 && fcc == c->fcc_ma / 50 &&
	       icl == c->icl_ma / 50;
}

static void gchg_unplugged(struct gchg *c)
{
	if (c->latched && c->latches < MAX_LATCHES) {
		c->latched = false;
		dev_info(c->dev, "unplugged: charging latch cleared\n");
	}
	c->full = false;
	c->limited = false;
	c->timed_out = false;
	c->started = 0;
	c->high_current = 0;
}

static void gchg_work(struct work_struct *work)
{
	struct gchg *c = container_of(work, struct gchg, work.work);
	int temp, uv, ua, capacity = -1, present = 0, band, target;
	unsigned int path, status1, status2;
	bool online, allow, changed = false;
	const char *reason = NULL;
	int ret;

	mutex_lock(&c->lock);
	if (c->stopping)
		goto out;

	ret = gchg_read(c, POWER_PATH_STATUS, &path);
	online = !ret && (path & PATH_USBIN) && (path & PATH_VALID);
	if (online != c->online) {
		c->online = online;
		changed = true;
		if (!online)
			gchg_unplugged(c);
	}
	if (!online) {
		if (c->enabled)
			gchg_set_enabled(c, false);
		goto reschedule;
	}

	if (gchg_gauge(c, POWER_SUPPLY_PROP_PRESENT, &present) || !present ||
	    gchg_gauge(c, POWER_SUPPLY_PROP_TEMP, &temp) ||
	    gchg_gauge(c, POWER_SUPPLY_PROP_VOLTAGE_NOW, &uv) ||
	    gchg_gauge(c, POWER_SUPPLY_PROP_CURRENT_NOW, &ua)) {
		reason = "battery gauge unavailable";
		allow = false;
		goto apply;
	}
	gchg_gauge(c, POWER_SUPPLY_PROP_CAPACITY, &capacity);

	if (c->latched) {
		reason = c->reason;
		allow = false;
		goto apply;
	}
	if (uv >= LATCH_MV * 1000) {
		gchg_latch(c, "battery over-voltage");
		reason = c->reason;
		allow = false;
		goto apply;
	}
	if (!gchg_read(c, CHG_STATUS_2, &status2) && (status2 & CHG_STATUS_BAT_OV)) {
		gchg_latch(c, "charger reports battery over-voltage");
		reason = c->reason;
		allow = false;
		goto apply;
	}

	band = gchg_band(c->band, temp);
	if (band != c->band) {
		dev_info(c->dev, "battery %d.%d C: %s\n", temp / 10, abs(temp % 10),
			 band == -1 ? "too cold to charge" : band == -2 ? "too hot to charge" :
			 "charge band changed");
		c->band = band;
		changed = true;
	}
	c->usb_type = gchg_usb_type(c);

	if (band < 0) {
		reason = band == -1 ? "battery too cold" : "battery too hot";
		allow = false;
		goto apply;
	}

	/* The charge limit: hold at the threshold, resume 5 % below it. */
	if (c->end_threshold < 100 && capacity >= 0) {
		if (capacity >= c->end_threshold)
			c->limited = true;
		else if (capacity <= c->end_threshold - 5)
			c->limited = false;
	} else {
		c->limited = false;
	}
	if (c->limited) {
		reason = "held at the charge limit";
		allow = false;
		goto apply;
	}

	/* Full: the charger terminated; recharge 100 mV under the float. */
	if (!gchg_read(c, CHG_STATUS_1, &status1) && (status1 & 7) == 5 && c->enabled && !c->full) {
		c->full = true;
		changed = true;
		dev_info(c->dev, "charge complete at %d mV\n", uv / 1000);
	}
	if (c->full && uv < (bands[band].fv_mv - RECHARGE_DROP_MV) * 1000) {
		c->full = false;
		c->started = 0;
		changed = true;
	}
	if (c->full) {
		reason = "full";
		allow = false;
		goto apply;
	}

	if (c->timed_out || (c->started && time_after(jiffies, c->started + MAX_CHARGE_TIME))) {
		if (!c->timed_out)
			dev_warn(c->dev, "10 hours of charging: stopping until unplugged\n");
		c->timed_out = true;
		reason = "charge time limit";
		allow = false;
		goto apply;
	}

	target = min(bands[band].fcc_ma, 3000);
	if (gchg_program(c, bands[band].fv_mv, target, gchg_icl_ma(c->usb_type))) {
		gchg_latch(c, "charger register write failed");
		reason = c->reason;
		allow = false;
		goto apply;
	}
	/* Gauge current is positive while charging; allow for its lag. */
	if (c->enabled && ua > (c->fcc_ma + 400) * 1000) {
		if (++c->high_current >= 3) {
			gchg_latch(c, "battery current above the programmed limit");
			reason = c->reason;
			allow = false;
			goto apply;
		}
	} else {
		c->high_current = 0;
	}
	allow = true;

apply:
	if (allow && !gchg_verify(c)) {
		gchg_latch(c, "charger registers changed underneath");
		reason = c->reason;
		allow = false;
	}
	if (allow != c->enabled) {
		if (gchg_set_enabled(c, allow) && allow)
			gchg_latch(c, "charge enable did not take");
		if (allow && !c->started)
			c->started = jiffies;
		dev_info(c->dev, "charging %s (%s, %d mV, %d mA, input %d mA)\n",
			 c->enabled ? "on" : "off", c->enabled ? "ok" : (reason ? reason : "-"),
			 c->fv_mv, c->fcc_ma, c->icl_ma);
		changed = true;
	}
	c->reason = allow ? NULL : reason;
reschedule:
	if (changed)
		power_supply_changed(c->psy);
	schedule_delayed_work(&c->work, msecs_to_jiffies(c->online ? 2000 : 10000));
out:
	mutex_unlock(&c->lock);
}

static const enum power_supply_property gchg_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static int gchg_get(struct power_supply *psy, enum power_supply_property prop,
		    union power_supply_propval *val)
{
	struct gchg *c = power_supply_get_drvdata(psy);
	int ret = 0;

	mutex_lock(&c->lock);
	switch (prop) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = c->online;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (!c->online)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (c->full)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else if (c->enabled)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = c->latched ? POWER_SUPPLY_HEALTH_UNSPEC_FAILURE :
			      c->band == -2 ? POWER_SUPPLY_HEALTH_OVERHEAT :
			      c->band == -1 ? POWER_SUPPLY_HEALTH_COLD :
			      c->timed_out ? POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE :
			      POWER_SUPPLY_HEALTH_GOOD;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = c->online ? c->usb_type : POWER_SUPPLY_USB_TYPE_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = c->icl_ma * 1000;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		val->intval = c->fcc_ma * 1000;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		val->intval = c->fv_mv * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD:
		val->intval = c->end_threshold < 100 ? c->end_threshold - 5 : 100;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		val->intval = c->end_threshold;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Qualcomm";
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "PM8150B (stock OnePlus 7 Pro profile)";
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&c->lock);
	return ret;
}

static int gchg_set(struct power_supply *psy, enum power_supply_property prop,
		    const union power_supply_propval *val)
{
	struct gchg *c = power_supply_get_drvdata(psy);

	if (prop != POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD)
		return -EINVAL;
	if (val->intval < 50 || val->intval > 100)
		return -EINVAL;
	mutex_lock(&c->lock);
	c->end_threshold = val->intval;
	c->limited = false;
	mutex_unlock(&c->lock);
	mod_delayed_work(system_wq, &c->work, 0);
	return 0;
}

static int gchg_writeable(struct power_supply *psy, enum power_supply_property prop)
{
	return prop == POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD;
}

static const struct power_supply_desc gchg_desc = {
	.name = "pm8150b-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) | BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP) | BIT(POWER_SUPPLY_USB_TYPE_CDP),
	.properties = gchg_props,
	.num_properties = ARRAY_SIZE(gchg_props),
	.get_property = gchg_get,
	.set_property = gchg_set,
	.property_is_writeable = gchg_writeable,
};

/* Stop charging and hand back the bring-up limits. */
static void gchg_release(void *data)
{
	struct gchg *c = data;

	mutex_lock(&c->lock);
	c->stopping = true;
	mutex_unlock(&c->lock);
	cancel_delayed_work_sync(&c->work);
	gchg_set_enabled(c, false);
	gchg_write(c, FCC_CFG, GUARDED_FCC);
	gchg_write(c, FV_CFG, GUARDED_FV);
	gchg_write(c, USBIN_ICL_CFG, GUARDED_ICL);
	regmap_update_bits(c->regmap, c->base + USBIN_ICL_OPTIONS, USBIN_MODE_HC,
			   c->icl_options & USBIN_MODE_HC);
	dev_info(c->dev, "released: charging off, bring-up limits restored\n");
}

static int gchg_probe(struct platform_device *pdev)
{
	struct power_supply_config config = {};
	struct device *dev = &pdev->dev;
	unsigned int reg[8];
	struct gchg *c;
	int ret, i;
	static const unsigned int dump[] = {
		FCC_CFG, FV_CFG, USBIN_ICL_CFG, USBIN_ICL_OPTIONS, CMD_ICL_OVERRIDE,
		USBIN_AICL_OPTIONS, APSD_RESULT, POWER_PATH_STATUS,
	};

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = dev;
	c->regmap = dev_get_regmap(dev->parent, NULL);
	if (!c->regmap)
		return -ENODEV;
	ret = device_property_read_u32(dev, "reg", &c->base);
	if (ret)
		return dev_err_probe(dev, ret, "no base address\n");

	/* Off before anything else, as the bring-up driver does. */
	ret = gchg_write(c, CHG_ENABLE_CMD, 0);
	if (ret)
		return dev_err_probe(dev, ret, "could not stop charging\n");

	c->gauge = devm_power_supply_get_by_reference(dev, "qcom,external-fuel-gauge");
	if (IS_ERR_OR_NULL(c->gauge))
		return dev_err_probe(dev, c->gauge ? PTR_ERR(c->gauge) : -EPROBE_DEFER,
				     "battery gauge unavailable\n");

	for (i = 0; i < ARRAY_SIZE(dump); i++)
		if (gchg_read(c, dump[i], &reg[i]))
			reg[i] = 0xffff;
	dev_info(dev, "at probe: FCC %#x FV %#x ICL %#x ICL_OPT %#x ICL_OVR %#x AICL %#x APSD %#x PATH %#x\n",
		 reg[0], reg[1], reg[2], reg[3], reg[4], reg[5], reg[6], reg[7]);

	c->icl_options = reg[3];
	/* High-current mode, so the ICL register applies beyond USB 2.0's 500 mA. */
	ret = regmap_update_bits(c->regmap, c->base + USBIN_ICL_OPTIONS, USBIN_MODE_HC, USBIN_MODE_HC);
	if (ret)
		return ret;
	/* Start from the bring-up limits; the worker raises them per band and charger. */
	c->fv_mv = 3600 + GUARDED_FV * 10;
	c->fcc_ma = GUARDED_FCC * 50;
	c->icl_ma = GUARDED_ICL * 50;
	ret = gchg_write(c, FV_CFG, GUARDED_FV) ?: gchg_write(c, FCC_CFG, GUARDED_FCC) ?:
	      gchg_write(c, USBIN_ICL_CFG, GUARDED_ICL);
	if (ret)
		return ret;

	c->band = 3;		/* normal, until the first reading says otherwise */
	c->end_threshold = 100;
	c->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	mutex_init(&c->lock);
	INIT_DELAYED_WORK(&c->work, gchg_work);

	config.drv_data = c;
	config.fwnode = dev_fwnode(dev);
	c->psy = devm_power_supply_register(dev, &gchg_desc, &config);
	if (IS_ERR(c->psy))
		return PTR_ERR(c->psy);
	ret = devm_add_action_or_reset(dev, gchg_release, c);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, c);
	schedule_delayed_work(&c->work, 0);
	dev_info(dev, "stock OnePlus 7 Pro charge profile: up to 4.39 V, 3 A, input by charger type\n");
	return 0;
}

static int gchg_suspend(struct device *dev)
{
	struct gchg *c = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&c->work);
	mutex_lock(&c->lock);
	gchg_set_enabled(c, false);
	mutex_unlock(&c->lock);
	return 0;
}

static int gchg_resume(struct device *dev)
{
	struct gchg *c = dev_get_drvdata(dev);

	schedule_delayed_work(&c->work, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(gchg_pm, gchg_suspend, gchg_resume);

static void gchg_shutdown(struct platform_device *pdev)
{
	struct gchg *c = platform_get_drvdata(pdev);

	if (c)
		gchg_release(c);
}

/* No match table: bound only through driver_override. */
static struct platform_driver gchg_driver = {
	.probe = gchg_probe,
	.shutdown = gchg_shutdown,
	.driver = {
		.name = "guacamole-charger",
		.pm = pm_sleep_ptr(&gchg_pm),
	},
};
module_platform_driver(gchg_driver);

MODULE_DESCRIPTION("OnePlus 7 Pro PM8150B charging with the stock charge profile");
MODULE_LICENSE("GPL");
