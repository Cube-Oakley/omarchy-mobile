// SPDX-License-Identifier: GPL-2.0-only
/* MAX77759 USB2 data-path reconnection for the GS201 peripheral-only gadget.
 * The TCPC opens the switch on cable removal. ABL sets it on boot, but our
 * inherited device-mode setup has no TCPM driver to reconnect it on insertion.
 * Google's enable_data_path_locked() writes USBSW_CTRL=USBSW_CONNECT (0x09).
 * No CC, PD, charger, VBUS-source or role control registers are written here.
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

static struct i2c_client *client;
static struct delayed_work reconnect_work;
static unsigned int reconnects;
module_param(reconnects, uint, 0444);

static void reconnect(struct work_struct *work)
{
	int status, value, ret;

	status = i2c_smbus_read_byte_data(client, POWER_STATUS);
	if (status < 0 || !(status & VBUS_PRESENT))
		return;
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
	/* Catch a quick unplug/replug between battery polls. Poll only while
	 * externally powered; unplugged insertion uses the supply notifier.
	 * A freezable queue adds no wakeups during system suspend. */
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
