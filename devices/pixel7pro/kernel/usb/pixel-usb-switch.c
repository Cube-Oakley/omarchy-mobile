// SPDX-License-Identifier: GPL-2.0-only
/* MAX77759 USB2 data-path reconnection for the GS201 peripheral-only gadget.
 * The TCPC opens the switch on cable removal. ABL sets it on boot, but our
 * inherited device-mode setup has no TCPM driver to reconnect it on insertion.
 * Google's enable_data_path_locked() writes USBSW_CTRL=USBSW_CONNECT (0x09).
 * No CC, PD, charger, VBUS-source or role control registers are written here.
 *
 * typec_status (read only) reports what the port's source offers, for the
 * charging work: the Type-C current (Rp) on CC, VBUS and the BC1.2 result.
 * pixel_usb_port_offer() gives pixel-battery the current to draw from it.
 */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#define TCPC_ADDR 0x25
#define VENDOR_ID 0x00
#define PRODUCT_ID 0x02
#define POWER_STATUS 0x1e
#define VBUS_PRESENT BIT(2)
#define USBSW_CTRL 0x93
#define USBSW_CONNECT 0x09

/* Read by typec_status only. TCPCI, then MAX77759 vendor registers. */
#define TCPC_CONTROL 0x19
#define ROLE_CONTROL 0x1a
#define CC_STATUS 0x1d
#define CC_STATUS_RD BIT(4)		/* presenting Rd: a sink */
#define CC_STATUS_LOOKING BIT(5)
#define RECEIVE_DETECT 0x2f
#define VBUS_VOLTAGE 0x70		/* 10 bits of 25 mV, scaled down by bits 11:10 */
#define VENDOR_ALERT1 0x80
#define BC_STATUS1 0x87
#define BC_CHGTYP GENMASK(1, 0)
#define BC_CHGTYPRUN BIT(6)
#define BC_STATUS2 0x88
#define BC_CTRL1 0x8f

static struct i2c_client *client;
static struct delayed_work reconnect_work;
static unsigned long vbus_seen;	/* jiffies when VBUS was last present */
static unsigned int reconnects;
module_param(reconnects, uint, 0444);

static void reconnect(struct work_struct *work)
{
	int status, value, ret;

	status = i2c_smbus_read_byte_data(client, POWER_STATUS);
	if (status < 0)
		return;
	if (!(status & VBUS_PRESENT)) {
		/* Keep watching for a while after the cable comes out. A replug
		 * within one battery poll changes nothing the battery driver
		 * sees, so its supply notifier would never wake this again. */
		if (time_before(jiffies, vbus_seen + 30 * HZ))
			goto again;
		return;
	}
	vbus_seen = jiffies;
	value = i2c_smbus_read_byte_data(client, USBSW_CTRL);
	/* Only repair the observed disconnected value. Preserve unknown routes. */
	if (value != 0)
		goto again;
	ret = i2c_smbus_write_byte_data(client, USBSW_CTRL, USBSW_CONNECT);
	if (!ret && i2c_smbus_read_byte_data(client, USBSW_CTRL) == USBSW_CONNECT) {
		reconnects++;
		dev_info(&client->dev, "USB2 data switch reconnected\n");
	} else {
		dev_warn_ratelimited(&client->dev, "USB2 data switch reconnect failed\n");
	}
again:
	/* Catch a quick unplug/replug between battery polls. Poll while
	 * externally powered and for 30 s after; a later insertion uses the
	 * supply notifier. A freezable queue adds no wakeups during system
	 * suspend. */
	queue_delayed_work(system_freezable_wq, &reconnect_work, 2 * HZ);
}

static int supply_changed(struct notifier_block *nb, unsigned long event, void *data)
{
	struct power_supply *psy = data;

	if (event == PSY_EVENT_PROP_CHANGED && !strcmp(psy->desc->name, "usb"))
		mod_delayed_work(system_freezable_wq, &reconnect_work, 0);
	return NOTIFY_OK;
}

static struct notifier_block supply_notifier = {
	.notifier_call = supply_changed,
};

/* A sink's view of a source's Rp on one CC line. */
static const char *const sink_cc[] = { "open", "default", "1.5A", "3.0A" };
static const char *const chgtyp[] = { "none", "SDP", "CDP", "DCP" };

/* What the port offers a sink, for pixel-battery's input limit, in mA:
 * 3000 or 1500 when the source advertises that Type-C current on CC, else
 * what its BC1.2 type allows: 1500 for a charging or dedicated charger port,
 * 500 for a plain data port or none. An advertisement with a plain data
 * port is a legacy cable with the wrong pull-up on a computer: 500. 0 while
 * BC1.2 detection runs. Also VBUS in mV. Negative without the TCPC.
 */
int pixel_usb_port_offer(int *ma, int *vbus_mv);	/* pixel-battery looks it up by name */
int pixel_usb_port_offer(int *ma, int *vbus_mv)
{
	struct i2c_client *c = READ_ONCE(client);
	int cc, bc, rp = 0, vbus;

	if (!c)
		return -ENODEV;
	cc = i2c_smbus_read_byte_data(c, CC_STATUS);
	if (cc < 0)
		return cc;
	bc = i2c_smbus_read_byte_data(c, BC_STATUS1);
	if (bc < 0)
		return bc;
	vbus = i2c_smbus_read_word_data(c, VBUS_VOLTAGE);
	if (vbus < 0)
		return vbus;
	*vbus_mv = (vbus & 0x3ff) * 25 << ((vbus >> 10) & 3);
	if (cc & CC_STATUS_RD)
		rp = max(cc & 3, (cc >> 2) & 3);
	if (bc & BC_CHGTYPRUN)
		*ma = 0;
	else if ((bc & BC_CHGTYP) == 1)		/* SDP */
		*ma = 500;
	else if (rp == 3)
		*ma = 3000;
	else if (rp == 2 || (bc & BC_CHGTYP) >= 2)	/* Rp 1.5 A, CDP, DCP */
		*ma = 1500;
	else
		*ma = 500;
	return 0;
}
EXPORT_SYMBOL_GPL(pixel_usb_port_offer);

static ssize_t typec_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	static const u8 regs[] = { TCPC_CONTROL, ROLE_CONTROL, CC_STATUS, POWER_STATUS,
				   RECEIVE_DETECT, VENDOR_ALERT1, BC_STATUS1, BC_STATUS2,
				   BC_CTRL1, USBSW_CTRL };
	struct i2c_client *c = to_i2c_client(dev);
	int val[ARRAY_SIZE(regs)], vbus, cc, bc, mv, len = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		val[i] = i2c_smbus_read_byte_data(c, regs[i]);
		if (val[i] < 0)
			return val[i];
	}
	vbus = i2c_smbus_read_word_data(c, VBUS_VOLTAGE);
	if (vbus < 0)
		return vbus;
	cc = val[2];
	bc = val[6];
	mv = (vbus & 0x3ff) * 25 << ((vbus >> 10) & 3);

	if (cc & CC_STATUS_RD)
		len += sysfs_emit_at(buf, len, "cc1 %s cc2 %s", sink_cc[cc & 3],
				     sink_cc[(cc >> 2) & 3]);
	else
		len += sysfs_emit_at(buf, len, "cc presenting Rp (%#04x)", cc);
	len += sysfs_emit_at(buf, len, "%s, vbus %d mV%s, bc1.2 %s%s\n",
			     cc & CC_STATUS_LOOKING ? " looking" : "", mv,
			     val[3] & VBUS_PRESENT ? " present" : "",
			     chgtyp[bc & BC_CHGTYP], bc & BC_CHGTYPRUN ? " (detecting)" : "");
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		len += sysfs_emit_at(buf, len, "%02x=%02x ", regs[i], val[i]);
	len += sysfs_emit_at(buf, len, "70=%04x\n", vbus);
	return len;
}
static DEVICE_ATTR_RO(typec_status);

static struct attribute *pixel_usb_switch_attrs[] = {
	&dev_attr_typec_status.attr,
	NULL
};
ATTRIBUTE_GROUPS(pixel_usb_switch);

static int pixel_usb_switch_probe(struct i2c_client *c)
{
	int ret;

	if (i2c_smbus_read_word_data(c, VENDOR_ID) != 0x0b6a ||
	    i2c_smbus_read_word_data(c, PRODUCT_ID) != 0x7759)
		return -ENODEV;
	client = c;
	INIT_DELAYED_WORK(&reconnect_work, reconnect);
	ret = power_supply_reg_notifier(&supply_notifier);
	if (ret)
		return ret;
	queue_delayed_work(system_freezable_wq, &reconnect_work, 0);
	return 0;
}

static void pixel_usb_switch_remove(struct i2c_client *c)
{
	WRITE_ONCE(client, NULL);
	power_supply_unreg_notifier(&supply_notifier);
	cancel_delayed_work_sync(&reconnect_work);
	/* Leave the existing connection usable; physical detach opens it. */
}

static int pixel_usb_switch_resume(struct device *dev)
{
	mod_delayed_work(system_freezable_wq, &reconnect_work, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(pixel_usb_switch_pm, NULL, pixel_usb_switch_resume);
static const struct i2c_device_id pixel_usb_switch_ids[] = {
	{ "pixel-usb-switch" }, { }
};
MODULE_DEVICE_TABLE(i2c, pixel_usb_switch_ids);

static struct i2c_driver pixel_usb_switch_driver = {
	.driver = {
		.name = "pixel-usb-switch",
		.pm = pm_sleep_ptr(&pixel_usb_switch_pm),
		.dev_groups = pixel_usb_switch_groups,
	},
	.probe = pixel_usb_switch_probe,
	.remove = pixel_usb_switch_remove,
	.id_table = pixel_usb_switch_ids,
};

static int find_bus(struct device *dev, void *data)
{
	struct i2c_adapter *adap = i2c_verify_adapter(dev);
	int *found = data;

	if (adap && !strcmp(adap->name, "Pixel hsi2c_13")) {
		/* i2c_for_each_dev already holds core_lock. Take the adapter
		 * reference only after returning from that iterator. */
		*found = adap->nr;
		return 1;
	}
	return 0;
}

static struct i2c_client *owned_client;

static int __init pixel_usb_switch_init(void)
{
	struct i2c_board_info info = { I2C_BOARD_INFO("pixel-usb-switch", TCPC_ADDR) };
	struct i2c_adapter *adap = NULL;
	int ret, bus = -1;

	if (!of_machine_is_compatible("google,GS201 CHEETAH"))
		return -ENODEV;
	i2c_for_each_dev(&bus, find_bus);
	if (bus >= 0)
		adap = i2c_get_adapter(bus);
	if (!adap)
		return -ENODEV;
	ret = i2c_add_driver(&pixel_usb_switch_driver);
	if (ret)
		goto put_bus;
	/* Fails if a full TCPC driver already owns this address. */
	owned_client = i2c_new_client_device(adap, &info);
	if (IS_ERR(owned_client)) {
		ret = PTR_ERR(owned_client);
		i2c_del_driver(&pixel_usb_switch_driver);
	} else if (!owned_client->dev.driver) {
		ret = -ENODEV;
		i2c_unregister_device(owned_client);
		i2c_del_driver(&pixel_usb_switch_driver);
	}
put_bus:
	i2c_put_adapter(adap);
	return ret;
}

static void __exit pixel_usb_switch_exit(void)
{
	i2c_unregister_device(owned_client);
	i2c_del_driver(&pixel_usb_switch_driver);
}
module_init(pixel_usb_switch_init);
module_exit(pixel_usb_switch_exit);
MODULE_DESCRIPTION("Pixel MAX77759 peripheral USB2 switch reconnection");
MODULE_LICENSE("GPL");
