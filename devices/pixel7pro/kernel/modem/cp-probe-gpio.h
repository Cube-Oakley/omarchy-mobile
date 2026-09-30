/* SPDX-License-Identifier: GPL-2.0-only */
/* Private CP GPIO helpers for the disposable probes; see cp-power-probe.c. */
#ifndef PIXEL_CP_PROBE_GPIO_H
#define PIXEL_CP_PROBE_GPIO_H
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
static const struct line ap_wake = { "ap2cp_wake_up", GPH0, 2 };
static const struct line *const outputs[] = {
	&pm_wrst, &cp_pwr, &nreset, &cp_wrst, &pda_active, &dump_noti, &ap_wake,
};

/* CP to AP */
static const struct line cp2ap_wrst = { "cp2ap_cp_wrst_n", GPA0, 2 };
static const struct line ps_hold = { "cp2ap_cp_ps_hold", GPA8, 2 };
static const struct line cp2ap_wake = { "cp2ap_wake_up", GPA8, 3 };
static const struct line phone_active = { "cp2ap_phone_active", GPA8, 4 };
static const struct line *const inputs[] = {
	&cp2ap_wrst, &ps_hold, &cp2ap_wake, &phone_active,
};

static void __iomem *regs[NR_BANKS];
static DEFINE_SPINLOCK(lock);

static void cp_rmw(const struct line *l, unsigned int reg, u32 mask, u32 val)
{
	void __iomem *r = regs[l->bank] + reg;
	unsigned long flags;

	spin_lock_irqsave(&lock, flags);
	writel((readl(r) & ~mask) | (val & mask), r);
	spin_unlock_irqrestore(&lock, flags);
}

static int cp_get(const struct line *l)
{
	return !!(readl(regs[l->bank] + 4) & BIT(l->pin));
}

/* mif_gpio_set_value(): the level, then the delay only if it changed. The
 * level is cp_set before the pin becomes an output, so it never glitches.
 */
static void cp_set(const struct line *l, int value, unsigned int delay_ms)
{
	u32 nibble = 0xfU << (4 * l->pin);
	bool changed = cp_get(l) != value ||
		       (readl(regs[l->bank]) & nibble) != (1U << (4 * l->pin));

	cp_rmw(l, 4, BIT(l->pin), value ? BIT(l->pin) : 0);
	cp_rmw(l, 0, nibble, 1U << (4 * l->pin));
	pr_info("cp-power-probe: %s = %d\n", l->name, value);
	if (delay_ms && changed)
		msleep(delay_ms);
}

static void cp_release(const struct line *l)
{
	u32 nibble = 0xfU << (4 * l->pin);

	cp_rmw(l, 8, nibble, 1U << (4 * l->pin));	/* pull-down */
	cp_rmw(l, 0, nibble, 0);			/* input */
}

static void cp_print_lines(const char *when)
{
	const struct line *const *l;
	char buf[256];
	int n = 0;

	for (l = outputs; l < outputs + ARRAY_SIZE(outputs); l++)
		n += scnprintf(buf + n, sizeof(buf) - n, " %s=%d%s", (*l)->name, cp_get(*l),
			       readl(regs[(*l)->bank]) & (0xfU << (4 * (*l)->pin)) ? "" : "(in)");
	pr_info("cp-power-probe: %s: AP:%s\n", when, buf);
	n = 0;
	for (l = inputs; l < inputs + ARRAY_SIZE(inputs); l++)
		n += scnprintf(buf + n, sizeof(buf) - n, " %s=%d", (*l)->name, cp_get(*l));
	pr_info("cp-power-probe: %s: CP:%s\n", when, buf);
}

/* gpio_power_off_cp() */
static void cp_power_off(void)
{
	cp_set(&nreset, 0, 0);
	cp_set(&cp_wrst, 0, 0);
	cp_set(&cp_pwr, 0, 30);
	cp_set(&pm_wrst, 0, 50);
}

/* power_on_cp(): PDA_ACTIVE and DUMP_NOTI, then gpio_power_offon_cp() */
static void cp_power_on(void)
{
	cp_set(&pda_active, 1, 0);
	cp_set(&dump_noti, 0, 0);
	cp_power_off();
	cp_set(&pm_wrst, 1, 10);
	cp_set(&cp_pwr, 1, 10);
	cp_set(&nreset, 1, 10);
	cp_set(&cp_wrst, 1, 0);
}



#endif
