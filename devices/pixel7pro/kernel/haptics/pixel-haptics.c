// SPDX-License-Identifier: GPL-2.0-only
/* Pixel 7 Pro vibration: the CS40L26A haptics driver's ROM firmware as a
 * force-feedback rumble device.
 *
 * The CS40L26A (revision A1) is at 0x43 on hsi2c_8 (kernel/i2c, "Pixel
 * hsi2c_8"). The bootloader leaves it out of reset (gpp24-3 high) with its
 * ROM firmware running and hibernating between uses; it NACKs the transfer
 * that wakes it. No RAM firmware is loaded, so only the ROM's effects play:
 * its wavetable's clicks (trigger index 0x01800000 on) and its default buzz
 * (0x01800080). Strength is not adjustable without RAM firmware and is
 * ignored.
 *
 * Registers and mailbox commands follow Cirrus Logic's cs40l26 driver
 * (linux-drivers, v6.18-cs40l26): 32-bit big-endian addresses and values,
 * commands written to VIRTUAL1_MBOX_1 and acknowledged when it reads back 0,
 * PREVENT_HIBERNATE before use and ALLOW_HIBERNATE after. The boost peak
 * current is set to the stock DT's 2.5 A (cirrus,bst-ipk-microamp) after each
 * wake: the chip's default is 4.5 A, and the value does not survive
 * hibernation. The boost voltage stays at the default, the stock 11 V.
 *
 * Rumble effects of 30 ms or less play a click; longer ones play the buzz,
 * stopped at the effect's end. The ROM buzz lasts about 0.7 s, so it is
 * triggered again every 600 ms for longer effects.
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define CS40L26_ADDR		0x43
#define CS40L26_DEVID		0x0
#define CS40L26_REVID		0x4
#define CS40L26_BST_IPK_CTL	0x3808
#define CS40L26_MBOX		0x13020		/* DSP_VIRTUAL1_MBOX_1 */
#define CS40L26_HALO_STATE	0x02800fa8	/* ROM A1/B0/B1 dsp_halo_state */
#define DEVID_CS40L26A		0x40a260
#define HALO_STATE_RUN		2
#define BST_IPK_2500MA		0x22		/* 2.5 A / 50 mA - 16 */
#define CMD_PREVENT_HIBERNATE	0x02000003
#define CMD_ALLOW_HIBERNATE	0x02000004
#define CMD_STOP_PLAYBACK	0x05000000
#define ROM_CLICK		0x01800000
#define ROM_BUZZ		0x01800080
#define CLICK_MAX_MS		30
#define BUZZ_RETRIGGER_MS	600
#define HIBERNATE_DELAY_MS	500
#define HAPTICS_EFFECTS		16

struct pixel_haptics {
	struct input_dev *input;
	struct i2c_client *client;
	struct work_struct work;
	struct delayed_work stop, retrigger, sleep;
	struct mutex lock;
	u16 lengths[HAPTICS_EFFECTS];	/* uploaded effects' replay lengths, ms */
	bool want;		/* the rumble is on */
	unsigned int length;	/* its replay length, ms */
	bool awake, buzzing;
};

static struct platform_device *pdev;

static int cs_read(struct pixel_haptics *h, u32 reg, u32 *val)
{
	u8 addr[4], data[4];
	struct i2c_msg msgs[] = {
		{ .addr = CS40L26_ADDR, .len = 4, .buf = addr },
		{ .addr = CS40L26_ADDR, .flags = I2C_M_RD, .len = 4, .buf = data },
	};
	int ret;

	put_unaligned_be32(reg, addr);
	ret = i2c_transfer(h->client->adapter, msgs, 2);
	if (ret != 2)
		return ret < 0 ? ret : -EIO;
	*val = get_unaligned_be32(data);
	return 0;
}

static int cs_write(struct pixel_haptics *h, u32 reg, u32 val)
{
	u8 buf[8];
	struct i2c_msg msg = { .addr = CS40L26_ADDR, .len = 8, .buf = buf };
	int ret;

	put_unaligned_be32(reg, buf);
	put_unaligned_be32(val, buf + 4);
	ret = i2c_transfer(h->client->adapter, &msg, 1);
	return ret == 1 ? 0 : ret < 0 ? ret : -EIO;
}

/* A hibernating chip NACKs the transfer that wakes it. */
static int cs_read_retry(struct pixel_haptics *h, u32 reg, u32 *val)
{
	int i, ret;

	for (i = 0; i < 10; i++) {
		ret = cs_read(h, reg, val);
		if (ret != -ENXIO)
			return ret;
		usleep_range(1000, 2000);
	}
	return ret;
}

static int cs_mailbox(struct pixel_haptics *h, u32 cmd)
{
	u32 val = 0;
	int i, ret;

	ret = cs_write(h, CS40L26_MBOX, cmd);
	if (ret)
		return ret;
	for (i = 0; i < 100; i++) {
		ret = cs_read(h, CS40L26_MBOX, &val);
		if (ret || !val)
			return ret;
		usleep_range(1000, 1100);
	}
	dev_err(&h->input->dev, "mailbox %#x not acknowledged (%#x)\n", cmd, val);
	return -ETIMEDOUT;
}

/* Called with lock held. */
static int haptics_wake(struct pixel_haptics *h)
{
	u32 id;
	int ret;

	if (h->awake)
		return 0;
	ret = cs_read_retry(h, CS40L26_DEVID, &id);
	if (!ret && id != DEVID_CS40L26A)
		ret = -ENODEV;
	ret = ret ?: cs_mailbox(h, CMD_PREVENT_HIBERNATE);
	ret = ret ?: cs_write(h, CS40L26_BST_IPK_CTL, BST_IPK_2500MA);
	if (ret) {
		dev_err(&h->input->dev, "wake failed: %d\n", ret);
		return ret;
	}
	h->awake = true;
	return 0;
}

static void haptics_work(struct work_struct *work)
{
	struct pixel_haptics *h = container_of(work, struct pixel_haptics, work);

	mutex_lock(&h->lock);
	if (READ_ONCE(h->want)) {
		bool buzz = READ_ONCE(h->length) > CLICK_MAX_MS;

		cancel_delayed_work(&h->sleep);
		if (!haptics_wake(h) && !cs_mailbox(h, buzz ? ROM_BUZZ : ROM_CLICK)) {
			h->buzzing = buzz;
			if (buzz)
				mod_delayed_work(system_dfl_wq, &h->retrigger,
						 msecs_to_jiffies(BUZZ_RETRIGGER_MS));
		}
	} else {
		cancel_delayed_work(&h->retrigger);
		if (h->buzzing)
			cs_mailbox(h, CMD_STOP_PLAYBACK);
		h->buzzing = false;
		if (h->awake)
			mod_delayed_work(system_dfl_wq, &h->sleep,
					 msecs_to_jiffies(HIBERNATE_DELAY_MS));
	}
	mutex_unlock(&h->lock);
}

static void haptics_retrigger(struct work_struct *work)
{
	struct pixel_haptics *h = container_of(to_delayed_work(work), struct pixel_haptics,
					       retrigger);

	mutex_lock(&h->lock);
	if (READ_ONCE(h->want) && h->buzzing && !cs_mailbox(h, ROM_BUZZ))
		mod_delayed_work(system_dfl_wq, &h->retrigger,
				 msecs_to_jiffies(BUZZ_RETRIGGER_MS));
	mutex_unlock(&h->lock);
}

static void haptics_sleep(struct work_struct *work)
{
	struct pixel_haptics *h = container_of(to_delayed_work(work), struct pixel_haptics,
					       sleep);

	mutex_lock(&h->lock);
	if (!READ_ONCE(h->want) && h->awake) {
		cs_mailbox(h, CMD_ALLOW_HIBERNATE);
		h->awake = false;
	}
	mutex_unlock(&h->lock);
}

static void haptics_stop(struct work_struct *work)
{
	struct pixel_haptics *h = container_of(to_delayed_work(work), struct pixel_haptics,
					       stop);

	WRITE_ONCE(h->want, false);
	queue_work(system_dfl_wq, &h->work);
}

static int haptics_upload(struct input_dev *dev, struct ff_effect *effect,
			  struct ff_effect *old)
{
	struct pixel_haptics *h = input_get_drvdata(dev);

	if (effect->type != FF_RUMBLE)
		return -EINVAL;
	h->lengths[effect->id] = effect->replay.length;
	return 0;
}

/* Called under the input device's event lock: the chip is driven from
 * work. The effect's replay length ends it; 0 plays until stopped.
 */
static int haptics_playback(struct input_dev *dev, int id, int value)
{
	struct pixel_haptics *h = input_get_drvdata(dev);
	unsigned int length = h->lengths[id];

	if (value > 0) {
		WRITE_ONCE(h->length, length);
		WRITE_ONCE(h->want, true);
		if (length)
			mod_delayed_work(system_dfl_wq, &h->stop, msecs_to_jiffies(length));
	} else {
		cancel_delayed_work(&h->stop);
		WRITE_ONCE(h->want, false);
	}
	queue_work(system_dfl_wq, &h->work);
	return 0;
}

static struct i2c_adapter *find_bus(void)
{
	struct i2c_adapter *adap;
	int nr;

	for (nr = 0; nr < 64; nr++) {
		adap = i2c_get_adapter(nr);
		if (!adap)
			continue;
		if (!strcmp(adap->name, "Pixel hsi2c_8"))
			return adap;
		i2c_put_adapter(adap);
	}
	return NULL;
}

static int pixel_haptics_probe(struct platform_device *pd)
{
	struct device *dev = &pd->dev;
	struct pixel_haptics *h;
	struct i2c_adapter *adap;
	u32 id, rev, halo;
	int ret;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	mutex_init(&h->lock);
	INIT_WORK(&h->work, haptics_work);
	INIT_DELAYED_WORK(&h->stop, haptics_stop);
	INIT_DELAYED_WORK(&h->retrigger, haptics_retrigger);
	INIT_DELAYED_WORK(&h->sleep, haptics_sleep);
	adap = find_bus();
	if (!adap)
		return -EPROBE_DEFER;
	h->client = i2c_new_dummy_device(adap, CS40L26_ADDR);
	i2c_put_adapter(adap);
	if (IS_ERR(h->client))
		return PTR_ERR(h->client);

	h->input = devm_input_allocate_device(dev);
	if (!h->input) {
		ret = -ENOMEM;
		goto unregister;
	}
	input_set_drvdata(h->input, h);

	/* Only the ROM revisions whose memory map is known (A1, B0, B1). */
	ret = cs_read_retry(h, CS40L26_DEVID, &id);
	ret = ret ?: cs_read(h, CS40L26_REVID, &rev);
	if (!ret && (id != DEVID_CS40L26A || (rev != 0xa1 && rev != 0xb0 && rev != 0xb1)))
		ret = -ENODEV;
	if (!ret) {
		ret = cs_mailbox(h, CMD_PREVENT_HIBERNATE);
		ret = ret ?: cs_read(h, CS40L26_HALO_STATE, &halo);
		cs_mailbox(h, CMD_ALLOW_HIBERNATE);
		if (!ret && halo != HALO_STATE_RUN)
			ret = -ENODEV;
	}
	if (ret) {
		dev_err(dev, "no CS40L26A with running ROM firmware: %d\n", ret);
		goto unregister;
	}

	h->input->name = "Pixel haptics";
	h->input->phys = "pixel-haptics/input0";
	h->input->id.bustype = BUS_I2C;
	input_set_capability(h->input, EV_FF, FF_RUMBLE);
	ret = input_ff_create(h->input, HAPTICS_EFFECTS);
	if (ret)
		goto unregister;
	h->input->ff->upload = haptics_upload;
	h->input->ff->playback = haptics_playback;
	ret = input_register_device(h->input);
	if (ret)
		goto unregister;
	platform_set_drvdata(pd, h);
	dev_info(dev, "CS40L26A rev %#x, ROM firmware, on hsi2c_8\n", rev);
	return 0;
unregister:
	i2c_unregister_device(h->client);
	return ret;
}

static void pixel_haptics_remove(struct platform_device *pd)
{
	struct pixel_haptics *h = platform_get_drvdata(pd);

	input_unregister_device(h->input);
	WRITE_ONCE(h->want, false);
	cancel_delayed_work_sync(&h->stop);
	cancel_work_sync(&h->work);
	cancel_delayed_work_sync(&h->retrigger);
	cancel_delayed_work_sync(&h->sleep);
	if (h->buzzing)
		cs_mailbox(h, CMD_STOP_PLAYBACK);
	if (h->awake)
		cs_mailbox(h, CMD_ALLOW_HIBERNATE);
	i2c_unregister_device(h->client);
}

static struct platform_driver pixel_haptics_driver = {
	.probe = pixel_haptics_probe,
	.remove = pixel_haptics_remove,
	.driver.name = "pixel-haptics",
};

static int __init pixel_haptics_init(void)
{
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	ret = platform_driver_register(&pixel_haptics_driver);
	if (ret)
		return ret;
	pdev = platform_device_register_simple("pixel-haptics", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&pixel_haptics_driver);
		return PTR_ERR(pdev);
	}
	if (!pdev->dev.driver) {
		platform_device_unregister(pdev);
		platform_driver_unregister(&pixel_haptics_driver);
		return -ENODEV;
	}
	return 0;
}
module_init(pixel_haptics_init);

static void __exit pixel_haptics_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&pixel_haptics_driver);
}
module_exit(pixel_haptics_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pixel 7 Pro CS40L26A vibration from its ROM firmware");
MODULE_SOFTDEP("pre: pixel-hsi2c");
