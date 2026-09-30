// SPDX-License-Identifier: GPL-2.0-only
/* Pixel 7 Pro flashlight: the rear LM3644 flash driver as an LED class torch.
 *
 * The stock DT's lwis "flash-lm3644" node puts the LM3644 at 0x63 on hsi2c_15
 * (kernel/i2c, "Pixel hsi2c_15"), enabled by gpp8-2 (HWEN, active high; the
 * node's second enable, gpp27-0, is active low and stays as the bootloader
 * leaves it, low). gpp8-2 is on the PERIC0 pin controller, bank offset 0x100
 * (Google's pinctrl-gs201.c); the bootloader leaves it an input with a
 * pull-down, which holds the LM3644 in shutdown.
 *
 * LM3644 registers (TI datasheet): enable 0x01 (mode in [3:2], 2 = torch;
 * LED1/LED2 enables in [1:0]; bit 7, the TX input, as reset), LED1 torch
 * brightness 0x05 (bit 7 makes LED2 follow LED1), device ID 0x0c. Torch
 * current is 0.977 mA + 1.4 mA per step, 0 to 127 (179 mA). Brightness here
 * is that step, on both LEDs, capped at max_level. HWEN is low whenever the
 * torch is off.
 *
 * Measured at the USB input over the 1.48 W idle draw: +0.62 W at step 16,
 * +0.81 W at 32, +1.86 W at 64. The shell lights a flashlight at half of
 * max_brightness, so the default cap of 64 gives step 32.
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define GPP8_BANK		0x10840100	/* PERIC0 gpp8: CON, DAT, PUD */
#define GPIO_CON		0x0
#define GPIO_DAT		0x4
#define GPIO_PUD		0x8
#define HWEN_PIN		2
#define CON_OUTPUT		1

#define LM3644_ADDR		0x63
#define LM3644_ENABLE		0x01
#define LM3644_TORCH1		0x05
#define LM3644_FLAGS1		0x0a
#define LM3644_FLAGS2		0x0b
#define LM3644_DEVICE_ID	0x0c
#define ENABLE_TX		BIT(7)
#define ENABLE_TORCH		(2 << 2)
#define ENABLE_LEDS		(BIT(1) | BIT(0))
#define TORCH_LED2_FOLLOWS	BIT(7)
#define TORCH_MAX		127

struct pixel_torch {
	struct led_classdev led;
	struct i2c_client *client;
	void __iomem *gpio;
	u32 con, dat, pud;	/* as handed over */
	struct mutex lock;
	bool powered;
};

static unsigned int max_level = 64;
module_param(max_level, uint, 0444);
MODULE_PARM_DESC(max_level, "Highest torch step (1-127), the LED's max_brightness");

static struct platform_device *pdev;

static void hwen(struct pixel_torch *t, bool on)
{
	u32 shift = 4 * HWEN_PIN;

	writel((readl(t->gpio + GPIO_DAT) & ~BIT(HWEN_PIN)) | (on ? BIT(HWEN_PIN) : 0),
	       t->gpio + GPIO_DAT);
	writel((readl(t->gpio + GPIO_CON) & ~(0xf << shift)) | (CON_OUTPUT << shift),
	       t->gpio + GPIO_CON);
	t->powered = on;
}

static void hwen_restore(struct pixel_torch *t)
{
	u32 mask = 0xf << (4 * HWEN_PIN);

	writel((readl(t->gpio + GPIO_CON) & ~mask) | (t->con & mask), t->gpio + GPIO_CON);
	writel((readl(t->gpio + GPIO_DAT) & ~BIT(HWEN_PIN)) | (t->dat & BIT(HWEN_PIN)),
	       t->gpio + GPIO_DAT);
	t->powered = false;
}

static int torch_power(struct pixel_torch *t)
{
	if (t->powered)
		return 0;
	hwen(t, true);
	usleep_range(1000, 1500);	/* HWEN to I2C ready */
	return 0;
}

static int torch_set(struct led_classdev *led, enum led_brightness value)
{
	struct pixel_torch *t = container_of(led, struct pixel_torch, led);
	int ret;

	mutex_lock(&t->lock);
	if (!value) {
		if (t->powered)
			i2c_smbus_write_byte_data(t->client, LM3644_ENABLE, ENABLE_TX);
		hwen(t, false);
		mutex_unlock(&t->lock);
		return 0;
	}
	torch_power(t);
	ret = i2c_smbus_write_byte_data(t->client, LM3644_TORCH1,
					TORCH_LED2_FOLLOWS | min_t(u32, value, TORCH_MAX));
	if (!ret)
		ret = i2c_smbus_write_byte_data(t->client, LM3644_ENABLE,
						ENABLE_TX | ENABLE_TORCH | ENABLE_LEDS);
	if (ret) {
		dev_err(led->dev, "torch on failed: %d\n", ret);
		hwen(t, false);
	}
	mutex_unlock(&t->lock);
	return ret;
}

static struct i2c_adapter *find_bus(void)
{
	struct i2c_adapter *adap;
	int nr;

	for (nr = 0; nr < 64; nr++) {
		adap = i2c_get_adapter(nr);
		if (!adap)
			continue;
		if (!strcmp(adap->name, "Pixel hsi2c_15"))
			return adap;
		i2c_put_adapter(adap);
	}
	return NULL;
}

static int pixel_torch_probe(struct platform_device *pd)
{
	struct device *dev = &pd->dev;
	struct i2c_adapter *adap;
	struct pixel_torch *t;
	int id, ret;

	t = devm_kzalloc(dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	mutex_init(&t->lock);
	t->gpio = devm_ioremap(dev, GPP8_BANK, 0x20);
	if (!t->gpio)
		return -ENOMEM;
	t->con = readl(t->gpio + GPIO_CON);
	t->dat = readl(t->gpio + GPIO_DAT);
	t->pud = readl(t->gpio + GPIO_PUD);
	/* As the bootloader leaves it: an input. Anything else has an owner. */
	if ((t->con >> (4 * HWEN_PIN)) & 0xf) {
		dev_err(dev, "gpp8-2 not an input (con %#x)\n", t->con);
		return -EBUSY;
	}
	adap = find_bus();
	if (!adap)
		return -EPROBE_DEFER;
	t->client = i2c_new_dummy_device(adap, LM3644_ADDR);
	i2c_put_adapter(adap);
	if (IS_ERR(t->client))
		return PTR_ERR(t->client);

	torch_power(t);
	id = i2c_smbus_read_byte_data(t->client, LM3644_DEVICE_ID);
	/* Reading the flags clears any fault latched since power-up. */
	i2c_smbus_read_byte_data(t->client, LM3644_FLAGS1);
	i2c_smbus_read_byte_data(t->client, LM3644_FLAGS2);
	hwen_restore(t);
	if (id < 0) {
		dev_err(dev, "no LM3644 at %#x: %d\n", LM3644_ADDR, id);
		ret = id;
		goto unregister;
	}

	t->led.name = "white:torch";
	t->led.max_brightness = clamp(max_level, 1U, (unsigned int)TORCH_MAX);
	t->led.brightness_set_blocking = torch_set;
	t->led.flags = LED_CORE_SUSPENDRESUME;
	ret = led_classdev_register(dev, &t->led);
	if (ret)
		goto unregister;
	platform_set_drvdata(pd, t);
	dev_info(dev, "LM3644 (device ID %#x) on hsi2c_15\n", id);
	return 0;
unregister:
	i2c_unregister_device(t->client);
	return ret;
}

static void pixel_torch_remove(struct platform_device *pd)
{
	struct pixel_torch *t = platform_get_drvdata(pd);

	led_classdev_unregister(&t->led);
	mutex_lock(&t->lock);
	if (t->powered)
		i2c_smbus_write_byte_data(t->client, LM3644_ENABLE, ENABLE_TX);
	hwen_restore(t);
	mutex_unlock(&t->lock);
	i2c_unregister_device(t->client);
}

static struct platform_driver pixel_torch_driver = {
	.probe = pixel_torch_probe,
	.remove = pixel_torch_remove,
	.driver.name = "pixel-torch",
};

static int __init pixel_torch_init(void)
{
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	ret = platform_driver_register(&pixel_torch_driver);
	if (ret)
		return ret;
	pdev = platform_device_register_simple("pixel-torch", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&pixel_torch_driver);
		return PTR_ERR(pdev);
	}
	if (!pdev->dev.driver) {
		platform_device_unregister(pdev);
		platform_driver_unregister(&pixel_torch_driver);
		return -ENODEV;
	}
	return 0;
}
module_init(pixel_torch_init);

static void __exit pixel_torch_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&pixel_torch_driver);
}
module_exit(pixel_torch_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pixel 7 Pro LM3644 flashlight on hsi2c_15");
MODULE_SOFTDEP("pre: pixel-hsi2c");
