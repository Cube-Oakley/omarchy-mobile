// SPDX-License-Identifier: GPL-2.0-only
/* GS201 HSI2C buses the bootloader leaves configured, as polled I2C adapters.
 *
 * - hsi2c_15 (PERIC1 USI15, 0x10DA0000, pins gpp24-0/1): the LM3644 flash
 *   (0x63), the P9412 wireless charger (0x3C) and the battery's EEPROM
 *   (0x50). Writes to the EEPROM are refused: it must never change.
 * - hsi2c_8 (PERIC0 USI8, 0x10970000, pins gpp16-0/1): the CS40L26 haptics
 *   driver (0x43) and the ST54J NFC controller (0x08), which also holds the
 *   eSIM. Every transfer to 0x08 is refused.
 *
 * The bootloader leaves both as hsi2c_13 (kernel/battery): USI in I2C mode
 * (sysreg SW_CONF 4), pins on the I2C function, clocks running undivided and
 * the controller in auto mode with the same 400 kHz timings. The module
 * checks all of that and refuses a bus that differs; it changes only the
 * controller's timeout enable, which mainline i2c-exynos5 also clears.
 * Transfers follow kernel/battery's i2c-exynos5 sequence. Register offsets:
 * Google's gs201 cmucal-sfr.c and pinctrl-gs201.c, and the stock DT's bus
 * nodes.
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
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

#define HSI2C_MASTER		BIT(3)
#define HSI2C_RXCHON		BIT(6)
#define HSI2C_TXCHON		BIT(7)
#define HSI2C_SW_RST		BIT(31)
#define HSI2C_RXFIFO_EN		BIT(0)
#define HSI2C_TXFIFO_EN		BIT(1)
#define HSI2C_RXFIFO_TRIGGER(x)	((x) << 4)
#define HSI2C_TXFIFO_TRIGGER(x)	((x) << 16)
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
#define HSI2C_TIMEOUT_EN	BIT(31)
#define HSI2C_READ_WRITE	BIT(16)
#define HSI2C_STOP_AFTER_TRANS	BIT(17)
#define HSI2C_MASTER_RUN	BIT(31)
#define HSI2C_CMD_SEND_STOP	BIT(2)
#define HSI2C_MASTER_ST_MASK	0xf
#define HSI2C_MASTER_ST_LOSE	0xc
#define HSI2C_SLV_ADDR_MAS(x)	(((x) & 0x3ff) << 10)
#define HSI2C_FIFO_DEPTH	64

#define USI_SW_CONF_I2C		0x4
#define GATE_MANUAL		BIT(20)
#define GATE_CG_VAL		BIT(21)
#define MUX_SEL_USER		BIT(4)

struct pixel_bus {
	unsigned int num;
	phys_addr_t base;
	phys_addr_t sw_conf;	/* sysreg USI mode */
	phys_addr_t pin_con;	/* the pin bank's CON; SCL/SDA are pins 0 and 1 */
	phys_addr_t cmu;
	u16 mux, ipclk, pclk;	/* CMU mux (PLL_CON0) and gates */
	u8 pin_func;		/* SCL/SDA function in CON */
	u16 no_write;		/* address never written, or 0 */
	u16 no_access;		/* address never addressed at all, or 0 */
};

static const struct pixel_bus pixel_buses[] = {
	{ 15, 0x10da0000, 0x10c21018, 0x10c40080, 0x10c00000, 0x670, 0x2058, 0x205c, 2, 0x50, 0 },
	{ 8, 0x10970000, 0x1082101c, 0x10840200, 0x10800000, 0x6c0, 0x20a0, 0x20a4, 3, 0, 0x08 },
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
	u32 timeout;		/* as handed over */
};

static unsigned int buses[ARRAY_SIZE(pixel_buses)] = { 15 };
static int nbuses = 1;
module_param_array(buses, uint, &nbuses, 0444);
MODULE_PARM_DESC(buses, "HSI2C buses to register (15, 8)");

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
	int i, ret;

	for (i = 0; i < num; i++) {
		if (i2c->bus->no_access && msgs[i].addr == i2c->bus->no_access)
			return -EPERM;
		if (i2c->bus->no_write && msgs[i].addr == i2c->bus->no_write &&
		    !(msgs[i].flags & I2C_M_RD) && (num == 1 || msgs[i].len > 1))
			return -EPERM;	/* only an address-setting write before a read */
	}
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

static int pixel_hsi2c_probe(struct platform_device *pd)
{
	const struct pixel_bus *b = platform_get_drvdata(pd);
	struct device *dev = &pd->dev;
	struct pixel_hsi2c *i2c;
	u32 conf, pins, mux, ipclk, pclk;
	unsigned int i;
	int ret;

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

	i2c = devm_kzalloc(dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;
	i2c->bus = b;
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
		writel(i2c->timeout, i2c->regs + HSI2C_TIMEOUT);
		return ret;
	}
	dev_info(dev, "hsi2c_%u at %pa\n", b->num, &b->base);
	return 0;
}

static void pixel_hsi2c_remove(struct platform_device *pd)
{
	struct pixel_hsi2c *i2c = platform_get_drvdata(pd);

	i2c_del_adapter(&i2c->adap);
	writel(i2c->timeout, i2c->regs + HSI2C_TIMEOUT);
}

static struct platform_driver pixel_hsi2c_driver = {
	.probe = pixel_hsi2c_probe,
	.remove = pixel_hsi2c_remove,
	.driver.name = "pixel-hsi2c",
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
			pr_err("pixel-hsi2c: unknown bus %u\n", buses[i]);
			continue;
		}
		pd = platform_device_alloc("pixel-hsi2c", b->num);
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
MODULE_DESCRIPTION("GS201 HSI2C buses left configured by the bootloader, as polled adapters");
