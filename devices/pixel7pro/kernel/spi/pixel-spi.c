// SPDX-License-Identifier: GPL-2.0-only
/* GS201 SPI7 (PERIC0 USI7): the bus to the two CS35L41 speaker amplifiers.
 *
 * The bootloader leaves USI7 unconfigured (sysreg SW_CONF 0), its pins as
 * inputs and both amplifiers in reset. This binds to the stock DT's
 * spi@10960000 node, so the SPI core creates the amplifiers from its
 * cs35l41@0/@1 children, and it:
 *
 * - checks the clock chain it relies on (PERIC0_IP from PLL_SHARED2/2 =
 *   399.36 MHz, USI7 gates on) and, per transfer, picks USI7's clock: its
 *   user mux selects OSCCLK (24.576 MHz) or PERIC0_IP, then a 1-16 divider;
 *   SCLK is that IPCLK/4, the fastest at or under the transfer's speed (the
 *   mainline cs35l41 driver asks for 4 MHz: OSCCLK/2, 3.072 MHz);
 * - puts USI7 in SPI mode and takes it out of reset;
 * - sets the bus pins (gpp14-0/1/2) to SPI with the stock pulls and drive;
 * - drives transfers by polling, 8 bits per word, a FIFO (64 bytes) at a time.
 *   The controller only shifts while its own slave select is asserted
 *   (manual mode), so each FIFO load asserts it; its pin is a GPIO here.
 *
 * Chip selects are the stock cs-gpios pins (gpp14-3, gpp22-1), driven here as
 * outputs by set_cs: the stock DT also names them in each amplifier's
 * controller-data, so gpiolib treats them as shared, and its shared-GPIO
 * lookup resolves only the first entry of a property. The reset and
 * interrupt lines come from pixel-gpio. The stock DT is adjusted while the
 * module is loaded: cs-gpios is dropped, the
 * pin states of the controller and amplifier nodes get pinctrl-use-default
 * (there is no pin controller driver to apply them; this module and
 * pixel-gpio set those pins), and the amplifier nodes get the
 * mainline cs35l41 GPIO2 properties for the stock cirrus,gpio-config2 values
 * (source 5, push-pull active-high interrupt, output enabled). Their
 * VA-supply, S2MPG13 BUCKA (the always-on 1.8 V IO rail, read back on at
 * 1.85 V), has no regulator driver here, so the link is dropped and the
 * amplifiers get a dummy supply instead of deferring. For the AOC card and
 * wm_adsp, each amplifier also gets its TDM slots (google,tdm-rx-slots and
 * google,tdm-tx-slots, the stock mixer_paths slot positions: left in slot 0,
 * right in slot 1, V/I feedback interleaved) and cirrus,subsystem-id
 * "cheetah", so the right amplifier loads its own ("-r") tuning.
 *
 * Register layout from mainline spi-s3c64xx.c (the gs101 variant) and
 * exynos-usi.c; CMU and sysreg offsets from Google's gs201 cmucal-sfr.c.
 * Unloading restores the pins, the clock divider and the USI mode.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

#define SPI_BASE		0x10960000
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

#define CH_HS_EN		BIT(6)
#define CH_SW_RST		BIT(5)
#define CH_CPOL			BIT(3)
#define CH_CPHA			BIT(2)
#define CH_RXCH_ON		BIT(1)
#define CH_TXCH_ON		BIT(0)
#define MODE_TRAILCNT		(0x3ff << 19)
#define CS_SIG_INACT		BIT(0)
#define ST_RX_LVL(v)		(((v) >> 15) & 0x1ff)
#define ST_TX_LVL(v)		(((v) >> 6) & 0x1ff)
#define PACKET_CNT_EN		BIT(16)
#define PND_CLR_ALL		0x1f
#define FIFO_DEPTH		64
#define FB_CLK_DELAY		1	/* stock samsung,spi-feedback-delay */
#define USI_CON_RESET		BIT(0)
#define USI_OPTION_CLKREQ_ON	BIT(1)
#define USI_OPTION_CLKSTOP_ON	BIT(2)

#define SYSREG_PERIC0		0x10821000
#define SW_CONF_USI7		0x18
#define SW_CONF_SPI		BIT(1)

#define CMU_TOP			0x1e080000
#define TOP_PLL_SHARED2_CON3	0x1cc
#define TOP_PERIC0_IP_MUX	0x10e8
#define TOP_PERIC0_IP_DIV	0x18e4
#define PLL_SHARED2_798M	0xa0820400	/* enabled, M 130, P 4, S 0 */
#define MUX_SHARED2_DIV2	1
#define RATE_PERIC0_IP		399360000
#define RATE_OSCCLK		24576000
#define SCLK_MAX		25000000	/* stock spi-max-frequency */
#define SCLK_MIN		(RATE_OSCCLK / 16 / 4)

#define CMU_PERIC0		0x10800000
#define USI7_MUX		0x6b0
#define USI7_DIV		0x1828
#define USI7_IPCLK_GATE		0x2098
#define USI7_PCLK_GATE		0x209c
#define MUX_SEL_USER		BIT(4)
#define CMU_BUSY		BIT(16)
#define GATE_MANUAL		BIT(20)
#define GATE_CG_VAL		BIT(21)
#define DIV_RATIO		0xf

#define PINCTRL_PERIC0		0x10840000
#define GPP14			0x1c0	/* CON; PUD +8, DRV +0xc */
#define GPP14_BUS_PINS		0x0fff	/* nibbles of pins 0-2 */
#define GPP14_CON_SPI		0x0333
#define GPP14_PUD		0x0100	/* MISO pulled down */
#define GPP14_DRV		0x0011	/* CLK and MOSI drive 1 */
#define PINCTRL_PERIC1		0x10c40000

static const struct { phys_addr_t pinctrl; u16 bank; u8 pin; } cs_pins[] = {
	{ PINCTRL_PERIC0, 0x1c0, 3 },	/* gpp14-3, left amplifier */
	{ PINCTRL_PERIC1, 0x040, 1 },	/* gpp22-1, right amplifier */
};

struct pixel_spi {
	void __iomem *regs;
	void __iomem *sysreg;
	void __iomem *cmu;
	void __iomem *pins;
	void __iomem *cs[ARRAY_SIZE(cs_pins)];
	u32 saved_cs_con[ARRAY_SIZE(cs_pins)], saved_cs_dat[ARRAY_SIZE(cs_pins)];
	u32 saved_sw_conf, saved_mux, saved_div, saved_con, saved_pud, saved_drv;
	u32 speed;		/* current SCLK */
};

static struct of_changeset changeset;
static bool changeset_applied;
static struct platform_device *spi7;

static bool gate_on(u32 gate)
{
	return !(gate & GATE_MANUAL) || (gate & GATE_CG_VAL);
}

static void pin_rmw(void __iomem *reg, u32 mask, u32 val)
{
	writel((readl(reg) & ~mask) | (val & mask), reg);
}

/* Fastest SCLK (IPCLK/4) at or under hz; programs USI7's mux and divider. */
static int pixel_spi_set_speed(struct pixel_spi *s, u32 hz)
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
	if (best == s->speed)
		return 0;
	pin_rmw(s->cmu + USI7_MUX, MUX_SEL_USER, mux);
	ret = readl_poll_timeout_atomic(s->cmu + USI7_MUX, v, !(v & CMU_BUSY), 1, 1000);
	if (!ret) {
		pin_rmw(s->cmu + USI7_DIV, DIV_RATIO, div);
		ret = readl_poll_timeout_atomic(s->cmu + USI7_DIV, v, !(v & CMU_BUSY), 1, 1000);
	}
	s->speed = ret ? 0 : best;
	return ret;
}

/* level is the line's: low selects. */
static void pixel_spi_set_cs(struct spi_device *spi, bool level)
{
	struct pixel_spi *s = spi_controller_get_devdata(spi->controller);
	unsigned int cs = spi_get_chipselect(spi, 0);
	u32 bit = BIT(cs_pins[cs].pin);

	pin_rmw(s->cs[cs] + 4, bit, level ? bit : 0);
}

static void spi_flush(struct pixel_spi *s)
{
	u32 ch = readl(s->regs + SPI_CH_CFG) & ~(CH_TXCH_ON | CH_RXCH_ON);

	writel(0, s->regs + SPI_PACKET_CNT);
	writel(ch | CH_SW_RST, s->regs + SPI_CH_CFG);
	writel(ch & ~CH_SW_RST, s->regs + SPI_CH_CFG);
}

static int pixel_spi_prepare_message(struct spi_controller *ctlr, struct spi_message *msg)
{
	struct pixel_spi *s = spi_controller_get_devdata(ctlr);
	u32 ch = readl(s->regs + SPI_CH_CFG) & ~(CH_CPOL | CH_CPHA);

	if (msg->spi->mode & SPI_CPOL)
		ch |= CH_CPOL;
	if (msg->spi->mode & SPI_CPHA)
		ch |= CH_CPHA;
	writel(ch, s->regs + SPI_CH_CFG);
	return 0;
}

static int pixel_spi_transfer_one(struct spi_controller *ctlr, struct spi_device *spi,
				  struct spi_transfer *xfer)
{
	struct pixel_spi *s = spi_controller_get_devdata(ctlr);
	const u8 *tx = xfer->tx_buf;
	u8 *rx = xfer->rx_buf;
	unsigned int done, n, i;
	u32 ch, st;
	int ret;

	ret = pixel_spi_set_speed(s, xfer->speed_hz);
	if (ret)
		return ret;
	for (done = 0; done < xfer->len; done += n) {
		n = min_t(unsigned int, xfer->len - done, FIFO_DEPTH);
		spi_flush(s);
		writel(PACKET_CNT_EN | n, s->regs + SPI_PACKET_CNT);
		for (i = 0; i < n; i++)
			writel(tx ? tx[done + i] : 0, s->regs + SPI_TX_DATA);
		ch = readl(s->regs + SPI_CH_CFG);
		writel(ch | CH_TXCH_ON | CH_RXCH_ON, s->regs + SPI_CH_CFG);
		writel(0, s->regs + SPI_CS_REG);
		/* 64 bytes take 1.4 ms at the slowest SCLK. */
		ret = readl_poll_timeout_atomic(s->regs + SPI_STATUS, st, ST_RX_LVL(st) >= n,
						1, 10000);
		for (i = 0; i < ST_RX_LVL(st) && i < n; i++) {
			u8 v = readl(s->regs + SPI_RX_DATA);

			if (rx)
				rx[done + i] = v;
		}
		writel(CS_SIG_INACT, s->regs + SPI_CS_REG);
		writel(ch & ~(CH_TXCH_ON | CH_RXCH_ON), s->regs + SPI_CH_CFG);
		if (ret) {
			dev_err(&ctlr->dev, "transfer timed out: status %#x, %u of %u bytes\n",
				st, done, xfer->len);
			spi_flush(s);
			return ret;
		}
	}
	return 0;
}

static int pixel_spi_check_clocks(struct device *dev, struct pixel_spi *s)
{
	void __iomem *top = devm_ioremap(dev, CMU_TOP, 0x2000);
	u32 pll, mux, div, usi_mux, ipclk, pclk;

	if (!top)
		return -ENOMEM;
	pll = readl(top + TOP_PLL_SHARED2_CON3);
	mux = readl(top + TOP_PERIC0_IP_MUX) & 0x3;
	div = readl(top + TOP_PERIC0_IP_DIV) & DIV_RATIO;
	usi_mux = readl(s->cmu + USI7_MUX);
	ipclk = readl(s->cmu + USI7_IPCLK_GATE);
	pclk = readl(s->cmu + USI7_PCLK_GATE);
	devm_iounmap(dev, top);
	if (pll != PLL_SHARED2_798M || mux != MUX_SHARED2_DIV2 || div ||
	    !gate_on(ipclk) || !gate_on(pclk)) {
		dev_err(dev, "unexpected clocks: PLL_SHARED2 %#x PERIC0_IP mux %u div %u, USI7 mux %#x gates %#x/%#x\n",
			pll, mux, div, usi_mux, ipclk, pclk);
		return -EBUSY;
	}
	return 0;
}

static void pixel_spi_restore(struct pixel_spi *s)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cs_pins); i++) {
		u32 nib = 0xf << (4 * cs_pins[i].pin);

		pin_rmw(s->cs[i], nib, s->saved_cs_con[i]);
		pin_rmw(s->cs[i] + 4, BIT(cs_pins[i].pin), s->saved_cs_dat[i]);
	}
	pin_rmw(s->pins + GPP14, GPP14_BUS_PINS, s->saved_con);
	pin_rmw(s->pins + GPP14 + 8, GPP14_BUS_PINS, s->saved_pud);
	pin_rmw(s->pins + GPP14 + 0xc, GPP14_BUS_PINS, s->saved_drv);
	writel(readl(s->regs + USI_CON) | USI_CON_RESET, s->regs + USI_CON);
	writel(s->saved_sw_conf, s->sysreg + SW_CONF_USI7);
	pin_rmw(s->cmu + USI7_DIV, DIV_RATIO, s->saved_div);
	pin_rmw(s->cmu + USI7_MUX, MUX_SEL_USER, s->saved_mux);
	s->speed = 0;
}

static int pixel_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *ctlr;
	struct resource *res;
	struct pixel_spi *s;
	unsigned int i;
	u32 v;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != SPI_BASE)
		return -ENODEV;

	ctlr = devm_spi_alloc_host(dev, sizeof(*s));
	if (!ctlr)
		return -ENOMEM;
	s = spi_controller_get_devdata(ctlr);
	platform_set_drvdata(pdev, s);
	s->regs = devm_ioremap(dev, SPI_BASE, 0x1000);
	s->sysreg = devm_ioremap(dev, SYSREG_PERIC0, 0x1000);
	s->cmu = devm_ioremap(dev, CMU_PERIC0, 0x4000);
	s->pins = devm_ioremap(dev, PINCTRL_PERIC0, 0x1000);
	if (!s->regs || !s->sysreg || !s->cmu || !s->pins)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(cs_pins); i++) {
		s->cs[i] = devm_ioremap(dev, cs_pins[i].pinctrl + cs_pins[i].bank, 0x20);
		if (!s->cs[i])
			return -ENOMEM;
	}

	ret = pixel_spi_check_clocks(dev, s);
	if (ret)
		return ret;
	s->saved_sw_conf = readl(s->sysreg + SW_CONF_USI7);
	if (s->saved_sw_conf & ~SW_CONF_SPI) {
		dev_err(dev, "USI7 is in use: SW_CONF %#x\n", s->saved_sw_conf);
		return -EBUSY;
	}

	/* Clock: the slowest until a transfer asks for more. */
	s->saved_mux = readl(s->cmu + USI7_MUX) & MUX_SEL_USER;
	s->saved_div = readl(s->cmu + USI7_DIV) & DIV_RATIO;
	ret = pixel_spi_set_speed(s, SCLK_MIN);
	if (ret) {
		dev_err(dev, "USI7 clock stuck busy\n");
		return ret;
	}

	/* USI: SPI mode, out of reset, clock requested continuously. */
	writel(SW_CONF_SPI, s->sysreg + SW_CONF_USI7);
	v = readl(s->regs + USI_CON);
	writel(v & ~USI_CON_RESET, s->regs + USI_CON);
	udelay(1);
	v = readl(s->regs + USI_OPTION);
	writel((v & ~USI_OPTION_CLKSTOP_ON) | USI_OPTION_CLKREQ_ON, s->regs + USI_OPTION);

	/* Controller: polled, manual (GPIO) chip select, as spi-s3c64xx inits it. */
	writel(CH_HS_EN, s->regs + SPI_CH_CFG);
	writel(0, s->regs + SPI_INT_EN);
	writel(MODE_TRAILCNT, s->regs + SPI_MODE_CFG);
	writel(0, s->regs + SPI_PACKET_CNT);
	writel(PND_CLR_ALL, s->regs + SPI_PENDING_CLR);
	writel(0, s->regs + SPI_PENDING_CLR);
	writel(0, s->regs + SPI_SWAP_CFG);
	writel(FB_CLK_DELAY, s->regs + SPI_FB_CLK);
	writel(CS_SIG_INACT, s->regs + SPI_CS_REG);
	spi_flush(s);

	/* Pins: SCLK, MOSI and MISO on the SPI function. */
	s->saved_con = readl(s->pins + GPP14) & GPP14_BUS_PINS;
	s->saved_pud = readl(s->pins + GPP14 + 8) & GPP14_BUS_PINS;
	s->saved_drv = readl(s->pins + GPP14 + 0xc) & GPP14_BUS_PINS;
	pin_rmw(s->pins + GPP14 + 8, GPP14_BUS_PINS, GPP14_PUD);
	pin_rmw(s->pins + GPP14 + 0xc, GPP14_BUS_PINS, GPP14_DRV);
	pin_rmw(s->pins + GPP14, GPP14_BUS_PINS, GPP14_CON_SPI);
	/* Chip selects: outputs, deselected (high). */
	for (i = 0; i < ARRAY_SIZE(cs_pins); i++) {
		u32 nib = 0xf << (4 * cs_pins[i].pin), bit = BIT(cs_pins[i].pin);

		s->saved_cs_con[i] = readl(s->cs[i]) & nib;
		s->saved_cs_dat[i] = readl(s->cs[i] + 4) & bit;
		pin_rmw(s->cs[i] + 4, bit, bit);
		pin_rmw(s->cs[i], nib, 0x11111111 & nib);
	}

	dev_info(dev, "SPI7 up: USI_CON %#x, status %#x\n", readl(s->regs + USI_CON),
		 readl(s->regs + SPI_STATUS));

	ctlr->dev.of_node = dev->of_node;
	ctlr->bus_num = 7;
	ctlr->num_chipselect = 2;
	ctlr->set_cs = pixel_spi_set_cs;
	ctlr->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->max_speed_hz = SCLK_MAX;
	ctlr->min_speed_hz = SCLK_MIN;
	ctlr->prepare_message = pixel_spi_prepare_message;
	ctlr->transfer_one = pixel_spi_transfer_one;
	ret = devm_spi_register_controller(dev, ctlr);
	if (ret) {
		/* A retry must start from the state the bootloader left. */
		pixel_spi_restore(s);
		return dev_err_probe(dev, ret, "register\n");
	}
	return 0;
}

static void pixel_spi_remove(struct platform_device *pdev)
{
	/* The controller (and its amplifiers) are gone: devm unregisters first. */
	pixel_spi_restore(platform_get_drvdata(pdev));
}

/* Bound to the stock spi@10960000 device only, through driver_override. */
static struct platform_driver pixel_spi_driver = {
	.probe = pixel_spi_probe,
	.remove = pixel_spi_remove,
	.driver = {
		.name = "pixel-spi7",
	},
};

/* Stock mixer_paths.xml ASPRXn/ASPTXn Slot Position, by chip select. */
static const u32 rx_slots[2][2] = { { 0, 1 }, { 1, 0 } };
static const u32 tx_slots[2][4] = { { 0, 2, 4, 6 }, { 1, 3, 5, 7 } };

static int add_bool(struct device_node *np, const char *name)
{
	if (of_property_present(np, name))
		return 0;
	return of_changeset_add_prop_bool(&changeset, np, name);
}

/* Each pin state of np: pinctrl-use-default, once per state node. */
static int use_default_pins(struct device_node *np, struct device_node **done, int *ndone)
{
	struct device_node *cfg;
	int i, j, ret = 0;

	for (i = 0; !ret && (cfg = of_parse_phandle(np, "pinctrl-0", i)); i++) {
		for (j = 0; j < *ndone && done[j] != cfg; j++)
			;
		if (j == *ndone && *ndone < 16) {
			done[(*ndone)++] = cfg;
			ret = add_bool(cfg, "pinctrl-use-default");
		}
		of_node_put(cfg);
	}
	return ret;
}

static int adjust_dt(void)
{
	struct device_node *bus, *amp, *done[16];
	struct property *prop;
	int ndone = 0, ret;
	u32 cs;

	bus = of_find_node_by_path("/spi@10960000");
	if (!bus)
		return -ENODEV;
	of_changeset_init(&changeset);
	ret = use_default_pins(bus, done, &ndone);
	prop = of_find_property(bus, "cs-gpios", NULL);
	if (!ret && prop)
		ret = of_changeset_remove_property(&changeset, bus, prop);
	for_each_child_of_node(bus, amp) {
		if (ret || !of_device_is_compatible(amp, "cirrus,cs35l41"))
			continue;
		ret = use_default_pins(amp, done, &ndone);
		if (!ret && !of_property_present(amp, "cirrus,gpio2-src-select"))
			ret = of_changeset_add_prop_u32(&changeset, amp, "cirrus,gpio2-src-select", 5);
		if (!ret)
			ret = add_bool(amp, "cirrus,gpio2-output-enable");
		prop = of_find_property(amp, "VA-supply", NULL);
		if (!ret && prop)
			ret = of_changeset_remove_property(&changeset, amp, prop);
		if (!ret)
			ret = of_property_read_u32(amp, "reg", &cs);
		if (!ret && cs >= ARRAY_SIZE(rx_slots))
			ret = -EINVAL;
		if (!ret && !of_property_present(amp, "cirrus,subsystem-id"))
			ret = of_changeset_add_prop_string(&changeset, amp, "cirrus,subsystem-id",
							   "cheetah");
		if (!ret)
			ret = of_changeset_add_prop_u32_array(&changeset, amp, "google,tdm-rx-slots",
							      rx_slots[cs], ARRAY_SIZE(rx_slots[cs]));
		if (!ret)
			ret = of_changeset_add_prop_u32_array(&changeset, amp, "google,tdm-tx-slots",
							      tx_slots[cs], ARRAY_SIZE(tx_slots[cs]));
	}
	of_node_put(bus);
	if (!ret)
		ret = of_changeset_apply(&changeset);
	if (ret)
		of_changeset_destroy(&changeset);
	else
		changeset_applied = true;
	return ret;
}

static int __init pixel_spi_init(void)
{
	struct device_node *np;
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	np = of_find_node_by_path("/spi@10960000");
	spi7 = np ? of_find_device_by_node(np) : NULL;
	of_node_put(np);
	if (!spi7)
		return -ENODEV;
	ret = adjust_dt();
	if (ret)
		goto put;
	ret = device_set_driver_override(&spi7->dev, pixel_spi_driver.driver.name);
	if (ret)
		goto revert;
	/* Registering probes the device synchronously. */
	ret = platform_driver_register(&pixel_spi_driver);
	if (ret)
		goto override;
	if (!spi7->dev.driver) {
		platform_driver_unregister(&pixel_spi_driver);
		ret = -ENODEV;
		goto override;
	}
	return 0;
override:
	device_set_driver_override(&spi7->dev, NULL);
revert:
	of_changeset_revert(&changeset);
	of_changeset_destroy(&changeset);
	changeset_applied = false;
put:
	put_device(&spi7->dev);
	return ret;
}
module_init(pixel_spi_init);

static void __exit pixel_spi_exit(void)
{
	platform_driver_unregister(&pixel_spi_driver);
	device_set_driver_override(&spi7->dev, NULL);
	put_device(&spi7->dev);
	if (changeset_applied) {
		of_changeset_revert(&changeset);
		of_changeset_destroy(&changeset);
	}
}
module_exit(pixel_spi_exit);

MODULE_DESCRIPTION("GS201 SPI7 host for the CS35L41 speaker amplifiers");
MODULE_LICENSE("GPL");
