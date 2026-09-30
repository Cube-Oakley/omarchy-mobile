// SPDX-License-Identifier: GPL-2.0-only
/* Power for the Pixel 7 Pro's four cameras, sequenced as the stock DT's LWIS
 * sensor nodes sequence it.
 *
 * Supplies:
 * - S2MPG13 LDO12S (L12S_CAMIO, 1.8 V), the camera IO rail of every sensor:
 *   PM bank 0x01, CTRL 0x37, enable bit 7, voltage 0.7 V + 25 mV x bits 5:0
 *   (Google's s2mpg13-regulator.c and s2mpg13-register.h). Reached through
 *   the ACPM PMIC channel, as kernel/aoc-power does. Refused unless the
 *   bootloader's 1.8 V (0x2C) is set; left alone if already on at load.
 * - The SLG51002 camera PMIC at 0x75 on hsi2c_8 ("Pixel hsi2c_8",
 *   kernel/i2c), revision AB (ID 0xB13103). Three S2MPG13 GPIOs hold it in
 *   reset: GPIO1 (bb), GPIO3 (buck), GPIO0 (cs); GPIOn_SET at 0x05 + n in
 *   bank 0x0C, bit 6 output enable, bit 5 value (s2mpg13-core.c,
 *   s2mpg1x-gpio-gs201.c). They go up in Google's probe order (bb, 2 ms;
 *   buck, 2 ms; cs, 10 ms) when the first sensor powers up, and down in
 *   Google's remove order (cs, 10 ms; buck, 1 ms; bb, 1 ms) after the last,
 *   so the chip sits in reset, as the bootloader leaves it, whenever no
 *   camera is on. Its registers (slg51002.h, slg51002-regulator.c): LDOn
 *   VSEL, with the chip's own selector range in MINV/MAXV (VSEL + 0x60/0x61),
 *   1.2 V + 10 mV x sel for LDO1-5 and 0.4 V + 5 mV x sel for LDO6-8; LDO
 *   enables in MATRIX_CONF_A (0x110D) bit n-1; GPIO outputs in GPIOn_CTRL
 *   (0x1710 + n - 1) bit 0, which rev AB drives without the software test
 *   mode. A voltage is set only if it lies inside MINV-MAXV. Every write is
 *   read back.
 * Shared rails (L12S, SLG LDO8) and the chip itself are reference counted.
 *
 * MCLK: CMU_TOP CIS_CLK0-3 (mux 0x1010, divider 0x1814, gate 0x2038, DFTMUX
 * Q-channel 0x3004, each + 4 x n; Google's cmucal-sfr.c). Set to OSCCLK
 * undivided, 24.576 MHz, which every stock sensor table assumes as EXTCLK;
 * the gate is made to pass as Google's ra_set_gate does and the Q-channel
 * set as stock's GATE_DFTMUX_CMU_CIS_CLKn enable leaves it (ENABLE 0,
 * CLOCK_REQ 1). All four are restored at power-down.
 *
 * Pins (PERIC0 pin controller 0x10840000, gpp banks at n x 0x20, CON +0,
 * DAT +4, PUD +8, DRV +0xc, 4-bit fields; pinctrl-gs201.c), from the stock
 * DT's pin states:
 * - reset: driven as gpiod_direction_output does, value then direction;
 * - MCLK pad: "sensor-mclkN-fn" (function 2, no pull) on, "-out" (driven
 *   low, pull-down) off;
 * - bus pins: "hsi2cN-bus" (function 3, pull-up) while the sensor is on,
 *   as LWIS selects "on_i2c" after the power-up sequence and "off_i2c"
 *   before power-down. The camera bus controller comes from
 *   pixel-hsi2c-cam; its adapter is held while the sensor is on.
 * Each pin must be an input at load (as the bootloader leaves it) or its
 * sensor is left unavailable, and is restored when the sensor powers down.
 *
 * Order, as lwis_dev_power_up_locked: CIS clock, the DT power-up sequence
 * with each step's delay after it, bus pins, 2 ms. Power-down: bus pins,
 * the DT power-down sequence, clock. Only the ultrawide (IMX386) is enabled
 * by default; experimental=1 allows the other three.
 *
 * The PMIC keeps its state across a warm reboot, so a camera left on by one
 * leaves the SLG51002 lines high (the module then refuses to load) and
 * LDO12S on; takeover=1 switches both off at load.
 *
 * sysfs, under /sys/devices/platform/pixel-camera-power: {uw,front,main,
 * tele}/power (0/1) and status. Unloading powers everything down.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define ACPM_PMIC_CHANNEL	2
#define PMIC_SUB		1	/* S2MPG13 */
#define PM_BANK			0x01
#define L12S_CTRL		0x37	/* S2MPG13_PM_L12S_CTRL */
#define L12S_EN			BIT(7)	/* S2MPG13_REG_ENABLE_MASK_7 */
#define L12S_VSEL		0x3f
#define L12S_1V8		0x2c	/* 0.7 V + 25 mV x 44, voltage group 6 */
#define L12S_ENABLE_US		128	/* S2MPG13_ENABLE_TIME_LDO */
#define GPIO_BANK		0x0c	/* I2C_ADDR_GPIO */
#define GPIO_SET(n)		(0x05 + (n))	/* S2MPG13_GPIO_n_SET */
#define GPIO_OE			BIT(6)
#define GPIO_VAL		BIT(5)

#define SLG_ADDR		0x75
#define SLG_PATN_ID_B0		0x1105
#define SLG_MATRIX_CONF_A	0x110d
#define SLG_FAULT_LOG1		0x1115
#define SLG_GPIO_CTRL(n)	(0x1710 + (n) - 1)	/* MUXARRAY_INPUT_SEL_16 + n - 1 */
#define SLG_MINV		0x60
#define SLG_MAXV		0x61
#define SLG_REV_AB		0xb13103
#define SLG_VOUT_OK		BIT(1)
#define SLG_ILIM		BIT(0)

#define CMU_TOP			0x1e080000
#define CIS_MUX(n)		(0x1010 + 4 * (n))	/* CLK_CON_MUX_MUX_CLKCMU_CIS_CLKn */
#define CIS_DIV(n)		(0x1814 + 4 * (n))	/* CLK_CON_DIV_CLKCMU_CIS_CLKn */
#define CIS_GATE(n)		(0x2038 + 4 * (n))	/* CLK_CON_GAT_GATE_CLKCMU_CIS_CLKn */
#define CIS_QCH(n)		(0x3004 + 4 * (n))	/* DMYQCH_CON_DFTMUX_CMU_QCH_CIS_CLKn */
#define MUX_SELECT		0x7
#define CIS_DIV_RATIO		0x1f
#define CMU_BUSY		BIT(16)
#define GATE_MANUAL		BIT(20)
#define GATE_CG_VAL		BIT(21)
#define GATE_AUTO_CLKGATING	BIT(28)
#define QCH_ENABLE		BIT(0)
#define QCH_CLOCK_REQ		BIT(1)
#define OSCCLK_HZ		24576000

#define PINCTRL_PERIC0		0x10840000
#define GPP(n)			((n) * 0x20)
#define PIN_CON			0x0
#define PIN_DAT			0x4
#define PIN_PUD			0x8
#define PIN_DRV			0xc
#define FN_INPUT		0
#define FN_OUTPUT		1
#define FN_MCLK			2
#define FN_I2C			3
#define PUD_NONE		0
#define PUD_DOWN		1
#define PUD_UP			3
#define BUS_PINS		(BIT(0) | BIT(1))	/* SCL, SDA */

enum { L12S, LDO1, LDO2, LDO3, LDO4, LDO6, LDO7, LDO8, SGPIO3, SGPIO4, NRAILS };

struct rail {
	const char *name;
	u16 vsel;		/* SLG LDOn_VSEL; 0 for L12S and the GPIO outputs */
	u16 status;		/* SLG LDOn_STATUS */
	u8 bit;			/* SLG LDO: MATRIX_CONF_A enable bit */
	u16 ctrl;		/* SLG GPIO output: GPIOn_CTRL, bit 0 */
	u32 base_uv, step_uv;
	u32 uv;			/* the stock DT's regulator-min/max-microvolt */
};

static const struct rail rails[NRAILS] = {
	[L12S] = { .name = "S2MPG13 LDO12S" },
	[LDO1] = { "SLG LDO1", 0x2000, 0x20c1, 0, 0, 1200000, 10000, 2850000 },
	[LDO2] = { "SLG LDO2", 0x2200, 0x22c1, 1, 0, 1200000, 10000, 2850000 },
	[LDO3] = { "SLG LDO3", 0x2300, 0x23c1, 2, 0, 1200000, 10000, 2250000 },
	[LDO4] = { "SLG LDO4", 0x2500, 0x25c1, 3, 0, 1200000, 10000, 2900000 },
	[LDO6] = { "SLG LDO6", 0x2900, 0x29c1, 5, 0, 400000, 5000, 1100000 },
	[LDO7] = { "SLG LDO7", 0x3100, 0x31c1, 6, 0, 400000, 5000, 1000000 },
	[LDO8] = { "SLG LDO8", 0x3200, 0x32c1, 7, 0, 400000, 5000, 1100000 },
	[SGPIO3] = { .name = "SLG GPIO3", .ctrl = SLG_GPIO_CTRL(3) },
	[SGPIO4] = { .name = "SLG GPIO4", .ctrl = SLG_GPIO_CTRL(4) },
};

enum step_type { RAIL, RESET, MCLK };

struct step {
	u8 type, rail;
	u16 delay_us;		/* after the step */
};

#define R(r, us)	{ RAIL, r, us }
#define RST(us)		{ RESET, 0, us }
#define CLK(us)		{ MCLK, 0, us }

/* The stock DT's power-up-seqs / power-down-seqs and their delays. */
static const struct step uw_up[] = { R(L12S, 0), R(LDO6, 0), R(LDO2, 1000), CLK(1000), RST(10000) };
static const struct step uw_down[] = { RST(1000), CLK(1000), R(LDO2, 0), R(LDO6, 0), R(L12S, 1000) };
static const struct step front_up[] = { R(L12S, 0), R(LDO8, 0), R(SGPIO4, 1000), CLK(1000), RST(8000) };
static const struct step front_down[] = { RST(0), CLK(1000), R(SGPIO4, 0), R(LDO8, 0), R(L12S, 1000) };
static const struct step main_up[] = {
	R(L12S, 0), R(LDO4, 1000), R(SGPIO3, 0), R(LDO8, 0), R(LDO1, 2000), RST(1000), CLK(9000)
};
static const struct step main_down[] = {
	CLK(1000), RST(1000), R(LDO1, 0), R(LDO8, 0), R(SGPIO3, 6000), R(LDO4, 0), R(L12S, 0)
};
static const struct step tele_up[] = { R(L12S, 300), R(LDO7, 0), R(LDO3, 0), CLK(1000), RST(10000) };
static const struct step tele_down[] = { RST(0), CLK(0), R(LDO7, 1000), R(L12S, 0), R(LDO3, 0) };

enum { UW, FRONT, MAIN, TELE, NSENSORS };

struct sensor {
	const char *name, *node, *part;
	u8 bus, addr;		/* hsi2c_N; 7-bit address */
	u16 id_reg, id;		/* for status: what cam-id.py reads */
	u8 cis;			/* CIS_CLKn */
	u16 reset_bank;
	u8 reset_pin;
	u16 mclk_bank;		/* pin 0 */
	u16 bus_bank;		/* pins 0 (SCL) and 1 (SDA) */
	bool experimental;
	const struct step *up, *down;
	u8 nup, ndown;
};

#define SEQ(s)	.up = s##_up, .nup = ARRAY_SIZE(s##_up), .down = s##_down, .ndown = ARRAY_SIZE(s##_down)

static const struct sensor sensors[NSENSORS] = {
	[UW] = { "uw", "sensor-sandworm", "Sony IMX386", 3, 0x1a, 0x0016, 0x0386, 0,
		 GPP(6), 3, GPP(3), GPP(6), false, SEQ(uw) },
	[FRONT] = { "front", "sensor-dokkaebi", "Samsung 3J1", 2, 0x10, 0x0000, 0x30a1, 1,
		    GPP(4), 2, GPP(5), GPP(4), true, SEQ(front) },
	[MAIN] = { "main", "sensor-nagual", "Samsung GN1", 1, 0x3d, 0x0000, 0x08e1, 3,
		   GPP(2), 2, GPP(9), GPP(2), true, SEQ(main) },
	[TELE] = { "tele", "sensor-kraken", "Samsung GM5", 4, 0x2d, 0x0000, 0x08d5, 2,
		   GPP(6), 2, GPP(7), GPP(8), true, SEQ(tele) },
};

/* SLG51002 enable lines, S2MPG13 GPIOs, in Google's power-up order. */
static const struct { const char *name; u8 gpio; u8 up_ms, down_ms; } slg_lines[] = {
	{ "GPIO1 (bb)", 1, 2, 1 },
	{ "GPIO3 (buck)", 3, 2, 1 },
	{ "GPIO0 (cs)", 0, 10, 10 },
};

struct pin_save {
	u32 con, dat, pud, drv;
};

struct cam_state {
	bool available, powered;
	bool slg, clk, reset, mclk, bus_on, touched;	/* what power-up has done */
	u16 held;					/* rails this sensor holds */
	struct i2c_adapter *bus;
	struct pin_save reset_save, mclk_save, bus_save;
	u32 mux, div, gate, qch;			/* CIS_CLKn as found */
};

struct cam_power {
	struct device *dev;
	struct mutex lock;
	struct acpm_handle *acpm;
	struct i2c_adapter *adap8;
	struct i2c_client *slg;
	void __iomem *top, *pins;
	unsigned int slg_users;
	bool line_up[ARRAY_SIZE(slg_lines)];
	unsigned int rail_users[NRAILS];
	bool l12s_boot_on;
	u32 slg_id;
	struct cam_state st[NSENSORS];
};

static bool experimental;
module_param(experimental, bool, 0444);
MODULE_PARM_DESC(experimental, "Allow powering the front, main and tele sensors");

static bool takeover;
module_param(takeover, bool, 0444);
MODULE_PARM_DESC(takeover, "Switch off camera supplies found on at load (S2MPG13 keeps them across a warm reboot)");

static struct platform_device *pdev;

/* S2MPG13 over ACPM */

static int pmic_read(struct cam_power *cp, u8 bank, u8 reg, u8 *val)
{
	return cp->acpm->ops->pmic.read_reg(cp->acpm, ACPM_PMIC_CHANNEL, bank, reg, PMIC_SUB, val);
}

static int pmic_write(struct cam_power *cp, u8 bank, u8 reg, u8 val)
{
	u8 back;
	int ret;

	ret = cp->acpm->ops->pmic.write_reg(cp->acpm, ACPM_PMIC_CHANNEL, bank, reg, PMIC_SUB, val);
	if (!ret)
		ret = pmic_read(cp, bank, reg, &back);
	if (!ret && back != val) {
		dev_err(cp->dev, "S2MPG13 %#04x:%#04x: wrote %#04x, reads %#04x\n", bank, reg, val,
			back);
		ret = -EIO;
	}
	return ret;
}

static int l12s_set(struct cam_power *cp, bool on)
{
	u8 v, want;
	int ret;

	ret = pmic_read(cp, PM_BANK, L12S_CTRL, &v);
	if (ret)
		return ret;
	if ((v & L12S_VSEL) != L12S_1V8) {
		dev_err(cp->dev, "LDO12S CTRL %#04x: not 1.8 V; left alone\n", v);
		return -ERANGE;
	}
	want = on ? v | L12S_EN : v & ~L12S_EN;
	if (want == v)
		return 0;
	ret = pmic_write(cp, PM_BANK, L12S_CTRL, want);
	if (!ret && on)
		fsleep(L12S_ENABLE_US);
	return ret;
}

static int line_set(struct cam_power *cp, unsigned int i, bool high)
{
	u8 reg = GPIO_SET(slg_lines[i].gpio), val, want;
	int ret;

	ret = pmic_read(cp, GPIO_BANK, reg, &val);
	if (ret)
		return ret;
	if (!(val & GPIO_OE)) {
		dev_err(cp->dev, "S2MPG13 %s is not an output (SET %#04x); left alone\n",
			slg_lines[i].name, val);
		return -EINVAL;
	}
	want = high ? val | GPIO_VAL : val & ~GPIO_VAL;
	if (want == val)
		return 0;
	ret = pmic_write(cp, GPIO_BANK, reg, want);
	if (!ret)
		dev_info(cp->dev, "S2MPG13 %s %s\n", slg_lines[i].name, high ? "high" : "low");
	return ret;
}

/* SLG51002 over hsi2c_8: 16-bit register, 8-bit value */

static int slg_read(struct cam_power *cp, u16 reg, u8 *val)
{
	u8 addr[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[] = {
		{ .addr = cp->slg->addr, .len = 2, .buf = addr },
		{ .addr = cp->slg->addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	int ret = i2c_transfer(cp->slg->adapter, msgs, ARRAY_SIZE(msgs));

	if (ret == ARRAY_SIZE(msgs))
		return 0;
	return ret < 0 ? ret : -EIO;
}

static int slg_write(struct cam_power *cp, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val }, back;
	struct i2c_msg msg = { .addr = cp->slg->addr, .len = 3, .buf = buf };
	int ret = i2c_transfer(cp->slg->adapter, &msg, 1);

	if (ret != 1)
		return ret < 0 ? ret : -EIO;
	ret = slg_read(cp, reg, &back);
	if (!ret && back != val) {
		dev_err(cp->dev, "SLG51002 %#06x: wrote %#04x, reads %#04x\n", reg, val, back);
		ret = -EIO;
	}
	return ret;
}

static void slg_lines_down(struct cam_power *cp)
{
	unsigned int i = ARRAY_SIZE(slg_lines);

	/* Google's slg51002_i2c_remove order: cs, buck, bb. */
	while (i--) {
		if (!cp->line_up[i])
			continue;
		if (line_set(cp, i, false))
			dev_err(cp->dev, "S2MPG13 %s stuck high\n", slg_lines[i].name);
		else
			cp->line_up[i] = false;
		fsleep(slg_lines[i].down_ms * 1000);
	}
}

/* Fresh out of reset: the right chip, every output off. */
static int slg_check(struct cam_power *cp)
{
	u8 id[3], conf, ctrl[4], fault;
	unsigned int i;
	int ret = 0;

	for (i = 0; i < 3 && !ret; i++)
		ret = slg_read(cp, SLG_PATN_ID_B0 + i, &id[i]);
	if (ret) {
		dev_err(cp->dev, "SLG51002 does not answer: %d\n", ret);
		return ret;
	}
	cp->slg_id = id[2] << 16 | id[1] << 8 | id[0];
	if (cp->slg_id != SLG_REV_AB) {
		dev_err(cp->dev, "SLG51002 ID %#08x, not rev AB (%#08x)\n", cp->slg_id, SLG_REV_AB);
		return -ENODEV;
	}
	ret = slg_read(cp, SLG_MATRIX_CONF_A, &conf);
	for (i = 0; i < 4 && !ret; i++)
		ret = slg_read(cp, SLG_GPIO_CTRL(i + 1), &ctrl[i]);
	if (!ret)
		ret = slg_read(cp, SLG_FAULT_LOG1, &fault);
	if (ret)
		return ret;
	dev_info(cp->dev, "SLG51002 up: MATRIX_CONF_A %#04x, GPIO1-4 CTRL %#04x %#04x %#04x %#04x, FAULT_LOG1 %#04x\n",
		conf, ctrl[0], ctrl[1], ctrl[2], ctrl[3], fault);
	if (conf || ((ctrl[0] | ctrl[1] | ctrl[2] | ctrl[3]) & BIT(0))) {
		dev_err(cp->dev, "SLG51002 outputs on out of reset: MATRIX_CONF_A %#04x, GPIO1-4 CTRL %#04x %#04x %#04x %#04x\n",
			conf, ctrl[0], ctrl[1], ctrl[2], ctrl[3]);
		return -EBUSY;
	}
	return 0;
}

static int slg_get(struct cam_power *cp)
{
	unsigned int i;
	int ret;

	if (cp->slg_users) {
		cp->slg_users++;
		return 0;
	}
	/* Google's slg51002_i2c_probe order; CS high to ready takes about 10 ms. */
	for (i = 0; i < ARRAY_SIZE(slg_lines); i++) {
		ret = line_set(cp, i, true);
		if (ret)
			goto fail;
		cp->line_up[i] = true;
		fsleep(slg_lines[i].up_ms * 1000);
	}
	ret = slg_check(cp);
	if (ret)
		goto fail;
	cp->slg_users = 1;
	return 0;
fail:
	slg_lines_down(cp);
	return ret;
}

static void slg_put(struct cam_power *cp)
{
	unsigned int r;
	u8 conf;

	if (WARN_ON(!cp->slg_users) || --cp->slg_users)
		return;
	for (r = 0; r < NRAILS; r++)
		WARN(r != L12S && cp->rail_users[r], "%s still has users\n", rails[r].name);
	if (!slg_read(cp, SLG_MATRIX_CONF_A, &conf) && conf)
		dev_warn(cp->dev, "SLG51002 MATRIX_CONF_A %#04x at power-off\n", conf);
	slg_lines_down(cp);
}

static int slg_set_voltage(struct cam_power *cp, unsigned int r)
{
	const struct rail *d = &rails[r];
	u32 sel = (d->uv - d->base_uv) / d->step_uv;
	u8 minv, maxv, cur;
	int ret;

	ret = slg_read(cp, d->vsel + SLG_MINV, &minv);
	if (!ret)
		ret = slg_read(cp, d->vsel + SLG_MAXV, &maxv);
	if (!ret)
		ret = slg_read(cp, d->vsel, &cur);
	if (ret)
		return ret;
	/* Google's driver takes the usable selector range from these registers. */
	if (sel < minv || sel > maxv) {
		dev_err(cp->dev, "%s: %u uV (selector %#x) outside the chip's range %#x-%#x\n",
			d->name, d->uv, sel, minv, maxv);
		return -ERANGE;
	}
	if (cur == sel)
		return 0;
	ret = slg_write(cp, d->vsel, sel);
	if (!ret)
		dev_info(cp->dev, "%s: VSEL %#04x -> %#04x (%u uV)\n", d->name, cur, sel, d->uv);
	return ret;
}

static int slg_output(struct cam_power *cp, unsigned int r, bool on)
{
	const struct rail *d = &rails[r];
	u16 reg = d->vsel ? SLG_MATRIX_CONF_A : d->ctrl;
	u8 mask = d->vsel ? BIT(d->bit) : BIT(0), v;
	int ret;

	ret = slg_read(cp, reg, &v);
	if (ret)
		return ret;
	if (!!(v & mask) == on)
		return 0;
	return slg_write(cp, reg, on ? v | mask : v & ~mask);
}

static int rail_get(struct cam_power *cp, unsigned int r)
{
	int ret = 0;

	if (cp->rail_users[r]++)
		return 0;
	if (r == L12S) {
		if (!cp->l12s_boot_on)
			ret = l12s_set(cp, true);
	} else {
		if (rails[r].vsel)
			ret = slg_set_voltage(cp, r);
		if (!ret)
			ret = slg_output(cp, r, true);
	}
	if (ret) {
		cp->rail_users[r]--;
		dev_err(cp->dev, "%s: on failed: %d\n", rails[r].name, ret);
	} else {
		dev_info(cp->dev, "%s on\n", rails[r].name);
	}
	return ret;
}

static int rail_put(struct cam_power *cp, unsigned int r)
{
	int ret = 0;

	if (WARN_ON(!cp->rail_users[r]))
		return -EINVAL;
	if (--cp->rail_users[r])
		return 0;
	if (r == L12S) {
		if (!cp->l12s_boot_on)
			ret = l12s_set(cp, false);
	} else {
		ret = slg_output(cp, r, false);
	}
	if (ret)
		dev_err(cp->dev, "%s: off failed: %d\n", rails[r].name, ret);
	else
		dev_info(cp->dev, "%s off\n", rails[r].name);
	return ret;
}

/* CMU_TOP CIS_CLKn */

static int cmu_set(void __iomem *reg, u32 mask, u32 val)
{
	u32 v = readl(reg);

	if ((v & mask) == (val & mask))
		return 0;
	writel((v & ~mask) | (val & mask), reg);
	return readl_poll_timeout_atomic(reg, v, !(v & CMU_BUSY), 1, 1000);
}

static int cis_clk_on(struct cam_power *cp, unsigned int s)
{
	struct cam_state *st = &cp->st[s];
	unsigned int n = sensors[s].cis;
	void __iomem *top = cp->top;
	u32 gate;
	int ret;

	st->mux = readl(top + CIS_MUX(n));
	st->div = readl(top + CIS_DIV(n));
	st->gate = readl(top + CIS_GATE(n));
	st->qch = readl(top + CIS_QCH(n));
	st->clk = true;
	ret = cmu_set(top + CIS_MUX(n), MUX_SELECT, 0);
	if (!ret)
		ret = cmu_set(top + CIS_DIV(n), CIS_DIV_RATIO, 0);
	if (ret) {
		dev_err(cp->dev, "CIS_CLK%u stuck busy\n", n);
		return ret;
	}
	gate = st->gate & GATE_MANUAL ? st->gate | GATE_CG_VAL : st->gate & ~GATE_AUTO_CLKGATING;
	if (gate != st->gate)
		writel(gate, top + CIS_GATE(n));
	writel((st->qch & ~QCH_ENABLE) | QCH_CLOCK_REQ, top + CIS_QCH(n));
	dev_info(cp->dev, "CIS_CLK%u at %u Hz: mux %#x div %#x gate %#x qch %#x (were %#x %#x %#x %#x)\n",
		n, OSCCLK_HZ, readl(top + CIS_MUX(n)), readl(top + CIS_DIV(n)),
		readl(top + CIS_GATE(n)), readl(top + CIS_QCH(n)), st->mux, st->div, st->gate,
		st->qch);
	return 0;
}

static void cis_clk_off(struct cam_power *cp, unsigned int s)
{
	struct cam_state *st = &cp->st[s];
	unsigned int n = sensors[s].cis;
	void __iomem *top = cp->top;

	writel(st->qch, top + CIS_QCH(n));
	writel(st->gate, top + CIS_GATE(n));
	if (cmu_set(top + CIS_DIV(n), CIS_DIV_RATIO, st->div) ||
	    cmu_set(top + CIS_MUX(n), MUX_SELECT, st->mux))
		dev_err(cp->dev, "CIS_CLK%u stuck busy while restoring\n", n);
	st->clk = false;
}

/* PERIC0 pins; every camera pin write happens under cp->lock */

static u32 nibbles(u8 pins, u32 v)
{
	u32 r = 0;
	unsigned int p;

	for (p = 0; p < 8; p++)
		if (pins & BIT(p))
			r |= (v & 0xf) << (4 * p);
	return r;
}

static void rmw(void __iomem *reg, u32 mask, u32 val)
{
	writel((readl(reg) & ~mask) | (val & mask), reg);
}

static void pins_save(struct cam_power *cp, u16 bank, u8 pins, struct pin_save *s)
{
	void __iomem *r = cp->pins + bank;
	u32 nm = nibbles(pins, 0xf);

	s->con = readl(r + PIN_CON) & nm;
	s->dat = readl(r + PIN_DAT) & pins;
	s->pud = readl(r + PIN_PUD) & nm;
	s->drv = readl(r + PIN_DRV) & nm;
}

static void pins_restore(struct cam_power *cp, u16 bank, u8 pins, const struct pin_save *s)
{
	void __iomem *r = cp->pins + bank;
	u32 nm = nibbles(pins, 0xf);

	rmw(r + PIN_PUD, nm, s->pud);
	rmw(r + PIN_DRV, nm, s->drv);
	rmw(r + PIN_DAT, pins, s->dat);
	rmw(r + PIN_CON, nm, s->con);
}

static void pins_config(struct cam_power *cp, u16 bank, u8 pins, u32 fn, u32 pud, u32 drv)
{
	void __iomem *r = cp->pins + bank;
	u32 nm = nibbles(pins, 0xf);

	rmw(r + PIN_PUD, nm, nibbles(pins, pud));
	rmw(r + PIN_DRV, nm, nibbles(pins, drv));
	rmw(r + PIN_CON, nm, nibbles(pins, fn));
}

static u32 pin_field(struct cam_power *cp, u16 bank, u16 reg, unsigned int pin)
{
	return (readl(cp->pins + bank + reg) >> (4 * pin)) & 0xf;
}

/* gpiod_direction_output(): value, then direction. */
static void reset_set(struct cam_power *cp, unsigned int s, bool high)
{
	const struct sensor *d = &sensors[s];
	void __iomem *r = cp->pins + d->reset_bank;
	u8 pin = d->reset_pin;

	cp->st[s].touched = true;
	rmw(r + PIN_DAT, BIT(pin), high ? BIT(pin) : 0);
	rmw(r + PIN_CON, 0xf << (4 * pin), FN_OUTPUT << (4 * pin));
}

/* The stock "mclk_on" and "mclk_off" pin states. */
static void mclk_set(struct cam_power *cp, unsigned int s, bool on)
{
	u16 bank = sensors[s].mclk_bank;

	cp->st[s].touched = true;
	if (on) {
		pins_config(cp, bank, BIT(0), FN_MCLK, PUD_NONE, 0);
	} else {
		rmw(cp->pins + bank + PIN_DAT, BIT(0), 0);
		pins_config(cp, bank, BIT(0), FN_OUTPUT, PUD_DOWN, 0);
	}
}

/* Stock "on_i2c" (hsi2cN-bus); off is the state found at load. */
static void bus_pins_set(struct cam_power *cp, unsigned int s, bool on)
{
	struct cam_state *st = &cp->st[s];
	u16 bank = sensors[s].bus_bank;

	if (on)
		pins_config(cp, bank, BUS_PINS, FN_I2C, PUD_UP, 0);
	else
		pins_restore(cp, bank, BUS_PINS, &st->bus_save);
	st->bus_on = on;
}

/* Sensors */

struct adap_match {
	const char *name;
	int nr;
};

static int adap_match(struct device *dev, void *data)
{
	struct i2c_adapter *adap = i2c_verify_adapter(dev);
	struct adap_match *m = data;

	if (!adap || strcmp(adap->name, m->name))
		return 0;
	m->nr = adap->nr;
	return 1;
}

static struct i2c_adapter *find_adapter(const char *name)
{
	struct adap_match m = { name, -1 };
	struct i2c_adapter *adap;

	/* i2c_get_adapter takes the core lock, which i2c_for_each_dev holds. */
	if (i2c_for_each_dev(&m, adap_match) <= 0)
		return NULL;
	adap = i2c_get_adapter(m.nr);
	if (adap && strcmp(adap->name, name)) {
		i2c_put_adapter(adap);
		adap = NULL;
	}
	return adap;
}

/* Undo whatever power-up did, in the stock power-down order. */
static int sensor_undo(struct cam_power *cp, unsigned int s)
{
	const struct sensor *d = &sensors[s];
	struct cam_state *st = &cp->st[s];
	unsigned int i;
	int ret, err = 0;

	if (st->bus_on)
		bus_pins_set(cp, s, false);
	for (i = 0; i < d->ndown; i++) {
		const struct step *p = &d->down[i];
		bool done = false;

		ret = 0;
		switch (p->type) {
		case RAIL:
			if (st->held & BIT(p->rail)) {
				st->held &= ~BIT(p->rail);
				ret = rail_put(cp, p->rail);
				done = true;
			}
			break;
		case RESET:
			if (st->reset) {
				reset_set(cp, s, false);
				st->reset = false;
				done = true;
			}
			break;
		case MCLK:
			if (st->mclk) {
				mclk_set(cp, s, false);
				st->mclk = false;
				done = true;
			}
			break;
		}
		if (ret && !err)
			err = ret;
		if (done && p->delay_us)
			fsleep(p->delay_us);
	}
	if (st->touched) {
		pins_restore(cp, d->reset_bank, BIT(d->reset_pin), &st->reset_save);
		pins_restore(cp, d->mclk_bank, BIT(0), &st->mclk_save);
		st->touched = false;
	}
	if (st->clk)
		cis_clk_off(cp, s);
	if (st->slg) {
		slg_put(cp);
		st->slg = false;
	}
	if (st->bus) {
		i2c_put_adapter(st->bus);
		st->bus = NULL;
	}
	st->powered = false;
	return err;
}

static void sensor_report(struct cam_power *cp, unsigned int s)
{
	const struct sensor *d = &sensors[s];
	unsigned int i;
	u8 v, fault;

	for (i = 0; i < d->nup; i++) {
		const struct rail *r = &rails[d->up[i].rail];

		if (d->up[i].type != RAIL || !r->vsel || slg_read(cp, r->status, &v))
			continue;
		if ((v & (SLG_VOUT_OK | SLG_ILIM)) != SLG_VOUT_OK)
			dev_warn(cp->dev, "%s: %s STATUS %#04x (VOUT_OK bit 1, ILIM bit 0)\n",
				 d->name, r->name, v);
	}
	if (!slg_read(cp, SLG_FAULT_LOG1, &fault) && fault)
		dev_warn(cp->dev, "%s: SLG51002 FAULT_LOG1 %#04x\n", d->name, fault);
}

static int sensor_power_up(struct cam_power *cp, unsigned int s)
{
	const struct sensor *d = &sensors[s];
	struct cam_state *st = &cp->st[s];
	char name[I2C_NAME_SIZE];
	unsigned int i;
	int ret;

	if (st->powered)
		return 0;
	if (!st->available)
		return -ENODEV;
	if (d->experimental && !experimental) {
		dev_err(cp->dev, "%s (%s) is experimental: load with experimental=1\n", d->name,
			d->part);
		return -EPERM;
	}
	snprintf(name, sizeof(name), "Pixel hsi2c_%u", d->bus);
	st->bus = find_adapter(name);
	if (!st->bus) {
		dev_err(cp->dev, "%s: no \"%s\"; load pixel-hsi2c-cam buses=%u first\n", d->name,
			name, d->bus);
		return -ENODEV;
	}
	ret = slg_get(cp);
	if (ret)
		goto fail;
	st->slg = true;
	ret = cis_clk_on(cp, s);
	if (ret)
		goto fail;
	for (i = 0; i < d->nup; i++) {
		const struct step *p = &d->up[i];

		switch (p->type) {
		case RAIL:
			ret = rail_get(cp, p->rail);
			if (!ret)
				st->held |= BIT(p->rail);
			break;
		case RESET:
			reset_set(cp, s, true);
			st->reset = true;
			break;
		case MCLK:
			mclk_set(cp, s, true);
			st->mclk = true;
			break;
		}
		if (ret)
			goto fail;
		if (p->delay_us)
			fsleep(p->delay_us);
	}
	bus_pins_set(cp, s, true);
	fsleep(2000);
	st->powered = true;
	sensor_report(cp, s);
	dev_info(cp->dev, "%s (%s) on: hsi2c_%u 0x%02x, CIS_CLK%u %u Hz\n", d->name, d->part,
		 d->bus, d->addr, d->cis, OSCCLK_HZ);
	return 0;
fail:
	dev_err(cp->dev, "%s power-up failed: %d\n", d->name, ret);
	sensor_undo(cp, s);
	return ret;
}

static int sensor_power_down(struct cam_power *cp, unsigned int s)
{
	int ret;

	if (!cp->st[s].powered)
		return 0;
	ret = sensor_undo(cp, s);
	dev_info(cp->dev, "%s off%s\n", sensors[s].name, ret ? " (with errors)" : "");
	return ret;
}

/* sysfs */

struct cam_attr {
	struct device_attribute attr;
	unsigned int sensor;
};

static ssize_t power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cam_power *cp = dev_get_drvdata(dev);
	unsigned int s = container_of(attr, struct cam_attr, attr)->sensor;

	return sysfs_emit(buf, "%d\n", cp->st[s].powered);
}

static ssize_t power_store(struct device *dev, struct device_attribute *attr, const char *buf,
			   size_t count)
{
	struct cam_power *cp = dev_get_drvdata(dev);
	unsigned int s = container_of(attr, struct cam_attr, attr)->sensor;
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;
	mutex_lock(&cp->lock);
	ret = on ? sensor_power_up(cp, s) : sensor_power_down(cp, s);
	mutex_unlock(&cp->lock);
	return ret ? ret : count;
}

static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cam_power *cp = dev_get_drvdata(dev);
	void __iomem *top = cp->top;
	unsigned int r, s;
	int n = 0;
	u8 v, a, b, c;

	mutex_lock(&cp->lock);
	n += sysfs_emit_at(buf, n, "SLG51002 (hsi2c_8 0x75, ID %#08x): %s", cp->slg_id,
			   cp->slg_users ? "on" : "in reset");
	if (cp->slg_users && !slg_read(cp, SLG_MATRIX_CONF_A, &a) &&
	    !slg_read(cp, SLG_FAULT_LOG1, &b))
		n += sysfs_emit_at(buf, n, ", MATRIX_CONF_A %#04x, FAULT_LOG1 %#04x", a, b);
	n += sysfs_emit_at(buf, n, "\n");
	if (!pmic_read(cp, PM_BANK, L12S_CTRL, &v))
		n += sysfs_emit_at(buf, n, "S2MPG13 LDO12S: CTRL %#04x (%s), users %u%s\n", v,
				   v & L12S_EN ? "on" : "off", cp->rail_users[L12S],
				   cp->l12s_boot_on ? ", on at load: left alone" : "");
	for (r = 0; r < NRAILS; r++) {
		const struct rail *d = &rails[r];

		if (r == L12S)
			continue;
		n += sysfs_emit_at(buf, n, "%s: users %u", d->name, cp->rail_users[r]);
		if (cp->slg_users && d->vsel && !slg_read(cp, d->vsel, &v) &&
		    !slg_read(cp, d->vsel + SLG_MINV, &a) &&
		    !slg_read(cp, d->vsel + SLG_MAXV, &b) && !slg_read(cp, d->status, &c))
			n += sysfs_emit_at(buf, n, ", VSEL %#04x (%u uV, range %#04x-%#04x), STATUS %#04x",
					   v, d->base_uv + d->step_uv * v, a, b, c);
		else if (cp->slg_users && !d->vsel && !slg_read(cp, d->ctrl, &v))
			n += sysfs_emit_at(buf, n, ", CTRL %#04x", v);
		n += sysfs_emit_at(buf, n, "\n");
	}
	for (s = 0; s < NSENSORS; s++) {
		const struct sensor *d = &sensors[s];
		const struct cam_state *st = &cp->st[s];

		n += sysfs_emit_at(buf, n, "%s (%s, %s): %s%s\n", d->name, d->node, d->part,
				   !st->available ? "unavailable" : st->powered ? "on" : "off",
				   d->experimental ? ", experimental" : "");
		n += sysfs_emit_at(buf, n, "  hsi2c_%u 0x%02x, ID register %#06x = %#06x expected\n",
				   d->bus, d->addr, d->id_reg, d->id);
		n += sysfs_emit_at(buf, n, "  CIS_CLK%u: mux %#x div %#x gate %#x qch %#x\n", d->cis,
				   readl(top + CIS_MUX(d->cis)), readl(top + CIS_DIV(d->cis)),
				   readl(top + CIS_GATE(d->cis)), readl(top + CIS_QCH(d->cis)));
		n += sysfs_emit_at(buf, n, "  reset gpp%u-%u: CON %x DAT %u PUD %x; MCLK gpp%u-0: CON %x PUD %x; SCL/SDA gpp%u-0/1: CON %x/%x PUD %x/%x\n",
				   d->reset_bank / 0x20, d->reset_pin,
				   pin_field(cp, d->reset_bank, PIN_CON, d->reset_pin),
				   (readl(cp->pins + d->reset_bank + PIN_DAT) >> d->reset_pin) & 1,
				   pin_field(cp, d->reset_bank, PIN_PUD, d->reset_pin),
				   d->mclk_bank / 0x20, pin_field(cp, d->mclk_bank, PIN_CON, 0),
				   pin_field(cp, d->mclk_bank, PIN_PUD, 0), d->bus_bank / 0x20,
				   pin_field(cp, d->bus_bank, PIN_CON, 0),
				   pin_field(cp, d->bus_bank, PIN_CON, 1),
				   pin_field(cp, d->bus_bank, PIN_PUD, 0),
				   pin_field(cp, d->bus_bank, PIN_PUD, 1));
	}
	mutex_unlock(&cp->lock);
	return n;
}
static DEVICE_ATTR_RO(status);

static struct attribute *cam_attrs[] = {
	&dev_attr_status.attr,
	NULL,
};

static const struct attribute_group cam_group = {
	.attrs = cam_attrs,
};

#define SENSOR_GROUP(_s, _name)							\
	static struct cam_attr _name##_power = {					\
		__ATTR(power, 0644, power_show, power_store), _s			\
	};									\
	static struct attribute *_name##_attrs[] = { &_name##_power.attr.attr, NULL };	\
	static const struct attribute_group _name##_group = {			\
		.name = #_name, .attrs = _name##_attrs,				\
	}

SENSOR_GROUP(UW, uw);
SENSOR_GROUP(FRONT, front);
SENSOR_GROUP(MAIN, main);
SENSOR_GROUP(TELE, tele);

static const struct attribute_group *cam_groups[] = {
	&cam_group, &uw_group, &front_group, &main_group, &tele_group, NULL,
};

/* Driver */

/* Each sensor's pins must be inputs, as the bootloader leaves them. */
static void sensor_check(struct cam_power *cp, unsigned int s)
{
	const struct sensor *d = &sensors[s];
	struct cam_state *st = &cp->st[s];
	u32 rst = pin_field(cp, d->reset_bank, PIN_CON, d->reset_pin);
	u32 mclk = pin_field(cp, d->mclk_bank, PIN_CON, 0);
	u32 scl = pin_field(cp, d->bus_bank, PIN_CON, 0);
	u32 sda = pin_field(cp, d->bus_bank, PIN_CON, 1);

	pins_save(cp, d->reset_bank, BIT(d->reset_pin), &st->reset_save);
	pins_save(cp, d->mclk_bank, BIT(0), &st->mclk_save);
	pins_save(cp, d->bus_bank, BUS_PINS, &st->bus_save);
	st->available = rst == FN_INPUT && mclk == FN_INPUT && scl == FN_INPUT && sda == FN_INPUT;
	if (!st->available)
		dev_err(cp->dev, "%s left alone: pin functions reset %x, MCLK %x, SCL/SDA %x/%x (expected inputs)\n",
			d->name, rst, mclk, scl, sda);
}

static int cam_power_probe(struct platform_device *pd)
{
	struct device *dev = &pd->dev;
	struct device_node *np;
	struct cam_power *cp;
	unsigned int i;
	int ret;
	u8 v;

	cp = devm_kzalloc(dev, sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return -ENOMEM;
	cp->dev = dev;
	ret = devm_mutex_init(dev, &cp->lock);
	if (ret)
		return ret;
	np = of_find_node_by_path("/power-management");
	cp->acpm = np ? devm_acpm_get_by_node(dev, np) : ERR_PTR(-ENODEV);
	of_node_put(np);
	if (IS_ERR(cp->acpm))
		return dev_err_probe(dev, PTR_ERR(cp->acpm), "no ACPM\n");
	cp->top = devm_ioremap(dev, CMU_TOP, 0x4000);
	cp->pins = devm_ioremap(dev, PINCTRL_PERIC0, 0x200);
	if (!cp->top || !cp->pins)
		return -ENOMEM;

	/* L12S at the bootloader's 1.8 V; if it is already on, it stays as it is. */
	ret = pmic_read(cp, PM_BANK, L12S_CTRL, &v);
	if (ret)
		return ret;
	if ((v & L12S_VSEL) != L12S_1V8) {
		dev_err(dev, "LDO12S CTRL %#04x: not 1.8 V\n", v);
		return -ERANGE;
	}
	cp->l12s_boot_on = v & L12S_EN;

	/* The SLG51002 enable lines: outputs, low, as the bootloader leaves them. */
	for (i = 0; i < ARRAY_SIZE(slg_lines); i++) {
		ret = pmic_read(cp, GPIO_BANK, GPIO_SET(slg_lines[i].gpio), &v);
		if (ret)
			return ret;
		if ((v & GPIO_OE) && (v & GPIO_VAL) && takeover) {
			cp->line_up[i] = true;
		} else if (!(v & GPIO_OE) || (v & GPIO_VAL)) {
			dev_err(dev, "S2MPG13 %s SET %#04x, expected a low output (pixel-cam-pmic loaded, or left on by a warm reboot: takeover=1)\n",
				slg_lines[i].name, v);
			return -EBUSY;
		}
	}
	/* takeover=1: back to the bootloader's state, SLG51002 in reset and LDO12S off. */
	if (takeover) {
		dev_warn(dev, "takeover: SLG51002 lines %d%d%d, LDO12S %s at load; switching off\n",
			 cp->line_up[0], cp->line_up[1], cp->line_up[2],
			 cp->l12s_boot_on ? "on" : "off");
		slg_lines_down(cp);
		if (cp->line_up[0] || cp->line_up[1] || cp->line_up[2])
			return -EIO;
		if (cp->l12s_boot_on) {
			ret = l12s_set(cp, false);
			if (ret)
				return ret;
			cp->l12s_boot_on = false;
		}
	}

	cp->adap8 = find_adapter("Pixel hsi2c_8");
	if (!cp->adap8) {
		dev_err(dev, "no \"Pixel hsi2c_8\": load pixel-hsi2c buses=15,8 first\n");
		return -ENODEV;
	}
	cp->slg = i2c_new_dummy_device(cp->adap8, SLG_ADDR);
	if (IS_ERR(cp->slg)) {
		ret = PTR_ERR(cp->slg);
		goto put;
	}

	/* Check the chip now; it goes back into reset until a sensor needs it. */
	ret = slg_get(cp);
	if (ret)
		goto unregister;
	slg_put(cp);

	for (i = 0; i < NSENSORS; i++)
		sensor_check(cp, i);
	platform_set_drvdata(pd, cp);
	dev_info(dev, "SLG51002 rev AB on hsi2c_8, LDO12S %s at load; sensors: uw%s\n",
		 cp->l12s_boot_on ? "on" : "off",
		 experimental ? ", front, main, tele (experimental)" : "");
	return 0;
unregister:
	i2c_unregister_device(cp->slg);
put:
	i2c_put_adapter(cp->adap8);
	return ret;
}

static void cam_power_remove(struct platform_device *pd)
{
	struct cam_power *cp = platform_get_drvdata(pd);
	unsigned int s = NSENSORS;

	mutex_lock(&cp->lock);
	while (s--)
		sensor_power_down(cp, s);
	if (WARN_ON(cp->slg_users)) {
		cp->slg_users = 1;
		slg_put(cp);
	}
	slg_lines_down(cp);
	if (WARN_ON(cp->rail_users[L12S]) && !cp->l12s_boot_on)
		l12s_set(cp, false);
	mutex_unlock(&cp->lock);
	i2c_unregister_device(cp->slg);
	i2c_put_adapter(cp->adap8);
}

static struct platform_driver cam_power_driver = {
	.probe = cam_power_probe,
	.remove = cam_power_remove,
	.driver = {
		.name = "pixel-camera-power",
		.dev_groups = cam_groups,
	},
};

static int __init cam_power_init(void)
{
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	ret = platform_driver_register(&cam_power_driver);
	if (ret)
		return ret;
	pdev = platform_device_register_simple("pixel-camera-power", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&cam_power_driver);
		return PTR_ERR(pdev);
	}
	if (!pdev->dev.driver) {
		platform_device_unregister(pdev);
		platform_driver_unregister(&cam_power_driver);
		return -ENODEV;
	}
	return 0;
}
module_init(cam_power_init);

static void __exit cam_power_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&cam_power_driver);
}
module_exit(cam_power_exit);

MODULE_DESCRIPTION("Pixel 7 Pro camera power: S2MPG13 LDO12S, SLG51002, MCLK, resets and bus pins");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: pixel-hsi2c pixel-hsi2c-cam");
