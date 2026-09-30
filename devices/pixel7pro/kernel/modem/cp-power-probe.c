// SPDX-License-Identifier: GPL-2.0-only
/* Modem bring-up stage 1: the S5300 CP's power through its GPIO lines alone,
 * with no PCIe. It never stays loaded (init returns -EAGAIN after acting):
 *
 *   insmod cp-power-probe.ko action=status   print the CP lines
 *   insmod cp-power-probe.ko action=on       cpif's power-on, then 2 s of
 *                                            the CP's lines
 *   insmod cp-power-probe.ko action=off      cpif's power-off, then the AP's
 *                                            lines back to the bootloader's
 *                                            state (input, pull-down)
 *
 * The sequences are gpio_power_offon_cp() and gpio_power_off_cp() of
 * Google's cpif (modem_ctrl_s5100.c, the branch without CP_WRESET_WA, which
 * Google's gs201 configuration leaves off), with power_on_cp()'s PDA_ACTIVE
 * and DUMP_NOTI first. The pins are the stock DT's /cpif lines. Bank offsets
 * and register layout are from Google's pinctrl-gs201.c: CON at +0 (4 bits
 * per pin, 0 input, 1 output), DAT at +4, PUD at +8. PERST# (gph0-0) and
 * PCIe are not touched.
 *
 * gph1-0 is the AoC's bus-ownership pin, and other pins of these banks
 * belong to other hardware. Every write is a read-modify-write of the CP's
 * own bits.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/string.h>

enum { GPH0, GPH1, GPA0, GPA8, NR_BANKS };

static const struct {
	const char *name;
	phys_addr_t base;
} bank_info[NR_BANKS] = {
	[GPH0] = { "gph0", 0x11840000 },	/* HSI1 */
	[GPH1] = { "gph1", 0x11840020 },	/* HSI1 */
	[GPA0] = { "gpa0", 0x180d0000 },	/* ALIVE */
	[GPA8] = { "gpa8", 0x180e0040 },	/* FAR_ALIVE */
};

struct line {
	const char *name;
	u8 bank, pin;
};

/* AP to CP */
static const struct line pm_wrst = { "ap2cp_pm_wrst_n", GPA0, 3 };
static const struct line cp_pwr = { "ap2cp_cp_pwr_on", GPH1, 2 };
static const struct line nreset = { "ap2cp_nreset_n", GPH1, 3 };
static const struct line cp_wrst = { "ap2cp_cp_wrst_n", GPH1, 6 };
static const struct line pda_active = { "ap2cp_pda_active", GPH1, 1 };
static const struct line dump_noti = { "ap2cp_dump_noti", GPH0, 3 };
static const struct line *const outputs[] = {
	&pm_wrst, &cp_pwr, &nreset, &cp_wrst, &pda_active, &dump_noti,
};

/* CP to AP */
static const struct line cp2ap_wrst = { "cp2ap_cp_wrst_n", GPA0, 2 };
static const struct line ps_hold = { "cp2ap_cp_ps_hold", GPA8, 2 };
static const struct line cp2ap_wake = { "cp2ap_wake_up", GPA8, 3 };
static const struct line phone_active = { "cp2ap_phone_active", GPA8, 4 };
static const struct line *const inputs[] = {
	&cp2ap_wrst, &ps_hold, &cp2ap_wake, &phone_active,
};

static char *action = "status";
module_param(action, charp, 0400);
MODULE_PARM_DESC(action, "status, on or off");

static void __iomem *regs[NR_BANKS];
static DEFINE_SPINLOCK(lock);

static void rmw(const struct line *l, unsigned int reg, u32 mask, u32 val)
{
	void __iomem *r = regs[l->bank] + reg;
	unsigned long flags;

	spin_lock_irqsave(&lock, flags);
	writel((readl(r) & ~mask) | (val & mask), r);
	spin_unlock_irqrestore(&lock, flags);
}

static int get(const struct line *l)
{
	return !!(readl(regs[l->bank] + 4) & BIT(l->pin));
}

/* mif_gpio_set_value(): the level, then the delay only if it changed. The
 * level is set before the pin becomes an output, so it never glitches.
 */
static void set(const struct line *l, int value, unsigned int delay_ms)
{
	u32 nibble = 0xfU << (4 * l->pin);
	bool changed = get(l) != value ||
		       (readl(regs[l->bank]) & nibble) != (1U << (4 * l->pin));

	rmw(l, 4, BIT(l->pin), value ? BIT(l->pin) : 0);
	rmw(l, 0, nibble, 1U << (4 * l->pin));
	pr_info("cp-power-probe: %s = %d\n", l->name, value);
	if (delay_ms && changed)
		msleep(delay_ms);
}

static void release(const struct line *l)
{
	u32 nibble = 0xfU << (4 * l->pin);

	rmw(l, 8, nibble, 1U << (4 * l->pin));	/* pull-down */
	rmw(l, 0, nibble, 0);			/* input */
}

static void print_lines(const char *when)
{
	const struct line *const *l;
	char buf[256];
	int n = 0;

	for (l = outputs; l < outputs + ARRAY_SIZE(outputs); l++)
		n += scnprintf(buf + n, sizeof(buf) - n, " %s=%d%s", (*l)->name, get(*l),
			       readl(regs[(*l)->bank]) & (0xfU << (4 * (*l)->pin)) ? "" : "(in)");
	pr_info("cp-power-probe: %s: AP:%s\n", when, buf);
	n = 0;
	for (l = inputs; l < inputs + ARRAY_SIZE(inputs); l++)
		n += scnprintf(buf + n, sizeof(buf) - n, " %s=%d", (*l)->name, get(*l));
	pr_info("cp-power-probe: %s: CP:%s\n", when, buf);
}

/* gpio_power_off_cp() */
static void power_off(void)
{
	set(&nreset, 0, 0);
	set(&cp_wrst, 0, 0);
	set(&cp_pwr, 0, 30);
	set(&pm_wrst, 0, 50);
}

/* power_on_cp(): PDA_ACTIVE and DUMP_NOTI, then gpio_power_offon_cp() */
static void power_on(void)
{
	set(&pda_active, 1, 0);
	set(&dump_noti, 0, 0);
	power_off();
	set(&pm_wrst, 1, 10);
	set(&cp_pwr, 1, 10);
	set(&nreset, 1, 10);
	set(&cp_wrst, 1, 0);
}

/* The CP's lines every 10 ms for 2 s: each change, with its time. */
static void watch(void)
{
	ktime_t start = ktime_get();
	int last[ARRAY_SIZE(inputs)], i, v;

	for (i = 0; i < ARRAY_SIZE(inputs); i++)
		last[i] = get(inputs[i]);
	while (ktime_ms_delta(ktime_get(), start) < 2000) {
		for (i = 0; i < ARRAY_SIZE(inputs); i++) {
			v = get(inputs[i]);
			if (v != last[i])
				pr_info("cp-power-probe: +%lld ms %s -> %d\n",
					ktime_ms_delta(ktime_get(), start), inputs[i]->name, v);
			last[i] = v;
		}
		usleep_range(10000, 11000);
	}
}

static int __init cp_power_probe_init(void)
{
	int i, ret = -EAGAIN;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	for (i = 0; i < NR_BANKS; i++) {
		regs[i] = ioremap(bank_info[i].base, 0x10);
		if (!regs[i]) {
			ret = -ENOMEM;
			goto unmap;
		}
	}
	print_lines("before");
	if (!strcmp(action, "on")) {
		power_on();
		print_lines("after power-on");
		watch();
		print_lines("2 s after power-on");
	} else if (!strcmp(action, "off")) {
		power_off();
		for (i = 0; i < ARRAY_SIZE(outputs); i++)
			release(outputs[i]);
		print_lines("after power-off");
	} else if (strcmp(action, "status")) {
		ret = -EINVAL;
	}
unmap:
	for (i = 0; i < NR_BANKS; i++)
		if (regs[i])
			iounmap(regs[i]);
	return ret;
}
module_init(cp_power_probe_init);

MODULE_DESCRIPTION("GS201 S5300 modem power through its GPIO lines (bring-up stage 1)");
MODULE_LICENSE("GPL");
