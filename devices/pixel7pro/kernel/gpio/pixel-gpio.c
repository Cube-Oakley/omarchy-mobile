// SPDX-License-Identifier: GPL-2.0-only
/* GS201 GPIO lines and wake-up interrupts for drivers bound from the stock DT.
 *
 * Without a GS201 pin controller driver, DT consumers of the stock pin banks
 * (reset-gpios, cs-gpios, interrupts) defer forever. This registers a GPIO
 * chip or an interrupt domain on individual bank nodes, so those lookups
 * resolve, and exposes only the lines listed here; anything else in a bank
 * stays invalid and untouched.
 *
 * - GPIO banks: lines are plain inputs/outputs (CON function 0/1, DAT).
 * - Wake-up EINT banks (gpa*): each listed pin becomes an interrupt, chained
 *   from its own GIC line (the bank node's per-pin interrupt list). Requesting
 *   it switches the pin to its EINT function; amplifier pulls are disabled,
 *   CP pulls preserved. Freeing the interrupt restores both.
 *
 * The stock DT's interrupt specifier is <pin type 0>. Bank layout from
 * Google's pinctrl-gs201.c: CON/DAT/PUD at the bank offset (+0/+4/+8), and
 * ECON 0x700, EMASK 0x900 and EPEND 0xa00 plus the bank's EINT offset; ECON
 * nibbles are 0 low, 1 high, 2 falling, 3 rising, 4 both edges.
 */
#include <linux/bitops.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

#define CON_INPUT	0x0
#define CON_OUTPUT	0x1
#define CON_EINT	0xf
#define ECON		0x700
#define EMASK		0x900
#define EPEND		0xa00

struct pixel_bank_desc {
	const char *node;
	phys_addr_t base;	/* pin controller */
	u16 bank;		/* CON; DAT +4, PUD +8 */
	u16 eint;		/* EINT register offset */
	u8 npins;
	u8 lines;		/* GPIO lines exposed */
	u8 irqs;		/* wake-up EINTs exposed (gpa banks only) */
	u8 modem_lines, modem_irqs; /* enabled only with modem=1 */
	const char *use;
};

static const struct pixel_bank_desc descs[] = {
	{ "/pinctrl@10840000/gpp17", 0x10840000, 0x220, 0x44, 2, BIT(1), 0, 0, 0, "left amp reset" },
	{ "/pinctrl@10C40000/gpp25", 0x10c40000, 0x0a0, 0x14, 4, BIT(2), 0, 0, 0, "right amp reset" },
	{ "/pinctrl@180E0000/gpa6", 0x180e0000, 0x000, 0x00, 8, 0, BIT(3), 0, 0, "left amp interrupt" },
	{ "/pinctrl@180E0000/gpa8", 0x180e0000, 0x040, 0x08, 8, 0, BIT(6), BIT(2) | BIT(3) | BIT(4), BIT(3) | BIT(4), "right amp / optional modem interrupts" },
	{ "/pinctrl@11840000/gph0", 0x11840000, 0x000, 0, 6, 0, 0, BIT(2) | BIT(3), 0, "modem wake / dump" },
	{ "/pinctrl@11840000/gph1", 0x11840000, 0x020, 0, 7, 0, 0, BIT(1) | BIT(2) | BIT(3) | BIT(6), 0, "modem power / reset" },
	{ "/pinctrl@180D0000/gpa0", 0x180d0000, 0x000, 0, 8, 0, 0, BIT(2) | BIT(3), 0, "modem watchdog / PM reset" },
};

/* Manual bring-up only; normal audio startup keeps its existing pin set. */
static bool modem;
module_param(modem, bool, 0400);
MODULE_PARM_DESC(modem, "Expose CP GPIOs and wake/phone-active interrupts");

struct pixel_bank {
	const struct pixel_bank_desc *desc;
	u8 lines, irqs;
	void __iomem *regs;
	struct device_node *np;
	raw_spinlock_t lock;
	struct gpio_chip gc;
	struct irq_domain *domain;
	unsigned int parent[8];
	u32 saved_con[8], saved_pud[8];
};

static struct platform_device *pdev;
static struct pixel_bank banks[ARRAY_SIZE(descs)];

static void rmw(struct pixel_bank *b, u32 reg, u32 mask, u32 val)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&b->lock, flags);
	writel((readl(b->regs + reg) & ~mask) | (val & mask), b->regs + reg);
	raw_spin_unlock_irqrestore(&b->lock, flags);
}

static u32 nibble(unsigned int pin)
{
	return 0xf << (4 * pin);
}

static int bank_valid_mask(struct gpio_chip *gc, unsigned long *mask, unsigned int ngpios)
{
	struct pixel_bank *b = gpiochip_get_data(gc);

	*mask = b->lines;
	return 0;
}

static int bank_get_direction(struct gpio_chip *gc, unsigned int pin)
{
	struct pixel_bank *b = gpiochip_get_data(gc);
	u32 con = (readl(b->regs + b->desc->bank) >> (4 * pin)) & 0xf;

	return con == CON_OUTPUT ? GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static int bank_direction_input(struct gpio_chip *gc, unsigned int pin)
{
	struct pixel_bank *b = gpiochip_get_data(gc);

	rmw(b, b->desc->bank, nibble(pin), CON_INPUT << (4 * pin));
	return 0;
}

static int bank_set(struct gpio_chip *gc, unsigned int pin, int value)
{
	struct pixel_bank *b = gpiochip_get_data(gc);

	rmw(b, b->desc->bank + 4, BIT(pin), value ? BIT(pin) : 0);
	return 0;
}

static int bank_direction_output(struct gpio_chip *gc, unsigned int pin, int value)
{
	struct pixel_bank *b = gpiochip_get_data(gc);

	bank_set(gc, pin, value);
	rmw(b, b->desc->bank, nibble(pin), CON_OUTPUT << (4 * pin));
	return 0;
}

static int bank_get(struct gpio_chip *gc, unsigned int pin)
{
	struct pixel_bank *b = gpiochip_get_data(gc);

	return !!(readl(b->regs + b->desc->bank + 4) & BIT(pin));
}

static int bank_to_irq(struct gpio_chip *gc, unsigned int pin)
{
	struct pixel_bank *b = gpiochip_get_data(gc);
	unsigned int irq;

	if (pin >= b->desc->npins || !(b->irqs & BIT(pin)))
		return -ENXIO;
	irq = irq_create_mapping(b->domain, pin);
	return irq ? irq : -ENXIO;
}

static void eint_ack(struct irq_data *d)
{
	struct pixel_bank *b = irq_data_get_irq_chip_data(d);

	writel(BIT(d->hwirq), b->regs + EPEND + b->desc->eint);
}

static void eint_mask(struct irq_data *d)
{
	struct pixel_bank *b = irq_data_get_irq_chip_data(d);

	rmw(b, EMASK + b->desc->eint, BIT(d->hwirq), BIT(d->hwirq));
}

static void eint_unmask(struct irq_data *d)
{
	struct pixel_bank *b = irq_data_get_irq_chip_data(d);

	/* A level interrupt must not fire for a level that is gone. */
	if (irqd_get_trigger_type(d) & IRQ_TYPE_LEVEL_MASK)
		eint_ack(d);
	rmw(b, EMASK + b->desc->eint, BIT(d->hwirq), 0);
}

static int eint_set_type(struct irq_data *d, unsigned int type)
{
	struct pixel_bank *b = irq_data_get_irq_chip_data(d);
	u32 con;

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_LEVEL_LOW:
		con = 0;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		con = 1;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		con = 2;
		break;
	case IRQ_TYPE_EDGE_RISING:
		con = 3;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		con = 4;
		break;
	default:
		return -EINVAL;
	}
	rmw(b, ECON + b->desc->eint, nibble(d->hwirq), con << (4 * d->hwirq));
	irq_set_handler_locked(d, type & IRQ_TYPE_LEVEL_MASK ? handle_level_irq : handle_edge_irq);
	return 0;
}

static int eint_request_resources(struct irq_data *d)
{
	struct pixel_bank *b = irq_data_get_irq_chip_data(d);
	unsigned int pin = d->hwirq;
	int ret;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	if (b->lines & BIT(pin)) {
		ret = gpiochip_lock_as_irq(&b->gc, pin);
		if (ret) {
			module_put(THIS_MODULE);
			return ret;
		}
	}
	b->saved_con[pin] = readl(b->regs + b->desc->bank) & nibble(pin);
	b->saved_pud[pin] = readl(b->regs + b->desc->bank + 8) & nibble(pin);
	/* Preserve bootloader CP input pulls: the ROM wake signal needs the
	 * existing pull-up. Applying Android's later pull-down loses it.
	 * Amplifier EINTs retain their established no-pull configuration.
	 */
	if (!(b->desc->modem_irqs & BIT(pin)))
		rmw(b, b->desc->bank + 8, nibble(pin), 0);
	rmw(b, b->desc->bank, nibble(pin), CON_EINT << (4 * pin));
	return 0;
}

static void eint_release_resources(struct irq_data *d)
{
	struct pixel_bank *b = irq_data_get_irq_chip_data(d);
	unsigned int pin = d->hwirq;

	rmw(b, b->desc->bank, nibble(pin), b->saved_con[pin]);
	rmw(b, b->desc->bank + 8, nibble(pin), b->saved_pud[pin]);
	if (b->lines & BIT(pin))
		gpiochip_unlock_as_irq(&b->gc, pin);
	module_put(THIS_MODULE);
}

static int eint_set_wake(struct irq_data *d, unsigned int on)
{
	struct pixel_bank *b = irq_data_get_irq_chip_data(d);

	return irq_set_irq_wake(b->parent[d->hwirq], on);
}

static const struct irq_chip eint_chip = {
	.name = "gs201-weint",
	.irq_ack = eint_ack,
	.irq_mask = eint_mask,
	.irq_unmask = eint_unmask,
	.irq_set_type = eint_set_type,
	.irq_set_wake = eint_set_wake,
	.irq_request_resources = eint_request_resources,
	.irq_release_resources = eint_release_resources,
	.flags = IRQCHIP_SET_TYPE_MASKED,
};

static void eint_parent_handler(struct irq_desc *desc)
{
	struct pixel_bank *b = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long pend;
	unsigned int pin;

	chained_irq_enter(chip, desc);
	pend = readl(b->regs + EPEND + b->desc->eint) & ~readl(b->regs + EMASK + b->desc->eint);
	pend &= b->irqs;
	for_each_set_bit(pin, &pend, b->desc->npins)
		generic_handle_domain_irq(b->domain, pin);
	chained_irq_exit(chip, desc);
}

static int eint_xlate(struct irq_domain *d, struct device_node *np, const u32 *spec,
		      unsigned int size, irq_hw_number_t *hwirq, unsigned int *type)
{
	struct pixel_bank *b = d->host_data;

	if (size < 2 || spec[0] >= b->desc->npins || !(b->irqs & BIT(spec[0])))
		return -EINVAL;
	*hwirq = spec[0];
	*type = spec[1] & IRQ_TYPE_SENSE_MASK;
	return 0;
}

static int eint_map(struct irq_domain *d, unsigned int virq, irq_hw_number_t hwirq)
{
	struct pixel_bank *b = d->host_data;

	irq_set_chip_data(virq, b);
	irq_set_chip_and_handler(virq, &eint_chip, handle_level_irq);
	return 0;
}

static const struct irq_domain_ops eint_domain_ops = {
	.xlate = eint_xlate,
	.map = eint_map,
};

static void bank_remove_irqs(void *data)
{
	struct pixel_bank *b = data;
	unsigned int pin;

	for (pin = 0; pin < b->desc->npins; pin++) {
		if (!b->parent[pin])
			continue;
		rmw(b, EMASK + b->desc->eint, BIT(pin), BIT(pin));
		irq_set_chained_handler_and_data(b->parent[pin], NULL, NULL);
		synchronize_irq(b->parent[pin]);
		irq_dispose_mapping(b->parent[pin]);
		b->parent[pin] = 0;
	}
	if (b->domain) {
		for (pin = 0; pin < b->desc->npins; pin++)
			irq_dispose_mapping(irq_find_mapping(b->domain, pin));
		irq_domain_remove(b->domain);
		b->domain = NULL;
	}
}

static int bank_add_irqs(struct pixel_bank *b)
{
	unsigned long irqs = b->irqs;
	unsigned int pin;

	b->domain = irq_domain_create_linear(of_fwnode_handle(b->np), b->desc->npins,
					     &eint_domain_ops, b);
	if (!b->domain)
		return -ENOMEM;
	for_each_set_bit(pin, &irqs, b->desc->npins) {
		/* Masked until a consumer unmasks it. */
		rmw(b, EMASK + b->desc->eint, BIT(pin), BIT(pin));
		b->parent[pin] = irq_of_parse_and_map(b->np, pin);
		if (!b->parent[pin]) {
			bank_remove_irqs(b);
			return -EINVAL;
		}
		irq_set_chained_handler_and_data(b->parent[pin], eint_parent_handler, b);
	}
	return 0;
}

static void bank_put_node(void *data)
{
	struct pixel_bank *b = data;

	of_node_put(b->np);
	b->np = NULL;
}

static int pixel_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(descs); i++) {
		struct pixel_bank *b = &banks[i];

		b->desc = &descs[i];
		b->lines = b->desc->lines | (modem ? b->desc->modem_lines : 0);
		b->irqs = b->desc->irqs | (modem ? b->desc->modem_irqs : 0);
		if (!b->lines && !b->irqs)
			continue;
		raw_spin_lock_init(&b->lock);
		b->np = of_find_node_by_path(b->desc->node);
		if (!b->np)
			return dev_err_probe(dev, -ENODEV, "no %s\n", b->desc->node);
		ret = devm_add_action_or_reset(dev, bank_put_node, b);
		if (ret)
			return ret;
		b->regs = devm_ioremap(dev, b->desc->base, 0x1000);
		if (!b->regs)
			return -ENOMEM;
		if (b->irqs) {
			ret = bank_add_irqs(b);
			if (ret)
				return dev_err_probe(dev, ret, "%s interrupts\n", b->desc->node);
			ret = devm_add_action_or_reset(dev, bank_remove_irqs, b);
			if (ret)
				return ret;
		}
		if (b->lines) {
			struct gpio_chip *gc = &b->gc;

			gc->label = kbasename(b->desc->node);
			gc->parent = dev;
			gc->fwnode = of_fwnode_handle(b->np);
			gc->owner = THIS_MODULE;
			gc->base = -1;
			gc->ngpio = b->desc->npins;
			gc->init_valid_mask = bank_valid_mask;
			gc->get_direction = bank_get_direction;
			gc->direction_input = bank_direction_input;
			gc->direction_output = bank_direction_output;
			gc->get = bank_get;
			gc->set = bank_set;
			gc->to_irq = bank_to_irq;
			ret = devm_gpiochip_add_data(dev, gc, b);
			if (ret)
				return dev_err_probe(dev, ret, "%s GPIO chip\n", b->desc->node);
		}
		dev_info(dev, "%s: %s\n", kbasename(b->desc->node), b->desc->use);
	}
	return 0;
}

static struct platform_driver pixel_gpio_driver = {
	.probe = pixel_gpio_probe,
	.driver = { .name = "pixel-gpio", .suppress_bind_attrs = true },
};

static int __init pixel_gpio_init(void)
{
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	ret = platform_driver_register(&pixel_gpio_driver);
	if (ret)
		return ret;
	pdev = platform_device_register_simple("pixel-gpio", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&pixel_gpio_driver);
		return PTR_ERR(pdev);
	}
	if (!pdev->dev.driver) {
		platform_device_unregister(pdev);
		platform_driver_unregister(&pixel_gpio_driver);
		return -ENODEV;
	}
	return 0;
}
module_init(pixel_gpio_init);

static void __exit pixel_gpio_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&pixel_gpio_driver);
}
module_exit(pixel_gpio_exit);

MODULE_DESCRIPTION("GS201 GPIO lines and wake-up interrupts for stock DT consumers");
MODULE_LICENSE("GPL");
