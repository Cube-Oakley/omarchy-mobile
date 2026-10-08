// SPDX-License-Identifier: GPL-2.0-only
/* Test: the Pixel 7 Pro display pipeline through a DPU/DISP power cycle
 * (Stage 1 of docs/display-cold-init-plan-20261001.md), as a module so the
 * sequence can be tried on the running phone without a new boot image.
 *
 * With the screen off (the panel asleep in DCS sleep, DECON idle with its
 * trigger masked), writing `cycle`:
 *   1  stops the DSIM into ULPS and soft-resets DECON (no power cycle);
 *   2  also isolates the DSIM PHY and powers DPU, then DISP, off for hold_ms.
 * Both then bring the DSIM up as stock's dsim_reg_init() and
 * dsim_reg_exit_ulps_and_start() do (SM DDI, command mode), with every value
 * taken from the registers read just before, write back the DECON, window,
 * DSC, DMA and DPP configuration, and log each register in pixel-disp-regs.h
 * that reads back differently. The scanout driver's normal screen-on (panel
 * wake, display on, first frame) follows from userspace.
 *
 * Sources: Google's display modules (kernel/google-modules/display @
 * 0137241), samsung/cal_9845/dsim_reg.c (dsim_reg_init :2080-2120,
 * dsim_reg_stop_and_enter_ulps :2200-2232, dsim_reg_set_smddi_ulps
 * :1959-2000), and the stock genpd sequence pixel-pd-off and pixel-sleep
 * already run. The domain save lists are pixel-pd-off's (kernel/pd), which
 * the running pixel-pd-off predates; this module carries its own copy of
 * the on/off sequence so the camera's saved domains are not disturbed.
 *
 * Every block access is gated on DPU and DISP reading powered. Only vendor
 * defined registers are touched; S2MPU CTRL0 only.
 */
#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/string.h>

enum { BLK_DSIM, BLK_PHY, BLK_BIAS, BLK_SYSREG, BLK_DECON, BLK_WIN, BLK_SUB, BLK_WINCON,
	BLK_DMA, BLK_DPP, BLK_PMU, NBLK };

static const struct {
	phys_addr_t pa;
	size_t len;
} blocks[NBLK] = {
	[BLK_DSIM] = { 0x1c2c0000, 0x300 },
	[BLK_PHY] = { 0x1c2e0100, 0x700 },	/* the DSIM node's second region */
	[BLK_BIAS] = { 0x1c2e0000, 0x100 },
	[BLK_SYSREG] = { 0x1c221000, 0x100 },	/* SYSREG_DISP + 0x1000 (disp_ss) */
	[BLK_DECON] = { 0x1c240000, 0x6000 },
	[BLK_WIN] = { 0x1c250000, 0x6000 },
	[BLK_SUB] = { 0x1c260000, 0xa000 },
	[BLK_WINCON] = { 0x1c270000, 0x6000 },
	[BLK_DMA] = { 0x1c0b0000, 0x1000 },
	[BLK_DPP] = { 0x1c0d0000, 0x1000 },
	[BLK_PMU] = { 0x18060000, 0x4000 },
};
static void __iomem *va[NBLK];
#define R(b, o)	(va[BLK_##b] + (o))

struct disp_reg {
	u8 blk;
	u16 off;
	const char *name;
};
#include "pixel-disp-regs.h"
static u32 before[ARRAY_SIZE(disp_regs)];

/* Registers that report or trigger rather than configure: read, compared
 * loosely, never written back. */
static bool status_reg(const char *name)
{
	static const char *const words[] = {
		"VERSION", "STATUS", "INTSRC", "INT_PEND", "FRAME_COUNT", "HW_TE_CNT",
		"TIMESTAMP_VAL", "SHD_REG_UP_REQ", "SWRST", "CMD_CONFIG", "DEBUG",
		"PLL_STAT", "CRC", "BIST", "_DBG_", "OF_LEVEL", "LAT_MON",
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(words); i++)
		if (strstr(name, words[i]))
			return true;
	return false;
}

static u32 saved(u8 blk, u16 off)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(disp_regs); i++)
		if (disp_regs[i].blk == blk && disp_regs[i].off == off)
			return before[i];
	pr_err("pixel-disp-cycle: register %u:%#x not in the list\n", blk, off);
	return 0;
}

/* ---- power domains (pixel-pd-off's sequence, kept here for the test) ---- */

#define GS201_SMC_PRIV_REG	0x82000504
#define GS201_SMC_PD_CONTEXT	0x82000410
#define PMUREG_WRITE		1
#define PMU_DSIM_PHY_ISO	0x3eb8

struct pd_reg {
	u32 pa;
	const char *name;
	bool wait;
	u32 mask, want;
};
#include "../pd/pixel-pd-lists.h"

static struct {
	const char *name;
	u32 pmu, cmu, tzpc;
	const struct pd_reg *save;
	unsigned int nsave;
	u32 *vals;
	u32 s2mpu[3], s2mpu_ctrl0[3];
} pds[2];
static u32 dpu_vals[ARRAY_SIZE(dpu_save)], disp_vals[ARRAY_SIZE(disp_save)];

static u32 reg_rw(u32 pa, bool write, u32 val)
{
	void __iomem *r = ioremap(pa, 4);

	if (!r)
		return 0;
	if (write)
		writel(val, r);
	else
		val = readl(r);
	iounmap(r);
	return val;
}

static bool pd_on(unsigned int i)
{
	return readl(va[BLK_PMU] + pds[i].pmu + 4) & BIT(0);
}

static bool display_powered(void)
{
	return pd_on(0) && pd_on(1);
}

static int pmu_write(u32 off, u32 val)
{
	struct arm_smccc_res res;

	arm_smccc_smc(GS201_SMC_PRIV_REG, blocks[BLK_PMU].pa + off, PMUREG_WRITE, val, 0, 0, 0, 0, &res);
	if (res.a0 || readl(va[BLK_PMU] + off) != val) {
		pr_err("pixel-disp-cycle: PMU %#x write %#x failed (%ld, reads %#x)\n", off, val,
		       (long)res.a0, readl(va[BLK_PMU] + off));
		return -EIO;
	}
	return 0;
}

static int domain_off(unsigned int i)
{
	struct arm_smccc_res res;
	unsigned int k;
	u32 val;
	int ret;

	for (k = 0; k < pds[i].nsave; k++)
		if (!pds[i].save[k].wait)
			pds[i].vals[k] = reg_rw(pds[i].save[k].pa, false, 0);
	for (k = 0; k < 3; k++)
		if (pds[i].s2mpu[k])
			pds[i].s2mpu_ctrl0[k] = reg_rw(pds[i].s2mpu[k], false, 0);
	arm_smccc_smc(GS201_SMC_PD_CONTEXT, 0, pds[i].tzpc, 2, 0, 0, 0, 0, &res);
	if (res.a0)
		return -EIO;
	reg_rw(pds[i].cmu + 0x800, true, reg_rw(pds[i].cmu + 0x800, false, 0) & ~BIT(24));
	if (pmu_write(pds[i].pmu, readl(va[BLK_PMU] + pds[i].pmu) & ~BIT(0)))
		return -EIO;
	ret = readl_poll_timeout(va[BLK_PMU] + pds[i].pmu + 4, val, !(val & BIT(0)), 10, 100000);
	pr_info("pixel-disp-cycle: %s %s\n", pds[i].name, ret ? "did not power off" : "off");
	return ret;
}

static int domain_on(unsigned int i)
{
	struct arm_smccc_res res;
	unsigned int k, tries;
	u32 val;
	int ret;

	if (pmu_write(pds[i].pmu, readl(va[BLK_PMU] + pds[i].pmu) | BIT(0)))
		return -EIO;
	ret = readl_poll_timeout(va[BLK_PMU] + pds[i].pmu + 4, val, val & BIT(0), 10, 100000);
	if (ret) {
		pr_err("pixel-disp-cycle: %s did not power on\n", pds[i].name);
		return ret;
	}
	arm_smccc_smc(GS201_SMC_PD_CONTEXT, 1, pds[i].tzpc, 2, 0, 0, 0, 0, &res);
	if (res.a0) {
		pr_err("pixel-disp-cycle: %s context restore failed %#lx\n", pds[i].name, res.a0);
		return -EIO;
	}
	for (k = 0; k < pds[i].nsave; k++) {
		const struct pd_reg *r = &pds[i].save[k];

		if (!r->wait) {
			reg_rw(r->pa, true, pds[i].vals[k]);
			continue;
		}
		for (tries = 0; tries < 1000; tries++) {
			if ((reg_rw(r->pa, false, 0) & r->mask) == r->want)
				break;
			udelay(10);
		}
		if (tries == 1000)
			pr_warn("pixel-disp-cycle: %s: %s still busy\n", pds[i].name, r->name);
	}
	for (k = 0; k < 3; k++) {
		if (!pds[i].s2mpu[k])
			continue;
		val = reg_rw(pds[i].s2mpu[k], false, 0);
		if (val != pds[i].s2mpu_ctrl0[k])
			reg_rw(pds[i].s2mpu[k], true, pds[i].s2mpu_ctrl0[k]);
		pr_info("pixel-disp-cycle: %s S2MPU %#x CTRL0 %#x -> %#x\n", pds[i].name,
			pds[i].s2mpu[k], val, reg_rw(pds[i].s2mpu[k], false, 0));
	}
	pr_info("pixel-disp-cycle: %s on, %u registers restored\n", pds[i].name, pds[i].nsave);
	return 0;
}

/* ---- DSIM (cal_9845/dsim_reg.c, command mode, SM DDI, four lanes) ---- */

#define DSIM_SWRST		0x04
#define DSIM_DPHY_STATUS	0x1c
#define DSIM_CLK_CTRL		0x20
#define DSIM_ESCMODE		0x2c
#define DSIM_INTSRC		0x50
#define DSIM_INTMSK		0x54
#define DSIM_SFR_CTRL		0x64
#define DSIM_CMD_CONFIG		0x80
#define CLK_SEL_WORD		BIT(26)
#define CLK_NONCONT		BIT(25)
#define CLK_TX_HSCLK		BIT(20)
#define CLK_WORDCLK_EN		BIT(17)
#define CLK_ESCCLK_EN		BIT(16)
#define CLK_LANE_ESC		(0x1f << 8)
#define ESC_ULPS_ENTER		(BIT(3) | BIT(1))	/* data, clock */
#define ESC_ULPS_EXIT		(BIT(2) | BIT(0))
#define DPHY_ULPS_CLK		BIT(9)
#define DPHY_STOP_CLK		BIT(8)
#define INT_STOCK_MASK		(BIT(30) | BIT(29) | BIT(28) | BIT(24) | BIT(23) | BIT(19) | BIT(18) | BIT(15))
#define PHY_PLL_CON0		0x00
#define PHY_PLL_EN		BIT(12)
#define PHY_PLL_STAT0		0x40
#define PHY_MC_GNR_CON0		0x200
#define PHY_MD_GNR_CON0(i)	(0x300 + 0x100 * (i))
#define PHY_READY		BIT(1)
#define SYS_MIPI_PHY_CON	0x008
#define SYS_M_RESETN_M0		BIT(0)
#define SYS_SEL_RESET_DPHY	BIT(4)
#define SFR_SHADOW_READ		BIT(1)

/* Written after SFR_CTRL's shadow enable, as dsim_reg_set_config() does:
 * CONFIG (DSC enable), CPRS_CTRL, SLICE01/23, CMD_TE_CTRL0/1. */
static const u16 shadowed[] = { 0x4c, 0x74, 0x78, 0x7c, 0x84, 0x88 };

static bool after_shadow(u16 off)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(shadowed); i++)
		if (shadowed[i] == off)
			return true;
	return false;
}

static void rmw(void __iomem *r, u32 clear, u32 set)
{
	writel((readl(r) & ~clear) | set, r);
}

static int poll(void __iomem *r, u32 mask, u32 want, const char *what)
{
	u32 val;
	int ret = readl_poll_timeout_atomic(r, val, (val & mask) == want, 10, 2000);

	if (ret)
		pr_err("pixel-disp-cycle: timeout waiting for %s (reads %#x)\n", what, val);
	return ret;
}

static int sw_reset(void)
{
	rmw(R(DSIM, DSIM_SWRST), 0, BIT(0));
	return poll(R(DSIM, DSIM_SWRST), BIT(0), 0, "DSIM SW reset");
}

static int phy_lanes(bool on)
{
	u32 v = on ? ~0U : 0;
	int i, ret = 0;

	writel(v, R(PHY, PHY_MC_GNR_CON0));
	for (i = 0; i < 4; i++)
		writel(v, R(PHY, PHY_MD_GNR_CON0(i)));
	ret |= poll(R(PHY, PHY_MC_GNR_CON0), PHY_READY, on ? PHY_READY : 0, "clock lane ready");
	for (i = 0; i < 4; i++)
		ret |= poll(R(PHY, PHY_MD_GNR_CON0(i)), PHY_READY, on ? PHY_READY : 0, "data lane ready");
	return ret ? -ETIMEDOUT : 0;
}

/* dsim_reg_stop_and_enter_ulps() for an SM DDI. */
static int dsim_down(void)
{
	int ret = 0;

	writel(~0U, R(DSIM, DSIM_INTSRC));
	rmw(R(DSIM, DSIM_INTMSK), 0, INT_STOCK_MASK);
	rmw(R(DSIM, DSIM_CLK_CTRL), CLK_TX_HSCLK, 0);
	ret |= poll(R(DSIM, DSIM_DPHY_STATUS), DPHY_STOP_CLK, DPHY_STOP_CLK, "clock lane stop state");
	rmw(R(DSIM, DSIM_ESCMODE), 0, ESC_ULPS_ENTER);
	ret |= poll(R(DSIM, DSIM_DPHY_STATUS), DPHY_ULPS_CLK | 0xf0, DPHY_ULPS_CLK | 0xf0, "ULPS entry");
	rmw(R(DSIM, DSIM_ESCMODE), ESC_ULPS_ENTER, 0);
	rmw(R(DSIM, DSIM_CLK_CTRL), CLK_SEL_WORD, 0);
	ret |= phy_lanes(false);
	rmw(R(DSIM, DSIM_CLK_CTRL), CLK_ESCCLK_EN | 0xff, 0xff);
	rmw(R(PHY, PHY_PLL_CON0), PHY_PLL_EN, 0);
	ret |= poll(R(PHY, PHY_PLL_STAT0), BIT(0), 0, "PLL unlock");
	ret |= sw_reset();
	return ret ? -EIO : 0;
}

/* dsim_reg_init() + dsim_reg_exit_ulps_and_start() for an SM DDI, with the
 * values read before (the bootloader's, which our driver then adjusts). */
static int dsim_up(void)
{
	u32 clk = saved(BLK_DSIM, DSIM_CLK_CTRL);
	unsigned int i, k;
	int ret = 0;

	rmw(R(SYSREG, SYS_MIPI_PHY_CON), SYS_SEL_RESET_DPHY, 0);
	rmw(R(DSIM, DSIM_CLK_CTRL), CLK_SEL_WORD, 0);
	ret |= sw_reset();
	rmw(R(DSIM, 0x4c), 0, 0x1f);				/* CONFIG lanes enable */
	rmw(R(DSIM, DSIM_CLK_CTRL), CLK_LANE_ESC, CLK_LANE_ESC);
	rmw(R(DSIM, DSIM_CLK_CTRL), 0, CLK_WORDCLK_EN);
	rmw(R(SYSREG, SYS_MIPI_PHY_CON), SYS_M_RESETN_M0, 0);	/* DPHY reset */

	if (readl(R(PHY, PHY_PLL_STAT0)) & BIT(0))
		pr_warn("pixel-disp-cycle: PLL still locked before init\n");
	for (i = 0; i < 5; i++)
		writel(saved(BLK_BIAS, 4 * i), R(BIAS, 4 * i));
	for (i = 0; i < 9; i++)
		writel(saved(BLK_PHY, 4 * i) & (i ? ~0U : ~PHY_PLL_EN), R(PHY, 4 * i));
	rmw(R(DSIM, DSIM_CLK_CTRL), CLK_ESCCLK_EN | 0xff, CLK_ESCCLK_EN | (clk & 0xff));
	for (i = 0; i < 5; i++)
		writel(saved(BLK_PHY, 0x230 + 4 * i), R(PHY, 0x230 + 4 * i));
	writel(saved(BLK_PHY, 0x250), R(PHY, 0x250));
	for (k = 0; k < 4; k++)
		for (i = 0; i < 5; i++)
			writel(saved(BLK_PHY, 0x330 + 0x100 * k + 4 * i), R(PHY, 0x330 + 0x100 * k + 4 * i));
	writel(0, R(PHY, PHY_MC_GNR_CON0));
	writel(saved(BLK_PHY, 0x204), R(PHY, 0x204));
	writel(saved(BLK_PHY, 0x208), R(PHY, 0x208));
	writel(saved(BLK_PHY, 0x20c), R(PHY, 0x20c));
	for (k = 0; k < 4; k++) {
		writel(0, R(PHY, PHY_MD_GNR_CON0(k)));
		writel(saved(BLK_PHY, 0x304 + 0x100 * k), R(PHY, 0x304 + 0x100 * k));
		for (i = 0; i < 4; i++)
			writel(saved(BLK_PHY, 0x308 + 0x100 * k + 4 * i), R(PHY, 0x308 + 0x100 * k + 4 * i));
	}
	writel(BIT(31), R(DSIM, DSIM_INTSRC));			/* PLL_STABLE */
	rmw(R(PHY, PHY_PLL_CON0), 0, PHY_PLL_EN);
	if (!(readl(R(PHY, PHY_PLL_CON0)) & PHY_PLL_EN)) {
		pr_err("pixel-disp-cycle: PLL_EN does not read back\n");
		return -EIO;
	}
	ret |= poll(R(PHY, PHY_PLL_STAT0), BIT(0), BIT(0), "PLL lock");
	ret |= phy_lanes(true);
	rmw(R(SYSREG, SYS_MIPI_PHY_CON), 0, SYS_M_RESETN_M0);	/* release */
	rmw(R(DSIM, DSIM_CLK_CTRL), 0, CLK_SEL_WORD);
	if (ret)
		return -EIO;

	/* dsim_reg_set_config(), in its order: shadow reads on, the link
	 * registers, shadow on, then CONFIG (video mode off, DSC on), the
	 * compression and slice registers, packet-go off and the command timers,
	 * which only take through the shadow. */
	writel(SFR_SHADOW_READ, R(DSIM, DSIM_SFR_CTRL));
	for (i = 0; i < ARRAY_SIZE(disp_regs); i++) {
		const struct disp_reg *r = &disp_regs[i];

		if (r->blk != BLK_DSIM || status_reg(r->name) || after_shadow(r->off) ||
		    r->off == DSIM_CLK_CTRL || r->off == DSIM_ESCMODE ||
		    r->off == DSIM_SFR_CTRL || r->off == DSIM_INTMSK)
			continue;
		writel(before[i], va[BLK_DSIM] + r->off);
	}
	writel(saved(BLK_DSIM, DSIM_ESCMODE) & ~(ESC_ULPS_ENTER | ESC_ULPS_EXIT), R(DSIM, DSIM_ESCMODE));
	rmw(R(DSIM, DSIM_CLK_CTRL), CLK_NONCONT, clk & CLK_NONCONT);
	writel(saved(BLK_DSIM, DSIM_SFR_CTRL), R(DSIM, DSIM_SFR_CTRL));
	for (i = 0; i < ARRAY_SIZE(shadowed); i++)
		writel(saved(BLK_DSIM, shadowed[i]), R(DSIM, shadowed[i]));
	writel(0, R(DSIM, DSIM_CMD_CONFIG));

	/* dsim_reg_set_smddi_ulps(exit): enter again, then exit. */
	rmw(R(DSIM, DSIM_ESCMODE), 0, ESC_ULPS_ENTER);
	ret |= poll(R(DSIM, DSIM_DPHY_STATUS), DPHY_ULPS_CLK | 0xf0, DPHY_ULPS_CLK | 0xf0, "ULPS re-entry");
	rmw(R(DSIM, DSIM_ESCMODE), 0, ESC_ULPS_EXIT);
	ret |= poll(R(DSIM, DSIM_DPHY_STATUS), DPHY_ULPS_CLK | 0xf0, 0, "ULPS exit");
	udelay(100);
	rmw(R(DSIM, DSIM_ESCMODE), ESC_ULPS_ENTER, 0);
	rmw(R(DSIM, DSIM_ESCMODE), ESC_ULPS_EXIT, 0);

	/* dsim_reg_start(): HS clock (non-continuous: no wait), interrupts. */
	rmw(R(DSIM, DSIM_CLK_CTRL), 0, clk & CLK_TX_HSCLK);
	writel(saved(BLK_DSIM, DSIM_INTMSK), R(DSIM, DSIM_INTMSK));
	writel(~0U, R(DSIM, DSIM_INTSRC));
	return ret ? -EIO : 0;
}

/* ---- DECON, windows, DSC, DMA, DPP: configuration as it was ---- */

#define DECON_GLOBAL_CON	0x20
#define GLOBAL_SRESET		BIT(28)
#define GLOBAL_LIVE		(BIT(0) | BIT(1) | BIT(4) | BIT(5) | GLOBAL_SRESET)

static void pipeline_restore(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(disp_regs); i++) {
		const struct disp_reg *r = &disp_regs[i];
		u32 v = before[i];

		if (r->blk == BLK_DSIM || r->blk == BLK_PHY || r->blk == BLK_BIAS || status_reg(r->name))
			continue;
		if (r->blk == BLK_DECON && r->off == DECON_GLOBAL_CON)
			v &= ~GLOBAL_LIVE;
		writel(v, va[r->blk] + r->off);
	}
}

/* dpp_reg_init(): a DMA that deadlocked (a frame that never finished) is
 * soft-reset before it is configured again. */
static bool reset_dma;
module_param(reset_dma, bool, 0644);
MODULE_PARM_DESC(reset_dma, "Test: soft-reset the DMA and DPP in cycle 1 even without a deadlock");

static int dma_reset(void)
{
	int ret;

	if (!reset_dma && !(readl(R(DMA, 0x4)) & BIT(17)))
		return 0;
	pr_info("pixel-disp-cycle: DMA soft reset (RDMA_IRQ %#x)\n", readl(R(DMA, 0x4)));
	rmw(R(DMA, 0x0), 0, BIT(8));
	ret = poll(R(DMA, 0x0), BIT(8), 0, "DMA soft reset");
	if (reset_dma) {
		rmw(R(DPP, 0x4), 0, BIT(0));
		ret |= poll(R(DPP, 0x4), BIT(0), 0, "DPP soft reset");
	}
	return ret;
}

static int decon_reset(void)
{
	rmw(R(DECON, DECON_GLOBAL_CON), 0, GLOBAL_SRESET);
	return poll(R(DECON, DECON_GLOBAL_CON), GLOBAL_SRESET, 0, "DECON soft reset");
}

/* ---- the test ---- */

static DEFINE_MUTEX(cycle_lock);
static bool force_dsc;
module_param(force_dsc, bool, 0644);
MODULE_PARM_DESC(force_dsc, "Test: take DSIM CONFIG's DSC enable as set (after a failed re-init lost it)");
static bool force_busy;
module_param(force_busy, bool, 0644);
MODULE_PARM_DESC(force_busy, "Test: cycle even with DECON stuck mid-frame (scanout driver failed closed)");
static int irq_start = -1, irq_done = -1;
static unsigned int hold_ms = 2000;
module_param(hold_ms, uint, 0644);
MODULE_PARM_DESC(hold_ms, "How long the domains stay off in cycle 2");

static void read_all(u32 *out)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(disp_regs); i++)
		out[i] = readl(va[disp_regs[i].blk] + disp_regs[i].off);
}

static unsigned int compare(void)
{
	static u32 after[ARRAY_SIZE(disp_regs)];
	unsigned int i, diffs = 0;

	read_all(after);
	for (i = 0; i < ARRAY_SIZE(disp_regs); i++) {
		if (after[i] == before[i])
			continue;
		pr_info("pixel-disp-cycle: %s%s %#010x -> %#010x\n", disp_regs[i].name,
			status_reg(disp_regs[i].name) ? " (status)" : "", before[i], after[i]);
		if (!status_reg(disp_regs[i].name))
			diffs++;
	}
	return diffs;
}

static int run_cycle(unsigned int kind)
{
	u32 global, trig;
	int ret;

	if (!display_powered())
		return -ENODEV;
	global = readl(R(DECON, DECON_GLOBAL_CON));
	trig = readl(R(DECON, 0x30));
	/* Idle with the trigger masked, as pixel_transfer() leaves it (DECON_EN
	 * and RUN_STATUS stay set in command mode): nothing is transferring. */
	if (!force_busy &&
	    (!(trig & BIT(4)) || !(global & BIT(5)) || readl(R(DSIM, DSIM_CMD_CONFIG)))) {
		pr_err("pixel-disp-cycle: display busy (GLOBAL_CON %#x TRIG_CON %#x); turn the screen off\n",
		       global, trig);
		return -EBUSY;
	}
	read_all(before);
	if (force_dsc) {
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(disp_regs); i++)
			if (disp_regs[i].blk == BLK_DSIM && disp_regs[i].off == 0x4c)
				before[i] |= BIT(19);
	}
	disable_irq(irq_start);
	disable_irq(irq_done);
	ret = dsim_down();
	pr_info("pixel-disp-cycle: DSIM stopped in ULPS: %d\n", ret);
	if (kind == 1) {
		ret = ret ?: decon_reset();
		ret = ret ?: dma_reset();
	} else if (!ret) {
		ret = pmu_write(PMU_DSIM_PHY_ISO, readl(va[BLK_PMU] + PMU_DSIM_PHY_ISO) & ~BIT(0));
		ret = ret ?: domain_off(0);	/* DPU */
		ret = ret ?: domain_off(1);	/* DISP */
		if (!ret) {
			msleep(hold_ms);
			ret = domain_on(1) ?: domain_on(0);
		}
		if (!ret && !display_powered())
			ret = -EIO;
		ret = ret ?: pmu_write(PMU_DSIM_PHY_ISO, readl(va[BLK_PMU] + PMU_DSIM_PHY_ISO) | BIT(0));
	}
	if (ret) {
		pr_err("pixel-disp-cycle: stopped before re-init: %d (display needs a reboot)\n", ret);
		return ret;	/* IRQs stay off: the blocks may be unpowered */
	}
	ret = dsim_up();
	pr_info("pixel-disp-cycle: DSIM up: %d\n", ret);
	pipeline_restore();
	pr_info("pixel-disp-cycle: cycle %u done, %u configuration registers differ\n", kind, compare());
	enable_irq(irq_start);
	enable_irq(irq_done);
	return ret;
}

/* One frame of the buffer DMA already points at, as pixel_transfer() sends
 * it, polling DECON's frame-done pending bit with the scanout driver's IRQs
 * held off. For checking a re-init while that driver has failed closed. */
static int send_frame(void)
{
	u32 trigger, status, addr;
	int ret;

	if (!display_powered())
		return -ENODEV;
	trigger = readl(R(DECON, 0x30));
	if (!(trigger & BIT(4)))
		return -EBUSY;
	disable_irq(irq_start);
	disable_irq(irq_done);
	ret = readl_poll_timeout(R(DECON, DECON_GLOBAL_CON), status, status & BIT(5), 50, 100000);
	if (ret)
		goto out;
	addr = readl(R(DMA, 0x40));
	writel(BIT(12) | BIT(13), R(DECON, 0x70));
	writel(BIT(19), R(DSIM, DSIM_INTSRC));
	writel(readl(R(DECON, 0x50)) | 0x3f, R(DECON, 0x50));
	writel(readl(R(DECON, DECON_GLOBAL_CON)) | 3, R(DECON, DECON_GLOBAL_CON));
	writel(readl(R(DECON, 0x50)) | 0x80100000, R(DECON, 0x50));
	writel((trigger & ~BIT(4)) | 1, R(DECON, 0x30));
	ret = readl_poll_timeout(R(DECON, 0x70), status, status & BIT(13), 100, 200000);
	writel(trigger, R(DECON, 0x30));
	pr_info("pixel-disp-cycle: frame %s: INT_PEND %#x DSIM INTSRC %#x underrun %u shadow DMA %#x (%#x)\n",
		ret ? "TIMED OUT" : "done", readl(R(DECON, 0x70)), readl(R(DSIM, DSIM_INTSRC)),
		!!(readl(R(DSIM, DSIM_INTSRC)) & BIT(19)), readl(R(DMA, 0x440)), addr);
	writel(BIT(12) | BIT(13), R(DECON, 0x70));
	if (!ret)
		ret = readl_poll_timeout(R(DECON, DECON_GLOBAL_CON), status, status & BIT(5), 50, 100000);
out:
	enable_irq(irq_start);
	enable_irq(irq_done);
	return ret;
}

static int frame_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	mutex_lock(&cycle_lock);
	ret = send_frame();
	mutex_unlock(&cycle_lock);
	return ret;
}
static const struct kernel_param_ops frame_ops = { .set = frame_set };
module_param_cb(frame, &frame_ops, NULL, 0200);
MODULE_PARM_DESC(frame, "Send one frame of the current buffer (any value)");

static int cycle_set(const char *val, const struct kernel_param *kp)
{
	unsigned int kind;
	int ret;

	if (kstrtouint(val, 0, &kind) || (kind != 1 && kind != 2))
		return -EINVAL;
	mutex_lock(&cycle_lock);
	ret = run_cycle(kind);
	mutex_unlock(&cycle_lock);
	return ret;
}
static const struct kernel_param_ops cycle_ops = { .set = cycle_set };
module_param_cb(cycle, &cycle_ops, NULL, 0200);
MODULE_PARM_DESC(cycle, "1: DSIM stop/start and DECON reset; 2: also a DPU/DISP power cycle");

static void unmap_all(void)
{
	unsigned int i;

	for (i = 0; i < NBLK; i++)
		if (va[i])
			iounmap(va[i]);
}

static int __init pixel_disp_cycle_init(void)
{
	struct device_node *np;
	unsigned int i;

	if (!of_machine_is_compatible("google,GS201 CHEETAH"))
		return -ENODEV;
	for (i = 0; i < NBLK; i++) {
		va[i] = ioremap(blocks[i].pa, blocks[i].len);
		if (!va[i]) {
			unmap_all();
			return -ENOMEM;
		}
	}
	pds[0] = (typeof(pds[0])) { "dpu", 0x2200, 0x1c000000, 0x1c010204, dpu_save,
		ARRAY_SIZE(dpu_save), dpu_vals, { 0x1c160000, 0x1c170000, 0x1c180000 } };
	pds[1] = (typeof(pds[1])) { "disp", 0x2280, 0x1c200000, 0x1c210204, disp_save,
		ARRAY_SIZE(disp_save), disp_vals };
	if (!display_powered() || readl(R(DSIM, 0)) != 0x02060000) {
		unmap_all();
		return -ENODEV;
	}
	np = of_find_node_by_path("/drmdecon@0x1C240000");
	if (np) {
		irq_start = of_irq_get_byname(np, "frame_start");
		irq_done = of_irq_get_byname(np, "frame_done");
		of_node_put(np);
	}
	if (irq_start <= 0 || irq_done <= 0) {
		unmap_all();
		return -ENODEV;
	}
	pr_info("pixel-disp-cycle: ready, %zu registers checked\n", ARRAY_SIZE(disp_regs));
	return 0;
}
module_init(pixel_disp_cycle_init);

static void __exit pixel_disp_cycle_exit(void)
{
	unmap_all();
}
module_exit(pixel_disp_cycle_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test: Pixel display pipeline through a DPU/DISP power cycle");
