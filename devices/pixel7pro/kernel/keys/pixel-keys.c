// SPDX-License-Identifier: GPL-2.0-only
/* GS201 power and volume keys on wake-up external interrupts.
 *
 * The power key is gpa10-0, which Google's gs201 DT names PMIC_PWRON_OD_L:
 * the S2MPG12's open-drain copy of the button. Volume down is gpa10-1 and
 * volume up gpa8-5 (stock gpio_keys). All three are active low with pull-ups
 * (volume up has none) on the always-on pin controllers, and each pin has its
 * own GIC line (the stock DT's gpa10 and gpa8 interrupt lists). Bank layout
 * from Google's pinctrl-gs201.c: CON/DAT at the bank offset, ECON 0x700,
 * FLTCON 0x800, EMASK 0x900 and EPEND 0xa00 plus the bank's EINT offset.
 *
 * Each pin becomes an EINT on both edges; the interrupt starts a short
 * debounce, then the pin level is reported. The power key is a wake-up
 * source. Unloading restores the pins as they were handed over.
 */
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/workqueue.h>

#define ECON		0x700
#define FLTCON		0x800
#define EMASK		0x900
#define EPEND		0xa00
#define ECON_BOTH	4
#define FLTCON_EN	BIT(7)
#define DEBOUNCE_MS	15

struct pixel_key {
	const char *name;
	phys_addr_t base;	/* pin controller */
	u32 bank;		/* CON; DAT at +4 */
	u32 eint;		/* EINT register offset */
	u32 fltcon;		/* first FLTCON register (4 pins each) */
	unsigned int pin;
	const char *node;	/* DT node carrying the per-pin interrupts */
	unsigned int code;
	bool wake;

	void __iomem *regs;
	int irq;
	bool pressed;
	u32 con, econ, fltcon_saved, emask;
	struct delayed_work work;
};

static struct pixel_key keys[] = {
	{ "power", 0x180d0000, 0xe0, 0x1c, 0x2c, 0, "/pinctrl@180D0000/gpa10",
	  KEY_POWER, true },
	{ "volume down", 0x180d0000, 0xe0, 0x1c, 0x2c, 1, "/pinctrl@180D0000/gpa10",
	  KEY_VOLUMEDOWN, false },
	{ "volume up", 0x180e0000, 0x40, 0x08, 0x0c, 5, "/pinctrl@180E0000/gpa8",
	  KEY_VOLUMEUP, false },
};

static struct platform_device *pdev;
static struct input_dev *input;

static void rmw(void __iomem *reg, u32 mask, u32 val)
{
	writel((readl(reg) & ~mask) | (val & mask), reg);
}

static u32 fltcon_reg(const struct pixel_key *k)
{
	return FLTCON + k->fltcon + 4 * (k->pin / 4);
}

static bool key_down(const struct pixel_key *k)
{
	return !(readl(k->regs + k->bank + 4) & BIT(k->pin));
}

static void key_work(struct work_struct *work)
{
	struct pixel_key *k = container_of(to_delayed_work(work), struct pixel_key, work);
	bool down = key_down(k);

	if (down != k->pressed) {
		k->pressed = down;
		input_report_key(input, k->code, down);
		input_sync(input);
	}
	if (k->wake)
		pm_relax(&pdev->dev);
}

static irqreturn_t key_irq(int irq, void *data)
{
	struct pixel_key *k = data;

	writel(BIT(k->pin), k->regs + EPEND + k->eint);
	if (k->wake)
		pm_stay_awake(&pdev->dev);
	mod_delayed_work(system_dfl_wq, &k->work, msecs_to_jiffies(DEBOUNCE_MS));
	return IRQ_HANDLED;
}

static void key_teardown(struct pixel_key *k)
{
	u32 nib = 0xf << (4 * k->pin);

	if (k->irq <= 0)
		return;
	if (k->wake)
		disable_irq_wake(k->irq);
	rmw(k->regs + EMASK + k->eint, BIT(k->pin), k->emask);
	free_irq(k->irq, k);
	cancel_delayed_work_sync(&k->work);
	rmw(k->regs + ECON + k->eint, nib, k->econ);
	rmw(k->regs + fltcon_reg(k), 0xff << (8 * (k->pin % 4)), k->fltcon_saved);
	rmw(k->regs + k->bank, nib, k->con);
	k->irq = 0;
}

static int key_setup(struct pixel_key *k)
{
	u32 nib = 0xf << (4 * k->pin);
	struct device_node *np;
	int ret;

	np = of_find_node_by_path(k->node);
	if (!np)
		return -ENODEV;
	k->irq = irq_of_parse_and_map(np, k->pin);
	of_node_put(np);
	if (!k->irq)
		return -EINVAL;
	k->regs = devm_ioremap(&pdev->dev, k->base, 0x1000);
	if (!k->regs)
		return -ENOMEM;

	k->con = readl(k->regs + k->bank) & nib;
	k->econ = readl(k->regs + ECON + k->eint) & nib;
	k->fltcon_saved = readl(k->regs + fltcon_reg(k)) & (0xff << (8 * (k->pin % 4)));
	k->emask = readl(k->regs + EMASK + k->eint) & BIT(k->pin);
	/* The bootloader hands these over as masked inputs; anything else
	 * means another owner.
	 */
	if (k->con || !k->emask) {
		dev_err(&pdev->dev, "%s key: unexpected pin state (con %#x, mask %#x)\n",
			k->name, k->con, k->emask);
		k->irq = 0;
		return -EBUSY;
	}

	INIT_DELAYED_WORK(&k->work, key_work);
	rmw(k->regs + ECON + k->eint, nib, ECON_BOTH << (4 * k->pin));
	rmw(k->regs + fltcon_reg(k), 0xff << (8 * (k->pin % 4)),
	    FLTCON_EN << (8 * (k->pin % 4)));
	rmw(k->regs + k->bank, nib, nib);	/* function 0xf: EINT */
	writel(BIT(k->pin), k->regs + EPEND + k->eint);
	ret = request_irq(k->irq, key_irq, 0, "pixel-keys", k);
	if (ret) {
		rmw(k->regs + k->bank, nib, k->con);
		k->irq = 0;
		return ret;
	}
	k->pressed = key_down(k);
	rmw(k->regs + EMASK + k->eint, BIT(k->pin), 0);
	if (k->wake)
		enable_irq_wake(k->irq);
	return 0;
}

static int __init pixel_keys_init(void)
{
	int ret, i;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	pdev = platform_device_register_simple("pixel-keys", -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	device_init_wakeup(&pdev->dev, true);
	input = devm_input_allocate_device(&pdev->dev);
	if (!input) {
		ret = -ENOMEM;
		goto unregister;
	}
	input->name = "Pixel keys";
	input->phys = "pixel-keys/input0";
	input->id.bustype = BUS_HOST;
	for (i = 0; i < ARRAY_SIZE(keys); i++)
		input_set_capability(input, EV_KEY, keys[i].code);
	ret = input_register_device(input);
	if (ret)
		goto unregister;
	for (i = 0; i < ARRAY_SIZE(keys); i++) {
		ret = key_setup(&keys[i]);
		if (ret) {
			dev_err(&pdev->dev, "%s key: %d\n", keys[i].name, ret);
			while (--i >= 0)
				key_teardown(&keys[i]);
			goto unregister;
		}
	}
	for (i = 0; i < ARRAY_SIZE(keys); i++)
		dev_info(&pdev->dev, "%s key on interrupt %d%s\n", keys[i].name, keys[i].irq,
			 keys[i].wake ? ", wakes the system" : "");
	return 0;
unregister:
	platform_device_unregister(pdev);
	return ret;
}

static void __exit pixel_keys_exit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(keys); i++)
		key_teardown(&keys[i]);
	platform_device_unregister(pdev);
}

module_init(pixel_keys_init);
module_exit(pixel_keys_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pixel GS201 power and volume keys on wake-up interrupts");
