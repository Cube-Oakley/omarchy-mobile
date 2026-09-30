// SPDX-License-Identifier: GPL-2.0-only
/* GS201 HSI2C buses as polled I2C adapters. This file builds two modules.
 *
 * pixel-hsi2c.ko adopts the buses the bootloader leaves configured:
 * - hsi2c_15 (PERIC1 USI15, 0x10DA0000, pins gpp24-0/1): the LM3644 flash
 *   (0x63), the P9412 wireless charger (0x3C) and the battery's EEPROM
 *   (0x50). Writes to the EEPROM are refused: it must never change.
 * - hsi2c_8 (PERIC0 USI8, 0x10970000, pins gpp16-0/1): the CS40L26 haptics
 *   driver (0x43), the SLG51002 camera PMIC (0x75) and the ST54J NFC
 *   controller (0x08), which also holds the eSIM. Every transfer to 0x08 is
 *   refused.
 * The bootloader leaves both as hsi2c_13 (kernel/battery): USI in I2C mode
 * (sysreg SW_CONF 4), pins on the I2C function, clocks running and the
 * controller in auto mode with the same 400 kHz timings. The module checks
 * all of that and refuses a bus that differs; it changes only the
 * controller's timeout enable, which mainline i2c-exynos5 also clears.
 *
 * pixel-hsi2c-cam.ko (pixel-hsi2c-cam.c, this file with PIXEL_HSI2C_CAM)
 * configures the camera buses from scratch. It is a module of its own so it
 * loads and unloads beside the running pixel-hsi2c:
 * - hsi2c_1 (PERIC0 USI1, 0x10900000, pins gpp2-0/1): main sensor, its AF,
 *   OIS and the laser AF;
 * - hsi2c_2 (USI2, 0x10910000, gpp4-0/1): front sensor, its EEPROM (0x51);
 * - hsi2c_3 (USI3, 0x10920000, gpp6-0/1): ultrawide sensor, its AF and
 *   EEPROM (0x50);
 * - hsi2c_4 (USI4, 0x10930000, gpp8-0/1): tele sensor, AF, OIS and EEPROM.
 * The bootloader leaves USI1-4 unconfigured (SW_CONF 0). For each bus it:
 * - reads the reference, hsi2c_8, which it requires as pixel-hsi2c adopts
 *   it: its CMU user mux and divider, its CONF and its 400 kHz timings;
 * - requires this USI unconfigured and its gates and Q-channel on, before
 *   any access to the controller;
 * - gives the USI hsi2c_8's clock source and divider, so hsi2c_8's timings
 *   give the same 400 kHz;
 * - puts the USI in I2C mode (SW_CONF 4), out of reset, clock requested
 *   (kernel/spi's order);
 * - initialises the controller as Google's i2c-exynos5 does (reset, master,
 *   auto mode, trailing count) with hsi2c_8's CONF and timings, plus the
 *   stock DT's samsung,no_lose_arbitration and samsung,reset-before-trans;
 * - leaves the bus pins alone. As on stock, where LWIS switches them to
 *   "on_i2c" only while a device on the bus is powered, pixel-camera-power
 *   (kernel/camera) switches them with each sensor's power.
 * Module EEPROM writes are refused (0x51 on hsi2c_2, 0x50 on hsi2c_3), apart
 * from the two address bytes before a read. Unloading puts each USI back in
 * reset and restores its USI_OPTION, SW_CONF, divider and mux.
 *
 * Transfers follow kernel/battery's i2c-exynos5 sequence. Register offsets:
 * Google's gs201 cmucal-sfr.c and pinctrl-gs201.c, the stock DT's bus nodes
 * (reg, samsung,usi-offset into sysreg_peric0 at 0x10821000) and Google's
 * i2c-exynos5.c.
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define HSI2C_CTL		0x00
#define HSI2C_FIFO_CTL		0x04
#define HSI2C_TRAILIG_CTL	0x08
#define HSI2C_INT_ENABLE	0x20
#define HSI2C_INT_STATUS	0x24
#define HSI2C_FIFO_STATUS	0x30
#define HSI2C_TX_DATA		0x34
#define HSI2C_RX_DATA		0x38
#define HSI2C_CONF		0x40
#define HSI2C_AUTO_CONF		0x44
#define HSI2C_TIMEOUT		0x48
#define HSI2C_MANUAL_CMD	0x4c
#define HSI2C_TRANS_STATUS	0x50
#define HSI2C_TIMING_FS1	0x60
#define HSI2C_TIMING_FS2	0x64
#define HSI2C_TIMING_FS3	0x68
#define HSI2C_TIMING_SLA	0x6c
#define HSI2C_ADDR		0x70
#define USI_CON			0xc4
#define USI_OPTION		0xc8

#define HSI2C_MASTER		BIT(3)
#define HSI2C_RXCHON		BIT(6)
#define HSI2C_TXCHON		BIT(7)
#define HSI2C_NO_LOSE_ARBITRATION BIT(22)
#define HSI2C_SW_RST		BIT(31)
#define HSI2C_RXFIFO_EN		BIT(0)
#define HSI2C_TXFIFO_EN		BIT(1)
#define HSI2C_RXFIFO_TRIGGER(x)	((x) << 4)
#define HSI2C_TXFIFO_TRIGGER(x)	((x) << 16)
#define HSI2C_TRAILING_COUNT	0xffffff
#define HSI2C_INT_TX_ALMOSTEMPTY_EN BIT(0)
#define HSI2C_INT_RX_ALMOSTFULL_EN BIT(1)
#define HSI2C_INT_TRAILING_EN	BIT(6)
#define HSI2C_INT_TRANS_DONE	BIT(7)
#define HSI2C_INT_TRANS_ABORT	BIT(8)
#define HSI2C_INT_NO_DEV_ACK	BIT(9)
#define HSI2C_INT_NO_DEV	BIT(10)
#define HSI2C_INT_TIMEOUT	BIT(11)
#define HSI2C_INT_ERRORS	(HSI2C_INT_TRANS_ABORT | HSI2C_INT_NO_DEV_ACK | \
				 HSI2C_INT_NO_DEV | HSI2C_INT_TIMEOUT)
#define HSI2C_RX_FIFO_LVL(x)	(((x) >> 16) & 0x7f)
#define HSI2C_TX_FIFO_LVL(x)	((x) & 0x7f)
#define HSI2C_AUTO_MODE		BIT(31)
#define HSI2C_10BIT_ADDR_MODE	BIT(30)
#define HSI2C_HS_MODE		BIT(29)
#define HSI2C_TIMEOUT_EN	BIT(31)
#define HSI2C_READ_WRITE	BIT(16)
#define HSI2C_STOP_AFTER_TRANS	BIT(17)
#define HSI2C_MASTER_RUN	BIT(31)
#define HSI2C_CMD_SEND_STOP	BIT(2)
#define HSI2C_MASTER_ST_MASK	0xf
#define HSI2C_MASTER_ST_LOSE	0xc
#define HSI2C_SLV_ADDR_MAS(x)	(((x) & 0x3ff) << 10)
#define HSI2C_FIFO_DEPTH	64

#define USI_CON_RESET		BIT(0)
#define USI_OPTION_CLKREQ_ON	BIT(1)
#define USI_OPTION_CLKSTOP_ON	BIT(2)
#define USI_SW_CONF_MASK	0x7
#define USI_SW_CONF_I2C		0x4
#define GATE_MANUAL		BIT(20)
#define GATE_CG_VAL		BIT(21)
#define MUX_SEL_USER		BIT(4)
#define DIV_RATIO		0xf
#define CMU_BUSY		BIT(16)
#define QCH_ENABLE		BIT(0)
#define QCH_CLOCK_REQ		BIT(1)

#define CMU_PERIC0		0x10800000
#define CMU_PERIC1		0x10c00000

#define BUS_FROM_SCRATCH	BIT(0)	/* configure the USI and controller */
#define BUS_RESET_BEFORE	BIT(1)	/* stock samsung,reset-before-trans */
#define BUS_NO_LOSE_ARB		BIT(2)	/* stock samsung,no_lose_arbitration */
#define CAM_BUS			(BUS_FROM_SCRATCH | BUS_RESET_BEFORE | BUS_NO_LOSE_ARB)

struct pixel_bus {
	unsigned int num;
	phys_addr_t base;
	phys_addr_t sw_conf;	/* sysreg USI mode */
	phys_addr_t pin_con;	/* adopted: the pin bank's CON; SCL/SDA are pins 0 and 1 */
	phys_addr_t cmu;
	u16 mux, div;		/* CMU user mux (PLL_CON0) and divider */
	u16 ipclk, pclk, qch;	/* CMU gates and Q-channel */
	u8 pin_func;		/* adopted: SCL/SDA function in CON */
	u8 flags;
	u16 no_write;		/* address never written, or 0 ... */
	u8 no_write_alen;	/* ... apart from this many address bytes before a read */
	u16 no_access;		/* address never addressed at all, or 0 */
};

#ifndef PIXEL_HSI2C_CAM
#define DRV_NAME		"pixel-hsi2c"
#define DEFAULT_BUSES		{ 15 }
static const struct pixel_bus pixel_buses[] = {
	{ .num = 15, .base = 0x10da0000, .sw_conf = 0x10c21018, .pin_con = 0x10c40080,
	  .cmu = CMU_PERIC1, .mux = 0x670, .ipclk = 0x2058, .pclk = 0x205c, .pin_func = 2,
	  .no_write = 0x50, .no_write_alen = 1 },
	{ .num = 8, .base = 0x10970000, .sw_conf = 0x1082101c, .pin_con = 0x10840200,
	  .cmu = CMU_PERIC0, .mux = 0x6c0, .ipclk = 0x20a0, .pclk = 0x20a4, .pin_func = 3,
	  .no_access = 0x08 },
};
#else
#define DRV_NAME		"pixel-hsi2c-cam"
#define DEFAULT_BUSES		{ 3 }
/* CMU_PERIC0 PLL_CON0_MUX_CLKCMU_PERIC0_USIn_USI_USER, CLK_CON_DIV_DIV_CLK_PERIC0_USIn_USI,
 * CLK_CON_GAT_CLK_BLK_PERIC0_UID_USIn_USI_IPCLKPORT_IPCLK/_PCLK, QCH_CON_USIn_USI_QCH.
 */
static const struct pixel_bus pixel_buses[] = {
	{ .num = 1, .base = 0x10900000, .sw_conf = 0x10821000, .cmu = CMU_PERIC0,
	  .mux = 0x650, .div = 0x1810, .ipclk = 0x2068, .pclk = 0x206c, .qch = 0x3074,
	  .flags = CAM_BUS },
	{ .num = 2, .base = 0x10910000, .sw_conf = 0x10821004, .cmu = CMU_PERIC0,
	  .mux = 0x660, .div = 0x1814, .ipclk = 0x2070, .pclk = 0x2074, .qch = 0x3078,
	  .flags = CAM_BUS, .no_write = 0x51, .no_write_alen = 2 },
	{ .num = 3, .base = 0x10920000, .sw_conf = 0x10821008, .cmu = CMU_PERIC0,
	  .mux = 0x670, .div = 0x1818, .ipclk = 0x2078, .pclk = 0x207c, .qch = 0x307c,
	  .flags = CAM_BUS, .no_write = 0x50, .no_write_alen = 2 },
	{ .num = 4, .base = 0x10930000, .sw_conf = 0x1082100c, .cmu = CMU_PERIC0,
	  .mux = 0x680, .div = 0x181c, .ipclk = 0x2080, .pclk = 0x2084, .qch = 0x3080,
	  .flags = CAM_BUS },
};
#endif

/* The reference for a bus built from scratch: hsi2c_8 as the bootloader leaves it. */
static const struct pixel_bus ref_bus = {
	.num = 8, .base = 0x10970000, .sw_conf = 0x1082101c, .cmu = CMU_PERIC0,
	.mux = 0x6c0, .div = 0x182c, .ipclk = 0x20a0, .pclk = 0x20a4, .qch = 0x3090,
};

static const u16 saved_regs[] = {
	HSI2C_CTL, HSI2C_TRAILIG_CTL, HSI2C_CONF, HSI2C_TIMEOUT, HSI2C_TIMING_FS1,
	HSI2C_TIMING_FS2, HSI2C_TIMING_FS3, HSI2C_TIMING_SLA, HSI2C_FIFO_CTL,
};

struct pixel_hsi2c {
	struct i2c_adapter adap;
	const struct pixel_bus *bus;
	void __iomem *regs;
	u32 saved[ARRAY_SIZE(saved_regs)];
	u32 timeout;		/* adopted: as handed over */
	/* From scratch: what the bus had before, restored on removal. */
	void __iomem *cmu, *sysreg;
	u32 saved_sw_conf, saved_mux, saved_div, saved_option;
	bool clk_set, usi_up;
};

static unsigned int buses[ARRAY_SIZE(pixel_buses)] = DEFAULT_BUSES;
static int nbuses = 1;
module_param_array(buses, uint, &nbuses, 0444);
#ifndef PIXEL_HSI2C_CAM
MODULE_PARM_DESC(buses, "HSI2C buses to register (15, 8)");
#else
MODULE_PARM_DESC(buses, "Camera HSI2C buses to configure and register (1-4)");
#endif

static struct platform_device *pdevs[ARRAY_SIZE(pixel_buses)];

static void hsi2c_reset(struct pixel_hsi2c *i2c)
{
	u32 ctl = readl(i2c->regs + HSI2C_CTL);
	unsigned int i;

	writel(ctl | HSI2C_SW_RST, i2c->regs + HSI2C_CTL);
	writel(ctl & ~HSI2C_SW_RST, i2c->regs + HSI2C_CTL);
	for (i = 0; i < ARRAY_SIZE(saved_regs); i++)
		writel(i2c->saved[i], i2c->regs + saved_regs[i]);
}

static void hsi2c_bus_check(struct pixel_hsi2c *i2c)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(100);

	while (time_before(jiffies, timeout)) {
		u32 st = readl(i2c->regs + HSI2C_TRANS_STATUS);

		if ((st & HSI2C_MASTER_ST_MASK) != HSI2C_MASTER_ST_LOSE)
			return;
		writel(HSI2C_CMD_SEND_STOP, i2c->regs + HSI2C_MANUAL_CMD);
		usleep_range(100, 200);
	}
}

static int hsi2c_message(struct pixel_hsi2c *i2c, struct i2c_msg *msg, bool stop)
{
	bool read = msg->flags & I2C_M_RD;
	u32 ctl, fifo_ctl, int_en, auto_conf, st, fifo;
	unsigned long timeout;
	unsigned int ptr = 0, trig;
	bool done = false;

	if (!msg->len || msg->len > 0xffff || (msg->flags & I2C_M_TEN))
		return -EOPNOTSUPP;
	ctl = readl(i2c->regs + HSI2C_CTL) & ~(HSI2C_TXCHON | HSI2C_RXCHON);
	fifo_ctl = HSI2C_RXFIFO_EN | HSI2C_TXFIFO_EN;
	int_en = HSI2C_INT_TRANS_DONE | HSI2C_INT_ERRORS;
	if (read) {
		ctl |= HSI2C_RXCHON;
		trig = msg->len > HSI2C_FIFO_DEPTH ? HSI2C_FIFO_DEPTH * 3 / 4 : msg->len;
		fifo_ctl |= HSI2C_RXFIFO_TRIGGER(trig);
		int_en |= HSI2C_INT_RX_ALMOSTFULL_EN | HSI2C_INT_TRAILING_EN;
		auto_conf = HSI2C_READ_WRITE;
	} else {
		ctl |= HSI2C_TXCHON;
		trig = msg->len > HSI2C_FIFO_DEPTH ? HSI2C_FIFO_DEPTH / 4 : msg->len;
		fifo_ctl |= HSI2C_TXFIFO_TRIGGER(trig);
		int_en |= HSI2C_INT_TX_ALMOSTEMPTY_EN;
		auto_conf = 0;
	}
	writel(HSI2C_SLV_ADDR_MAS(msg->addr), i2c->regs + HSI2C_ADDR);
	writel(fifo_ctl, i2c->regs + HSI2C_FIFO_CTL);
	writel(ctl, i2c->regs + HSI2C_CTL);
	hsi2c_bus_check(i2c);
	writel(readl(i2c->regs + HSI2C_INT_STATUS), i2c->regs + HSI2C_INT_STATUS);
	writel(int_en, i2c->regs + HSI2C_INT_ENABLE);
	if (stop)
		auto_conf |= HSI2C_STOP_AFTER_TRANS;
	writel(auto_conf | msg->len | HSI2C_MASTER_RUN, i2c->regs + HSI2C_AUTO_CONF);

	timeout = jiffies + msecs_to_jiffies(100);
	for (;;) {
		st = readl(i2c->regs + HSI2C_INT_STATUS);
		writel(st, i2c->regs + HSI2C_INT_STATUS);
		fifo = readl(i2c->regs + HSI2C_FIFO_STATUS);
		if (read) {
			unsigned int lvl = HSI2C_RX_FIFO_LVL(fifo);

			while (lvl-- && ptr < msg->len)
				msg->buf[ptr++] = readl(i2c->regs + HSI2C_RX_DATA);
		} else {
			unsigned int room = HSI2C_FIFO_DEPTH - HSI2C_TX_FIFO_LVL(fifo);

			while (room-- && ptr < msg->len)
				writel(msg->buf[ptr++], i2c->regs + HSI2C_TX_DATA);
		}
		if (st & HSI2C_INT_ERRORS)
			break;
		if (st & HSI2C_INT_TRANS_DONE)
			done = true;
		if (done && ptr == msg->len)
			break;
		if (time_after(jiffies, timeout))
			break;
		usleep_range(20, 50);
	}
	writel(0, i2c->regs + HSI2C_INT_ENABLE);

	/* As mainline, reset after any failure: a NACKed write leaves its
	 * bytes in the TX FIFO, ahead of the next message's. The CS40L26 NACKs
	 * the transfer that wakes it from hibernation.
	 */
	if (st & (HSI2C_INT_NO_DEV_ACK | HSI2C_INT_NO_DEV)) {
		hsi2c_reset(i2c);
		return -ENXIO;
	}
	if (st & HSI2C_INT_TRANS_ABORT) {
		hsi2c_reset(i2c);
		return -EAGAIN;
	}
	if (!(done && ptr == msg->len)) {
		dev_warn_ratelimited(&i2c->adap.dev, "transfer to %#x timed out (int %#x, trans %#x)\n",
				     msg->addr, st, readl(i2c->regs + HSI2C_TRANS_STATUS));
		hsi2c_reset(i2c);
		return -ETIMEDOUT;
	}
	return 0;
}

static int hsi2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct pixel_hsi2c *i2c = i2c_get_adapdata(adap);
	const struct pixel_bus *b = i2c->bus;
	int i, ret;

	for (i = 0; i < num; i++) {
		if (b->no_access && msgs[i].addr == b->no_access)
			return -EPERM;
		/* Only address-setting writes ahead of a read. */
		if (b->no_write && msgs[i].addr == b->no_write &&
		    !(msgs[i].flags & I2C_M_RD) && (num == 1 || msgs[i].len > b->no_write_alen))
			return -EPERM;
	}
	if (b->flags & BUS_RESET_BEFORE)
		hsi2c_reset(i2c);
	for (i = 0; i < num; i++) {
		ret = hsi2c_message(i2c, &msgs[i], i == num - 1);
		if (ret)
			return ret;
	}
	return num;
}

static u32 hsi2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | (I2C_FUNC_SMBUS_EMUL & ~I2C_FUNC_SMBUS_QUICK);
}

static const struct i2c_algorithm hsi2c_algo = {
	.xfer = hsi2c_xfer,
	.functionality = hsi2c_func,
};

static u32 read_phys(phys_addr_t addr)
{
	void __iomem *reg = ioremap(addr, 4);
	u32 val;

	if (!reg)
		return 0;
	val = readl(reg);
	iounmap(reg);
	return val;
}

static bool gate_on(u32 gate)
{
	return !(gate & GATE_MANUAL) || (gate & GATE_CG_VAL);
}

/* As kernel/battery: a Q-channel either hands the clock over on request or is held requested. */
static bool qch_on(u32 qch)
{
	return qch & (QCH_ENABLE | QCH_CLOCK_REQ);
}

static int cmu_set(void __iomem *reg, u32 mask, u32 val)
{
	u32 v = readl(reg);

	if ((v & mask) == (val & mask))
		return 0;
	writel((v & ~mask) | (val & mask), reg);
	return readl_poll_timeout_atomic(reg, v, !(v & CMU_BUSY), 1, 1000);
}

/* hsi2c_8's clock and 400 kHz setup, as the bootloader leaves it. */
struct ref_setup {
	u32 mux, div, conf, fs1, fs2, fs3, sla;
};

static int ref_read(struct ref_setup *r)
{
	const struct pixel_bus *b = &ref_bus;
	u32 conf, ipclk, pclk, qch, ctl;
	void __iomem *regs;

	conf = read_phys(b->sw_conf);
	ipclk = read_phys(b->cmu + b->ipclk);
	pclk = read_phys(b->cmu + b->pclk);
	qch = read_phys(b->cmu + b->qch);
	r->mux = read_phys(b->cmu + b->mux) & MUX_SEL_USER;
	r->div = read_phys(b->cmu + b->div) & DIV_RATIO;
	if (conf != USI_SW_CONF_I2C || !r->mux || !gate_on(ipclk) || !gate_on(pclk) ||
	    !qch_on(qch)) {
		pr_err(DRV_NAME ": reference hsi2c_8 not as the bootloader leaves it: USI %#x mux %#x gates %#x/%#x qch %#x\n",
		       conf, r->mux, ipclk, pclk, qch);
		return -ENODEV;
	}
	regs = ioremap(b->base, 0x100);
	if (!regs)
		return -ENOMEM;
	ctl = readl(regs + HSI2C_CTL);
	r->conf = readl(regs + HSI2C_CONF);
	r->fs1 = readl(regs + HSI2C_TIMING_FS1);
	r->fs2 = readl(regs + HSI2C_TIMING_FS2);
	r->fs3 = readl(regs + HSI2C_TIMING_FS3);
	r->sla = readl(regs + HSI2C_TIMING_SLA);
	iounmap(regs);
	if (!(ctl & HSI2C_MASTER) || !(r->conf & HSI2C_AUTO_MODE) ||
	    (r->conf & HSI2C_HS_MODE) || !r->fs1) {
		pr_err(DRV_NAME ": reference hsi2c_8 controller not configured (ctl %#x conf %#x fs1 %#x)\n",
		       ctl, r->conf, r->fs1);
		return -ENODEV;
	}
	return 0;
}

/* Undo hsi2c_setup: USI in reset, then its mode, divider and mux as found. */
static void hsi2c_teardown(struct pixel_hsi2c *i2c)
{
	const struct pixel_bus *b = i2c->bus;

	if (i2c->usi_up) {
		writel(i2c->saved_option, i2c->regs + USI_OPTION);
		writel(readl(i2c->regs + USI_CON) | USI_CON_RESET, i2c->regs + USI_CON);
		writel(i2c->saved_sw_conf, i2c->sysreg);
		i2c->usi_up = false;
	}
	if (i2c->clk_set) {
		if (cmu_set(i2c->cmu + b->div, DIV_RATIO, i2c->saved_div) ||
		    cmu_set(i2c->cmu + b->mux, MUX_SEL_USER, i2c->saved_mux))
			pr_err(DRV_NAME ": hsi2c_%u clock stuck busy while restoring\n", b->num);
		i2c->clk_set = false;
	}
}

static int hsi2c_setup(struct device *dev, struct pixel_hsi2c *i2c)
{
	const struct pixel_bus *b = i2c->bus;
	struct ref_setup r, r2;
	u32 ipclk, pclk, qch, v;
	int ret;

	/* Twice: pixel-hsi2c resets hsi2c_8 after a failed transfer. */
	ret = ref_read(&r);
	if (!ret)
		ret = ref_read(&r2);
	if (ret)
		return ret;
	if (memcmp(&r, &r2, sizeof(r))) {
		dev_err(dev, "hsi2c_8 changed while being read; try again\n");
		return -EAGAIN;
	}

	i2c->cmu = devm_ioremap(dev, b->cmu, 0x4000);
	i2c->sysreg = devm_ioremap(dev, b->sw_conf, 4);
	if (!i2c->cmu || !i2c->sysreg)
		return -ENOMEM;
	i2c->saved_sw_conf = readl(i2c->sysreg);
	i2c->saved_mux = readl(i2c->cmu + b->mux);
	i2c->saved_div = readl(i2c->cmu + b->div);
	ipclk = readl(i2c->cmu + b->ipclk);
	pclk = readl(i2c->cmu + b->pclk);
	qch = readl(i2c->cmu + b->qch);
	dev_info(dev, "hsi2c_%u as found: USI %#x mux %#x div %#x gates %#x/%#x qch %#x\n",
		 b->num, i2c->saved_sw_conf, i2c->saved_mux, i2c->saved_div, ipclk, pclk, qch);
	if (i2c->saved_sw_conf & USI_SW_CONF_MASK) {
		dev_err(dev, "hsi2c_%u: USI already in use (SW_CONF %#x)\n", b->num,
			i2c->saved_sw_conf);
		return -EBUSY;
	}
	/* The controller is not touched unless its clocks can run. */
	if (!gate_on(ipclk) || !gate_on(pclk) || !qch_on(qch)) {
		dev_err(dev, "hsi2c_%u: clocks off\n", b->num);
		return -ENODEV;
	}
	i2c->saved_mux &= MUX_SEL_USER;
	i2c->saved_div &= DIV_RATIO;

	/* Clock: hsi2c_8's source and divider, so its timings give 400 kHz here too.
	 * Every PERIC0 USI user mux picks OSCCLK_PERIC0 (0) or CLKCMU_PERIC0_IP (1)
	 * (cmucal-node.c).
	 */
	i2c->clk_set = true;
	ret = cmu_set(i2c->cmu + b->mux, MUX_SEL_USER, r.mux);
	if (!ret)
		ret = cmu_set(i2c->cmu + b->div, DIV_RATIO, r.div);
	if (ret) {
		dev_err(dev, "hsi2c_%u: clock stuck busy\n", b->num);
		goto fail;
	}

	/* USI: I2C mode, out of reset, clock requested continuously. */
	writel((i2c->saved_sw_conf & ~USI_SW_CONF_MASK) | USI_SW_CONF_I2C, i2c->sysreg);
	i2c->usi_up = true;
	i2c->saved_option = readl(i2c->regs + USI_OPTION);
	v = readl(i2c->regs + USI_CON);
	writel(v & ~USI_CON_RESET, i2c->regs + USI_CON);
	udelay(1);
	writel((i2c->saved_option & ~USI_OPTION_CLKSTOP_ON) | USI_OPTION_CLKREQ_ON,
	       i2c->regs + USI_OPTION);

	/* Controller: Google's exynos5_i2c_reset and exynos5_i2c_init. */
	v = readl(i2c->regs + HSI2C_CTL);
	writel(v | HSI2C_SW_RST, i2c->regs + HSI2C_CTL);
	writel(v & ~HSI2C_SW_RST, i2c->regs + HSI2C_CTL);
	writel(HSI2C_MASTER | (b->flags & BUS_NO_LOSE_ARB ? HSI2C_NO_LOSE_ARBITRATION : 0),
	       i2c->regs + HSI2C_CTL);
	writel(HSI2C_TRAILING_COUNT, i2c->regs + HSI2C_TRAILIG_CTL);
	writel(r.fs1, i2c->regs + HSI2C_TIMING_FS1);
	writel(r.fs2, i2c->regs + HSI2C_TIMING_FS2);
	writel(r.fs3, i2c->regs + HSI2C_TIMING_FS3);
	writel(r.sla, i2c->regs + HSI2C_TIMING_SLA);
	writel((r.conf & ~HSI2C_10BIT_ADDR_MODE) | HSI2C_AUTO_MODE, i2c->regs + HSI2C_CONF);
	writel(readl(i2c->regs + HSI2C_TIMEOUT) & ~HSI2C_TIMEOUT_EN, i2c->regs + HSI2C_TIMEOUT);
	writel(0, i2c->regs + HSI2C_INT_ENABLE);
	writel(readl(i2c->regs + HSI2C_INT_STATUS), i2c->regs + HSI2C_INT_STATUS);
	if (!(readl(i2c->regs + HSI2C_CTL) & HSI2C_MASTER) ||
	    readl(i2c->regs + HSI2C_TIMING_FS1) != r.fs1 ||
	    !(readl(i2c->regs + HSI2C_CONF) & HSI2C_AUTO_MODE)) {
		dev_err(dev, "hsi2c_%u: controller did not take its setup (ctl %#x conf %#x fs1 %#x)\n",
			b->num, readl(i2c->regs + HSI2C_CTL), readl(i2c->regs + HSI2C_CONF),
			readl(i2c->regs + HSI2C_TIMING_FS1));
		ret = -EIO;
		goto fail;
	}
	dev_info(dev, "hsi2c_%u configured: mux %#x div %u (as hsi2c_8), USI_CON %#x, conf %#x fs %#x/%#x/%#x sla %#x\n",
		 b->num, r.mux, r.div, readl(i2c->regs + USI_CON), r.conf, r.fs1, r.fs2, r.fs3,
		 r.sla);
	return 0;
fail:
	hsi2c_teardown(i2c);
	return ret;
}

/* A bus the bootloader configured: check it and take it as it is. */
static int hsi2c_adopt(struct device *dev, struct pixel_hsi2c *i2c)
{
	const struct pixel_bus *b = i2c->bus;
	u32 conf, pins, mux, ipclk, pclk;

	/* Clocks first: the controller must not be touched if they are off. */
	conf = read_phys(b->sw_conf);
	pins = read_phys(b->pin_con);
	mux = read_phys(b->cmu + b->mux);
	ipclk = read_phys(b->cmu + b->ipclk);
	pclk = read_phys(b->cmu + b->pclk);
	if (conf != USI_SW_CONF_I2C || (pins & 0xff) != b->pin_func * 0x11 ||
	    !(mux & MUX_SEL_USER) ||
	    !gate_on(ipclk) || !gate_on(pclk)) {
		dev_err(dev, "hsi2c_%u not as the bootloader leaves it: USI %#x pins %#x mux %#x gates %#x/%#x\n",
			b->num, conf, pins, mux, ipclk, pclk);
		return -ENODEV;
	}

	i2c->regs = devm_ioremap(dev, b->base, 0x1000);
	if (!i2c->regs)
		return -ENOMEM;
	if (!(readl(i2c->regs + HSI2C_CTL) & HSI2C_MASTER) ||
	    !(readl(i2c->regs + HSI2C_CONF) & HSI2C_AUTO_MODE) ||
	    !readl(i2c->regs + HSI2C_TIMING_FS1)) {
		dev_err(dev, "hsi2c_%u controller not configured (ctl %#x conf %#x fs1 %#x)\n",
			b->num, readl(i2c->regs + HSI2C_CTL), readl(i2c->regs + HSI2C_CONF),
			readl(i2c->regs + HSI2C_TIMING_FS1));
		return -ENODEV;
	}
	i2c->timeout = readl(i2c->regs + HSI2C_TIMEOUT);
	writel(i2c->timeout & ~HSI2C_TIMEOUT_EN, i2c->regs + HSI2C_TIMEOUT);
	return 0;
}

static void hsi2c_release(struct pixel_hsi2c *i2c)
{
	if (i2c->bus->flags & BUS_FROM_SCRATCH)
		hsi2c_teardown(i2c);
	else
		writel(i2c->timeout, i2c->regs + HSI2C_TIMEOUT);
}

static int pixel_hsi2c_probe(struct platform_device *pd)
{
	const struct pixel_bus *b = platform_get_drvdata(pd);
	struct device *dev = &pd->dev;
	struct pixel_hsi2c *i2c;
	unsigned int i;
	int ret;

	i2c = devm_kzalloc(dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;
	i2c->bus = b;
	if (b->flags & BUS_FROM_SCRATCH) {
		/* Mapped only; hsi2c_setup checks the clocks before any access. */
		i2c->regs = devm_ioremap(dev, b->base, 0x1000);
		if (!i2c->regs)
			return -ENOMEM;
		ret = hsi2c_setup(dev, i2c);
	} else {
		ret = hsi2c_adopt(dev, i2c);
	}
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(saved_regs); i++)
		i2c->saved[i] = readl(i2c->regs + saved_regs[i]);

	i2c->adap.owner = THIS_MODULE;
	i2c->adap.algo = &hsi2c_algo;
	i2c->adap.dev.parent = dev;
	i2c->adap.retries = 2;
	snprintf(i2c->adap.name, sizeof(i2c->adap.name), "Pixel hsi2c_%u", b->num);
	i2c_set_adapdata(&i2c->adap, i2c);
	platform_set_drvdata(pd, i2c);
	ret = i2c_add_adapter(&i2c->adap);
	if (ret) {
		hsi2c_release(i2c);
		return ret;
	}
	dev_info(dev, "hsi2c_%u at %pa\n", b->num, &b->base);
	return 0;
}

static void pixel_hsi2c_remove(struct platform_device *pd)
{
	struct pixel_hsi2c *i2c = platform_get_drvdata(pd);

	i2c_del_adapter(&i2c->adap);
	hsi2c_release(i2c);
}

static struct platform_driver pixel_hsi2c_driver = {
	.probe = pixel_hsi2c_probe,
	.remove = pixel_hsi2c_remove,
	.driver.name = DRV_NAME,
};

static void pixel_hsi2c_unregister(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pdevs); i++) {
		if (pdevs[i])
			platform_device_unregister(pdevs[i]);
		pdevs[i] = NULL;
	}
}

static int __init pixel_hsi2c_init(void)
{
	unsigned int i, j, added = 0;
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	ret = platform_driver_register(&pixel_hsi2c_driver);
	if (ret)
		return ret;
	for (i = 0; i < nbuses; i++) {
		const struct pixel_bus *b = NULL;
		struct platform_device *pd;

		for (j = 0; j < ARRAY_SIZE(pixel_buses); j++)
			if (pixel_buses[j].num == buses[i])
				b = &pixel_buses[j];
		if (!b) {
			pr_err(DRV_NAME ": unknown bus %u\n", buses[i]);
			continue;
		}
		pd = platform_device_alloc(DRV_NAME, b->num);
		if (!pd)
			continue;
		platform_set_drvdata(pd, (void *)b);
		if (platform_device_add(pd)) {
			platform_device_put(pd);
			continue;
		}
		if (!pd->dev.driver) {
			platform_device_unregister(pd);
			continue;
		}
		pdevs[i] = pd;
		added++;
	}
	if (!added) {
		platform_driver_unregister(&pixel_hsi2c_driver);
		return -ENODEV;
	}
	return 0;
}
module_init(pixel_hsi2c_init);

static void __exit pixel_hsi2c_exit(void)
{
	pixel_hsi2c_unregister();
	platform_driver_unregister(&pixel_hsi2c_driver);
}
module_exit(pixel_hsi2c_exit);

MODULE_LICENSE("GPL");
#ifndef PIXEL_HSI2C_CAM
MODULE_DESCRIPTION("GS201 HSI2C buses left configured by the bootloader, as polled adapters");
#else
MODULE_DESCRIPTION("GS201 camera HSI2C buses (hsi2c_1-4), configured from scratch, as polled adapters");
#endif
