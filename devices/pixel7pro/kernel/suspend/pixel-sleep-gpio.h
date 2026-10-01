/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * GPIO bank state across SYS_SLEEP, as stock's Samsung pin controller keeps
 * it (this kernel has no GS201 pin controller driver).
 *
 * The PERIC0, PERIC1, HSI1 and HSI2 pin banks lose their registers when their
 * domains power down in SYS_SLEEP. Pad retention holds the pins meanwhile, and
 * the exit list's TOP_OUT writes release it. Stock saves every bank that has a
 * power-down configuration (samsung_pinctrl_suspend_dev(),
 * pinctrl-samsung.c:1271-1319: CON, DAT, PUD, DRV, CON_PDN, PUD_PDN) and each
 * EINT bank's ECON, EFLTCON and EMASK (exynos_pinctrl_suspend_bank(),
 * pinctrl-exynos.c:733-758), and puts them back before exynos-pm releases
 * retention: its syscore ops are registered at postcore_initcall, before
 * exynos-pm's, so they resume first (pinctrl-samsung.c:1499-1504), EINT
 * registers before the bank registers (pinctrl-exynos.c:785-819,
 * pinctrl-samsung.c:1331-1360). Without that, the first deep resume left
 * every such pin at its reset default; the modem's control lines on gph0/gph1
 * dropped and cpif declared a CP crash.
 *
 * While a domain is down, each pin takes its CON_PDN state (2 bits: output
 * 0, output 1, input, previous) with its PUD_PDN pull (4 bits). Stock's pin
 * controller programs these from the consumers' pinctrl states; nothing here
 * does, so the bootloader's values would apply. psg_pdn[] holds the stock
 * states of the HSI1/HSI2 pins: the modem's control lines (s5100_ap2cp_*,
 * gs201-cloudripper-cp-s5300-sit.dtsi:91-133), both PCIe ports' PERST and
 * CLKREQ (gs201-pinctrl.dtsi:258-271, 343-358), WLAN_EN and WLAN_DEV_WAKE
 * (gs201-cloudripper-wlan.dtsi:23-40) and UFS RST_N and REFCLK
 * (gs201-pinctrl.dtsi:1182-1195): all "previous". They are written at arm,
 * before the banks are saved.
 *
 * The always-on banks (ALIVE, FAR_ALIVE: bank_type_7, no power-down field)
 * keep their state and are not touched, as in stock; nor are the secure GSA
 * banks, which have no suspend hooks (pinctrl-gs201.c:175-185).
 * Bank tables from pinctrl-gs201.c:105-155 (EXYNOS9_PIN_BANK_EINTG: pins, bank
 * offset, EINT offset, filter offset); register offsets from bank_type_6
 * (pinctrl-gs201.c:57-61) and pinctrl-exynos.h:20-22.
 */
#ifndef PIXEL_SLEEP_GPIO_H
#define PIXEL_SLEEP_GPIO_H

#define PSG_ECON		0x700
#define PSG_EFLTCON		0x800
#define PSG_EMASK		0x900
#define PSG_NREGS		6	/* CON, DAT, PUD, DRV, CON_PDN, PUD_PDN */

struct psg_bank {
	const char *name;
	u16 off;		/* bank registers */
	u8 pins;
	u8 eint, flt;		/* EINT and filter offsets */
};

struct psg_ctl {
	const char *name;
	phys_addr_t pa;
	u16 pd_status;		/* PMU status of the domain; 0: TOP, on while awake */
	const struct psg_bank *banks;
	unsigned int nbanks;
};

#define PSG_B(n, p, o, e, f)	{ n, o, p, e, f }

static const struct psg_bank psg_peric0[] = {
	PSG_B("gpp0", 5, 0x000, 0x00, 0x00), PSG_B("gpp1", 4, 0x020, 0x04, 0x08),
	PSG_B("gpp2", 4, 0x040, 0x08, 0x0c), PSG_B("gpp3", 2, 0x060, 0x0c, 0x10),
	PSG_B("gpp4", 4, 0x080, 0x10, 0x14), PSG_B("gpp5", 2, 0x0a0, 0x14, 0x18),
	PSG_B("gpp6", 4, 0x0c0, 0x18, 0x1c), PSG_B("gpp7", 2, 0x0e0, 0x1c, 0x20),
	PSG_B("gpp8", 4, 0x100, 0x20, 0x24), PSG_B("gpp9", 2, 0x120, 0x24, 0x28),
	PSG_B("gpp10", 4, 0x140, 0x28, 0x2c), PSG_B("gpp11", 2, 0x160, 0x2c, 0x30),
	PSG_B("gpp12", 4, 0x180, 0x30, 0x34), PSG_B("gpp13", 2, 0x1a0, 0x34, 0x38),
	PSG_B("gpp14", 4, 0x1c0, 0x38, 0x3c), PSG_B("gpp15", 2, 0x1e0, 0x3c, 0x40),
	PSG_B("gpp16", 4, 0x200, 0x40, 0x44), PSG_B("gpp17", 2, 0x220, 0x44, 0x48),
	PSG_B("gpp18", 4, 0x240, 0x48, 0x4c), PSG_B("gpp19", 5, 0x260, 0x4c, 0x50),
};
static const struct psg_bank psg_peric1[] = {
	PSG_B("gpp20", 8, 0x00, 0x00, 0x00), PSG_B("gpp21", 4, 0x20, 0x04, 0x08),
	PSG_B("gpp22", 2, 0x40, 0x08, 0x0c), PSG_B("gpp23", 8, 0x60, 0x0c, 0x10),
	PSG_B("gpp24", 4, 0x80, 0x10, 0x18), PSG_B("gpp25", 4, 0xa0, 0x14, 0x1c),
	PSG_B("gpp26", 5, 0xc0, 0x18, 0x20), PSG_B("gpp27", 4, 0xe0, 0x1c, 0x28),
};
static const struct psg_bank psg_hsi1[] = {
	PSG_B("gph0", 6, 0x00, 0x00, 0x00), PSG_B("gph1", 7, 0x20, 0x04, 0x08),
};
static const struct psg_bank psg_hsi2[] = {
	PSG_B("gph2", 6, 0x00, 0x00, 0x00), PSG_B("gph4", 6, 0x20, 0x04, 0x08),
};
static const struct psg_bank psg_hsi2ufs[] = {
	PSG_B("gph3", 2, 0x00, 0x00, 0x00),
};

/* PMU HSI1_STATUS 0x2104, HSI2_STATUS 0x2184 (flexpmu_cal_local_gs201.h). */
static const struct psg_ctl psg_ctls[] = {
	{ "peric0", 0x10840000, 0, psg_peric0, ARRAY_SIZE(psg_peric0) },
	{ "peric1", 0x10c40000, 0, psg_peric1, ARRAY_SIZE(psg_peric1) },
	{ "hsi1", 0x11840000, 0x2104, psg_hsi1, ARRAY_SIZE(psg_hsi1) },
	{ "hsi2", 0x14440000, 0x2184, psg_hsi2, ARRAY_SIZE(psg_hsi2) },
	{ "hsi2ufs", 0x14460000, 0x2184, psg_hsi2ufs, ARRAY_SIZE(psg_hsi2ufs) },
};

/* Stock pin power-down states: controller, bank, pin, CON_PDN, PUD_PDN
 * (-1: stock leaves it unset).
 */
struct psg_pdn {
	u8 ctl, bank, pin;
	s8 con, pud;
};

#define PDN_PREV	3
#define PUD_NONE	0
#define PUD_DOWN	1
#define PUD_UP		3

static const struct psg_pdn psg_pdn[] = {
	{ 2, 0, 0, PDN_PREV, -1 },		/* pcie0_perst */
	{ 2, 0, 1, PDN_PREV, PUD_UP },		/* pcie0_clkreq */
	{ 2, 0, 2, PDN_PREV, PUD_NONE },	/* s5100_ap2cp_wakeup */
	{ 2, 0, 3, PDN_PREV, PUD_NONE },	/* s5100_ap2cp_dump_noti */
	{ 2, 1, 1, PDN_PREV, PUD_NONE },	/* s5100_ap2cp_pda_active */
	{ 2, 1, 2, PDN_PREV, PUD_NONE },	/* s5100_ap2cp_cp_pwr_on */
	{ 2, 1, 3, PDN_PREV, PUD_NONE },	/* s5100_ap2cp_cp_nreset_n */
	{ 2, 1, 6, PDN_PREV, PUD_NONE },	/* s5100_ap2cp_cp_wrst_n */
	{ 3, 0, 0, PDN_PREV, PUD_NONE },	/* pcie1_perst */
	{ 3, 0, 1, PDN_PREV, PUD_UP },		/* pcie1_clkreq */
	{ 3, 0, 4, PDN_PREV, PUD_NONE },	/* cfg_wlanen */
	{ 3, 0, 5, PDN_PREV, PUD_DOWN },	/* wlan_dev_wake */
	{ 4, 0, 0, PDN_PREV, PUD_NONE },	/* ufs_refclk_out */
	{ 4, 0, 1, PDN_PREV, PUD_NONE },	/* ufs_rst_n */
};

#define PSG_MAX_BANKS		20

static struct {
	void __iomem *va[ARRAY_SIZE(psg_ctls)];
	bool saved[ARRAY_SIZE(psg_ctls)];
	u32 regs[ARRAY_SIZE(psg_ctls)][PSG_MAX_BANKS][PSG_NREGS];
	u32 econ[ARRAY_SIZE(psg_ctls)][PSG_MAX_BANKS];
	u32 eflt[ARRAY_SIZE(psg_ctls)][PSG_MAX_BANKS][2];
	u32 emask[ARRAY_SIZE(psg_ctls)][PSG_MAX_BANKS];
	unsigned int saved_banks, restored, differed, skipped;
	unsigned int pdn_differed, pdn_written;
} psg;

static bool psg_powered(const struct psg_ctl *c)
{
	return !c->pd_status || (ps_pmu_read(c->pd_status) & BIT(0));
}

static int psg_map(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(psg_ctls); i++) {
		psg.va[i] = ioremap(psg_ctls[i].pa, 0x1000);
		if (!psg.va[i])
			return -ENOMEM;
	}
	return 0;
}

static void psg_unmap(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(psg_ctls); i++)
		if (psg.va[i])
			iounmap(psg.va[i]);
}

/* samsung_pinconf_rw() for PINCFG_TYPE_CON_PDN / PUD_PDN (fields 2 and 4
 * bits wide, bank_type_6). With write false, only count.
 */
static void psg_apply_pdn(bool write)
{
	unsigned int i;

	psg.pdn_differed = psg.pdn_written = 0;
	for (i = 0; i < ARRAY_SIZE(psg_pdn); i++) {
		const struct psg_pdn *d = &psg_pdn[i];
		const struct psg_ctl *c = &psg_ctls[d->ctl];
		void __iomem *base = psg.va[d->ctl] + c->banks[d->bank].off;
		u32 con, pud, ncon, npud;

		if (!psg_powered(c))
			continue;
		con = readl(base + 0x10);
		pud = readl(base + 0x14);
		ncon = (con & ~(0x3 << (2 * d->pin))) | ((u32)d->con << (2 * d->pin));
		npud = d->pud < 0 ? pud :
		       (pud & ~(0xf << (4 * d->pin))) | ((u32)d->pud << (4 * d->pin));
		if (ncon == con && npud == pud)
			continue;
		psg.pdn_differed++;
		if (!write)
			continue;
		writel(ncon, base + 0x10);
		writel(npud, base + 0x14);
		psg.pdn_written++;
	}
}

/* samsung_pinctrl_suspend_dev() + exynos_pinctrl_suspend_bank(). */
static void psg_save(void)
{
	unsigned int i, b, r;

	psg.saved_banks = psg.restored = psg.differed = psg.skipped = 0;
	for (i = 0; i < ARRAY_SIZE(psg_ctls); i++) {
		const struct psg_ctl *c = &psg_ctls[i];
		void __iomem *va = psg.va[i];

		psg.saved[i] = psg_powered(c);
		if (!psg.saved[i]) {
			psg.skipped += c->nbanks;
			continue;
		}
		for (b = 0; b < c->nbanks; b++) {
			const struct psg_bank *k = &c->banks[b];

			for (r = 0; r < PSG_NREGS; r++)
				psg.regs[i][b][r] = readl(va + k->off + 4 * r);
			psg.econ[i][b] = readl(va + PSG_ECON + k->eint);
			psg.eflt[i][b][0] = readl(va + PSG_EFLTCON + k->flt);
			if (k->pins > 4)
				psg.eflt[i][b][1] = readl(va + PSG_EFLTCON + k->flt + 4);
			psg.emask[i][b] = readl(va + PSG_EMASK + k->eint);
			psg.saved_banks++;
		}
	}
}

static void psg_put(void __iomem *reg, u32 val, bool write)
{
	if (readl(reg) == val)
		return;
	psg.differed++;
	if (write) {
		writel(val, reg);
		psg.restored++;
	}
}

/* exynos_pinctrl_resume_bank() then samsung_pinctrl_resume_dev(); before
 * the exit list releases pad retention. With write false, only count.
 */
static void psg_restore(bool write)
{
	unsigned int i, b, r;

	for (i = 0; i < ARRAY_SIZE(psg_ctls); i++) {
		const struct psg_ctl *c = &psg_ctls[i];
		void __iomem *va = psg.va[i];

		if (!psg.saved[i])
			continue;
		if (!psg_powered(c)) {
			psg.skipped += c->nbanks;
			continue;
		}
		for (b = 0; b < c->nbanks; b++) {
			const struct psg_bank *k = &c->banks[b];

			psg_put(va + PSG_ECON + k->eint, psg.econ[i][b], write);
			psg_put(va + PSG_EFLTCON + k->flt, psg.eflt[i][b][0], write);
			if (k->pins > 4)
				psg_put(va + PSG_EFLTCON + k->flt + 4, psg.eflt[i][b][1], write);
			psg_put(va + PSG_EMASK + k->eint, psg.emask[i][b], write);
			for (r = 0; r < PSG_NREGS; r++)
				psg_put(va + k->off + 4 * r, psg.regs[i][b][r], write);
		}
		psg.saved[i] = false;
	}
}

#endif
