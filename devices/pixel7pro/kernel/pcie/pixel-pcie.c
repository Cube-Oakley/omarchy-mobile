// SPDX-License-Identifier: GPL-2.0-only
/* GS201 PCIe channel 1 (HSI2 GEN4A_1, the BCM4389 Wi-Fi link) as a mainline
 * DesignWare host on the stock DT node pcie@14520000; channel 0 (the modem,
 * pcie@11920000) is refused before any register access.
 *
 * Sequence from Google's GS201 sources (drivers/pci/controller/dwc-whi/
 * pcie-exynos-rc.c and pcie-exynos-gs201-rc-cal.c, the same CAL the stock 5.10
 * kernel ran): PMU isolation bypass, PHY power-down, WL_REG_ON high for
 * 200 ms, PHY configuration, soft resets, PERST# release, controller setup,
 * LTSSM enable. The DWC core does the root-port setup, the iATU and
 * enumeration.
 *
 * The PMU PHY isolation is released before the first ELBI access: with it in
 * place (the bootloader's state) even an ELBI read hangs the SoC, although the
 * vendor probe writes ELBI first.
 *
 * Differences from the vendor driver:
 * - INTx only. msi_init is a stub, so the DWC iMSI-RX never claims the shared
 *   controller interrupt (the DT has use-msi = "false").
 * - The root port advertises no ASPM. The PCI core enables L0s/L1 by default
 *   on DT platforms, and the vendor gates L1 substates on its own sequence.
 * - No SysMMU: it stays in bypass. Endpoint DMA addresses are physical, and
 *   the DT dma-coherent flag matches the IOCC setup here.
 * - The link-history and UDBG trace are left out.
 * - L1 substates are the vendor's, but they follow the Wi-Fi network device
 *   instead of calls from the Wi-Fi driver. brcmfmac registers it once the
 *   firmware runs, and unregisters it before any chip reset (reload or crash
 *   recovery), which is when the vendor driver turns them on and off.
 */
#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/string_choices.h>
#include <linux/suspend.h>

#include "pcie-designware.h"

static bool ia = true;
module_param(ia, bool, 0400);
MODULE_PARM_DESC(ia, "Program the IA sequencer as the vendor does (CDR/AFC after L1.2 exit)");
static uint attempts = 3;
module_param(attempts, uint, 0400);
MODULE_PARM_DESC(attempts, "Link training attempts, each with a full PHY configuration");
static bool system_link_off;
module_param(system_link_off, bool, 0644);
MODULE_PARM_DESC(system_link_off, "Experimental: power down Wi-Fi PCIe PHY during s2idle; no Wi-Fi wake");
static unsigned int sleep_entries, sleep_resumes, sleep_errors;
module_param(sleep_entries, uint, 0444);
module_param(sleep_resumes, uint, 0444);
module_param(sleep_errors, uint, 0444);
static bool trace;
module_param(trace, bool, 0400);
MODULE_PARM_DESC(trace, "Log each bring-up stage and pause so the log leaves the phone");

#define PIXEL_PCIE_ELBI		0x14520000
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

/* L1 substates, exynos_pcie_rc_set_l1ss() for EP_BCM_WIFI. The vendor driver
 * hard-codes where both ends keep these capabilities; they are checked here.
 */
#define RC_EXP_CAP		0x70
#define RC_L1SS_CAP		0x194
#define EP_EXP_CAP		0xac
#define EP_LTR_CAP		0x1b0
#define EP_L1SS_CAP		0x240
#define L1SS_T_POWER_ON_130US	0x69
#define L1SS_LTR_L12_TH_160US	(0x40a0 << 16)
#define RC_T_COMMON_32US	(0x20 << 8)
#define EP_T_COMMON_10US	(0xa << 8)
#define EP_LTR_MAX_LAT_3MS	0x10031003	/* snoop and no-snoop */
/* PHY PCS power state (link_state_show()): 0 L0, 2 L1, 5 L1.1, 6 L1.2 */
#define PCS_PM_POWER_STATE	0x188
#define PM_STATE		GENMASK(2, 0)

#define SYSREG_HSI2_SHARE	0x730
#define SHARE_INNER_OUTER	0x3

/* gph2, the first bank of the HSI2 pinctrl (pinctrl-gs201.c) */
#define PIXEL_GPH2		0x14440000
#define GPIO_CON		0x00
#define GPIO_DAT		0x04
#define GPIO_PUD		0x08
#define GPIO_CONPDN		0x10
#define GPIO_PUDPDN		0x14
#define PIN_PERST		0
#define PIN_CLKREQ		1
#define PIN_WLAN_EN		4
#define PIN_DEV_WAKE		5
#define CON_INPUT		0
#define CON_OUTPUT		1
#define CON_CLKREQ		2
#define PUD_NONE		0
#define PUD_DOWN		1
#define PUD_UP			3
#define CONPDN_PREV		3

struct pixel_pcie {
	struct dw_pcie pci;
	void __iomem *phy;
	void __iomem *pcs;
	void __iomem *ia;
	void __iomem *sysreg;
	void __iomem *gpio;
	phys_addr_t isolation;
	int irq;
	bool sleep_link_off, restore_l1ss, sleep_irq_disabled;
	unsigned long storm_start;
	unsigned int storm_count;
};

static DEFINE_MUTEX(pixel_host_lock);
static struct pixel_pcie *pixel_host;
static bool l1ss;		/* wanted */
static bool l1ss_on;
static unsigned int ep_netdevs;	/* network devices of the Wi-Fi chip */

/* Bring-up tracing: a hard bus hang leaves no log, so give each stage's
 * message time to reach the host before the next register access.
 */
static void stage(struct pixel_pcie *p, const char *what)
{
	if (!trace)
		return;
	dev_info(p->pci.dev, "stage: %s\n", what);
	msleep(150);
}

static struct pixel_pcie *to_pixel(struct dw_pcie *pci)
{
	return container_of(pci, struct pixel_pcie, pci);
}

static u32 elbi_read(struct pixel_pcie *p, u32 reg)
{
	return readl(p->pci.elbi_base + reg);
}

static void elbi_write(struct pixel_pcie *p, u32 reg, u32 val)
{
	writel(val, p->pci.elbi_base + reg);
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

/* Same secure-register path as pixel-reboot.c; channel 1's PMU PHY control
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
		dev_err(p->pci.dev, "PHY isolation write failed: SMC %ld, PMU %#x\n",
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
	rmw(p->pcs + 0x184, 0xb0, 0);		/* no rate switching */
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
	writel(0, p->pcs + 0x184);
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

/* exynos_pcie_rc_pcie_phy_config(), CAL ver 210802, one lane */
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
	/* 38.4 MHz input clock, both lanes as the vendor loop does */
	{ 0xbcc, 0x41 }, { 0xbd4, 0x41 }, { 0xbdc, 0x68 }, { 0xbe0, 0x00 },
	{ 0xbe4, 0xd0 },
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
	struct device *dev = p->pci.dev;
	unsigned int i;
	u32 val;

	for (i = 0; i < ARRAY_SIZE(reset_values); i++) {
		val = readl(p->phy + reset_values[i].reg);
		if (val != reset_values[i].val)
			dev_warn(dev, "PHY %#x = %#x, reset value %#x\n",
				 reset_values[i].reg, val, reset_values[i].val);
	}

	writel(0x28, p->phy + 0xd8);			/* input clock path */
	rmw(p->pcs + 0x008, 0, BIT(7));			/* PCS mux glitch W/A */

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
	write_table(p->phy, pma_lane1_clk, ARRAY_SIZE(pma_lane1_clk));

	/* PCS: aggregation, RC, L2 entry and power-down delay, L1.2 ERIO gating,
	 * PLL and bias off delay
	 */
	writel(0x00, p->pcs + 0x004);
	writel(0x700d5, p->pcs + 0x154);
	writel(0x300ff, p->pcs + 0x150);
	writel(0x40, p->pcs + 0x170);
	rmw(p->pcs + 0x008, 0, BIT(4) | BIT(5));
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
	struct platform_device *pdev = to_platform_device(p->pci.dev);
	struct resource *elbi, *phy, *iares;
	unsigned int i;

	elbi = platform_get_resource_byname(pdev, IORESOURCE_MEM, "elbi");
	phy = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy");
	iares = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ia");
	elbi_write(p, ELBI_IA_IRQ_SEL, 0x400);
	writel(elbi->start, p->ia + 0x30);
	writel(phy->start, p->ia + 0x34);
	writel(iares->start, p->ia + 0x38);
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
		dev_warn(p->pci.dev, "PHY lock: PLL %#x CDR %#x OC %#x\n", v1, v2, v3);
	return !(e1 || e2 || e3);
}

/* The first half of exynos_pcie_rc_establish_link(): PHY, resets, PERST#. */
static void pixel_pcie_bring_up(struct pixel_pcie *p)
{
	struct dw_pcie *pci = &p->pci;
	u32 val;

	stage(p, "PHY configuration");
	pixel_phy_config(p);
	if (ia) {
		stage(p, "IA sequencer");
		pixel_ia_config(p);
	}
	stage(p, "PHY lock polls");
	pixel_phy_locked(p);
	stage(p, "soft resets");

	val = elbi_read(p, ELBI_SOFT_RESET);
	elbi_write(p, ELBI_SOFT_RESET, val & ~SOFT_PWR_RESET);
	mdelay(1);
	elbi_write(p, ELBI_SOFT_RESET, val | SOFT_PWR_RESET);
	if (!(readl(p->phy + 0xe18) & BIT(7)))
		dev_warn(pci->dev, "offset calibration lost after the power reset\n");

	elbi_write(p, ELBI_DEVICE_TYPE, DEVICE_TYPE_RC);

	val = elbi_read(p, ELBI_SOFT_RESET) | SOFT_NON_STICKY_RESET;
	elbi_write(p, ELBI_SOFT_RESET, val);
	usleep_range(10, 12);
	elbi_write(p, ELBI_SOFT_RESET, val & ~SOFT_NON_STICKY_RESET);
	mdelay(1);
	elbi_write(p, ELBI_SOFT_RESET, val);

	stage(p, "first DBI write");
	dw_pcie_writel_dbi(pci, DBI_GEN3_RELATED, GEN3_EQ_OFF);

	stage(p, "PERST# release");
	pin_set(p, PIN_PERST, true);
	usleep_range(20000, 22000);

	rmw(pci->elbi_base + ELBI_L1_EXIT_MODE, 0, APP_REQ_EXIT_L1 | L1_REQ_NAK_MASTER);
	elbi_write(p, ELBI_LINKDOWN_RST, LINKDOWN_RST_MANUAL);
	rmw(pci->elbi_base + ELBI_QCH_SEL, QCH_GATING, 0);
	elbi_write(p, ELBI_MSTR_PEND_NAK, 1);
	elbi_write(p, ELBI_DBI_L1_EXIT_DIS, 1);
	stage(p, "controller ready for the DWC core");
}

/* exynos_pcie_rc_send_pme_turn_off(): leave L1 under software control, then
 * PME_Turn_Off, so that the link is in L2 before PERST#. A link torn down in
 * L1.2 left the next bring-up hanging in link training.
 */
static void pixel_pcie_turn_off(struct pixel_pcie *p)
{
	struct device *dev = p->pci.dev;
	u32 val;

	val = FIELD_GET(LTSSM_STATE, elbi_read(p, ELBI_LTSSM_STATE));
	if (val < LTSSM_RCVRY_LOCK || val > LTSSM_L1_IDLE)
		return;
	elbi_write(p, ELBI_APP_REQ_EXIT_L1, 1);
	rmw(p->pci.elbi_base + ELBI_L1_EXIT_MODE, APP_REQ_EXIT_L1, L1_REQ_NAK_MASTER);
	elbi_write(p, ELBI_XMIT_PME_TURNOFF, 1);
	if (readl_poll_timeout(p->pci.elbi_base + ELBI_IRQ0, val, val & IRQ0_RADM_PM_TO_ACK,
			       10, 20000))
		dev_warn(dev, "no PME_TO_Ack from the endpoint\n");
	elbi_write(p, ELBI_XMIT_PME_TURNOFF, 0);
	if (readl_poll_timeout(p->pci.elbi_base + ELBI_LTSSM_STATE, val,
			       FIELD_GET(LTSSM_STATE, val) == LTSSM_L2_IDLE, 10, 20000))
		dev_warn(dev, "link did not reach L2 (LTSSM %#lx)\n", FIELD_GET(LTSSM_STATE, val));
}

static void pixel_pcie_reset_link(struct pixel_pcie *p)
{
	pin_set(p, PIN_PERST, false);
	elbi_write(p, ELBI_LTSSM_EN, 0);
}

static int pixel_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct pixel_pcie *p = to_pixel(pci);

	/* exynos_pcie_rc_resumed_phydown() after the isolation bypass (probe):
	 * interrupts off, PHY powered down, then poweron's power-down clear.
	 */
	elbi_write(p, ELBI_IRQ0_EN, 0);
	elbi_write(p, ELBI_IRQ1_EN, 0);
	elbi_write(p, ELBI_IRQ2_EN, 0);

	pin_set(p, PIN_PERST, false);
	pin_config(p, PIN_PERST, CON_OUTPUT, PUD_NONE, PUD_NONE);
	pin_set(p, PIN_WLAN_EN, false);
	pin_config(p, PIN_WLAN_EN, CON_OUTPUT, PUD_NONE, PUD_NONE);
	pin_config(p, PIN_CLKREQ, CON_CLKREQ, PUD_UP, PUD_UP);
	pin_config(p, PIN_DEV_WAKE, CON_INPUT, PUD_DOWN, PUD_DOWN);

	stage(p, "PHY power-down");
	pixel_phy_power_down(p);

	/* dhd_wlan_power(): WL_REG_ON, then WIFI_TURNON_DELAY */
	stage(p, "WL_REG_ON high");
	pin_set(p, PIN_WLAN_EN, true);
	msleep(200);

	stage(p, "PHY power-down clear");
	pixel_phy_power_up(p);
	pixel_pcie_bring_up(p);
	return 0;
}

static void pixel_pcie_host_deinit(struct dw_pcie_rp *pp)
{
	struct pixel_pcie *p = to_pixel(to_dw_pcie_from_pp(pp));
	u32 val;

	elbi_write(p, ELBI_IRQ0_EN, 0);
	elbi_write(p, ELBI_IRQ1_EN, 0);
	elbi_write(p, ELBI_IRQ2_EN, 0);
	pixel_pcie_reset_link(p);
	/* exynos_pcie_rc_poweroff(): power reset after LTSSM disable */
	val = elbi_read(p, ELBI_SOFT_RESET);
	elbi_write(p, ELBI_SOFT_RESET, val & ~SOFT_PWR_RESET);
	udelay(20);
	elbi_write(p, ELBI_SOFT_RESET, val | SOFT_PWR_RESET);
	pin_set(p, PIN_WLAN_EN, false);
	/* Isolation stays released: the shared interrupt handler reads ELBI
	 * until the IRQ is freed, and an isolated ELBI hangs the SoC.
	 */
	pixel_phy_power_down(p);
}

/* No MSI controller: endpoints fall back to INTx on the shared line. */
static int pixel_pcie_msi_init(struct dw_pcie_rp *pp)
{
	return 0;
}

static const struct dw_pcie_host_ops pixel_pcie_host_ops = {
	.init = pixel_pcie_host_init,
	.deinit = pixel_pcie_host_deinit,
	.msi_init = pixel_pcie_msi_init,
};

static u32 pixel_pcie_ltssm(struct pixel_pcie *p)
{
	return FIELD_GET(LTSSM_STATE, elbi_read(p, ELBI_LTSSM_STATE));
}

/* exynos_pcie_rc_link_up(): recovery through L1 idle counts as up. The DWC
 * core refuses endpoint config accesses otherwise, which would fault.
 */
static bool pixel_pcie_link_up(struct dw_pcie *pci)
{
	u32 state = pixel_pcie_ltssm(to_pixel(pci));

	return state >= LTSSM_RCVRY_LOCK && state <= LTSSM_L1_IDLE;
}

static enum dw_pcie_ltssm pixel_pcie_get_ltssm(struct dw_pcie *pci)
{
	return pixel_pcie_ltssm(to_pixel(pci));
}

/* exynos_pcie_setup_rc() and exynos_pcie_rc_set_iocc(), after the core's
 * dw_pcie_setup_rc(). The core already set the link speed from the DT.
 */
static void pixel_pcie_setup_rc(struct pixel_pcie *p)
{
	struct dw_pcie *pci = &p->pci;
	u8 cap = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	u32 val;

	dw_pcie_dbi_ro_wr_en(pci);
	dw_pcie_writew_dbi(pci, PCI_VENDOR_ID, PCI_VENDOR_ID_SAMSUNG);
	dw_pcie_writew_dbi(pci, PCI_DEVICE_ID, 0xeced);
	val = dw_pcie_readl_dbi(pci, cap + PCI_EXP_LNKCAP);
	val &= ~(PCI_EXP_LNKCAP_L1EL | PCI_EXP_LNKCAP_ASPMS);
	val |= FIELD_PREP(PCI_EXP_LNKCAP_L1EL, 7);
	dw_pcie_writel_dbi(pci, cap + PCI_EXP_LNKCAP, val);
	dw_pcie_writel_dbi(pci, DBI_AUX_CLK_FREQ, AUX_CLK_26MHZ);
	dw_pcie_writel_dbi(pci, DBI_L1_SUBSTATES, L1_SUB_VAL);
	dw_pcie_writel_dbi(pci, DBI_PM_CTRL, 0);
	/* L1 substates and ASPM start off, whatever an earlier run left */
	val = dw_pcie_readl_dbi(pci, RC_L1SS_CAP + PCI_L1SS_CTL1);
	dw_pcie_writel_dbi(pci, RC_L1SS_CAP + PCI_L1SS_CTL1, val & ~PCI_L1SS_CTL1_L1SS_MASK);
	val = dw_pcie_readl_dbi(pci, cap + PCI_EXP_LNKCTL);
	dw_pcie_writel_dbi(pci, cap + PCI_EXP_LNKCTL, val & ~PCI_EXP_LNKCTL_ASPMC);
	/* completion timeout: default range, 28-44 ms */
	val = dw_pcie_readl_dbi(pci, cap + PCI_EXP_DEVCTL2);
	dw_pcie_writel_dbi(pci, cap + PCI_EXP_DEVCTL2, val & ~PCI_EXP_DEVCTL2_COMP_TIMEOUT);
	dw_pcie_dbi_ro_wr_dis(pci);

	dw_pcie_writel_dbi(pci, DBI_COHERENCY_3, AXCACHE_ALLOCATE);
	rmw(p->sysreg + SYSREG_HSI2_SHARE, SHARE_INNER_OUTER, SHARE_INNER_OUTER);
}

/* The second half of exynos_pcie_rc_establish_link(), with its retries. */
static int pixel_pcie_start_link(struct dw_pcie *pci)
{
	struct pixel_pcie *p = to_pixel(pci);
	unsigned int attempt, reg;
	u32 state;
	u16 status;
	int ret;

	for (attempt = 1; ; attempt++) {
		stage(p, "vendor RC setup and IOCC");
		pixel_pcie_setup_rc(p);
		stage(p, "LTSSM enable");
		elbi_write(p, ELBI_LTSSM_EN, 1);
		ret = readl_poll_timeout(pci->elbi_base + ELBI_LTSSM_STATE, state,
					 (state & LTSSM_STATE) == LTSSM_L0, 20, 100000);
		if (!ret)
			break;
		dev_warn(pci->dev, "link attempt %u failed, LTSSM %#x\n", attempt,
			 (u32)(state & LTSSM_STATE));
		pixel_pcie_reset_link(p);
		if (attempt >= attempts)
			return -ETIMEDOUT;
		pixel_pcie_bring_up(p);
		ret = dw_pcie_setup_rc(&pci->pp);
		if (ret)
			return ret;
	}

	/* the vendor's wait for the Gen1 -> Gen2 change */
	usleep_range(2800, 3000);
	status = dw_pcie_readw_dbi(pci, dw_pcie_find_capability(pci, PCI_CAP_ID_EXP) +
				   PCI_EXP_LNKSTA);
	dev_info(pci->dev, "link up on attempt %u: Gen%u x%u, LTSSM %#x\n", attempt,
		 (u32)FIELD_GET(PCI_EXP_LNKSTA_CLS, status),
		 (u32)FIELD_GET(PCI_EXP_LNKSTA_NLW, status),
		 pixel_pcie_ltssm(p));

	stage(p, "link up");
	for (reg = ELBI_IRQ0; reg <= ELBI_IRQ2; reg += 4)
		elbi_write(p, reg, elbi_read(p, reg));
	elbi_write(p, ELBI_IRQ0_EN, IRQ0_INTX);
	elbi_write(p, ELBI_IRQ1_EN, IRQ1_LINK_DOWN);
	elbi_write(p, ELBI_IRQ2_EN, IRQ2_CPL_TIMEOUT);
	stage(p, "interrupts enabled; enumeration next");
	return 0;
}

static void pixel_pcie_stop_link(struct dw_pcie *pci)
{
	struct pixel_pcie *p = to_pixel(pci);

	pixel_pcie_turn_off(p);
	pixel_pcie_reset_link(p);
}

static const struct dw_pcie_ops pixel_pcie_ops = {
	.link_up = pixel_pcie_link_up,
	.get_ltssm = pixel_pcie_get_ltssm,
	.start_link = pixel_pcie_start_link,
	.stop_link = pixel_pcie_stop_link,
};

/* The controller line also carries the endpoint's INTx (the DT maps every
 * INTx pin to it), so it is shared; the endpoint driver's handler does the
 * work and this one clears the sub-controller's latched status.
 */
static irqreturn_t pixel_pcie_irq(int irq, void *data)
{
	struct pixel_pcie *p = data;
	u32 irq0, irq1, irq2;

	irq0 = elbi_read(p, ELBI_IRQ0);
	irq1 = elbi_read(p, ELBI_IRQ1);
	irq2 = elbi_read(p, ELBI_IRQ2);
	elbi_write(p, ELBI_IRQ0, irq0);
	elbi_write(p, ELBI_IRQ1, irq1);
	elbi_write(p, ELBI_IRQ2, irq2);
	if (irq1 & IRQ1_LINK_DOWN)
		dev_err_ratelimited(p->pci.dev, "link down, LTSSM %#x\n", pixel_pcie_ltssm(p));
	if (irq2 & IRQ2_CPL_TIMEOUT)
		dev_err_ratelimited(p->pci.dev, "completion timeout\n");
	if (!((irq0 & elbi_read(p, ELBI_IRQ0_EN)) || (irq1 & elbi_read(p, ELBI_IRQ1_EN)) ||
	      (irq2 & elbi_read(p, ELBI_IRQ2_EN))))
		return IRQ_NONE;
	/* A status that re-asserts as fast as it is cleared would hold this CPU
	 * in the handler (and USB with it): mask the sources instead.
	 */
	if (time_after(jiffies, p->storm_start + HZ)) {
		p->storm_start = jiffies;
		p->storm_count = 0;
	}
	if (++p->storm_count == 20000) {
		elbi_write(p, ELBI_IRQ0_EN, 0);
		elbi_write(p, ELBI_IRQ1_EN, 0);
		elbi_write(p, ELBI_IRQ2_EN, 0);
		dev_err(p->pci.dev, "interrupt storm (%#x %#x %#x): sources masked\n",
			irq0, irq1, irq2);
	}
	return IRQ_HANDLED;
}

static int pixel_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node, *pmu;
	struct resource *res, pmu_res;
	struct pixel_pcie *p;
	u32 offset;
	int ret;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "elbi");
	if (!res || res->start != PIXEL_PCIE_ELBI)
		return -ENODEV;
	if (of_property_read_u32(np, "pmu-offset", &offset) || offset != 0x3ec4)
		return -ENODEV;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->pci.dev = dev;
	p->pci.ops = &pixel_pcie_ops;
	p->pci.pp.ops = &pixel_pcie_host_ops;

	p->pci.elbi_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(p->pci.elbi_base))
		return PTR_ERR(p->pci.elbi_base);
	p->phy = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(p->phy))
		return PTR_ERR(p->phy);
	p->pcs = devm_platform_ioremap_resource_byname(pdev, "pcs");
	if (IS_ERR(p->pcs))
		return PTR_ERR(p->pcs);
	p->ia = devm_platform_ioremap_resource_byname(pdev, "ia");
	if (IS_ERR(p->ia))
		return PTR_ERR(p->ia);
	/* SYSREG_HSI2 and the pin bank are shared with other blocks: map only. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sysreg");
	if (!res)
		return -EINVAL;
	p->sysreg = devm_ioremap(dev, res->start, resource_size(res));
	p->gpio = devm_ioremap(dev, PIXEL_GPH2, 0x20);
	if (!p->sysreg || !p->gpio)
		return -ENOMEM;

	pmu = of_parse_phandle(np, "samsung,syscon-phandle", 0);
	ret = pmu ? of_address_to_resource(pmu, 0, &pmu_res) : -ENODEV;
	of_node_put(pmu);
	if (ret)
		return ret;
	p->isolation = pmu_res.start + offset;

	stage(p, "PMU isolation bypass");
	ret = pixel_pcie_isolation(p, true);
	if (ret)
		return ret;
	stage(p, "first ELBI access");
	elbi_write(p, ELBI_IRQ0_EN, 0);
	elbi_write(p, ELBI_IRQ1_EN, 0);
	elbi_write(p, ELBI_IRQ2_EN, 0);
	p->irq = platform_get_irq(pdev, 0);
	if (p->irq < 0)
		return p->irq;
	ret = devm_request_irq(dev, p->irq, pixel_pcie_irq, IRQF_SHARED, "pixel-pcie", p);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, p);
	ret = dw_pcie_host_init(&p->pci.pp);
	if (ret)
		return ret;
	mutex_lock(&pixel_host_lock);
	pixel_host = p;
	mutex_unlock(&pixel_host_lock);
	return 0;
}

static struct pci_dev *pixel_pcie_ep(struct pixel_pcie *p)
{
	return pci_get_domain_bus_and_slot(pci_domain_nr(p->pci.pp.bridge->bus), 1,
					   PCI_DEVFN(0, 0));
}

/* The vendor's order: PCI-PM substates on the root port, then the chip, then
 * ASPM L1 on the root port, then the chip with its clock request; disabling
 * reverses it. Called with pixel_host_lock held.
 */
static int pixel_pcie_set_l1ss(struct pixel_pcie *p, bool enable)
{
	struct dw_pcie *pci = &p->pci;
	struct device *dev = pci->dev;
	struct pci_dev *ep = pixel_pcie_ep(p);
	u16 rc_exp, rc_l1ss, ep_ltr, ep_l1ss;
	u32 val;

	if (!ep)
		return -ENODEV;
	rc_exp = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	rc_l1ss = dw_pcie_find_ext_capability(pci, PCI_EXT_CAP_ID_L1SS);
	ep_ltr = pci_find_ext_capability(ep, PCI_EXT_CAP_ID_LTR);
	ep_l1ss = pci_find_ext_capability(ep, PCI_EXT_CAP_ID_L1SS);
	if (rc_exp != RC_EXP_CAP || rc_l1ss != RC_L1SS_CAP || ep->pcie_cap != EP_EXP_CAP ||
	    ep_ltr != EP_LTR_CAP || ep_l1ss != EP_L1SS_CAP) {
		dev_err(dev, "L1SS: capabilities at %#x/%#x and %#x/%#x/%#x, not the vendor's\n",
			rc_exp, rc_l1ss, ep->pcie_cap, ep_ltr, ep_l1ss);
		pci_dev_put(ep);
		return -ENODEV;
	}

	if (enable) {
		dw_pcie_writel_dbi(pci, rc_l1ss + PCI_L1SS_CTL2, L1SS_T_POWER_ON_130US);
		dw_pcie_writel_dbi(pci, DBI_L1_SUBSTATES, L1_SUB_VAL);
		val = dw_pcie_readl_dbi(pci, rc_exp + PCI_EXP_DEVCTL2);
		dw_pcie_writel_dbi(pci, rc_exp + PCI_EXP_DEVCTL2, val | PCI_EXP_DEVCTL2_LTR_EN);
		val = dw_pcie_readl_dbi(pci, rc_l1ss + PCI_L1SS_CTL1);
		dw_pcie_writel_dbi(pci, rc_l1ss + PCI_L1SS_CTL1, val | L1SS_LTR_L12_TH_160US |
				   RC_T_COMMON_32US | PCI_L1SS_CTL1_L1SS_MASK);

		pci_write_config_dword(ep, ep_l1ss + PCI_L1SS_CTL2, L1SS_T_POWER_ON_130US);
		pci_write_config_dword(ep, ep_ltr + PCI_LTR_MAX_SNOOP_LAT, EP_LTR_MAX_LAT_3MS);
		pcie_capability_set_word(ep, PCI_EXP_DEVCTL2, PCI_EXP_DEVCTL2_LTR_EN);
		pci_read_config_dword(ep, ep_l1ss + PCI_L1SS_CTL1, &val);
		pci_write_config_dword(ep, ep_l1ss + PCI_L1SS_CTL1, val | L1SS_LTR_L12_TH_160US |
				       EP_T_COMMON_10US | PCI_L1SS_CTL1_L1SS_MASK);

		val = dw_pcie_readl_dbi(pci, rc_exp + PCI_EXP_LNKCTL) & ~PCI_EXP_LNKCTL_ASPMC;
		dw_pcie_writel_dbi(pci, rc_exp + PCI_EXP_LNKCTL,
				   val | PCI_EXP_LNKCTL_CCC | PCI_EXP_LNKCTL_ASPM_L1);
		pcie_capability_clear_and_set_word(ep, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_ASPMC,
						   PCI_EXP_LNKCTL_CLKREQ_EN | PCI_EXP_LNKCTL_CCC |
						   PCI_EXP_LNKCTL_ASPM_L1);
	} else {
		pcie_capability_clear_word(ep, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_ASPMC);
		val = dw_pcie_readl_dbi(pci, rc_exp + PCI_EXP_LNKCTL);
		dw_pcie_writel_dbi(pci, rc_exp + PCI_EXP_LNKCTL, val & ~PCI_EXP_LNKCTL_ASPMC);
		pci_read_config_dword(ep, ep_l1ss + PCI_L1SS_CTL1, &val);
		pci_write_config_dword(ep, ep_l1ss + PCI_L1SS_CTL1, val & ~PCI_L1SS_CTL1_L1SS_MASK);
		val = dw_pcie_readl_dbi(pci, rc_l1ss + PCI_L1SS_CTL1);
		dw_pcie_writel_dbi(pci, rc_l1ss + PCI_L1SS_CTL1, val & ~PCI_L1SS_CTL1_L1SS_MASK);
	}
	pci_dev_put(ep);
	dev_info(dev, "L1 substates %s\n", enable ? "on" : "off");
	return 0;
}

/* L1 substates while wanted and the chip's firmware runs; pixel_host_lock
 * held.
 */
static int pixel_pcie_update_l1ss(void)
{
	bool want = l1ss && pixel_host && ep_netdevs;
	int ret;

	if (pixel_host && pixel_host->sleep_link_off)
		return -EHOSTDOWN;
	if (want == l1ss_on)
		return 0;
	ret = pixel_pcie_set_l1ss(pixel_host, want);
	if (!ret)
		l1ss_on = want;
	return ret;
}

static int l1ss_set(const char *val, const struct kernel_param *kp)
{
	bool enable;
	int ret;

	ret = kstrtobool(val, &enable);
	if (ret)
		return ret;
	mutex_lock(&pixel_host_lock);
	l1ss = enable;
	ret = pixel_pcie_update_l1ss();
	mutex_unlock(&pixel_host_lock);
	return ret;
}

static const struct kernel_param_ops l1ss_ops = {
	.set = l1ss_set,
	.get = param_get_bool,
};
module_param_cb(l1ss, &l1ss_ops, &l1ss, 0644);
MODULE_PARM_DESC(l1ss, "L1 substates on the Wi-Fi link while its network device exists");

static int pixel_pcie_netdev_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct net_device *ndev = netdev_notifier_info_to_dev(ptr);
	struct device *parent = ndev->dev.parent;
	struct pci_dev *pdev;

	if (event != NETDEV_REGISTER && event != NETDEV_UNREGISTER)
		return NOTIFY_DONE;
	if (!parent || !dev_is_pci(parent))
		return NOTIFY_DONE;
	pdev = to_pci_dev(parent);
	mutex_lock(&pixel_host_lock);
	if (pixel_host && pdev->bus->parent == pixel_host->pci.pp.bridge->bus) {
		if (event == NETDEV_REGISTER)
			ep_netdevs++;
		else if (ep_netdevs)
			ep_netdevs--;
		pixel_pcie_update_l1ss();
	}
	mutex_unlock(&pixel_host_lock);
	return NOTIFY_DONE;
}

static struct notifier_block pixel_pcie_netdev_nb = {
	.notifier_call = pixel_pcie_netdev_event,
};

static int link_state_get(char *buf, const struct kernel_param *kp)
{
	int len;

	mutex_lock(&pixel_host_lock);
	if (pixel_host && pixel_host->sleep_link_off)
		len = sysfs_emit(buf, "suspended\n");
	else if (pixel_host)
		len = sysfs_emit(buf, "ltssm %#x pm %u l1ss %s\n",
				 (u32)FIELD_GET(LTSSM_STATE,
						elbi_read(pixel_host, ELBI_LTSSM_STATE)),
				 (u32)FIELD_GET(PM_STATE,
						readl(pixel_host->pcs + PCS_PM_POWER_STATE)),
				 str_on_off(l1ss_on));
	else
		len = sysfs_emit(buf, "down\n");
	mutex_unlock(&pixel_host_lock);
	return len;
}

static const struct kernel_param_ops link_state_ops = {
	.get = link_state_get,
};
module_param_cb(link_state, &link_state_ops, NULL, 0444);
MODULE_PARM_DESC(link_state, "LTSSM state, PHY power state (0 L0, 2 L1, 5 L1.1, 6 L1.2) and L1 substates");

static void pixel_pcie_remove(struct platform_device *pdev)
{
	struct pixel_pcie *p = platform_get_drvdata(pdev);

	mutex_lock(&pixel_host_lock);
	if (l1ss_on && !pixel_pcie_set_l1ss(p, false))
		l1ss_on = false;
	pixel_host = NULL;
	ep_netdevs = 0;
	mutex_unlock(&pixel_host_lock);
	dw_pcie_host_deinit(&p->pci.pp);
	/* No endpoint or host IRQ may touch ELBI once isolation is asserted. */
	devm_free_irq(&pdev->dev, p->irq, p);
	if (pixel_pcie_isolation(p, false))
		dev_warn(&pdev->dev, "PHY isolation failed after host removal\n");
}

/* PCI children have completed suspend_noirq before their host. Keep the
 * endpoint's WL_REG_ON high so its firmware can survive PERST/link loss;
 * only the host PHY and controller are reset. Resume retrains the host and
 * restores its windows before the PCI core restores downstream config.
 * Mask and synchronize the shared IRQ before isolation; release isolation
 * before any resume ELBI access. This deliberately does not advertise wake-on-Wi-Fi support.
 */
static int pixel_pcie_resume_noirq(struct device *dev);

static int pixel_pcie_suspend_noirq(struct device *dev)
{
	struct pixel_pcie *p = dev_get_drvdata(dev);
	struct pci_dev *ep;
	u32 val, old_exit, old_request;
	int ret;

	if (!READ_ONCE(system_link_off))
		return 0;
	if (pm_suspend_target_state != PM_SUSPEND_TO_IDLE || trace)
		return -EBUSY;
	/* Restrict the experiment to the known endpoint with no wake request. */
	ep = pci_get_domain_bus_and_slot(pci_domain_nr(p->pci.pp.bridge->bus),
					p->pci.pp.bridge->bus->number + 1, PCI_DEVFN(0, 0));
	if (!ep)
		return -ENODEV;
	if (ep->vendor != 0x14e4 || ep->device != 0x4441 ||
	    device_may_wakeup(&ep->dev)) {
		pci_dev_put(ep);
		return -EBUSY;
	}
	pci_dev_put(ep);
	old_exit = elbi_read(p, ELBI_L1_EXIT_MODE);
	old_request = elbi_read(p, ELBI_APP_REQ_EXIT_L1);
	pixel_pcie_turn_off(p);
	if (pixel_pcie_ltssm(p) != LTSSM_L2_IDLE) {
		elbi_write(p, ELBI_L1_EXIT_MODE, old_exit);
		elbi_write(p, ELBI_APP_REQ_EXIT_L1, old_request);
		sleep_errors++;
		return -EBUSY;
	}
	elbi_write(p, ELBI_IRQ0_EN, 0);
	elbi_write(p, ELBI_IRQ1_EN, 0);
	elbi_write(p, ELBI_IRQ2_EN, 0);
	pixel_pcie_reset_link(p);
	val = elbi_read(p, ELBI_SOFT_RESET);
	elbi_write(p, ELBI_SOFT_RESET, val & ~SOFT_PWR_RESET);
	udelay(20);
	elbi_write(p, ELBI_SOFT_RESET, val | SOFT_PWR_RESET);
	pixel_phy_power_down(p);
	p->restore_l1ss = l1ss_on;
	l1ss_on = false;
	p->sleep_link_off = true;
	disable_irq(p->irq);
	p->sleep_irq_disabled = true;
	ret = pixel_pcie_isolation(p, false);
	if (ret) {
		sleep_errors++;
		pixel_pcie_resume_noirq(dev);
		return ret;
	}
	sleep_entries++;
	return 0;
}

static int pixel_pcie_resume_noirq(struct device *dev)
{
	struct pixel_pcie *p = dev_get_drvdata(dev);
	int ret;

	if (!p->sleep_link_off)
		return 0;
	ret = pixel_pcie_isolation(p, true);
	if (ret) {
		sleep_errors++;
		return ret;
	}
	pixel_phy_power_up(p);
	pixel_pcie_bring_up(p);
	ret = dw_pcie_setup_rc(&p->pci.pp);
	if (!ret)
		ret = pixel_pcie_start_link(&p->pci);
	if (ret) {
		sleep_errors++;
		dev_err(dev, "suspend link restore failed: %d\n", ret);
		return ret;
	}
	p->sleep_link_off = false;
	if (p->sleep_irq_disabled) {
		enable_irq(p->irq);
		p->sleep_irq_disabled = false;
	}
	sleep_resumes++;
	return 0;
}

static int pixel_pcie_resume_early(struct device *dev)
{
	struct pixel_pcie *p = dev_get_drvdata(dev);
	int ret = 0;

	/* All PCI resume_noirq callbacks have restored config by this phase. */
	if (p->restore_l1ss && !p->sleep_link_off) {
		mutex_lock(&pixel_host_lock);
		ret = pixel_pcie_update_l1ss();
		mutex_unlock(&pixel_host_lock);
		if (ret)
			sleep_errors++;
		p->restore_l1ss = false;
	}
	return ret;
}

static const struct dev_pm_ops pixel_pcie_pm = {
	.suspend_noirq = pixel_pcie_suspend_noirq,
	.resume_noirq = pixel_pcie_resume_noirq,
	.resume_early = pixel_pcie_resume_early,
};

static struct platform_driver pixel_pcie_driver = {
	.probe = pixel_pcie_probe,
	.remove = pixel_pcie_remove,
	.driver = { .name = "pixel-pcie", .probe_type = PROBE_FORCE_SYNCHRONOUS,
		    .pm = &pixel_pcie_pm },
};

static struct platform_device *pixel_pdev;

/* The stock node lists pin states on the GS201 pin controllers, which have no
 * mainline driver, so the driver core would defer it forever. A device that
 * reuses the node skips pinctrl binding and keeps its ranges, interrupt-map
 * and dma-coherent; this driver sets the pins itself.
 */
static int __init pixel_pcie_init(void)
{
	struct platform_device *stock = NULL;
	struct resource res[8];
	struct device_node *np;
	unsigned int n;
	u32 channel;
	int ret = -ENODEV;

	np = of_find_node_by_path("/pcie@14520000");
	if (!np)
		return -ENODEV;
	/* Channel 0 (pcie@11920000) is the modem's link: never touch it. */
	if (!of_machine_is_compatible("google,GS201") ||
	    !of_device_is_compatible(np, "samsung,exynos-pcie-rc") ||
	    of_property_read_u32(np, "ch-num", &channel) || channel != 1)
		goto put_node;
	stock = of_find_device_by_node(np);
	if (!stock)
		goto put_node;
	ret = -EBUSY;
	if (stock->dev.driver)
		goto put_node;
	for (n = 0; n < ARRAY_SIZE(res); n++)
		if (of_address_to_resource(np, n, &res[n]))
			break;

	ret = platform_driver_register(&pixel_pcie_driver);
	if (ret)
		goto put_node;
	pixel_pdev = platform_device_alloc("pixel-pcie", PLATFORM_DEVID_NONE);
	if (!pixel_pdev) {
		ret = -ENOMEM;
		goto unregister;
	}
	device_set_of_node_from_dev(&pixel_pdev->dev, &stock->dev);
	ret = platform_device_add_resources(pixel_pdev, res, n);
	if (!ret)
		ret = platform_device_add(pixel_pdev);
	if (ret) {
		platform_device_put(pixel_pdev);
		goto unregister;
	}
	if (!pixel_pdev->dev.driver) {
		/* probe failed: the link is powered down again */
		platform_device_unregister(pixel_pdev);
		ret = -EIO;
		goto unregister;
	}
	put_device(&stock->dev);
	of_node_put(np);
	ret = register_netdevice_notifier(&pixel_pcie_netdev_nb);
	if (ret) {
		platform_device_unregister(pixel_pdev);
		platform_driver_unregister(&pixel_pcie_driver);
	}
	return ret;

unregister:
	platform_driver_unregister(&pixel_pcie_driver);
put_node:
	if (stock)
		put_device(&stock->dev);
	of_node_put(np);
	return ret;
}
module_init(pixel_pcie_init);

static void __exit pixel_pcie_exit(void)
{
	unregister_netdevice_notifier(&pixel_pcie_netdev_nb);
	platform_device_unregister(pixel_pdev);
	platform_driver_unregister(&pixel_pcie_driver);
}
module_exit(pixel_pcie_exit);

MODULE_DESCRIPTION("GS201 PCIe channel 1 (Wi-Fi) DesignWare host");
MODULE_LICENSE("GPL");
