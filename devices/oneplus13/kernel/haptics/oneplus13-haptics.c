// SPDX-License-Identifier: GPL-2.0-only
/*
 * OnePlus 13 (CPH2655) vibration motor: PMIH0108 HAP530_HV haptics, direct play.
 *
 * Exposes an FF_RUMBLE input device. The bootloader leaves the haptics module
 * enabled and tuned for this LRA (Vmax 2.4 V, open-loop period, brake), so only
 * Qualcomm's DIRECT_PLAY sequence is used (qcom-hv-haptics.c, OnePlus kernel
 * source): clear faults, ignore SoundWire triggers, set the amplitude, play with
 * brake; on stop wait until the brake pattern has finished.
 *
 * The registers sit in PMIH0108 (SPMI SID 7) at 0xf000 (config), 0xf100
 * (pattern) and 0xf200 (boost, hardware-follow). The module reaches the PMIC's
 * regmap through its eUSB2 repeater child, so no device-tree change is needed.
 */
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define CFG		0xf000
#define PTN		0xf100
#define HBST		0xf200

#define CFG_REVISION1		0x00
#define CFG_HPWR_INTF		0x0b
#define  HPWR_INTF_STATUS	GENMASK(1, 0)
#define CFG_FAULT_STATUS	0x0c
#define CFG_EN_CTL		0x46
#define  HAPTICS_EN		BIT(7)
#define CFG_VMAX		0x48
#define CFG_TRIG_PRIORITY	0x4b
#define  SWR_IGNORE		BIT(4)
#define CFG_SPMI_PLAY		0x4c
#define  PLAY_EN		BIT(7)
#define  BRAKE_EN		BIT(3)
#define  SRC_DIRECT_PLAY	1
#define CFG_AUTORES		0x63
#define  AUTORES_EN		BIT(7)
#define CFG_FAULT_CLR		0x66
#define  FAULT_CLR_ALL		(BIT(4) | BIT(2) | BIT(1) | BIT(0))
#define CFG_VSET		0x68
#define  FORCE_VREG_RDY		(BIT(1) | BIT(0))
#define PTN_REVISION1		0x00
#define PTN_DIRECT_PLAY		0x26
#define HBST_HW_CTRL_FOLLOW	0x41
#define  FOLLOW_HW_EN		BIT(7)
#define HBST_VREG_EN		0x46

#define T_LRA_US	7692	/* qcom,lra-period-us */
#define MAX_PLAY_MS	5000

struct op13_haptics {
	struct regmap *map;
	struct device *dev;
	struct input_dev *input;
	struct work_struct work;
	struct delayed_work guard;
	struct mutex lock;
	u8 amplitude;	/* requested by ff-memless */
	bool playing;
};

static int hap_write(struct op13_haptics *h, unsigned int reg, u8 val)
{
	return regmap_write(h->map, reg, val);
}

static void hap_stop(struct op13_haptics *h)
{
	unsigned int val;
	int i;

	if (!h->playing)
		return;
	hap_write(h, CFG + CFG_SPMI_PLAY, SRC_DIRECT_PLAY | BRAKE_EN);
	/* Brake pattern plus a cycle of margin, polled every half LRA period. */
	for (i = 0; i < 12; i++) {
		usleep_range(T_LRA_US / 2, T_LRA_US / 2 + 100);
		if (!regmap_read(h->map, CFG + CFG_HPWR_INTF, &val) &&
		    !(val & HPWR_INTF_STATUS))
			break;
	}
	if (i == 12) {
		dev_warn(h->dev, "brake did not finish; toggling module enable\n");
		usleep_range(500, 600);
		hap_write(h, CFG + CFG_EN_CTL, 0);
		usleep_range(500, 600);
		hap_write(h, CFG + CFG_EN_CTL, HAPTICS_EN);
	}
	usleep_range(500, 600);	/* discharge VNDRV (HAP530 workaround) */
	regmap_update_bits(h->map, CFG + CFG_TRIG_PRIORITY, SWR_IGNORE, 0);
	h->playing = false;
	cancel_delayed_work(&h->guard);
}

static void hap_start(struct op13_haptics *h, u8 amplitude)
{
	unsigned int fault = 0;

	regmap_update_bits(h->map, CFG + CFG_TRIG_PRIORITY, SWR_IGNORE, SWR_IGNORE);
	hap_write(h, PTN + PTN_DIRECT_PLAY, amplitude);
	if (h->playing)
		return;
	regmap_read(h->map, CFG + CFG_FAULT_STATUS, &fault);
	if (fault)
		dev_dbg(h->dev, "clearing fault status %02x\n", fault);
	/* OnePlus plays direct mode open loop: no auto-resonance tracking. */
	regmap_update_bits(h->map, CFG + CFG_AUTORES, AUTORES_EN, 0);
	hap_write(h, CFG + CFG_FAULT_CLR, FAULT_CLR_ALL);
	regmap_update_bits(h->map, CFG + CFG_VSET, FORCE_VREG_RDY, 0);
	hap_write(h, CFG + CFG_SPMI_PLAY, SRC_DIRECT_PLAY | BRAKE_EN | PLAY_EN);
	h->playing = true;
	schedule_delayed_work(&h->guard, msecs_to_jiffies(MAX_PLAY_MS));
}

static void hap_work(struct work_struct *work)
{
	struct op13_haptics *h = container_of(work, struct op13_haptics, work);
	u8 amplitude = READ_ONCE(h->amplitude);

	mutex_lock(&h->lock);
	if (amplitude)
		hap_start(h, amplitude);
	else
		hap_stop(h);
	mutex_unlock(&h->lock);
}

/* Never leave the motor running if userspace dies mid-effect. */
static void hap_guard(struct work_struct *work)
{
	struct op13_haptics *h = container_of(to_delayed_work(work), struct op13_haptics, guard);

	mutex_lock(&h->lock);
	if (h->playing) {
		dev_warn(h->dev, "stopping vibration after %d ms\n", MAX_PLAY_MS);
		hap_stop(h);
	}
	mutex_unlock(&h->lock);
}

static int hap_play(struct input_dev *dev, void *data, struct ff_effect *effect)
{
	struct op13_haptics *h = input_get_drvdata(dev);
	u32 mag = max(effect->u.rumble.strong_magnitude, effect->u.rumble.weak_magnitude);

	WRITE_ONCE(h->amplitude, mag ? max(1u, mag * 255 / 0xffff) : 0);
	schedule_work(&h->work);
	return 0;
}

static void hap_close(struct input_dev *dev)
{
	struct op13_haptics *h = input_get_drvdata(dev);

	WRITE_ONCE(h->amplitude, 0);
	cancel_work_sync(&h->work);
	mutex_lock(&h->lock);
	hap_stop(h);
	mutex_unlock(&h->lock);
}

static struct op13_haptics *haptics;

static struct regmap *pmic_regmap(struct device **pmic)
{
	struct device_node *np, *child;
	struct platform_device *pdev;
	struct regmap *map = NULL;

	np = of_find_compatible_node(NULL, NULL, "qcom,pmih0108");
	if (!np)
		return NULL;
	child = of_get_compatible_child(np, "qcom,pm8550b-eusb2-repeater");
	of_node_put(np);
	if (!child)
		return NULL;
	pdev = of_find_device_by_node(child);
	of_node_put(child);
	if (!pdev)
		return NULL;
	if (pdev->dev.parent) {
		map = dev_get_regmap(pdev->dev.parent, NULL);
		*pmic = get_device(pdev->dev.parent);
	}
	put_device(&pdev->dev);
	return map;
}

static int __init hap_init(void)
{
	struct device *pmic = NULL;
	unsigned int rev[2], ptn[2], en, follow, vmax;
	struct op13_haptics *h;
	int ret;

	if (!of_machine_is_compatible("oneplus,cph2655-omarchy-probe"))
		return -ENODEV;
	h = kzalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->map = pmic_regmap(&pmic);
	ret = -ENODEV;
	if (!h->map)
		goto free;
	h->dev = pmic;
	/* HAP530_HV: config and pattern revision 0x0501, boost following
	 * haptics in hardware, module enabled with a programmed Vmax. */
	if (regmap_read(h->map, CFG + CFG_REVISION1, &rev[0]) ||
	    regmap_read(h->map, CFG + CFG_REVISION1 + 1, &rev[1]) ||
	    regmap_read(h->map, PTN + PTN_REVISION1, &ptn[0]) ||
	    regmap_read(h->map, PTN + PTN_REVISION1 + 1, &ptn[1]) ||
	    regmap_read(h->map, CFG + CFG_EN_CTL, &en) ||
	    regmap_read(h->map, HBST + HBST_HW_CTRL_FOLLOW, &follow) ||
	    regmap_read(h->map, CFG + CFG_VMAX, &vmax))
		goto put;
	if (rev[0] != 0x01 || rev[1] != 0x05 || ptn[0] != 0x01 || ptn[1] != 0x05 ||
	    !(en & HAPTICS_EN) || !(follow & FOLLOW_HW_EN) || !vmax || vmax > 0x40) {
		dev_err(pmic, "unexpected haptics state rev %02x%02x/%02x%02x en %02x follow %02x vmax %02x\n",
			rev[1], rev[0], ptn[1], ptn[0], en, follow, vmax);
		goto put;
	}
	mutex_init(&h->lock);
	INIT_WORK(&h->work, hap_work);
	INIT_DELAYED_WORK(&h->guard, hap_guard);
	h->input = input_allocate_device();
	ret = -ENOMEM;
	if (!h->input)
		goto put;
	h->input->name = "pmih0108-haptics";
	h->input->id.bustype = BUS_HOST;
	h->input->close = hap_close;
	input_set_drvdata(h->input, h);
	input_set_capability(h->input, EV_FF, FF_RUMBLE);
	ret = input_ff_create_memless(h->input, NULL, hap_play);
	if (ret)
		goto free_input;
	ret = input_register_device(h->input);
	if (ret)
		goto free_input;
	haptics = h;
	dev_info(pmic, "haptics ready: HAP530_HV, Vmax %u mV, direct play\n", vmax * 50);
	return 0;
free_input:
	input_free_device(h->input);
put:
	put_device(pmic);
free:
	kfree(h);
	return ret;
}

static void __exit hap_exit(void)
{
	struct op13_haptics *h = haptics;

	input_unregister_device(h->input);
	cancel_work_sync(&h->work);
	cancel_delayed_work_sync(&h->guard);
	mutex_lock(&h->lock);
	hap_stop(h);
	mutex_unlock(&h->lock);
	put_device(h->dev);
	kfree(h);
}

module_init(hap_init);
module_exit(hap_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OnePlus 13 PMIH0108 vibration motor (direct play)");
