// SPDX-License-Identifier: GPL-2.0-only
/* Experimental GS201 UFS host using the bootloader's powered PHY/clocks.
 * Standard Linux UFS/SCSI queues; no private block layer or MMIO data path.
 * No runtime suspend, crypto or clock scaling.
 *
 * vendor_cal=1 applies Google's GS201 calibration (drivers/ufs/gs201/
 * ufs-cal-if.c, evt0 tables, SMDK board, 38.4 MHz reference) around link
 * startup and power mode changes; the HCE reset at probe clears the UniPro
 * side PCS configuration the bootloader left. hs_gear=N (with vendor_cal)
 * then requests HS-GN rate B on the connected lanes; a failed change falls
 * back to the link-startup PWM gear. Both default off: PWM gear 1, as before.
 * dev_reset=1 pulses the device's RST_N before host enable and in error
 * recovery, as the vendor driver does, instead of inheriting the device in
 * the bootloader's session.
 * deep_link_off=1 (with both) handles deep suspend as stock does: the link
 * goes off and the device powers down; the host is re-initialized on resume.
 */
#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/suspend.h>
#include <scsi/scsi_host.h>
#include <ufs/ufshcd.h>
#include <ufs/ufshci.h>
#include <ufs/unipro.h>

static struct platform_device *pixel_pdev;
static bool reprobe;
module_param(reprobe, bool, 0400);
static bool vendor_cal;
module_param(vendor_cal, bool, 0400);
MODULE_PARM_DESC(vendor_cal, "Apply the GS201 PHY/UniPro calibration tables");
static uint hs_gear;
module_param(hs_gear, uint, 0444);
MODULE_PARM_DESC(hs_gear, "HS gear to request with vendor_cal (0: stay in PWM)");
static uint cdr_timeouts;
module_param(cdr_timeouts, uint, 0444);
MODULE_PARM_DESC(cdr_timeouts, "Lanes whose CDR did not lock after an HS change");
static bool dev_reset;
module_param(dev_reset, bool, 0400);
MODULE_PARM_DESC(dev_reset, "Reset the UFS device (RST_N) before host enable");
static bool system_hibern8;
module_param(system_hibern8, bool, 0644);
MODULE_PARM_DESC(system_hibern8, "Opt in to calibrated HS Hibern8 during system suspend");
static uint hibern8_entries, hibern8_exits;
module_param(hibern8_entries, uint, 0444);
module_param(hibern8_exits, uint, 0444);
static bool deep_link_off;
module_param(deep_link_off, bool, 0644);
MODULE_PARM_DESC(deep_link_off, "Deep suspend: link off and device power-down, full host re-initialization on resume (needs vendor_cal and dev_reset)");
static uint smu_call = 2;
module_param(smu_call, uint, 0644);
MODULE_PARM_DESC(smu_call, "After HSI2 lost power: 2 SMC SMU(INIT) (default), 1 SMC FMP_SMU_RESUME, 0 none");
static long smu_ret;
module_param(smu_ret, long, 0444);
static uint reinits, reinits_lost;
module_param(reinits, uint, 0444);
MODULE_PARM_DESC(reinits, "Completed re-initializations after link off");
module_param(reinits_lost, uint, 0444);
MODULE_PARM_DESC(reinits_lost, "... of which found the host configuration lost (power-cycled)");

/* An HS change was asked for, and one completed: a probe that fails in
 * between failed the change itself, not the inherited-link handoff.
 */
static bool hs_requested, hs_done;

static u64 pixel_dma_mask = DMA_BIT_MASK(64);

/* Register blocks from the stock DT ufs@0x14700000 reg list. */
#define GS201_UNIPRO		0x14780000
#define GS201_UNIPRO_SIZE	0xa000
#define GS201_PMA		0x14704000
#define GS201_PMA_SIZE		0x3000
#define GS201_CMU_TOP		0x1e080000
#define GS201_CMU_HSI2		0x14400000
#define GS201_OSC_HZ		24576000UL

/* Stock gs201-ufs.dtsi: ufs-iocc (SYSREG_HSI2 0x710, mask 3, value 3) and
 * ufs-phy-iso (PMU 0x3ec8 bit 0; 1 = isolation bypassed). PMU writes go
 * through the secure monitor, as exynos_pmu_update() does on GS201.
 */
#define GS201_SYSREG_HSI2	0x14420000
#define SYSREG_UFS_IOCC		0x710
#define UFS_IOCC		0x3
#define GS201_PMU_UFS_PHY	0x18063ec8
#define GS201_SMC_PRIV_REG	0x82000504
#define PMUREG_WRITE		1
/* The UFS DMA filter (SMU) resets with HSI2 and then blocks every data
 * transfer (descriptors still work: NOP OUT passes, READ never completes).
 * SMU(INIT) through the monitor sets it up again; FMP_SMU_RESUME returns 0
 * but changes nothing here, as the bootloader, not the kernel, initialized it.
 * SIP fast SMC64 0x1860 FMP_SMU_RESUME and 0x1850 SMU (INIT 0), SMU_EMBEDDED
 * 0 (mainline ufs-exynos.c:1349-1360, exynos_ufs_fmp_resume()).
 */
#define SMC_FMP_SMU_RESUME	0xc2001860
#define SMC_SMU			0xc2001850

/* The device's VCC: a fixed regulator enabled by gpp0-1, active high
 * (gs201-ufs.dtsi ufs_fixed_vcc; PERIC0 GPIO 0x10840000, CON +0, DAT +4).
 * Stock's level 5 switches it off with the link (ufshcd_vreg_set_lpm), so
 * the device powers down cleanly when VCCQ (L8S, PMIC pin-controlled) drops
 * in SYS_SLEEP, and powers up again on resume.
 */
#define GS201_GPP0		0x10840000
#define GPP0_VCC		BIT(1)

/* Vendor HCI registers (ufs-vs-regs.h). */
#define HCI_SW_RST		0x50
#define UFS_SW_RST_MASK		(BIT(1) | BIT(0))
#define HCI_GPIO_OUT		0x70
#define HCI_CLKSTOP_CTRL		0xb0
#define HCI_FORCE_HCS		0xb4
#define CLK_STOP_ALL		(BIT(4) | BIT(2) | BIT(1) | BIT(0))
#define MPHY_APBCLK_STOP_EN	BIT(10)
#define UNIPRO_MCLK_STOP_EN	BIT(5)

#define PMA_LANE(reg, lane)	((reg) + 0x800 * (lane))
#define UNIP_COMP_AXI_AUX_FIELD	0x040
#define PCS_WSTRB		(0xfU << 24)
#define PCS_TX_LANE(l)		(l)
#define PCS_RX_LANE(l)		(4 + (l))
#define UNIP_DBG_PRD		0x044
#define UNIP_DL_ERROR_IRQ_MASK	0x4844
#define PA_ERROR_IND_RECEIVED	BIT(15)
#define UNIP_PA_ACTIVERXDATALANES	0x3200
#define UNIP_PA_CONNECTEDRXDATALANES	0x3204
#define UNIP_PA_DBG_OPTION_SUITE_1	0x39a8
#define UNIP_PA_DBG_OPTION_SUITE_2	0x39b4

/* The host configuration exynos_ufs_config_host() programs after its
 * software reset, and related vendor controls, as the bootloader left them:
 * TO_CNT_DIV, 1US_TO_CNT, VENDOR_SPECIFIC_IE, UTRL/UTMRL_NEXUS_TYPE,
 * E2EFC_CTRL, IDLE_TIMER_CONFIG, DATA_REORDER, MAX_DOUT_DATA_SIZE,
 * UNIPRO_APB_CLK_CTRL, AXIDMA_RWDATA_BURST_LEN, WRITE_DMA_CTRL,
 * ERROR_EN_PA/DL/N/T/DME, UFSHCI_V2P1_CTRL, REQ_HOLD_EN, CLKSTOP_CTRL,
 * FORCE_HCS, UFS_AXI_DMA_IF_CTRL, UFS/IOP_ACG_DISABLE, MPHY_REFCLK_SEL.
 * PRDT sizes are set in link startup; RST_N by the device reset.
 */
static const u16 hci_cfg[] = {
	0x08, 0x0c, 0x3c, 0x40, 0x44, 0x48, 0x58, 0x60, 0x64, 0x68, 0x6c, 0x74,
	0x78, 0x7c, 0x80, 0x84, 0x88, 0x8c, 0xac, 0xb0, 0xb4, 0xf8, 0xfc, 0x100,
	0x108,
};

struct pixel_ufs {
	void __iomem *vendor;
	void __iomem *unipro;
	void __iomem *pma;
	void __iomem *sysreg;
	void __iomem *pmu_phy;
	void __iomem *gpp0;
	bool vcc_off;	/* this driver switched the device's VCC off */
	u32 hci_boot[ARRAY_SIZE(hci_cfg)];
	bool reinit;	/* the link was turned off: re-initialize the host */
	bool diag_linked;	/* the first link's registers were logged */
	u32 mclk;
	u8 lanes;
	bool hs;	/* the change being made is to an HS mode */
	bool h8_calibrated;
	u32 h8_force_hcs;
	u32 h8_clkstop;
};

/* Entry kinds: where a calibration value goes. */
enum {
	UNIPRO,		/* UniPro block offset, value */
	UNIPRO_PRD18,	/* UniPro block offset, 16e9 / mclk */
	PCS_TX,		/* PCS offset per TX lane, value */
	PCS_RX,		/* PCS offset per RX lane, value */
	PCS_TX_RND,	/* per TX lane, rounded mclk period in ns */
	PCS_RX_RND,	/* per RX lane, rounded mclk period in ns */
	PCS_TX_LR,	/* per TX lane, 3200 us of line reset in mclk ticks */
	PCS_RX_LR,	/* per RX lane, 1000 us of line reset detect */
	PCS_COMN,	/* UniPro block offset (PCS common), value */
	PMA_COMN,	/* PHY offset, value */
	PMA_TRSV,	/* PHY offset per lane, value */
	PMA_CAL_WAIT,	/* per lane: wait for the calibration-done bit */
	PMA_CDR_WAIT,	/* per active RX lane: wait for CDR lock */
	ADAPT_LENGTH,	/* UniPro offset: vendor adapt-length fix-up */
};

struct gs201_cal {
	u16 addr;
	u32 val;
	u8 kind;
};

/* ufs-cal-if.c init_cfg_evt0 (evt1 is identical), USE_38_4_MHZ, all boards
 * except the ZEBU emulator entries.
 */
static const struct gs201_cal pre_link[] = {
	{ UNIP_DBG_PRD, 0, UNIPRO_PRD18 },
	{ 0x2800, 0x40, PCS_COMN }, { 0x2808, 0x02, PCS_COMN },
	{ 0x0a4, 0x22, PMA_COMN },
	{ 0x2048, 0, PCS_RX_RND }, { 0x22a8, 0, PCS_TX_RND },
	{ 0x22a4, 0x02, PCS_TX }, { 0x22ac, 0, PCS_TX_LR },
	{ 0x2044, 0x00, PCS_RX }, { 0x206c, 0, PCS_RX_LR },
	{ 0x20bc, 0x69, PCS_RX }, { 0x2210, 0x01, PCS_RX },
	{ 0x2010, 0x01, PCS_TX }, { 0x2094, 0xf6, PCS_RX },
	{ 0x21fc, 0x00, PCS_TX }, { 0x2800, 0x00, PCS_COMN },
	{ 0x3178, 0x0, UNIPRO }, { 0x5000, 0x0, UNIPRO }, { 0x5004, 0x1, UNIPRO },
	{ 0x6084, 0x1, UNIPRO }, { 0x6080, 0x1, UNIPRO },
	{ 0x4818, 0x80000000, UNIPRO },
	{ 0x10c, 0x10, PMA_COMN }, { 0x0f0, 0x14, PMA_COMN }, { 0x118, 0x48, PMA_COMN },
	{ 0x800, 0x00, PMA_TRSV }, { 0x804, 0x06, PMA_TRSV }, { 0x808, 0x06, PMA_TRSV },
	{ 0x80c, 0x0a, PMA_TRSV }, { 0x810, 0x00, PMA_TRSV }, { 0x814, 0x11, PMA_TRSV },
	{ 0x81c, 0x0c, PMA_TRSV }, { 0xb84, 0xc0, PMA_TRSV }, { 0x8b4, 0xb8, PMA_TRSV },
	{ 0x8d0, 0x60, PMA_TRSV }, { 0x8e0, 0x13, PMA_TRSV }, { 0x8e4, 0x48, PMA_TRSV },
	{ 0x8e8, 0x01, PMA_TRSV }, { 0x8ec, 0x25, PMA_TRSV }, { 0x8f0, 0x2a, PMA_TRSV },
	{ 0x8f4, 0x01, PMA_TRSV }, { 0x8f8, 0x13, PMA_TRSV }, { 0x8fc, 0x13, PMA_TRSV },
	{ 0x900, 0x4a, PMA_TRSV }, { 0x90c, 0x40, PMA_TRSV }, { 0x910, 0x02, PMA_TRSV },
	{ 0x974, 0x00, PMA_TRSV }, { 0x978, 0x3f, PMA_TRSV }, { 0x97c, 0xff, PMA_TRSV },
	{ 0x9cc, 0x33, PMA_TRSV }, { 0x9d0, 0x50, PMA_TRSV }, { 0xa10, 0x02, PMA_TRSV },
	{ 0xa14, 0x02, PMA_TRSV }, { 0xa88, 0x04, PMA_TRSV }, { 0x9f4, 0x01, PMA_TRSV },
	{ 0xbe8, 0x01, PMA_TRSV }, { 0xa18, 0x03, PMA_TRSV }, { 0xa1c, 0x03, PMA_TRSV },
	{ 0xa20, 0x03, PMA_TRSV }, { 0xa24, 0x03, PMA_TRSV }, { 0xacc, 0x04, PMA_TRSV },
	{ 0xad8, 0x0b, PMA_TRSV }, { 0xadc, 0x0b, PMA_TRSV }, { 0xae0, 0x0b, PMA_TRSV },
	{ 0xae4, 0x0b, PMA_TRSV }, { 0xae8, 0x0b, PMA_TRSV }, { 0xaec, 0x06, PMA_TRSV },
	{ 0xaf0, 0x06, PMA_TRSV }, { 0xaf4, 0x06, PMA_TRSV }, { 0xaf8, 0x06, PMA_TRSV },
	{ 0xb90, 0x1a, PMA_TRSV }, { 0xbb4, 0x25, PMA_TRSV }, { 0x9a4, 0x1a, PMA_TRSV },
	{ 0xbd0, 0x2f, PMA_TRSV }, { 0xd2c, 0x01, PMA_TRSV }, { 0xd30, 0x23, PMA_TRSV },
	{ 0xd34, 0x23, PMA_TRSV }, { 0xd38, 0x45, PMA_TRSV }, { 0xd3c, 0x00, PMA_TRSV },
	{ 0xd40, 0x31, PMA_TRSV }, { 0xd44, 0x00, PMA_TRSV }, { 0xd48, 0x02, PMA_TRSV },
	{ 0xd4c, 0x00, PMA_TRSV }, { 0xd50, 0x01, PMA_TRSV },
	{ 0x10c, 0x18, PMA_COMN }, { 0x10c, 0x00, PMA_COMN },
	{ 0xce0, 0x08, PMA_CAL_WAIT },
	{ 0x4818, 0x0, UNIPRO },
};

/* post_init_cfg_evt0: PA_TActivate 1000 through the debug MIB window. */
static const struct gs201_cal post_link[] = {
	{ 0x3348, 0, ADAPT_LENGTH }, { 0x334c, 0, ADAPT_LENGTH },
	{ 0x38a4, 0x01, UNIPRO }, { 0x3290, 0x3e8, UNIPRO }, { 0x38a4, 0x00, UNIPRO },
};

/* calib_of_hs_rate_a/_b (identical on GS201): PA_TxHsAdaptType, the DL
 * protection/replay/AFC timeouts and their PA_PWRModeUserData copies.
 */
static const struct gs201_cal pre_hs[] = {
	{ 0x3350, 0x1, UNIPRO },
	{ 0x4104, 8064, UNIPRO }, { 0x4108, 28224, UNIPRO }, { 0x410c, 20160, UNIPRO },
	{ 0x32c0, 12000, UNIPRO }, { 0x32c4, 32000, UNIPRO }, { 0x32c8, 16000, UNIPRO },
	{ 0x7888, 8064, UNIPRO }, { 0x788c, 28224, UNIPRO }, { 0x7890, 20160, UNIPRO },
	{ 0x78b8, 12000, UNIPRO }, { 0x78bc, 32000, UNIPRO }, { 0x78c0, 16000, UNIPRO },
	{ 0xda4, 0x11, PMA_TRSV }, { 0x918, 0x03, PMA_TRSV },
};

static const struct gs201_cal post_hs[] = {
	{ 0xce4, 0x08, PMA_CDR_WAIT }, { 0x918, 0x01, PMA_TRSV },
};

static const struct gs201_cal pre_pwm[] = {
	{ 0x4104, 8064, UNIPRO }, { 0x4108, 28224, UNIPRO }, { 0x410c, 20160, UNIPRO },
	{ 0x32c0, 12000, UNIPRO }, { 0x32c4, 32000, UNIPRO }, { 0x32c8, 16000, UNIPRO },
	{ 0x7888, 8064, UNIPRO }, { 0x788c, 28224, UNIPRO }, { 0x7890, 20160, UNIPRO },
	{ 0x78b8, 12000, UNIPRO }, { 0x78bc, 32000, UNIPRO }, { 0x78c0, 16000, UNIPRO },
};

static const struct gs201_cal post_pwm[] = {
	{ 0x020, 0x60, PMA_COMN }, { 0x888, 0x08, PMA_TRSV }, { 0x918, 0x01, PMA_TRSV },
};

static void pcs_write(struct pixel_ufs *p, u8 sel, u32 addr, u32 val)
{
	writel(PCS_WSTRB | sel, p->unipro + UNIP_COMP_AXI_AUX_FIELD);
	writel(val, p->unipro + addr);
	writel(PCS_WSTRB, p->unipro + UNIP_COMP_AXI_AUX_FIELD);
}

static void pcs_ticks(struct pixel_ufs *p, u8 sel, u32 addr, u32 us)
{
	u32 ticks = div_u64((u64)p->mclk * us, 1000000);

	pcs_write(p, sel, addr, (ticks >> 16) & 0xff);
	pcs_write(p, sel, addr + 4, (ticks >> 8) & 0xff);
	pcs_write(p, sel, addr + 8, ticks & 0xff);
}

/* ufs30_cal_wait_cdr_lock: up to 100 tries, kicking the CDR between them. */
static bool cdr_lock(struct pixel_ufs *p, u32 addr, u32 mask, u8 lane)
{
	int i;

	for (i = 0; i < 100; i++) {
		udelay(40);
		if ((readl(p->pma + PMA_LANE(addr, lane)) & mask) == mask)
			return true;
		udelay(40);
		writel(0x10, p->pma + PMA_LANE(0x888, lane));
		writel(0x18, p->pma + PMA_LANE(0x888, lane));
	}
	return false;
}

static int apply(struct ufs_hba *hba, const struct gs201_cal *cal, size_t n,
		 u8 active)
{
	struct pixel_ufs *p = ufshcd_get_variant(hba);
	u32 rnd = DIV_ROUND_CLOSEST(1000000000UL, p->mclk);
	int failed = 0;
	size_t i;
	u8 l;

	for (i = 0; i < n; i++) {
		const struct gs201_cal *c = &cal[i];
		u32 v;

		switch (c->kind) {
		case UNIPRO:
		case PCS_COMN:
			writel(c->val, p->unipro + c->addr);
			break;
		case UNIPRO_PRD18:
			writel(div_u64(16ULL * 1000 * 1000000, p->mclk), p->unipro + c->addr);
			break;
		case ADAPT_LENGTH:
			v = readl(p->unipro + c->addr);
			if (v & 0x80) {
				if ((v & 0x7f) < 2)
					writel(0x82, p->unipro + c->addr);
			} else if ((v + 1) & 0x3) {
				writel(v | 0x3, p->unipro + c->addr);
			}
			break;
		case PMA_COMN:
			writel(c->val, p->pma + c->addr);
			break;
		default:
			for (l = 0; l < p->lanes; l++) {
				switch (c->kind) {
				case PCS_TX:
					pcs_write(p, PCS_TX_LANE(l), c->addr, c->val);
					break;
				case PCS_RX:
					pcs_write(p, PCS_RX_LANE(l), c->addr, c->val);
					break;
				case PCS_TX_RND:
					pcs_write(p, PCS_TX_LANE(l), c->addr, rnd);
					break;
				case PCS_RX_RND:
					pcs_write(p, PCS_RX_LANE(l), c->addr, rnd);
					break;
				case PCS_TX_LR:
					pcs_ticks(p, PCS_TX_LANE(l), c->addr, 3200);
					break;
				case PCS_RX_LR:
					pcs_ticks(p, PCS_RX_LANE(l), c->addr, 1000);
					break;
				case PMA_TRSV:
					writel(c->val, p->pma + PMA_LANE(c->addr, l));
					break;
				case PMA_CAL_WAIT:
					/* ufs30_cal_done_wait never fails; neither does this. */
					readl_poll_timeout_atomic(p->pma + PMA_LANE(c->addr, l),
								  v, (v & c->val) == c->val, 40, 4000);
					break;
				case PMA_CDR_WAIT:
					if (l < active && !cdr_lock(p, c->addr, c->val, l)) {
						dev_err(hba->dev, "lane %u CDR did not lock\n", l);
						failed++;
					}
					break;
				}
			}
		}
	}
	return failed;
}

/* The UniPro clock is CMU_HSI2's UFS_EMBD user mux over CMU_TOP's
 * CLKCMU_HSI2_UFS_EMBD, from PLL_SHARED0 / 2 / 2 (Google's GS201
 * cmucal-node.c and cmucal-sfr.c). Anything else is refused.
 */
static int gs201_mclk(struct device *dev, u32 *rate)
{
	void __iomem *top, *hsi2;
	u32 con0, con3, div2, div4, sel, div, user, m, pdiv, s;
	u64 hz;

	top = ioremap(GS201_CMU_TOP, 0x2000);
	hsi2 = ioremap(GS201_CMU_HSI2, 0x1000);
	if (!top || !hsi2) {
		if (top)
			iounmap(top);
		if (hsi2)
			iounmap(hsi2);
		return -ENOMEM;
	}
	con0 = readl(top + 0x140);
	con3 = readl(top + 0x14c);
	div2 = readl(top + 0x1928) & 1;
	div4 = readl(top + 0x1930) & 1;
	sel = readl(top + 0x10a8) & 3;
	div = readl(top + 0x18a4) & 0xf;
	user = readl(hsi2 + 0x630);
	iounmap(top);
	iounmap(hsi2);

	m = (con3 >> 16) & 0x3ff;
	pdiv = (con3 >> 8) & 0x3f;
	s = con3 & 7;
	if (!(con0 & BIT(4)) || !(con3 & BIT(31)) || !pdiv || sel != 1 || !(user & BIT(4))) {
		dev_err(dev, "unexpected UFS clock path: con0=%#x con3=%#x sel=%u user=%#x\n",
			con0, con3, sel, user);
		return -EINVAL;
	}
	hz = div_u64((u64)GS201_OSC_HZ * m, pdiv << s);
	hz = div_u64(hz, (div2 + 1) * (div4 + 1) * (div + 1));
	*rate = hz;
	return 0;
}

static int pixel_init(struct ufs_hba *hba)
{
	struct pixel_ufs *p;
	unsigned int i;
	int ret;

	p = devm_kzalloc(hba->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->vendor = devm_platform_ioremap_resource(to_platform_device(hba->dev), 1);
	if (IS_ERR(p->vendor))
		return PTR_ERR(p->vendor);
	p->lanes = 2;
	if (vendor_cal) {
		p->unipro = devm_ioremap(hba->dev, GS201_UNIPRO, GS201_UNIPRO_SIZE);
		p->pma = devm_ioremap(hba->dev, GS201_PMA, GS201_PMA_SIZE);
		if (!p->unipro || !p->pma)
			return -ENOMEM;
		ret = gs201_mclk(hba->dev, &p->mclk);
		if (ret)
			return ret;
		if (p->mclk < 100000000 || p->mclk > 200000000)
			return dev_err_probe(hba->dev, -EINVAL, "UniPro clock %u Hz out of range\n",
					     p->mclk);
		dev_info(hba->dev, "vendor calibration, UniPro clock %u Hz, HS gear %u\n",
			 p->mclk, hs_gear);
	}
	ufshcd_set_variant(hba, p);
	/* GS201 stock DT has fixed-prdt-req_list-ocs: do not copy the older
	 * Exynos PRDT-byte-granularity or inverted request-clear quirks.
	 */
	hba->quirks = UFSHCD_QUIRK_SKIP_DEF_UNIPRO_TIMEOUT_SETTING |
		      UFSHCD_QUIRK_BROKEN_AUTO_HIBERN8;
	hba->caps = 0;
	hba->rpm_lvl = UFS_PM_LVL_0;
	hba->spm_lvl = UFS_PM_LVL_0;
	hba->lanes_per_direction = 2;
	hba->host->max_segment_size = 4096;
	if (ufshcd_readl(hba, REG_UFS_VERSION) != 0x300 ||
	    (!reprobe && ufshcd_readl(hba, REG_CONTROLLER_ENABLE) != 1) ||
	    !(ufshcd_readl(hba, REG_CONTROLLER_STATUS) & DEVICE_PRESENT) ||
	    ufshcd_readl(hba, REG_UTP_TRANSFER_REQ_DOOR_BELL) ||
	    ufshcd_readl(hba, REG_UTP_TASK_REQ_DOOR_BELL) ||
	    readl(p->vendor + 0x60) != 0xa || readl(p->vendor + 0xb0))
		return dev_err_probe(hba->dev, -EINVAL, "unexpected UFS handoff state\n");
	if (dev_reset) {
		/* The bootloader leaves the device out of reset (RST_N high). */
		u32 gpio = readl(p->vendor + HCI_GPIO_OUT);

		if (gpio != 1)
			return dev_err_probe(hba->dev, -EINVAL, "unexpected RST_N state %#x\n", gpio);
		dev_info(hba->dev, "device reset before host enable\n");
	}
	if (vendor_cal && dev_reset) {
		p->sysreg = devm_ioremap(hba->dev, GS201_SYSREG_HSI2, 0x1000);
		p->pmu_phy = devm_ioremap(hba->dev, GS201_PMU_UFS_PHY & PAGE_MASK, PAGE_SIZE);
		if (!p->sysreg || !p->pmu_phy)
			return -ENOMEM;
		p->pmu_phy += GS201_PMU_UFS_PHY & ~PAGE_MASK;
		/* VCC switching only with gpp0-1 an output driven high. */
		p->gpp0 = devm_ioremap(hba->dev, GS201_GPP0, 0x10);
		if (p->gpp0 && ((readl(p->gpp0) >> 4) & 0xf) == 1 &&
		    (readl(p->gpp0 + 4) & GPP0_VCC)) {
			dev_info(hba->dev, "VCC on gpp0-1\n");
		} else {
			dev_warn(hba->dev, "unexpected VCC pin state; VCC stays on\n");
			p->gpp0 = NULL;
		}
		for (i = 0; i < ARRAY_SIZE(hci_cfg); i++)
			p->hci_boot[i] = readl(p->vendor + hci_cfg[i]);
		dev_info(hba->dev, "IOCC %#x, PHY isolation bypass %u\n",
			 readl(p->sysreg + SYSREG_UFS_IOCC), readl(p->pmu_phy) & 1);
	}
	return 0;
}

static void pixel_diag(struct ufs_hba *hba, const char *when);

/* exynos_ufs_ctrl_phy_pwr(): PMU 0x3ec8 bit 0, through the monitor. */
static int pixel_phy_bypass(struct ufs_hba *hba, bool bypass)
{
	struct pixel_ufs *p = ufshcd_get_variant(hba);
	struct arm_smccc_res res;
	u32 v = readl(p->pmu_phy);

	if (!!(v & 1) == bypass)
		return 0;
	v = bypass ? v | 1 : v & ~1;
	arm_smccc_smc(GS201_SMC_PRIV_REG, GS201_PMU_UFS_PHY, PMUREG_WRITE, v, 0, 0, 0, 0, &res);
	if (res.a0 || !!(readl(p->pmu_phy) & 1) != bypass) {
		dev_err(hba->dev, "PHY isolation %s failed (%ld)\n",
			bypass ? "bypass" : "enable", (long)res.a0);
		return -EIO;
	}
	return 0;
}

/* One line of registers around a re-initialization, for diagnosis: vendor
 * HCI clock controls, REFCLK select and RST_N; UniPro registers the
 * calibration writes; PHY common and lane-0 registers it writes, and both
 * lanes' calibration-done status (0xce0 bit 3, PMA_CAL_WAIT). All are in
 * the tables above; the UFSHCI UIC error codes clear on read.
 */
static void pixel_diag(struct ufs_hba *hba, const char *when)
{
	struct pixel_ufs *p = ufshcd_get_variant(hba);

	dev_info(hba->dev,
		 "diag %s: hci b4=%x b0=%x 108=%x 70=%x hcs=%x uic pa=%x dl=%x unipro 44=%x 39a8=%x 3178=%x 4818=%x pma a4=%x 10c=%x f0=%x 118=%x 804=%x 8b4=%x ce0=%x/%x\n",
		 when, readl(p->vendor + HCI_FORCE_HCS), readl(p->vendor + HCI_CLKSTOP_CTRL),
		 readl(p->vendor + 0x108), readl(p->vendor + HCI_GPIO_OUT),
		 ufshcd_readl(hba, REG_CONTROLLER_STATUS),
		 ufshcd_readl(hba, REG_UIC_ERROR_CODE_PHY_ADAPTER_LAYER),
		 ufshcd_readl(hba, REG_UIC_ERROR_CODE_DATA_LINK_LAYER),
		 readl(p->unipro + UNIP_DBG_PRD), readl(p->unipro + UNIP_PA_DBG_OPTION_SUITE_1),
		 readl(p->unipro + 0x3178), readl(p->unipro + 0x4818),
		 readl(p->pma + 0x0a4), readl(p->pma + 0x10c), readl(p->pma + 0x0f0),
		 readl(p->pma + 0x118), readl(p->pma + PMA_LANE(0x804, 0)),
		 readl(p->pma + PMA_LANE(0x8b4, 0)), readl(p->pma + PMA_LANE(0xce0, 0)),
		 readl(p->pma + PMA_LANE(0xce0, 1)));
}

/* __exynos_ufs_suspend(): with the link off, hold the device in reset
 * and isolate the PHY. Only system PM turns the link off here.
 */
static int pixel_suspend(struct ufs_hba *hba, enum ufs_pm_op op,
			 enum ufs_notify_change_status status)
{
	struct pixel_ufs *p = ufshcd_get_variant(hba);

	if (status != POST_CHANGE || op != UFS_SYSTEM_PM || !ufshcd_is_link_off(hba) ||
	    !p->pmu_phy)
		return 0;
	writel(0, p->vendor + HCI_GPIO_OUT);
	p->reinit = true;
	if (p->gpp0) {
		writel(readl(p->gpp0 + 4) & ~GPP0_VCC, p->gpp0 + 4);
		p->vcc_off = true;
	}
	return pixel_phy_bypass(hba, false);
}

/* __exynos_ufs_resume() -> exynos_ufs_config_externals(): PHY isolation
 * bypass and IO coherency, before the core re-initializes the host. SYS_SLEEP
 * resets SYSREG_HSI2; this driver maps the device DMA-coherent, so no
 * request may run until IOCC is back. The UniPro clock must also be the one
 * the calibration was computed for.
 */
static int pixel_resume(struct ufs_hba *hba, enum ufs_pm_op op)
{
	struct pixel_ufs *p = ufshcd_get_variant(hba);
	u32 iocc, mclk;
	bool lost;
	int ret;

	if (!p->reinit)
		return 0;
	if (p->vcc_off) {
		/* VCC up before the device reset in host enable. */
		writel(readl(p->gpp0 + 4) | GPP0_VCC, p->gpp0 + 4);
		p->vcc_off = false;
		usleep_range(10000, 11000);
	}
	/* SYS_SLEEP resets SYSREG_HSI2 with the domain: IOCC reads 0x10. */
	lost = (readl(p->sysreg + SYSREG_UFS_IOCC) & UFS_IOCC) != UFS_IOCC;
	ret = pixel_phy_bypass(hba, true);
	if (ret)
		return ret;
	iocc = readl(p->sysreg + SYSREG_UFS_IOCC);
	if ((iocc & UFS_IOCC) != UFS_IOCC)
		writel(iocc | UFS_IOCC, p->sysreg + SYSREG_UFS_IOCC);
	if ((readl(p->sysreg + SYSREG_UFS_IOCC) & UFS_IOCC) != UFS_IOCC) {
		dev_err(hba->dev, "IOCC did not take (%#x)\n", readl(p->sysreg + SYSREG_UFS_IOCC));
		return -EIO;
	}
	ret = gs201_mclk(hba->dev, &mclk);
	if (!ret && mclk != p->mclk) {
		dev_err(hba->dev, "UniPro clock %u Hz after resume, calibrated for %u\n",
			mclk, p->mclk);
		ret = -EIO;
	}
	if (ret)
		return ret;
	if (lost && smu_call) {
		struct arm_smccc_res res;

		if (smu_call == 2)
			arm_smccc_smc(SMC_SMU, 0, 0, 0, 0, 0, 0, 0, &res);
		else
			arm_smccc_smc(SMC_FMP_SMU_RESUME, 0, 0, 0, 0, 0, 0, 0, &res);
		smu_ret = res.a0;
		dev_info(hba->dev, "SMU %s: %ld\n", smu_call == 2 ? "init" : "resume",
			 (long)res.a0);
	}
	if (lost)
		reinits_lost++;
	dev_info(hba->dev, "resume from link off: HSI2 %s, IOCC was %#x\n",
		 lost ? "power-cycled" : "kept", iocc);
	pixel_diag(hba, "resume");
	return 0;
}

/* exynos_ufs_hce_enable_notify() PRE_CHANGE after a link-off suspend:
 * exynos_ufs_init_host() (software reset, host configuration), then
 * exynos_ufs_dev_hw_reset().
 */
static int pixel_hce(struct ufs_hba *hba, enum ufs_notify_change_status status)
{
	struct pixel_ufs *p = ufshcd_get_variant(hba);
	unsigned int i;
	u32 v;

	if (status != PRE_CHANGE || !p->reinit)
		return 0;
	writel(UFS_SW_RST_MASK, p->vendor + HCI_SW_RST);
	if (readl_poll_timeout_atomic(p->vendor + HCI_SW_RST, v, !(v & UFS_SW_RST_MASK), 10, 1000))
		dev_err(hba->dev, "host software reset timed out\n");
	for (i = 0; i < ARRAY_SIZE(hci_cfg); i++)
		writel(p->hci_boot[i], p->vendor + hci_cfg[i]);
	p->h8_calibrated = false;
	writel(0, p->vendor + HCI_GPIO_OUT);
	udelay(5);
	writel(1, p->vendor + HCI_GPIO_OUT);
	return 0;
}

static int pixel_link(struct ufs_hba *hba, enum ufs_notify_change_status status)
{
	struct pixel_ufs *p = ufshcd_get_variant(hba);

	if (status == PRE_CHANGE) {
		/* GS201 vendor ufs-cal-if.c initializes these standard UniPro
		 * attributes before link startup, just as mainline GS101 does.
		 */
		static const u32 attrs[][2] = {
			{ N_DEVICEID, 0 }, { N_DEVICEID_VALID, 1 },
			{ T_PEERDEVICEID, 1 }, { T_CONNECTIONSTATE, 1 },
		};
		u32 state;
		int i, ret;

		if (vendor_cal) {
			/* Keep the PHY context through link startup (UniPro 1.8). */
			writel(0x90913c1c, p->unipro + UNIP_PA_DBG_OPTION_SUITE_1);
			writel(0xe01c115f, p->unipro + UNIP_PA_DBG_OPTION_SUITE_2);
			apply(hba, pre_link, ARRAY_SIZE(pre_link), 0);
			if (p->reinit)
				pixel_diag(hba, "pre-link");
		}
		ret = ufshcd_dme_get(hba, UIC_ARG_MIB(T_CONNECTIONSTATE), &state);
		if (ret)
			return ret;
		dev_info(hba->dev, "inherited CPort connection=%u\n", state);
		for (i = 0; i < ARRAY_SIZE(attrs); i++) {
			ret = ufshcd_dme_set(hba, UIC_ARG_MIB(attrs[i][0]), attrs[i][1]);
			if (ret)
				return ret;
		}
	}

	if (status == POST_CHANGE) {
		if (vendor_cal) {
			apply(hba, post_link, ARRAY_SIZE(post_link), 0);
			dev_info(hba->dev, "connected lanes %u\n",
				 readl(p->unipro + UNIP_PA_CONNECTEDRXDATALANES));
		}
		/* Standard unencrypted PRDT, 4 KiB maximum segment. */
		writel(0xc, p->vendor);
		writel(0xc, p->vendor + 4);
		writel(0xa, p->vendor + 0x60);
		if (vendor_cal && (p->reinit || !p->diag_linked)) {
			pixel_diag(hba, p->reinit ? "relinked" : "linked");
			p->diag_linked = true;
		}
		if (p->reinit) {
			p->reinit = false;
			reinits++;
		}
	}
	return 0;
}

static int pixel_negotiate(struct ufs_hba *hba,
			   const struct ufs_pa_layer_attr *desired,
			   struct ufs_pa_layer_attr *final)
{
	/* Without the calibration, stay in the link-startup PWM gear. */
	if (!vendor_cal || !hs_gear) {
		*final = hba->pwr_info;
		return 0;
	}
	*final = *desired;
	final->gear_rx = min_t(u32, desired->gear_rx, hs_gear);
	final->gear_tx = min_t(u32, desired->gear_tx, hs_gear);
	final->pwr_rx = FAST_MODE;
	final->pwr_tx = FAST_MODE;
	final->hs_rate = PA_HS_MODE_B;
	hs_requested = true;
	dev_info(hba->dev, "requesting HS-G%u rate B on %u/%u lanes\n",
		 final->gear_rx, final->lane_rx, final->lane_tx);
	return 0;
}

static int pixel_pwr_change(struct ufs_hba *hba, enum ufs_notify_change_status status,
			    struct ufs_pa_layer_attr *mode)
{
	struct pixel_ufs *p = ufshcd_get_variant(hba);
	u32 dl_mask;
	u8 active;
	int failed;

	if (!vendor_cal)
		return 0;
	if (status == PRE_CHANGE) {
		p->hs = mode->pwr_rx == FAST_MODE || mode->pwr_rx == FASTAUTO_MODE;
		/* The vendor masks PA_ERROR_IND_RECEIVED across the change. */
		dl_mask = readl(p->unipro + UNIP_DL_ERROR_IRQ_MASK) | PA_ERROR_IND_RECEIVED;
		writel(dl_mask, p->unipro + UNIP_DL_ERROR_IRQ_MASK);
		if (p->hs)
			apply(hba, pre_hs, ARRAY_SIZE(pre_hs), 0);
		else
			apply(hba, pre_pwm, ARRAY_SIZE(pre_pwm), 0);
		return 0;
	}
	active = readl(p->unipro + UNIP_PA_ACTIVERXDATALANES);
	if (p->hs) {
		hs_done = true;
		failed = apply(hba, post_hs, ARRAY_SIZE(post_hs), active);
		cdr_timeouts += failed;
		/* Stop asking for HS after repeated failures: errors would
		 * otherwise loop through reset and the same change again.
		 */
		if (cdr_timeouts >= 2 && hs_gear) {
			dev_err(hba->dev, "HS unreliable; later changes stay in PWM\n");
			hs_gear = 0;
		}
	} else {
		apply(hba, post_pwm, ARRAY_SIZE(post_pwm), active);
	}
	dev_info(hba->dev, "power mode %s G%u, %u active lanes\n",
		 p->hs ? "HS" : "PWM", mode->gear_rx, active);
	return 0;
}

/* The vendor exynos_ufs_dev_hw_reset(): RST_N low for 5 us. Without it the
 * device stays in the bootloader's session, and the first fDeviceInit query
 * after link startup answers 0xff until a second full initialization.
 */
static int pixel_device_reset(struct ufs_hba *hba)
{
	struct pixel_ufs *p = ufshcd_get_variant(hba);

	if (!dev_reset || !p)
		return -EOPNOTSUPP;
	writel(0, p->vendor + HCI_GPIO_OUT);
	udelay(5);
	writel(1, p->vendor + HCI_GPIO_OUT);
	usleep_range(10, 15);
	return 0;
}

static void pixel_xfer(struct ufs_hba *hba, int tag, bool scsi)
{
	struct pixel_ufs *p = ufshcd_get_variant(hba);
	u32 value = readl(p->vendor + 0x40);

	writel(scsi ? value | BIT(tag) : value & ~BIT(tag), p->vendor + 0x40);
}

static int pixel_dma(struct ufs_hba *hba)
{
	int ret;

	/* Keep descriptor allocations below 4 GiB in this retained handoff.
	 * Data buffers need the advertised 64-bit addressing: RAM extends
	 * above 4 GiB and the kernel has no 32-bit bounce pool.
	 */
	if (!(hba->capabilities & MASK_64_ADDRESSING_SUPPORT))
		return -EINVAL;
	ret = dma_set_mask(hba->dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;
	return dma_set_coherent_mask(hba->dev, DMA_BIT_MASK(32));
}

/* Google's GS201 post_h8_enter / pre_h8_exit tables. System PM is
 * restricted to the calibrated two-lane HS link, so TRSV_SQ applies to
 * both connected lanes and the HS entries always apply. Keep controller
 * power, RST_N and external CMU clocks unchanged.
 */
static const struct gs201_cal post_h8_enter[] = {
	{ 0x988, 0x08, PMA_TRSV }, { 0x994, 0x0a, PMA_TRSV },
	{ 0x004, 0x08, PMA_COMN }, { 0x000, 0x86, PMA_COMN },
	{ 0x020, 0x60, PMA_COMN }, { 0x888, 0x08, PMA_TRSV },
	{ 0x918, 0x01, PMA_TRSV },
};
static const struct gs201_cal pre_h8_exit[] = {
	{ 0x000, 0xc6, PMA_COMN }, { 0x004, 0x0c, PMA_COMN },
	{ 0x988, 0x00, PMA_TRSV }, { 0x994, 0x00, PMA_TRSV },
	{ 0x020, 0xe0, PMA_COMN }, { 0x918, 0x03, PMA_TRSV },
	{ 0x888, 0x18, PMA_TRSV }, { 0xce4, 0x08, PMA_CDR_WAIT },
};

static void pixel_hibern8(struct ufs_hba *hba, enum uic_cmd_dme cmd,
			  enum ufs_notify_change_status status)
{
	struct pixel_ufs *p = ufshcd_get_variant(hba);
	u32 force;

	if (cmd == UIC_CMD_DME_HIBER_ENTER && status == POST_CHANGE) {
		/* Also guard non-system-PM callers, including recovery paths. */
		if (!vendor_cal || hba->pwr_info.pwr_rx != FAST_MODE ||
		    hba->pwr_info.pwr_tx != FAST_MODE ||
		    hba->pwr_info.lane_rx != 2 || hba->pwr_info.lane_tx != 2)
			return;
		p->h8_force_hcs = readl(p->vendor + HCI_FORCE_HCS);
		p->h8_clkstop = readl(p->vendor + HCI_CLKSTOP_CTRL);
		/* Match ufs_call_cal: keep UniPro and PHY APB accessible. */
		force = p->h8_force_hcs &
			~(UNIPRO_MCLK_STOP_EN | MPHY_APBCLK_STOP_EN);
		writel(force, p->vendor + HCI_FORCE_HCS);
		apply(hba, post_h8_enter, ARRAY_SIZE(post_h8_enter), 2);
		writel(force | MPHY_APBCLK_STOP_EN, p->vendor + HCI_FORCE_HCS);
		writel(p->h8_clkstop | CLK_STOP_ALL, p->vendor + HCI_CLKSTOP_CTRL);
		p->h8_calibrated = true;
		hibern8_entries++;
	} else if (cmd == UIC_CMD_DME_HIBER_EXIT && status == PRE_CHANGE &&
		   p->h8_calibrated) {
		writel(p->h8_clkstop, p->vendor + HCI_CLKSTOP_CTRL);
		force = readl(p->vendor + HCI_FORCE_HCS);
		writel(force & ~(UNIPRO_MCLK_STOP_EN | MPHY_APBCLK_STOP_EN),
		       p->vendor + HCI_FORCE_HCS);
		cdr_timeouts += apply(hba, pre_h8_exit, ARRAY_SIZE(pre_h8_exit), 2);
		/* Restore the retained clock policy, rather than enabling new
		 * automatic gating as a side effect of the sleep experiment.
		 */
		writel(p->h8_force_hcs, p->vendor + HCI_FORCE_HCS);
		p->h8_calibrated = false;
	} else if (cmd == UIC_CMD_DME_HIBER_EXIT && status == POST_CHANGE) {
		hibern8_exits++;
	}
}

static int pixel_suspend_prepare(struct device *dev)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);
	bool enable = READ_ONCE(system_hibern8);
	bool off = READ_ONCE(deep_link_off) && pm_suspend_target_state == PM_SUSPEND_MEM;

	if (off && (!vendor_cal || !dev_reset))
		return -EOPNOTSUPP;
	if (enable && !off && (!vendor_cal || hba->pwr_info.pwr_rx != FAST_MODE ||
			       hba->pwr_info.pwr_tx != FAST_MODE ||
			       hba->pwr_info.lane_rx != 2 || hba->pwr_info.lane_tx != 2))
		return -EOPNOTSUPP;
	/* The WLUN child enters Hibern8 before the parent's suspend callback.
	 * Select its level in prepare, before any device starts suspending.
	 * Deep suspend powers HSI2 down in SYS_SLEEP: stock's level 5
	 * (ufs-exynos.c exynos_ufs_override_hba_params), device power-down
	 * and link off.
	 */
	hba->spm_lvl = off ? UFS_PM_LVL_5 : enable ? UFS_PM_LVL_1 : UFS_PM_LVL_0;
	return ufshcd_suspend_prepare(dev);
}

static const struct dev_pm_ops pixel_pm_ops = {
	.prepare = pixel_suspend_prepare,
	.complete = ufshcd_resume_complete,
	.suspend = ufshcd_system_suspend,
	.resume = ufshcd_system_resume,
};

static const struct ufs_hba_variant_ops pixel_ops = {
	.name = "gs201-handoff",
	.init = pixel_init,
	.set_dma_mask = pixel_dma,
	.link_startup_notify = pixel_link,
	.negotiate_pwr_mode = pixel_negotiate,
	.pwr_change_notify = pixel_pwr_change,
	.setup_xfer_req = pixel_xfer,
	.device_reset = pixel_device_reset,
	.hibern8_notify = pixel_hibern8,
	.hce_enable_notify = pixel_hce,
	.suspend = pixel_suspend,
	.resume = pixel_resume,
};

static int pixel_probe(struct platform_device *pdev)
{
	struct ufs_hba *hba;
	void __iomem *base;
	int ret, irq;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = ufshcd_alloc_host(&pdev->dev, &hba);
	if (ret)
		return ret;
	hba->vops = &pixel_ops;
	ret = ufshcd_init(hba, base, irq);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "UFS startup failed\n");
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_forbid(&pdev->dev);
	return 0;
}

static void pixel_remove(struct platform_device *pdev)
{
	struct ufs_hba *hba = platform_get_drvdata(pdev);

	ufshcd_remove(hba);
	pm_runtime_disable(&pdev->dev);
}

static struct platform_driver pixel_driver = {
	.probe = pixel_probe,
	.remove = pixel_remove,
	.driver = { .name = "pixel-ufs", .probe_type = PROBE_FORCE_SYNCHRONOUS,
		    .pm = &pixel_pm_ops },
};

static int __init pixel_ufs_init(void)
{
	struct device_node *np;
	struct platform_device *stock;
	struct resource res[3];
	int ret, irq, attempt;

	if (!of_machine_is_compatible("google,GS201 CHEETAH"))
		return -ENODEV;
	np = of_find_node_by_path("/ufs@0x14700000");
	if (!np)
		return -ENODEV;
	stock = of_find_device_by_node(np);
	if (stock && stock->dev.driver) {
		put_device(&stock->dev);
		of_node_put(np);
		return -EBUSY;
	}
	if (stock)
		put_device(&stock->dev);
	ret = of_address_to_resource(np, 0, &res[0]);
	if (!ret)
		ret = of_address_to_resource(np, 1, &res[1]);
	irq = irq_of_parse_and_map(np, 0);
	of_node_put(np);
	if (ret || !irq || res[0].start != 0x14700000 ||
	    res[1].start != 0x14701100)
		return -EINVAL;
	res[2] = (struct resource)DEFINE_RES_IRQ(irq);
	ret = platform_driver_register(&pixel_driver);
	if (ret)
		return ret;
	attempt = 0;
retry:
	pixel_pdev = platform_device_alloc("pixel-ufs", PLATFORM_DEVID_NONE);
	if (!pixel_pdev) {
		ret = -ENOMEM;
		goto unregister;
	}
	pixel_pdev->dev.dma_mask = &pixel_dma_mask;
	pixel_pdev->dev.coherent_dma_mask = pixel_dma_mask;
	/* Stock SYSREG_HSI2 IOCC readback is 0x13 (read/write coherent). */
	dev_set_dma_coherent(&pixel_pdev->dev);
	ret = platform_device_add_resources(pixel_pdev, res, ARRAY_SIZE(res));
	if (!ret)
		ret = platform_device_add(pixel_pdev);
	if (!ret) {
		if (pixel_pdev->dev.driver)
			return 0;
		/* The first inherited link can answer QUERY with invalid 0xff.
		 * A complete failed-probe teardown and one fresh initialization
		 * recovers it. No block devices exist at this point. Never retry
		 * a bound controller or bypass the remaining handoff checks.
		 * A failed HS change is retried once in the PWM gear.
		 */
		platform_device_unregister(pixel_pdev);
		pixel_pdev = NULL;
		if (!attempt++) {
			reprobe = true;
			if (hs_requested && !hs_done) {
				pr_info("pixel-ufs: HS change failed; retrying in the PWM gear\n");
				hs_gear = 0;
			} else {
				pr_info("pixel-ufs: retrying once after failed handoff initialization\n");
			}
			goto retry;
		}
		ret = -ENODEV;
		goto unregister;
	}
	platform_device_put(pixel_pdev);
unregister:
	platform_driver_unregister(&pixel_driver);
	return ret;
}

static void __exit pixel_ufs_exit(void)
{
	platform_device_unregister(pixel_pdev);
	platform_driver_unregister(&pixel_driver);
}
module_init(pixel_ufs_init);
module_exit(pixel_ufs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GS201 UFS bring-up on retained PHY and clocks");
