// SPDX-License-Identifier: GPL-2.0-only
/* GS201 platform layer for the Pixel 7 Pro GNSS receiver, a Broadcom BCM4776
 * (BCM4775x family) on PERIC0 USI5, under Google's bbd driver (bcm_gps_spi.c,
 * bbd.c). On stock, that driver takes its lines from gpiolib, their states
 * from pinctrl and its bus from the vendor spi-s3c64xx host; this kernel has
 * none of them for these pins. This sets the hardware up directly at probe,
 * and devm restores it after remove:
 *
 * - Supplies (was pixel-gnss-power.c): S2MPG13 LDO9S (GNSS core, 1.2 V),
 *   LDO10S (RF) and LDO11S (aux, both 1.8 V), switched on through ACPM in
 *   that order, about 1 ms apart, with the enable bit (7) Google's s2mpg13
 *   regulator driver uses, at the voltage the bootloader set. The stock DT
 *   keeps them always on; the bootloader leaves them off. A rail outside its
 *   stock DT range fails the probe. Only the rails switched on here are
 *   switched off again.
 * - Lines, in the stock DT's "default" pin state: nstandby (gph2-3) and
 *   mcu_req (gpp4-3) outputs, low (standby) until lhd raises them, set before
 *   the supplies come up; mcu_resp (gph2-2) an input with pull-down; host_req
 *   (gpa6-4) on its wake-up EINT function, no pull.
 * - host_req interrupt: each gpa6 pin has its own GIC line (SPI 44-51) in
 *   the gpa6 node. Pin 4's (SPI 48, level high) is requested directly, as
 *   pixel-gpio.ko owns pin 3's. The EINT is set to level high and unmasked
 *   once, at probe; afterwards the driver gates only the GIC line
 *   (enable_irq(), disable_irq_nosync()). The line follows the latched pend
 *   bit, so the handler clears it, and so does every enable: a level that has
 *   gone must not fire.
 * - SPI: USI5 in SPI mode; SCLK the fastest IPCLK/4 at or under spi_hz
 *   (24.96 MHz: PERIC0_IP 399.36 MHz / 4 / 4); mode 3 (the DT's spi-cpol and
 *   spi-cpha); the DT's feedback delay. Transfers are polled, 8-bit words, a
 *   64-byte FIFO load at a time; the chip select (gpp10-3) is a GPIO held low
 *   for the whole transfer, as the chip needs over frames of up to 8 KiB.
 *   The controller's own slave select is asserted per FIFO load (it only
 *   shifts then), as in pixel-spi.c; its pin is a GPIO, so nothing sees it.
 *
 * 8-bit words give the byte stream Google's host gave with 32-bit words.
 * spi-s3c64xx shifts each FIFO word most significant bit first; a 32-bit word
 * loaded from a little-endian buffer would put byte 3 first, which is why
 * the stock DT has swap-mode = <1> (TX/RX byte and halfword swap, restoring
 * memory order). Mainline spi-s3c64xx always writes SWAP_CFG 0, and with
 * 8-bit words every byte goes out in buffer order, MSB first: the same
 * wire order. So SWAP_CFG stays 0 here.
 *
 * Probe refuses a handoff that differs from what this expects: the stock DT
 * node (compatible, chip select, mode, the four lines' banks and pins, the
 * bus's FIFO size and USI offset), the clock chain, USI5 unconfigured (or
 * already in SPI mode) and each pin an input or already on its function.
 * Only registers the vendor sources define are touched, and the SPI block
 * only after USI5 is in SPI mode (pixel-spi.c's order).
 *
 * Shared banks: gph2 pins 0, 1, 4 and 5 are pixel-pcie.ko's (Wi-Fi), gpa6
 * pin 3's EINT is pixel-gpio.ko's. Only this module's own fields and bits
 * are changed, by read-modify-write under this module's lock; the other
 * modules have their own, so a write here can race one of theirs to the
 * same register. That is limited to probe and remove, except gph2's DAT
 * register, which also changes when lhd switches nstandby.
 *
 * Layouts from pixel-spi.c (USI7, the same controller), pixel-gpio.c (banks,
 * wake-up EINTs) and pixel-pcie.c (gph2); CMU and sysreg offsets from
 * Google's gs201 cmucal-sfr.c; LDO registers from s2mpg13-register.h.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>

#include "pixel-gnss-hw.h"

static unsigned int spi_hz = 24960000;
module_param(spi_hz, uint, 0444);
MODULE_PARM_DESC(spi_hz, "SPI clock ceiling in Hz: the fastest IPCLK/4 at or under it, at most the DT's spi-max-frequency");
static int fb_delay = -1;
module_param(fb_delay, int, 0444);
MODULE_PARM_DESC(fb_delay, "SPI RX feedback delay 0-3 (-1: the DT's samsung,spi-feedback-delay, else 1)");

/* Supplies: ACPM PMIC channel 2, S2MPG13, PM bank */
#define ACPM_PMIC_CHANNEL	2
#define PMIC_SUB		1	/* S2MPG13 */
#define PM_BANK			0x01
#define VSEL			0x3f
#define LDO_EN			BIT(7)

struct rail {
	const char *name;
	u8 reg;
	u32 min_uv, step_uv;	/* voltage encoding of this LDO group */
	u32 lo_uv, hi_uv;	/* stock DT range */
};

/* In the order they are switched on. */
static const struct rail rails[] = {
	{ "LDO9S (GNSS core)", 0x34, 725000, 12500, 725000, 1300000 },
	{ "LDO10S (GNSS RF)", 0x35, 700000, 25000, 1600000, 1950000 },
	{ "LDO11S (GNSS aux)", 0x36, 700000, 25000, 1600000, 1950000 },
};

/* SPI controller, as pixel-spi.c's USI7 (spi-s3c64xx.c gs101 layout) */
#define SPI_BASE		0x10940000
#define SPI_CH_CFG		0x00
#define SPI_MODE_CFG		0x08
#define SPI_CS_REG		0x0c
#define SPI_INT_EN		0x10
#define SPI_STATUS		0x14
#define SPI_TX_DATA		0x18
#define SPI_RX_DATA		0x1c
#define SPI_PACKET_CNT		0x20
#define SPI_PENDING_CLR		0x24
#define SPI_SWAP_CFG		0x28
#define SPI_FB_CLK		0x2c
#define USI_CON			0xc4
#define USI_OPTION		0xc8

#define CH_SW_RST		BIT(5)
#define CH_CPOL			BIT(3)
#define CH_CPHA			BIT(2)
#define CH_RXCH_ON		BIT(1)
#define CH_TXCH_ON		BIT(0)
#define MODE_TRAILCNT		(0x3ff << 19)	/* and TSZ 0: 8-bit words */
#define CS_SIG_INACT		BIT(0)
#define ST_RX_LVL(v)		(((v) >> 15) & 0x1ff)
#define PACKET_CNT_EN		BIT(16)
#define PND_CLR_ALL		0x1f
#define FIFO_DEPTH		64	/* stock samsung,spi-fifosize */
#define FIFO_TIMEOUT_US		10000	/* a FIFO load is 1.4 ms at the slowest SCLK */
#define USI_CON_RESET		BIT(0)
#define USI_OPTION_CLKREQ_ON	BIT(1)
#define USI_OPTION_CLKSTOP_ON	BIT(2)

#define SYSREG_PERIC0		0x10821000
#define SW_CONF_USI5		0x10	/* stock samsung,usi-offset */
#define SW_CONF_SPI		BIT(1)

#define CMU_TOP			0x1e080000
#define TOP_PLL_SHARED2_CON3	0x1cc
#define TOP_PERIC0_IP_MUX	0x10e8
#define TOP_PERIC0_IP_DIV	0x18e4
#define PLL_SHARED2_798M	0xa0820400	/* enabled, M 130, P 4, S 0 */
#define MUX_SHARED2_DIV2	1
#define RATE_PERIC0_IP		399360000
#define RATE_OSCCLK		24576000
#define SCLK_MAX		26000000	/* stock spi-max-frequency */

#define CMU_PERIC0		0x10800000
#define USI5_MUX		0x690
#define USI5_DIV		0x1820
#define USI5_IPCLK_GATE		0x2088
#define USI5_PCLK_GATE		0x208c
#define MUX_SEL_USER		BIT(4)
#define CMU_BUSY		BIT(16)
#define GATE_MANUAL		BIT(20)
#define GATE_CG_VAL		BIT(21)
#define DIV_RATIO		0xf

/* Pin banks (pinctrl-gs201.c): CON, DAT, PUD, DRV; 4 bits a pin, 1 in DAT */
#define PIN_CON			0x0
#define PIN_DAT			0x4
#define PIN_PUD			0x8
#define PIN_DRV			0xc
#define CON_INPUT		0x0
#define CON_OUTPUT		0x1
#define CON_SPI			0x3
#define CON_EINT		0xf
#define PUD_NONE		0
#define PUD_DOWN		1
/* Wake-up EINTs of the ALIVE pin controller, plus the bank's EINT offset */
#define ECON			0x700	/* 4 bits a pin: 0 low, 1 high, 2-4 edges */
#define EMASK			0x900
#define EPEND			0xa00	/* write 1 to clear */
#define ECON_LEVEL_HIGH		1
#define GPA6_EINT		0x00

#define GPP10			0x140
#define GPP10_BUS		0x0fff	/* nibbles of SCLK, MOSI, MISO (pins 0-2) */
#define GPP10_BUS_SPI		0x0333

enum { PERIC0, HSI2, ALIVE, NR_PINCTRL };

static const phys_addr_t pinctrl_base[NR_PINCTRL] = {
	[PERIC0] = 0x10840000,
	[HSI2] = 0x14440000,
	[ALIVE] = 0x180e0000,
};

struct gnss_pin {
	const char *name;
	const char *prop;	/* naming it in the stock DT node */
	const char *bank_name;
	u8 ctrl;
	u16 bank;		/* CON offset in the pin controller */
	u8 pin;
	u8 con, pud;		/* stock "default" state */
};

static const struct gnss_pin pins[GNSS_NR_PINS] = {
	[GNSS_PIN_HOST_REQ] = { "host_req", "host-req-gpios", "gpa6", ALIVE, 0x000, 4, CON_EINT, PUD_NONE },
	[GNSS_PIN_MCU_REQ] = { "mcu_req", "mcu-req-gpios", "gpp4", PERIC0, 0x080, 3, CON_OUTPUT, PUD_NONE },
	[GNSS_PIN_MCU_RESP] = { "mcu_resp", "mcu-resp-gpios", "gph2", HSI2, 0x000, 2, CON_INPUT, PUD_DOWN },
	[GNSS_PIN_NSTANDBY] = { "nstandby", "nstandby-gpios", "gph2", HSI2, 0x000, 3, CON_OUTPUT, PUD_NONE },
	/* Stock puts it on the controller's slave select (function 3). */
	[GNSS_PIN_CS] = { "cs", NULL, "gpp10", PERIC0, GPP10, 3, CON_OUTPUT, PUD_NONE },
};

/* Set up (and restored) with the supplies; the others with the bus and EINT. */
static const int lines[] = { GNSS_PIN_NSTANDBY, GNSS_PIN_MCU_REQ, GNSS_PIN_MCU_RESP };

struct pixel_gnss_hw {
	struct device *dev;
	struct acpm_handle *acpm;
	void __iomem *pinctrl[NR_PINCTRL];
	void __iomem *regs;
	void __iomem *sysreg;
	void __iomem *cmu;
	spinlock_t lock;		/* this module's pin and CMU read-modify-writes */
	struct mutex xfer_lock;		/* one transfer at a time */
	int irq;
	u32 max_hz;			/* DT spi-max-frequency */
	u32 fb;				/* feedback delay */
	u32 speed;			/* SCLK */

	/* What probe changed, and the values it found, for the restore. */
	bool rail_on[ARRAY_SIZE(rails)];
	bool lines_set, clk_set, usi_set, bus_set, eint_set;
	u32 saved_con[GNSS_NR_PINS], saved_pud[GNSS_NR_PINS], saved_dat[GNSS_NR_PINS];
	u32 saved_cs_drv, saved_bus_con, saved_bus_pud, saved_bus_drv;
	u32 saved_econ, saved_emask;
	u32 saved_sw_conf, saved_mux, saved_div;
};

static void __iomem *pin_reg(struct pixel_gnss_hw *hw, int pin, u32 reg)
{
	return hw->pinctrl[pins[pin].ctrl] + pins[pin].bank + reg;
}

static u32 nibble(int pin)
{
	return 0xf << (4 * pins[pin].pin);
}

static void rmw(struct pixel_gnss_hw *hw, void __iomem *reg, u32 mask, u32 val)
{
	unsigned long flags;

	spin_lock_irqsave(&hw->lock, flags);
	writel((readl(reg) & ~mask) | (val & mask), reg);
	spin_unlock_irqrestore(&hw->lock, flags);
}

/* The pin's field of a CON, PUD or DRV register. */
static u32 pin_field_get(struct pixel_gnss_hw *hw, int pin, u32 reg)
{
	return (readl(pin_reg(hw, pin, reg)) >> (4 * pins[pin].pin)) & 0xf;
}

static void pin_field_set(struct pixel_gnss_hw *hw, int pin, u32 reg, u32 val)
{
	rmw(hw, pin_reg(hw, pin, reg), nibble(pin), val << (4 * pins[pin].pin));
}

static void pin_level(struct pixel_gnss_hw *hw, int pin, int value)
{
	u32 bit = BIT(pins[pin].pin);

	rmw(hw, pin_reg(hw, pin, PIN_DAT), bit, value ? bit : 0);
}

/* The state probe found: CON and PUD fields and the DAT bit. */
static void pin_save(struct pixel_gnss_hw *hw, int pin)
{
	hw->saved_con[pin] = readl(pin_reg(hw, pin, PIN_CON)) & nibble(pin);
	hw->saved_pud[pin] = readl(pin_reg(hw, pin, PIN_PUD)) & nibble(pin);
	hw->saved_dat[pin] = readl(pin_reg(hw, pin, PIN_DAT)) & BIT(pins[pin].pin);
}

static void pin_restore(struct pixel_gnss_hw *hw, int pin)
{
	rmw(hw, pin_reg(hw, pin, PIN_CON), nibble(pin), hw->saved_con[pin]);
	rmw(hw, pin_reg(hw, pin, PIN_PUD), nibble(pin), hw->saved_pud[pin]);
	rmw(hw, pin_reg(hw, pin, PIN_DAT), BIT(pins[pin].pin), hw->saved_dat[pin]);
}

int pixel_gnss_gpio_get(struct pixel_gnss_hw *hw, int pin)
{
	if (pin < 0 || pin >= GNSS_NR_PINS)
		return 0;
	return !!(readl(pin_reg(hw, pin, PIN_DAT)) & BIT(pins[pin].pin));
}

/* nstandby and mcu_req only, and only while they are set up. */
void pixel_gnss_gpio_set(struct pixel_gnss_hw *hw, int pin, int value)
{
	if ((pin != GNSS_PIN_NSTANDBY && pin != GNSS_PIN_MCU_REQ) || !hw->lines_set)
		return;
	pin_level(hw, pin, value);
}

/* Supplies */

static int rail_read(struct pixel_gnss_hw *hw, unsigned int i, u8 *val)
{
	return hw->acpm->ops->pmic.read_reg(hw->acpm, ACPM_PMIC_CHANNEL, PM_BANK,
					    rails[i].reg, PMIC_SUB, val);
}

static int rail_on(struct pixel_gnss_hw *hw, unsigned int i)
{
	const struct rail *r = &rails[i];
	u32 uv;
	u8 val;
	int ret;

	ret = rail_read(hw, i, &val);
	if (ret) {
		dev_err(hw->dev, "%s: ACPM read failed (%d)\n", r->name, ret);
		return ret;
	}
	uv = r->min_uv + r->step_uv * (val & VSEL);
	if (uv < r->lo_uv || uv > r->hi_uv) {
		dev_err(hw->dev, "%s at %u uV (CTRL %#04x), outside the stock range; left alone\n",
			r->name, uv, val);
		return -ERANGE;
	}
	if (val & LDO_EN) {
		dev_info(hw->dev, "%s already on at %u mV\n", r->name, uv / 1000);
		return 0;
	}
	ret = hw->acpm->ops->pmic.write_reg(hw->acpm, ACPM_PMIC_CHANNEL, PM_BANK, r->reg,
					    PMIC_SUB, val | LDO_EN);
	if (ret) {
		dev_err(hw->dev, "%s: ACPM write failed (%d)\n", r->name, ret);
		return ret;
	}
	hw->rail_on[i] = true;
	dev_info(hw->dev, "%s on at %u mV (CTRL was %#04x)\n", r->name, uv / 1000, val);
	usleep_range(1000, 2000);
	return 0;
}

static void rail_restore(struct pixel_gnss_hw *hw, unsigned int i)
{
	u8 val;

	if (!hw->rail_on[i])
		return;
	hw->rail_on[i] = false;
	if (rail_read(hw, i, &val) ||
	    hw->acpm->ops->pmic.write_reg(hw->acpm, ACPM_PMIC_CHANNEL, PM_BANK, rails[i].reg,
					  PMIC_SUB, val & ~LDO_EN))
		dev_err(hw->dev, "%s: could not switch it off\n", rails[i].name);
	else
		dev_info(hw->dev, "%s off\n", rails[i].name);
}

/* SPI */

static bool gate_on(u32 gate)
{
	return !(gate & GATE_MANUAL) || (gate & GATE_CG_VAL);
}

static int gnss_check_clocks(struct pixel_gnss_hw *hw)
{
	void __iomem *top = ioremap(CMU_TOP, 0x2000);
	u32 pll, mux, div, usi_mux, ipclk, pclk;

	if (!top)
		return -ENOMEM;
	pll = readl(top + TOP_PLL_SHARED2_CON3);
	mux = readl(top + TOP_PERIC0_IP_MUX) & 0x3;
	div = readl(top + TOP_PERIC0_IP_DIV) & DIV_RATIO;
	iounmap(top);
	usi_mux = readl(hw->cmu + USI5_MUX);
	ipclk = readl(hw->cmu + USI5_IPCLK_GATE);
	pclk = readl(hw->cmu + USI5_PCLK_GATE);
	if (pll != PLL_SHARED2_798M || mux != MUX_SHARED2_DIV2 || div ||
	    !gate_on(ipclk) || !gate_on(pclk)) {
		dev_err(hw->dev, "unexpected clocks: PLL_SHARED2 %#x PERIC0_IP mux %u div %u, USI5 mux %#x gates %#x/%#x\n",
			pll, mux, div, usi_mux, ipclk, pclk);
		return -EBUSY;
	}
	return 0;
}

/* Fastest SCLK (IPCLK/4) at or under hz; programs USI5's mux and divider. */
static int gnss_set_speed(struct pixel_gnss_hw *hw, u32 hz)
{
	u32 best = 0, mux = 0, div = 0, rate, d, v;
	unsigned int src;
	int ret;

	for (src = 0; src < 2; src++) {
		rate = src ? RATE_PERIC0_IP : RATE_OSCCLK;
		for (d = 0; d < 16; d++) {
			u32 sclk = rate / (d + 1) / 4;

			if (sclk <= hz && sclk > best) {
				best = sclk;
				mux = src ? MUX_SEL_USER : 0;
				div = d;
			}
		}
	}
	if (!best)
		return -EINVAL;
	rmw(hw, hw->cmu + USI5_MUX, MUX_SEL_USER, mux);
	ret = readl_poll_timeout_atomic(hw->cmu + USI5_MUX, v, !(v & CMU_BUSY), 1, 1000);
	if (!ret) {
		rmw(hw, hw->cmu + USI5_DIV, DIV_RATIO, div);
		ret = readl_poll_timeout_atomic(hw->cmu + USI5_DIV, v, !(v & CMU_BUSY), 1, 1000);
	}
	hw->speed = ret ? 0 : best;
	return ret;
}

static void gnss_spi_flush(struct pixel_gnss_hw *hw)
{
	u32 ch = readl(hw->regs + SPI_CH_CFG) & ~(CH_TXCH_ON | CH_RXCH_ON);

	writel(0, hw->regs + SPI_PACKET_CNT);
	writel(ch | CH_SW_RST, hw->regs + SPI_CH_CFG);
	writel(ch & ~CH_SW_RST, hw->regs + SPI_CH_CFG);
}

/*
 * One transfer, chip select low throughout: len bytes out of tx (zeros if
 * NULL) and into rx (if not NULL). Called from the rx/tx worker and debugfs,
 * which may sleep: it busy-polls one FIFO load (20 us at 24.96 MHz) at a time
 * and yields between loads, with the chip select held.
 */
int pixel_gnss_spi_xfer(struct pixel_gnss_hw *hw, const void *tx_buf, void *rx_buf,
			unsigned int len)
{
	const u8 *tx = tx_buf;
	u8 *rx = rx_buf;
	unsigned int done, n, i;
	u32 ch, st = 0;
	int ret = 0;

	mutex_lock(&hw->xfer_lock);
	if (!hw->bus_set) {
		mutex_unlock(&hw->xfer_lock);
		return -ENODEV;
	}
	pin_level(hw, GNSS_PIN_CS, 0);
	for (done = 0; done < len; done += n) {
		n = min_t(unsigned int, len - done, FIFO_DEPTH);
		gnss_spi_flush(hw);
		writel(PACKET_CNT_EN | n, hw->regs + SPI_PACKET_CNT);
		for (i = 0; i < n; i++)
			writel(tx ? tx[done + i] : 0, hw->regs + SPI_TX_DATA);
		ch = readl(hw->regs + SPI_CH_CFG);
		writel(ch | CH_TXCH_ON | CH_RXCH_ON, hw->regs + SPI_CH_CFG);
		writel(0, hw->regs + SPI_CS_REG);
		ret = readl_poll_timeout_atomic(hw->regs + SPI_STATUS, st, ST_RX_LVL(st) >= n,
						1, FIFO_TIMEOUT_US);
		for (i = 0; i < ST_RX_LVL(st) && i < n; i++) {
			u8 v = readl(hw->regs + SPI_RX_DATA);

			if (rx)
				rx[done + i] = v;
		}
		writel(CS_SIG_INACT, hw->regs + SPI_CS_REG);
		writel(ch & ~(CH_TXCH_ON | CH_RXCH_ON), hw->regs + SPI_CH_CFG);
		if (ret) {
			dev_err(hw->dev, "transfer timed out: status %#x, %u of %u bytes\n",
				st, done, len);
			gnss_spi_flush(hw);
			break;
		}
		cond_resched();
	}
	pin_level(hw, GNSS_PIN_CS, 1);
	mutex_unlock(&hw->xfer_lock);
	return ret;
}

static int gnss_spi_start(struct pixel_gnss_hw *hw)
{
	u32 hz = min(spi_hz, hw->max_hz), v;
	int cs = GNSS_PIN_CS, ret;
	void __iomem *bus = hw->pinctrl[PERIC0] + GPP10;

	if (spi_hz > hw->max_hz)
		dev_warn(hw->dev, "spi_hz %u is above the DT's spi-max-frequency %u\n",
			 spi_hz, hw->max_hz);

	/* Clock */
	hw->saved_mux = readl(hw->cmu + USI5_MUX) & MUX_SEL_USER;
	hw->saved_div = readl(hw->cmu + USI5_DIV) & DIV_RATIO;
	hw->clk_set = true;
	ret = gnss_set_speed(hw, hz);
	if (ret) {
		dev_err(hw->dev, "no USI5 clock at or under %u Hz (%d)\n", hz, ret);
		return ret;
	}

	/* USI: SPI mode, out of reset, clock requested continuously. */
	writel(SW_CONF_SPI, hw->sysreg + SW_CONF_USI5);
	hw->usi_set = true;
	v = readl(hw->regs + USI_CON);
	writel(v & ~USI_CON_RESET, hw->regs + USI_CON);
	udelay(1);
	v = readl(hw->regs + USI_OPTION);
	writel((v & ~USI_OPTION_CLKSTOP_ON) | USI_OPTION_CLKREQ_ON, hw->regs + USI_OPTION);

	/*
	 * Controller: polled, manual slave select, mode 3, 8-bit words, no
	 * swap. HS_EN stays off: spi-s3c64xx sets it only at 30 MHz and up,
	 * and never with CPHA.
	 */
	writel(CH_CPOL | CH_CPHA, hw->regs + SPI_CH_CFG);
	writel(0, hw->regs + SPI_INT_EN);
	writel(MODE_TRAILCNT, hw->regs + SPI_MODE_CFG);
	writel(0, hw->regs + SPI_PACKET_CNT);
	writel(PND_CLR_ALL, hw->regs + SPI_PENDING_CLR);
	writel(0, hw->regs + SPI_PENDING_CLR);
	writel(0, hw->regs + SPI_SWAP_CFG);
	writel(hw->fb, hw->regs + SPI_FB_CLK);
	writel(CS_SIG_INACT, hw->regs + SPI_CS_REG);
	gnss_spi_flush(hw);

	/* Pins: the chip select first, deselected; then the bus (SCLK idles high). */
	pin_save(hw, cs);
	hw->saved_cs_drv = readl(pin_reg(hw, cs, PIN_DRV)) & nibble(cs);
	hw->saved_bus_con = readl(bus + PIN_CON) & GPP10_BUS;
	hw->saved_bus_pud = readl(bus + PIN_PUD) & GPP10_BUS;
	hw->saved_bus_drv = readl(bus + PIN_DRV) & GPP10_BUS;
	hw->bus_set = true;
	pin_level(hw, cs, 1);
	pin_field_set(hw, cs, PIN_PUD, PUD_NONE);
	pin_field_set(hw, cs, PIN_DRV, 0);
	pin_field_set(hw, cs, PIN_CON, CON_OUTPUT);
	rmw(hw, bus + PIN_PUD, GPP10_BUS, 0);
	rmw(hw, bus + PIN_DRV, GPP10_BUS, 0);
	rmw(hw, bus + PIN_CON, GPP10_BUS, GPP10_BUS_SPI);
	return 0;
}

static void gnss_spi_restore(struct pixel_gnss_hw *hw)
{
	void __iomem *bus = hw->pinctrl[PERIC0] + GPP10;
	int cs = GNSS_PIN_CS;

	if (hw->bus_set) {
		hw->bus_set = false;
		rmw(hw, bus + PIN_CON, GPP10_BUS, hw->saved_bus_con);
		rmw(hw, bus + PIN_PUD, GPP10_BUS, hw->saved_bus_pud);
		rmw(hw, bus + PIN_DRV, GPP10_BUS, hw->saved_bus_drv);
		pin_restore(hw, cs);
		rmw(hw, pin_reg(hw, cs, PIN_DRV), nibble(cs), hw->saved_cs_drv);
	}
	if (hw->usi_set) {
		hw->usi_set = false;
		writel(readl(hw->regs + USI_CON) | USI_CON_RESET, hw->regs + USI_CON);
		writel(hw->saved_sw_conf, hw->sysreg + SW_CONF_USI5);
	}
	if (hw->clk_set) {
		hw->clk_set = false;
		rmw(hw, hw->cmu + USI5_DIV, DIV_RATIO, hw->saved_div);
		rmw(hw, hw->cmu + USI5_MUX, MUX_SEL_USER, hw->saved_mux);
		hw->speed = 0;
	}
}

/* host_req interrupt */

static void __iomem *eint_reg(struct pixel_gnss_hw *hw, u32 reg)
{
	return hw->pinctrl[ALIVE] + reg + GPA6_EINT;
}

void pixel_gnss_irq_ack(struct pixel_gnss_hw *hw)
{
	if (hw->eint_set)
		writel(BIT(pins[GNSS_PIN_HOST_REQ].pin), eint_reg(hw, EPEND));
}

void pixel_gnss_irq_enable(struct pixel_gnss_hw *hw)
{
	pixel_gnss_irq_ack(hw);
	enable_irq(hw->irq);
}

int pixel_gnss_irq(struct pixel_gnss_hw *hw)
{
	return hw->irq;
}

static void gnss_eint_start(struct pixel_gnss_hw *hw)
{
	int pin = GNSS_PIN_HOST_REQ;
	u32 bit = BIT(pins[pin].pin);

	hw->saved_emask = readl(eint_reg(hw, EMASK)) & bit;
	hw->saved_econ = readl(eint_reg(hw, ECON)) & nibble(pin);
	pin_save(hw, pin);
	hw->eint_set = true;
	rmw(hw, eint_reg(hw, EMASK), bit, bit);
	rmw(hw, eint_reg(hw, ECON), nibble(pin), ECON_LEVEL_HIGH << (4 * pins[pin].pin));
	pin_field_set(hw, pin, PIN_PUD, pins[pin].pud);
	pin_field_set(hw, pin, PIN_CON, CON_EINT);
	writel(bit, eint_reg(hw, EPEND));
	rmw(hw, eint_reg(hw, EMASK), bit, 0);
}

static void gnss_eint_restore(struct pixel_gnss_hw *hw)
{
	int pin = GNSS_PIN_HOST_REQ;
	u32 bit = BIT(pins[pin].pin);

	if (!hw->eint_set)
		return;
	rmw(hw, eint_reg(hw, EMASK), bit, bit);
	hw->eint_set = false;
	pin_restore(hw, pin);
	rmw(hw, eint_reg(hw, ECON), nibble(pin), hw->saved_econ);
	writel(bit, eint_reg(hw, EPEND));
	rmw(hw, eint_reg(hw, EMASK), bit, hw->saved_emask);
}

static int gnss_map_irq(struct pixel_gnss_hw *hw)
{
	struct device_node *np = of_find_node_by_path("/pinctrl@180E0000/gpa6");

	if (!np) {
		dev_err(hw->dev, "no /pinctrl@180E0000/gpa6 node\n");
		return -ENODEV;
	}
	/* The bank's interrupts list has one GIC line per pin. */
	hw->irq = irq_of_parse_and_map(np, pins[GNSS_PIN_HOST_REQ].pin);
	of_node_put(np);
	if (!hw->irq) {
		dev_err(hw->dev, "no GIC line for gpa6-4\n");
		return -ENXIO;
	}
	if (irq_get_trigger_type(hw->irq) != IRQ_TYPE_LEVEL_HIGH) {
		dev_err(hw->dev, "gpa6-4's GIC line is not level-high (type %#x)\n",
			irq_get_trigger_type(hw->irq));
		irq_dispose_mapping(hw->irq);
		hw->irq = 0;
		return -EINVAL;
	}
	return 0;
}

/* Probe and teardown */

static int gnss_check_dt_pin(struct pixel_gnss_hw *hw, struct device_node *np, int i)
{
	const struct gnss_pin *p = &pins[i];
	struct of_phandle_args args;
	struct device_node *ctrl;
	struct resource res = {};
	bool ok;

	if (of_parse_phandle_with_args(np, p->prop, "#gpio-cells", 0, &args)) {
		dev_err(hw->dev, "stock DT node has no %s\n", p->prop);
		return -ENODEV;
	}
	ctrl = of_get_parent(args.np);
	ok = of_node_name_eq(args.np, p->bank_name) && args.args_count >= 1 &&
	     args.args[0] == p->pin && ctrl && !of_address_to_resource(ctrl, 0, &res) &&
	     res.start == pinctrl_base[p->ctrl];
	if (!ok)
		dev_err(hw->dev, "stock DT %s is %pOFn-%u at %pa, not %s-%u at %pa\n",
			p->prop, args.np, args.args_count ? args.args[0] : 0, &res.start,
			p->bank_name, p->pin, &pinctrl_base[p->ctrl]);
	of_node_put(ctrl);
	of_node_put(args.np);
	return ok ? 0 : -ENODEV;
}

/* The stock DT describes this hardware; bail out if it says otherwise. */
static int gnss_check_dt(struct pixel_gnss_hw *hw)
{
	struct device_node *bus, *np, *cd;
	struct resource res = {};
	u32 reg = ~0, fifo = 0, usi = ~0;
	int i, ret = -ENODEV;

	bus = of_find_node_by_path("/spi@10940000");
	np = of_find_node_by_path("/spi@10940000/bcm4775@0");
	if (!bus || !np) {
		dev_err(hw->dev, "no stock DT node /spi@10940000/bcm4775@0\n");
		goto out;
	}
	if (of_address_to_resource(bus, 0, &res) || res.start != SPI_BASE ||
	    of_property_read_u32(bus, "samsung,spi-fifosize", &fifo) || fifo != FIFO_DEPTH ||
	    of_property_read_u32(bus, "samsung,usi-offset", &usi) || usi != SW_CONF_USI5) {
		dev_err(hw->dev, "spi@10940000: reg %pa, FIFO %u, USI offset %#x; expected a %u-byte FIFO at USI offset %#x\n",
			&res.start, fifo, usi, FIFO_DEPTH, SW_CONF_USI5);
		goto out;
	}
	if (!of_device_is_compatible(np, "ssp,bcm4775") ||
	    of_property_read_u32(np, "reg", &reg) || reg != 0 ||
	    !of_property_read_bool(np, "spi-cpol") || !of_property_read_bool(np, "spi-cpha")) {
		dev_err(hw->dev, "bcm4775@0 is not an ssp,bcm4775 on chip select 0 in SPI mode 3\n");
		goto out;
	}
	for (i = 0; i < GNSS_NR_PINS; i++) {
		if (pins[i].prop && gnss_check_dt_pin(hw, np, i))
			goto out;
	}
	hw->max_hz = SCLK_MAX;
	of_property_read_u32(np, "spi-max-frequency", &hw->max_hz);
	hw->fb = 1;
	cd = of_get_child_by_name(np, "controller-data");
	of_property_read_u32(cd, "samsung,spi-feedback-delay", &hw->fb);
	of_node_put(cd);
	ret = 0;
out:
	of_node_put(np);
	of_node_put(bus);
	return ret;
}

/* Each pin an input, or already on the function this sets. */
static int gnss_check_pins(struct pixel_gnss_hw *hw)
{
	u32 bus = readl(hw->pinctrl[PERIC0] + GPP10 + PIN_CON);
	int i;

	for (i = 0; i < GNSS_NR_PINS; i++) {
		u32 con = pin_field_get(hw, i, PIN_CON);

		if (con != CON_INPUT && con != pins[i].con &&
		    !(i == GNSS_PIN_CS && con == CON_SPI)) {
			dev_err(hw->dev, "%s (%s-%u) is on function %#x: in use?\n",
				pins[i].name, pins[i].bank_name, pins[i].pin, con);
			return -EBUSY;
		}
	}
	for (i = 0; i < 3; i++) {
		u32 con = (bus >> (4 * i)) & 0xf;

		if (con != CON_INPUT && con != CON_SPI) {
			dev_err(hw->dev, "gpp10-%d (SPI bus) is on function %#x: in use?\n",
				i, con);
			return -EBUSY;
		}
	}
	return 0;
}

static void gnss_teardown(struct pixel_gnss_hw *hw)
{
	unsigned int i;

	gnss_eint_restore(hw);
	gnss_spi_restore(hw);
	/* The chip to standby before its supplies go, its inputs after. */
	if (hw->lines_set) {
		pin_level(hw, GNSS_PIN_MCU_REQ, 0);
		pin_level(hw, GNSS_PIN_NSTANDBY, 0);
	}
	for (i = ARRAY_SIZE(rails); i--; )
		rail_restore(hw, i);
	if (hw->lines_set) {
		hw->lines_set = false;
		for (i = 0; i < ARRAY_SIZE(lines); i++)
			pin_restore(hw, lines[i]);
	}
}

/* devm: after the interrupt is freed. */
static void gnss_release(void *data)
{
	struct pixel_gnss_hw *hw = data;

	gnss_teardown(hw);
	irq_dispose_mapping(hw->irq);
}

/* System shutdown: as the bootloader left it; the interrupt stays mapped. */
void pixel_gnss_hw_shutdown(struct pixel_gnss_hw *hw)
{
	mutex_lock(&hw->xfer_lock);
	gnss_teardown(hw);
	mutex_unlock(&hw->xfer_lock);
}

struct pixel_gnss_hw *pixel_gnss_hw_init(struct device *dev)
{
	struct pixel_gnss_hw *hw;
	struct device_node *np;
	unsigned int i;
	int ret;

	hw = devm_kzalloc(dev, sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return ERR_PTR(-ENOMEM);
	hw->dev = dev;
	spin_lock_init(&hw->lock);
	ret = devm_mutex_init(dev, &hw->xfer_lock);
	if (ret)
		return ERR_PTR(ret);

	ret = gnss_check_dt(hw);
	if (ret)
		return ERR_PTR(ret);
	if (fb_delay > 3) {
		dev_err(dev, "fb_delay %d: 0-3, or -1 for the DT's\n", fb_delay);
		return ERR_PTR(-EINVAL);
	}
	if (fb_delay >= 0)
		hw->fb = fb_delay;
	hw->fb &= 0x3;

	hw->regs = devm_ioremap(dev, SPI_BASE, 0x1000);
	hw->sysreg = devm_ioremap(dev, SYSREG_PERIC0, 0x1000);
	hw->cmu = devm_ioremap(dev, CMU_PERIC0, 0x4000);
	if (!hw->regs || !hw->sysreg || !hw->cmu)
		return ERR_PTR(-ENOMEM);
	for (i = 0; i < NR_PINCTRL; i++) {
		hw->pinctrl[i] = devm_ioremap(dev, pinctrl_base[i], 0x1000);
		if (!hw->pinctrl[i])
			return ERR_PTR(-ENOMEM);
	}

	/* Handoff checks: reads only, and not of the SPI block yet. */
	ret = gnss_check_clocks(hw);
	if (ret)
		return ERR_PTR(ret);
	hw->saved_sw_conf = readl(hw->sysreg + SW_CONF_USI5);
	if (hw->saved_sw_conf & ~SW_CONF_SPI) {
		dev_err(dev, "USI5 is in use: SW_CONF %#x\n", hw->saved_sw_conf);
		return ERR_PTR(-EBUSY);
	}
	ret = gnss_check_pins(hw);
	if (ret)
		return ERR_PTR(ret);

	np = of_find_node_by_path("/power-management");
	hw->acpm = np ? devm_acpm_get_by_node(dev, np) : ERR_PTR(-ENODEV);
	of_node_put(np);
	if (IS_ERR(hw->acpm))
		return ERR_PTR(dev_err_probe(dev, PTR_ERR(hw->acpm), "no ACPM\n"));

	ret = gnss_map_irq(hw);
	if (ret)
		return ERR_PTR(ret);
	/* From here on, gnss_release() undoes whatever was set up. */
	ret = devm_add_action_or_reset(dev, gnss_release, hw);
	if (ret)
		return ERR_PTR(ret);

	/* The chip's inputs low (standby) before its supplies come up. */
	for (i = 0; i < ARRAY_SIZE(lines); i++)
		pin_save(hw, lines[i]);
	hw->lines_set = true;
	for (i = 0; i < ARRAY_SIZE(lines); i++) {
		const struct gnss_pin *p = &pins[lines[i]];

		if (p->con == CON_OUTPUT)
			pin_level(hw, lines[i], 0);
		pin_field_set(hw, lines[i], PIN_PUD, p->pud);
		pin_field_set(hw, lines[i], PIN_CON, p->con);
	}
	for (i = 0; i < ARRAY_SIZE(rails); i++) {
		ret = rail_on(hw, i);
		if (ret)
			return ERR_PTR(ret);
	}
	ret = gnss_spi_start(hw);
	if (ret)
		return ERR_PTR(ret);
	gnss_eint_start(hw);

	dev_info(dev, "chip in standby; SPI %u Hz, mode 3, feedback delay %u; host_req GIC hwirq %lu (irq %d)\n",
		 hw->speed, hw->fb, irqd_to_hwirq(irq_get_irq_data(hw->irq)), hw->irq);
	return hw;
}

void pixel_gnss_hw_show(struct pixel_gnss_hw *hw, struct seq_file *s)
{
	int host_req = GNSS_PIN_HOST_REQ;
	u32 bit = BIT(pins[host_req].pin);
	unsigned int i;
	u8 val;

	for (i = 0; i < GNSS_NR_PINS; i++)
		seq_printf(s, "%-8s %s-%u: con %#x pud %u level %d\n", pins[i].name,
			   pins[i].bank_name, pins[i].pin, pin_field_get(hw, i, PIN_CON),
			   pin_field_get(hw, i, PIN_PUD), pixel_gnss_gpio_get(hw, i));
	seq_printf(s, "host_req EINT: econ %#x, %s, %s\n",
		   (readl(eint_reg(hw, ECON)) >> (4 * pins[host_req].pin)) & 0xf,
		   readl(eint_reg(hw, EMASK)) & bit ? "masked" : "unmasked",
		   readl(eint_reg(hw, EPEND)) & bit ? "pending" : "not pending");
	seq_printf(s, "USI5: SW_CONF %#x, SCLK %u Hz", readl(hw->sysreg + SW_CONF_USI5),
		   hw->speed);
	if (hw->usi_set)
		seq_printf(s, ", SPI status %#x", readl(hw->regs + SPI_STATUS));
	seq_putc(s, '\n');
	for (i = 0; i < ARRAY_SIZE(rails); i++) {
		if (rail_read(hw, i, &val))
			seq_printf(s, "%s: read failed\n", rails[i].name);
		else
			seq_printf(s, "%s: CTRL %#04x, %s at %u mV%s\n", rails[i].name, val,
				   val & LDO_EN ? "on" : "off",
				   (rails[i].min_uv + rails[i].step_uv * (val & VSEL)) / 1000,
				   hw->rail_on[i] ? " (switched on here)" : "");
	}
}
