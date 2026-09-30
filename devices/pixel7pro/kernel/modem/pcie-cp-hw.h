/* SPDX-License-Identifier: GPL-2.0-only */
/* GS201 channel-0 hardware sequence shared by the disposable probe and host.
 * Sources pinned in sources.json; no DWC host/iATU calls.
 */
#ifndef PIXEL_PCIE_CP_HW_H
#define PIXEL_PCIE_CP_HW_H

#define GS201_SMC_PRIV_REG	0x82000504

/* ELBI (sub-controller) registers, pcie-exynos-rc.h */
#define ELBI_IRQ0		0x000
#define ELBI_IRQ1		0x004
#define ELBI_IRQ2		0x008
#define ELBI_IRQ0_EN		0x010
#define ELBI_IRQ1_EN		0x014
#define ELBI_IRQ2_EN		0x018
#define IRQ0_INTX		(BIT(14) | BIT(16) | BIT(18) | BIT(20))
#define IRQ1_LINK_DOWN		BIT(10)
#define IRQ0_RADM_PM_TO_ACK	BIT(29)
#define IRQ2_CPL_TIMEOUT	BIT(24)
#define ELBI_APP_REQ_EXIT_L1	0x06c
#define ELBI_XMIT_PME_TURNOFF	0x118
#define ELBI_LTSSM_EN		0x054
#define ELBI_DEVICE_TYPE	0x080
#define DEVICE_TYPE_RC		0x4
#define ELBI_LTSSM_STATE	0x2c8
#define LTSSM_STATE		GENMASK(5, 0)
#define LTSSM_RCVRY_LOCK	0x0d
#define LTSSM_L0		0x11
#define LTSSM_L1_IDLE		0x14
#define LTSSM_L2_IDLE		0x15
#define ELBI_IA_IRQ_SEL		0x388
#define ELBI_LINKDOWN_RST	0x3a0
#define LINKDOWN_RST_MANUAL	BIT(1)
#define ELBI_SOFT_RESET		0x3a4
#define SOFT_PWR_RESET		BIT(1)
#define SOFT_NON_STICKY_RESET	BIT(3)
#define ELBI_QCH_SEL		0x3a8
#define QCH_GATING		GENMASK(11, 0)
#define ELBI_L1_EXIT_MODE	0x3bc
#define APP_REQ_EXIT_L1		BIT(0)
#define L1_REQ_NAK_MASTER	BIT(4)
#define ELBI_MSTR_PEND_NAK	0x474
#define ELBI_DBI_L1_EXIT_DIS	0x1078
#define ELBI_PHY_PORT_RST	0x1400
#define ELBI_PHY_CMN_RST	0x1404
#define ELBI_PHY_INIT_RST	0x1408

/* DBI registers beyond the standard header (pcie-exynos-rc.h) */
#define DBI_PM_CTRL		0x44
#define DBI_GEN3_RELATED	0x890
#define GEN3_EQ_OFF		0x12000
#define DBI_AUX_CLK_FREQ	0xb40
#define AUX_CLK_26MHZ		0x1a
#define DBI_L1_SUBSTATES	0xb44
#define L1_SUB_VAL		0xea
#define DBI_COHERENCY_3		0x8e8
#define AXCACHE_ALLOCATE	0x10101010


#define RC_EXP_CAP 0x70
#define GPIO_CON 0x00
#define GPIO_DAT 0x04
#define GPIO_PUD 0x08
#define GPIO_CONPDN 0x10
#define GPIO_PUDPDN 0x14
#define PIN_PERST 0
#define PIN_CLKREQ 1
#define CON_OUTPUT 1
#define CON_CLKREQ 2
#define PUD_NONE 0
#define PUD_UP 3
#define CONPDN_PREV 3

struct pixel_pcie {
	struct device *dev;
	void __iomem *elbi, *phy, *pcs, *ia, *sysreg, *dbi, *dbi2;
	void __iomem *config, *doorbell, *gpio;
	phys_addr_t isolation;
	unsigned int diagnostic_delay_ms;
};

static void stage(struct pixel_pcie *p, const char *what)
{
	dev_info(p->dev, "stage: %s\n", what);
	if (p->diagnostic_delay_ms)
		msleep(p->diagnostic_delay_ms);
}

static u32 elbi_read(struct pixel_pcie *p, u32 reg)
{
	return readl(p->elbi + reg);
}

static void elbi_write(struct pixel_pcie *p, u32 reg, u32 val)
{
	writel(val, p->elbi + reg);
}

static void rmw(void __iomem *reg, u32 clear, u32 set)
{
	writel((readl(reg) & ~clear) | set, reg);
}

static void pin_field(struct pixel_pcie *p, u32 reg, unsigned int pin,
			  unsigned int width, u32 val)
{
	u32 shift = pin * width;

	rmw(p->gpio + reg, GENMASK(shift + width - 1, shift), val << shift);
}

static void pin_config(struct pixel_pcie *p, unsigned int pin, u32 con, u32 pud,
			   u32 pudpdn)
{
	pin_field(p, GPIO_PUD, pin, 4, pud);
	pin_field(p, GPIO_CONPDN, pin, 2, CONPDN_PREV);
	pin_field(p, GPIO_PUDPDN, pin, 4, pudpdn);
	pin_field(p, GPIO_CON, pin, 4, con);
}

static void pin_set(struct pixel_pcie *p, unsigned int pin, bool high)
{
	pin_field(p, GPIO_DAT, pin, 1, high);
}

/* Same secure-register path as pixel-reboot.c; channel 0's PMU PHY control
 * is bit 0 (0 isolates, 1 bypasses) and nothing else in the register is ours.
 */
static int pixel_pcie_isolation(struct pixel_pcie *p, bool bypass)
{
	struct arm_smccc_res res;
	void __iomem *reg;
	u32 val;

	reg = ioremap(p->isolation, 4);
	if (!reg)
		return -ENOMEM;
	val = readl(reg);
	val = bypass ? val | BIT(0) : val & ~BIT(0);
	arm_smccc_smc(GS201_SMC_PRIV_REG, p->isolation, 1, val, 0, 0, 0, 0, &res);
	val = readl(reg);
	iounmap(reg);
	if (res.a0 || !!(val & BIT(0)) != bypass) {
		dev_err(p->dev, "PHY isolation write failed: SMC %ld, PMU %#x\n",
			(long)res.a0, val);
		return -EIO;
	}
	return 0;
}

/* exynos_pcie_rc_phy_all_pwrdn() */
static void pixel_phy_power_down(struct pixel_pcie *p)
{
	static const u16 regs[] = { 0x404, 0x408, 0x40c, 0x800, 0x804, 0xa40, 0xa44,
					0xa48, 0xa4c, 0xa50, 0x1000, 0x1004, 0x1240,
					0x1244, 0x1248, 0x124c, 0x1250 };
	static const u8 vals[] = { 0xa8, 0x20, 0x0a, 0x0a, 0xbf, 0x02, 0x2a, 0xaa,
				   0xa8, 0x80, 0x0a, 0xbf, 0x02, 0x2a, 0xaa, 0xa8,
				   0x80 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++)
		writel(vals[i], p->phy + regs[i]);
	rmw(p->phy + 0x400, BIT(7), 0);		/* PMA off */
}

/* exynos_pcie_rc_phy_all_pwrdn_clear() */
static void pixel_phy_power_up(struct pixel_pcie *p)
{
	static const u16 regs[] = { 0x404, 0x408, 0x40c, 0x800, 0x804, 0xa40, 0xa44,
					0xa48, 0xa4c, 0xa50, 0x1000, 0x1004, 0x1240,
					0x1244, 0x1248, 0x124c, 0x1250 };
	unsigned int i;

	writel(0x28, p->phy + 0xd8);
	mdelay(1);
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		writel(0, p->phy + regs[i]);
}

struct reg_val {
	u16 reg;
	u8 val;
};

static void write_table(void __iomem *base, const struct reg_val *t, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		writel(t[i].val, base + t[i].reg);
}

/* exynos_pcie_rc_pcie_phy_config(), CAL ver 210802, two lanes */
static const struct reg_val pma_common[] = {
	{ 0x018, 0x50 }, { 0x048, 0x33 }, { 0x068, 0x01 }, { 0x070, 0x12 },
	{ 0x08c, 0x00 }, { 0x090, 0x21 }, { 0x0b0, 0x14 }, { 0x0b8, 0x50 },
	{ 0x0e0, 0x51 }, { 0x100, 0x00 }, { 0x104, 0x80 }, { 0x140, 0x38 },
	{ 0x180, 0xa4 }, { 0x188, 0x03 }, { 0x2a8, 0x38 }, { 0x2e4, 0x12 },
	/* PMA enable, aggregation mode */
	{ 0x400, 0x80 }, { 0x408, 0x20 }, { 0x550, 0x00 }, { 0x5a8, 0x00 },
	{ 0x5ec, 0xff },
	/* RC: 100 MHz reference out, differential REFCLK source, L1.2 delay */
	{ 0x458, 0x02 }, { 0x5b0, 0x34 }, { 0x450, 0x20 },
	/* 38.4 MHz input clock */
	{ 0x0ac, 0x34 }, { 0x0b4, 0x34 }, { 0x0e0, 0x50 }, { 0x0f4, 0x00 },
	{ 0x0f8, 0x08 }, { 0x104, 0xa0 }, { 0x11c, 0x19 }, { 0x124, 0x17 },
	{ 0x220, 0x41 },
};

/* Lane 0, in the vendor's order; some registers are written twice. */
static const struct reg_val pma_lane0[] = {
	{ 0x82c, 0x08 }, { 0x830, 0x24 }, { 0x878, 0x80 }, { 0x894, 0x40 },
	{ 0x8c0, 0x00 }, { 0x8f4, 0x30 }, { 0x908, 0x05 }, { 0x90c, 0xe0 },
	{ 0x914, 0xd4 }, { 0x91c, 0xd3 }, { 0x920, 0xce }, { 0x924, 0x01 },
	{ 0x928, 0x35 }, { 0x92c, 0xba }, { 0x930, 0x41 }, { 0x934, 0x15 },
	{ 0x938, 0x13 }, { 0x93c, 0x4e }, { 0x948, 0x43 }, { 0x94c, 0xfc },
	{ 0x954, 0x10 }, { 0x958, 0x69 }, { 0x964, 0x40 }, { 0x9b4, 0xf6 },
	{ 0x9c0, 0x2d }, { 0x9c4, 0xb7 }, { 0x9cc, 0x3c }, { 0x9dc, 0x7e },
	{ 0xa40, 0x02 }, { 0xa70, 0x26 }, { 0xa74, 0x00 }, { 0xb40, 0x06 },
	{ 0xb44, 0x06 }, { 0xb48, 0x04 }, { 0xb4c, 0x03 }, { 0xb50, 0x03 },
	{ 0xb54, 0x03 }, { 0xb58, 0x03 }, { 0xb5c, 0x03 }, { 0xc10, 0x1b },
	{ 0xc44, 0x10 }, { 0xc48, 0x10 }, { 0xc4c, 0x10 }, { 0xc50, 0x10 },
	{ 0xc54, 0x02 }, { 0xc58, 0x02 }, { 0xc5c, 0x02 }, { 0xc60, 0x02 },
	{ 0xc6c, 0x02 }, { 0xc70, 0x02 }, { 0xca8, 0xe7 }, { 0xcac, 0x00 },
	{ 0xcb0, 0x0e }, { 0xccc, 0x1c }, { 0xcd4, 0x05 }, { 0xcd8, 0x77 },
	{ 0xcdc, 0x7a }, { 0xdb4, 0x2f },
	/* RX tuning */
	{ 0x8fc, 0x80 }, { 0x914, 0xf4 }, { 0x91c, 0xd3 }, { 0x920, 0xca },
	{ 0x928, 0x3d }, { 0x92c, 0xb8 }, { 0x930, 0x41 }, { 0x934, 0x17 },
	{ 0x93c, 0x4c }, { 0x948, 0x73 }, { 0x94c, 0xfc }, { 0x96c, 0x55 },
	{ 0x988, 0x78 }, { 0x994, 0x3b }, { 0x9b4, 0xf6 }, { 0x9c4, 0xff },
	{ 0x9c8, 0x20 }, { 0xa08, 0x2f }, { 0xb9c, 0x3f },
	/* TX termination and drive */
	{ 0x9cc, 0x00 }, { 0xcd8, 0xff }, { 0xcdc, 0x6e },
	{ 0x82c, 0x0f }, { 0x830, 0x60 }, { 0x834, 0x7e },
	/* auto FBB, DFE */
	{ 0xc08, 0x00 }, { 0xc10, 0x09 }, { 0xc40, 0x04 }, { 0xc70, 0x00 },
	{ 0x9b4, 0x76 },

};

/* The vendor's second lane loop adds i * 0x800 cumulatively, so lane 1 of the
 * 38.4 MHz block lands at 0x800 above lane 0.
 */
static const struct reg_val pma_lane1_clk[] = {
	{ 0x13cc, 0x41 }, { 0x13d4, 0x41 }, { 0x13dc, 0x68 }, { 0x13e0, 0x00 },
	{ 0x13e4, 0xd0 },
};

static void pixel_phy_config(struct pixel_pcie *p)
{
	static const struct { u16 reg; u8 val; } reset_values[] = {
		{ 0x010, 0x55 }, { 0x014, 0x51 }, { 0x040, 0x50 },
		{ 0x044, 0x0c }, { 0x0d8, 0x28 }, { 0x1054, 0x77 },
	};
	struct device *dev = p->dev;
	unsigned int i;
	u32 val;

	for (i = 0; i < ARRAY_SIZE(reset_values); i++) {
		val = readl(p->phy + reset_values[i].reg);
		if (val != reset_values[i].val)
			dev_warn(dev, "PHY %#x = %#x, reset value %#x\n",
				 reset_values[i].reg, val, reset_values[i].val);
	}

	writel(0x28, p->phy + 0xd8);			/* input clock path */
	rmw(p->pcs + 0x008, 0, BIT(7));
	rmw(p->pcs + 0x808, 0, BIT(7));

	/* PHY CMN_RST, INIT_RST, PORT_RST assert */
	elbi_write(p, ELBI_PHY_CMN_RST, 1);
	elbi_write(p, ELBI_PHY_INIT_RST, 1);
	elbi_write(p, ELBI_PHY_PORT_RST, 1);
	elbi_write(p, ELBI_PHY_CMN_RST, 0);
	elbi_write(p, ELBI_PHY_INIT_RST, 0);
	elbi_write(p, ELBI_PHY_PORT_RST, 0);
	udelay(10);
	elbi_write(p, ELBI_PHY_CMN_RST, 1);
	udelay(10);

	write_table(p->phy, pma_common, ARRAY_SIZE(pma_common));
	write_table(p->phy, pma_lane0, ARRAY_SIZE(pma_lane0));
	write_table(p->phy + 0x800, pma_lane0, ARRAY_SIZE(pma_lane0));
	for (i = 0; i < ARRAY_SIZE(pma_lane1_clk); i++)
		writel(pma_lane1_clk[i].val, p->phy + pma_lane1_clk[i].reg - 0x800);
	write_table(p->phy, pma_lane1_clk, ARRAY_SIZE(pma_lane1_clk));

	/* PCS: aggregation, RC, L2 entry and power-down delay, L1.2 ERIO gating,
	 * PLL and bias off delay
	 */
	writel(0x00, p->pcs + 0x004);
	writel(0x00, p->pcs + 0x804);
	writel(0x700d5, p->pcs + 0x154);
	writel(0x700d5, p->pcs + 0x954);
	writel(0x300ff, p->pcs + 0x150);
	writel(0x300ff, p->pcs + 0x950);
	writel(0x40, p->pcs + 0x170);
	writel(0x40, p->pcs + 0x970);
	rmw(p->pcs + 0x008, BIT(4) | BIT(5), BIT(4));
	rmw(p->pcs + 0x808, BIT(4) | BIT(5), BIT(4));
	writel(0x100b0808, p->pcs + 0x190);

	/* PHY CMN_RST, PORT_RST release */
	elbi_write(p, ELBI_PHY_PORT_RST, 1);
	elbi_write(p, ELBI_PHY_INIT_RST, 1);

	rmw(p->phy + 0x5d0, BIT(3), BIT(4));		/* XO clock */
	writel(0x4, p->phy + 0xbf4);			/* AFC always from its initial value */
}

/* exynos_pcie_rc_use_ia(): a sequencer that re-runs CDR/AFC on L1.2 exit */
static void pixel_ia_config(struct pixel_pcie *p)
{
	static const u32 seq[] = {
		0x50000004, 0x100, 0x20420008, 0x100, 0x50000004, 0x400,
		0x10000008, 0x000, 0x40020008, 0x100, 0x50000004, 0x0f0,
		0x20d10fc0, 0x0f0, 0x40010a48, 0x020, 0x40010a48, 0x030,
		0x40010a48, 0x000, 0x40010bf4, 0x005, 0x40010bf4, 0x004,
		0x30010fc0, 0x0f0, 0x40010bf4, 0x005, 0x40000008, 0x400,
		0x80000000, 0x000,
	};
	unsigned int i;

	elbi_write(p, ELBI_IA_IRQ_SEL, 0x400);
	writel(0x11920000, p->ia + 0x30);
	writel(0x11950000, p->ia + 0x34);
	writel(0x11900000, p->ia + 0x38);
	writel(0x00023000, p->ia + 0x40);		/* loop interval */
	for (i = 0; i < ARRAY_SIZE(seq); i++)
		writel(seq[i], p->ia + 0x100 + 4 * i);
	writel(1, p->ia + 0x000);
}

static bool pixel_phy_locked(struct pixel_pcie *p)
{
	void __iomem *pll = p->phy + 0x3f0, *cdr = p->phy + 0xe0c, *oc = p->phy + 0xde8;
	u32 v1, v2, v3;
	int e1, e2, e3;

	e1 = readl_poll_timeout_atomic(pll, v1, v1 & BIT(3), 1, 10000);
	e2 = readl_poll_timeout_atomic(cdr, v2, v2 & BIT(2), 1, 10000);
	e3 = readl_poll_timeout_atomic(oc, v3, (v3 & 0xf) == 0xf, 1, 10000);
	if (e1 || e2 || e3)
		dev_warn(p->dev, "PHY lock: PLL %#x CDR %#x OC %#x\n", v1, v2, v3);
	return !(e1 || e2 || e3);
}

/* Only the vendor's named DBI registers; no DWC core and no iATU probing. */
static void setup_rc(struct pixel_pcie *p, unsigned int speed)
{
	writel(1, p->dbi + 0x8bc); /* DBI_RO_WR_EN */
	/* dw_pcie_setup(): DLL on, fast-link off, two lanes. */
	rmw(p->dbi + 0x710, BIT(7) | GENMASK(21, 16), BIT(5) | (3 << 16));
	rmw(p->dbi + 0x80c, GENMASK(12, 8), (2 << 8) | BIT(17));
	writel(0, p->dbi2 + PCI_BASE_ADDRESS_0);
	writel(0, p->dbi2 + PCI_ROM_ADDRESS); /* vendor's 0x100030 */
	writel(0, p->dbi + PCI_BASE_ADDRESS_0);
	writel(0, p->dbi + PCI_BASE_ADDRESS_1);
	rmw(p->dbi + PCI_INTERRUPT_LINE, 0xff00, 0x100);
	rmw(p->dbi + PCI_PRIMARY_BUS, 0xffffff, 0x010100);
	writew(PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_SERR,
		   p->dbi + PCI_COMMAND);
	writew(PCI_CLASS_BRIDGE_PCI, p->dbi + PCI_CLASS_DEVICE);
	writel(0xecec144d, p->dbi + PCI_VENDOR_ID);
	rmw(p->dbi + RC_EXP_CAP + PCI_EXP_LNKCAP,
		PCI_EXP_LNKCAP_L1EL | PCI_EXP_LNKCAP_SLS | PCI_EXP_LNKCAP_ASPMS,
		(7 << 15) | 3);
	writel(0x1a, p->dbi + DBI_AUX_CLK_FREQ);
	writel(0xea, p->dbi + DBI_L1_SUBSTATES);
	writew(0, p->dbi + DBI_PM_CTRL);
	/* No ASPM during the probe, Gen1 for CP ROM, Gen3 for BL1. */
	writew(readw(p->dbi + RC_EXP_CAP + PCI_EXP_LNKCTL) & ~PCI_EXP_LNKCTL_ASPMC,
		   p->dbi + RC_EXP_CAP + PCI_EXP_LNKCTL);
	rmw(p->dbi + RC_EXP_CAP + PCI_EXP_LNKCTL2, PCI_EXP_LNKCTL2_TLS, speed);
	rmw(p->dbi + RC_EXP_CAP + PCI_EXP_DEVCTL2, 0xf, 0);
	writel(AXCACHE_ALLOCATE, p->dbi + DBI_COHERENCY_3);
	rmw(p->sysreg + 0x704, 3, 3);
	writel(0, p->dbi + 0x8bc);
}

static int train_link(struct pixel_pcie *p, unsigned int speed)
{
	u32 val;
	u16 status;
	int ret;

	stage(p, "PHY configuration, two lanes (pinned CAL)");
	pixel_phy_config(p);
	stage(p, "IA sequencer");
	pixel_ia_config(p);
	stage(p, "PHY lock polls");
	pixel_phy_locked(p);
	stage(p, "soft resets");
	val = elbi_read(p, ELBI_SOFT_RESET);
	elbi_write(p, ELBI_SOFT_RESET, val & ~SOFT_PWR_RESET);
	mdelay(1);
	elbi_write(p, ELBI_SOFT_RESET, val | SOFT_PWR_RESET);
	elbi_write(p, ELBI_DEVICE_TYPE, DEVICE_TYPE_RC);
	val = elbi_read(p, ELBI_SOFT_RESET) | SOFT_NON_STICKY_RESET;
	elbi_write(p, ELBI_SOFT_RESET, val);
	usleep_range(10, 12);
	elbi_write(p, ELBI_SOFT_RESET, val & ~SOFT_NON_STICKY_RESET);
	mdelay(1);
	elbi_write(p, ELBI_SOFT_RESET, val);
	stage(p, "first DBI write, EQ off");
	writel(GEN3_EQ_OFF, p->dbi + DBI_GEN3_RELATED);
	stage(p, "PERST release");
	pin_set(p, PIN_PERST, true);
	usleep_range(18000, 20000);
	rmw(p->elbi + ELBI_L1_EXIT_MODE, 0, APP_REQ_EXIT_L1 | L1_REQ_NAK_MASTER);
	elbi_write(p, ELBI_LINKDOWN_RST, LINKDOWN_RST_MANUAL);
	rmw(p->elbi + ELBI_QCH_SEL, QCH_GATING, 0);
	elbi_write(p, ELBI_MSTR_PEND_NAK, 1);
	elbi_write(p, ELBI_DBI_L1_EXIT_DIS, 1);
	stage(p, "root complex x2 and IO coherency");
	setup_rc(p, speed);
	stage(p, "LTSSM enable");
	elbi_write(p, ELBI_LTSSM_EN, 1);
	ret = readl_poll_timeout(p->elbi + ELBI_LTSSM_STATE, val,
				(val & LTSSM_STATE) == LTSSM_L0, 10, 1000000);
	if (!ret) {
		/* The first L0 can be Gen1 before the endpoint retrains to Gen3.
		 * The pinned vendor establish_link waits 2.8..3 ms here. Without
		 * that hardware delay a following config access can race Recovery.
		 */
		usleep_range(2800, 3000);
		ret = readw_poll_timeout(p->dbi + RC_EXP_CAP + PCI_EXP_LNKSTA, status,
				(status & PCI_EXP_LNKSTA_CLS) == speed &&
				!(status & PCI_EXP_LNKSTA_LT) &&
				(elbi_read(p, ELBI_LTSSM_STATE) & LTSSM_STATE) == LTSSM_L0,
				100, 100000);
	}
	dev_info(p->dev, "link result=%d LTSSM=%#x link_status=%#x\n", ret,
		 (u32)(val & LTSSM_STATE), readw(p->dbi + RC_EXP_CAP + PCI_EXP_LNKSTA));
	if (p->diagnostic_delay_ms)
		msleep(p->diagnostic_delay_ms);
	return ret;
}

static bool link_up(struct pixel_pcie *p)
{
	return (elbi_read(p, ELBI_LTSSM_STATE) & LTSSM_STATE) == LTSSM_L0;
}

static void turn_off_link(struct pixel_pcie *p)
{
	u32 val;

	if (link_up(p)) {
		stage(p, "PME turn-off");
		elbi_write(p, ELBI_APP_REQ_EXIT_L1, 1);
		rmw(p->elbi + ELBI_L1_EXIT_MODE, APP_REQ_EXIT_L1, L1_REQ_NAK_MASTER);
		elbi_write(p, ELBI_XMIT_PME_TURNOFF, 1);
		if (readl_poll_timeout(p->elbi + ELBI_IRQ0, val,
					  val & IRQ0_RADM_PM_TO_ACK, 10, 20000))
			dev_warn(p->dev, "no PME acknowledgement\n");
		elbi_write(p, ELBI_XMIT_PME_TURNOFF, 0);
		if (readl_poll_timeout(p->elbi + ELBI_LTSSM_STATE, val,
					  (val & LTSSM_STATE) == LTSSM_L2_IDLE, 10, 20000))
			dev_warn(p->dev, "no L2 entry: LTSSM=%#x\n", (u32)(val & LTSSM_STATE));
	}
	pin_set(p, PIN_PERST, false);
	elbi_write(p, ELBI_LTSSM_EN, 0);
	val = elbi_read(p, ELBI_SOFT_RESET);
	elbi_write(p, ELBI_SOFT_RESET, val & ~SOFT_PWR_RESET);
	udelay(20);
	elbi_write(p, ELBI_SOFT_RESET, val | SOFT_PWR_RESET);
	pixel_phy_power_down(p);
}


#endif
