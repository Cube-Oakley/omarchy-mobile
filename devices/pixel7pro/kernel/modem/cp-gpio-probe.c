// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the cpif GPIO lookup, ownership and wake IRQ path without PCIe,
 * firmware transfer or radio commands. Manual run=1; powers off and returns
 * -EAGAIN on success so no probe code remains loaded. Requires pixel-gpio
 * modem=1. Never run with cpif or another power probe active.
 */
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>

static bool run;
module_param(run, bool, 0400);

enum { PM_RESET, POWER, RESET, CP_RESET, ACTIVE, DUMP, AP_WAKE,
       CP_WATCHDOG, PS_HOLD, CP_WAKE, PHONE_ACTIVE, NR_LINES };

static const char *const names[NR_LINES] = {
	"gpio_ap2cp_pm_wrst_n", "gpio_ap2cp_cp_pwr_on", "gpio_ap2cp_nreset_n",
	"gpio_ap2cp_cp_wrst_n", "gpio_ap2cp_pda_active", "gpio_ap2cp_dump_noti",
	"gpio_ap2cp_wake_up", "gpio_cp2ap_cp_wrst_n", "gpio_cp2ap_cp_ps_hold",
	"gpio_cp2ap_wake_up", "gpio_cp2ap_phone_active",
};
static int gpios[NR_LINES];
static atomic_t rises = ATOMIC_INIT(0), falls = ATOMIC_INIT(0);
static int type_error;

static int lookup(struct device_node *np, const char *name)
{
	struct of_phandle_args args;
	struct gpio_device *gdev;
	struct gpio_desc *desc;
	int ret;

	ret = of_parse_phandle_with_args(np, name, "#gpio-cells", 0, &args);
	if (ret)
		return ret;
	gdev = gpio_device_find_by_fwnode(of_fwnode_handle(args.np));
	of_node_put(args.np);
	if (!gdev)
		return -EPROBE_DEFER;
	desc = args.args_count ? gpio_device_get_desc(gdev, args.args[0]) : ERR_PTR(-EINVAL);
	ret = IS_ERR(desc) ? PTR_ERR(desc) : desc_to_gpio(desc);
	gpio_device_put(gdev);
	return ret;
}

static irqreturn_t wake_irq(int irq, void *data)
{
	int high = gpio_get_value(gpios[CP_WAKE]);

	atomic_inc(high ? &rises : &falls);
	/* cpif also alternates the level after each CP wake transition. */
	WRITE_ONCE(type_error, irq_set_irq_type(irq,
		high ? IRQ_TYPE_LEVEL_LOW : IRQ_TYPE_LEVEL_HIGH));
	return IRQ_HANDLED;
}

static irqreturn_t active_irq(int irq, void *data)
{
	return IRQ_HANDLED;
}

static void set(unsigned int pin, int value, unsigned int delay_ms)
{
	gpio_set_value(gpios[pin], value);
	if (delay_ms)
		msleep(delay_ms);
}

static void power_off(void)
{
	set(RESET, 0, 0);
	set(CP_RESET, 0, 0);
	set(POWER, 0, 30);
	set(PM_RESET, 0, 50);
}

static int __init cp_gpio_probe_init(void)
{
	struct device_node *np;
	int i, ret, requested = 0, outputs = 0;
	int wake = 0, active = 0;
	bool wake_requested = false, active_requested = false, wake_enabled = false;

	if (!run || !of_machine_is_compatible("google,GS201"))
		return -EINVAL;
	np = of_find_node_by_path("/cpif");
	if (!np)
		return -ENODEV;
	for (i = 0; i < NR_LINES; i++) {
		gpios[i] = lookup(np, names[i]);
		if (gpios[i] < 0) {
			ret = gpios[i];
			goto out;
		}
		ret = gpio_request(gpios[i], "cp-gpio-probe");
		if (ret)
			goto out;
		requested++;
	}
	for (i = CP_WATCHDOG; i < NR_LINES; i++) {
		ret = gpio_direction_input(gpios[i]);
		if (ret)
			goto out;
	}
	for (i = 0; i < CP_WATCHDOG; i++) {
		ret = gpio_direction_output(gpios[i], 0);
		if (ret)
			goto out;
		outputs++;
	}
	wake = gpio_to_irq(gpios[CP_WAKE]);
	active = gpio_to_irq(gpios[PHONE_ACTIVE]);
	if (wake <= 0 || active <= 0) {
		ret = -ENXIO;
		goto out;
	}
	ret = request_irq(wake, wake_irq, IRQF_TRIGGER_HIGH, "cp-gpio-wake-test", &rises);
	if (ret)
		goto out;
	wake_requested = true;
	ret = request_irq(active, active_irq, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			  "cp-gpio-active-test", &falls);
	if (ret)
		goto out;
	active_requested = true;
	ret = enable_irq_wake(wake);
	if (ret)
		goto out;
	wake_enabled = true;
	pr_info("cp-gpio-probe: 11 GPIOs owned; wake/active IRQs and wake propagation ready\n");
	msleep(150);
	power_off();
	/* Discard any initial level before measuring the actual power cycle. */
	atomic_set(&rises, 0);
	atomic_set(&falls, 0);
	set(ACTIVE, 1, 0);
	set(PM_RESET, 1, 10);
	set(POWER, 1, 10);
	set(RESET, 1, 10);
	set(CP_RESET, 1, 0);
	msleep(500);
	pr_info("cp-gpio-probe: powered on: wake=%d rises=%d falls=%d\n",
		gpio_get_value(gpios[CP_WAKE]), atomic_read(&rises), atomic_read(&falls));
	msleep(150);
	ret = gpio_get_value(gpios[CP_WAKE]) && atomic_read(&rises) ? 0 : -EIO;
	power_off();
	msleep(500);
	if (gpio_get_value(gpios[CP_WAKE]) || !atomic_read(&falls) || READ_ONCE(type_error))
		ret = -EIO;
	pr_info("cp-gpio-probe: powered off: wake=%d rises=%d falls=%d type_error=%d result=%d\n",
		gpio_get_value(gpios[CP_WAKE]), atomic_read(&rises), atomic_read(&falls),
		READ_ONCE(type_error), ret);
	msleep(150);
out:
	if (outputs == CP_WATCHDOG)
		power_off();
	if (wake_enabled)
		disable_irq_wake(wake);
	if (active_requested)
		free_irq(active, &falls);
	if (wake_requested)
		free_irq(wake, &rises);
	for (i = 0; i < outputs; i++) {
		gpio_set_value(gpios[i], 0);
		gpio_direction_input(gpios[i]);
	}
	while (requested)
		gpio_free(gpios[--requested]);
	of_node_put(np);
	pr_info("cp-gpio-probe: cleanup complete result=%d\n", ret);
	return ret ?: -EAGAIN;
}
module_init(cp_gpio_probe_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Manual CP GPIO and wake IRQ power-cycle test");
