// SPDX-License-Identifier: GPL-2.0-only
/* Pixel 7 Pro battery state and charge control over hsi2c_13.
 *
 * The bootloader leaves PERIC1 USI13 in I2C mode (sysreg_peric1 SW_CONF 4),
 * with gpp25-0/1 on the I2C function, its clocks running and the HSI2C in auto
 * mode with 400 kHz timings; it uses the bus itself for the MAX77759. This
 * module drives transfers on that setup, polled, with the mainline
 * i2c-exynos5 register sequence (Exynos7 interrupt layout, 64-byte FIFOs). It
 * never changes the clocks, pins or timings, and refuses to load if the
 * handoff differs.
 *
 * The bus is registered as an I2C adapter for later drivers. Two supplies read
 * the MAX77759:
 * - "battery": the ModelGauge m5 fuel gauge at 0x36 (5 mOhm sense resistor,
 *   per the stock DT), and the charger's view of the charge state.
 * - "usb": the charger at 0x69, for whether USB input is present and its limit.
 *
 * A guarded restore_model sysfs operation can restore the stock gauge model
 * after POR, using the read-only battery ID and the bootloader's device tree.
 * Temperature is computed from AIN with the stock NTC constants, including
 * before restoration when Config.TEx leaves Temp at its host-written default.
 *
 * The charger's float voltage and charge current follow Google's step
 * charging, from the google,battery tables in the bootloader's device tree
 * (see charger_step()). The bootloader leaves a 4.35 V float, which stops a
 * 4.45 V cell at about 92 %. The charge current never goes above what the
 * bootloader set. The module also changes two more things, as stock does:
 * - the USB input current limit (CHG_CNFG_09). The bootloader leaves it under
 *   the charger's own port detection (AutoIBUS), which holds a computer port
 *   to 500 mA. input_limit_ma, 1500 by default as stock sets on this host's
 *   BC1.2 charging port, is applied with NO_AUTOIBUS set, as Google's stack
 *   does once its BC1.2 detection is done. The charger's input voltage
 *   regulation (AICL) backs off if a weaker port sags.
 * - the mode (CHG_CNFG_00), between charge + buck (5) and buck only (4, the
 *   system runs from USB and the battery rests), for the charge limit
 *   (charge_control_end_threshold, restart 5 % below it).
 * Charging also pauses at 45 C (resuming at 40 C) on that temperature; the
 * charger's own JEITA control on the same thermistor stays enabled.
 * Unloading restores all four registers to what the bootloader left.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#define PERIC1_HSI2C13		0x10d60000
#define PERIC1_CMU		0x10c00000
#define CMU_USI13_PCLK		0x2054
#define CMU_USI13_QCH		0x3048
#define GATE_MANUAL		BIT(20)
#define GATE_CG_VAL		BIT(21)
#define QCH_ENABLE		BIT(0)
#define QCH_CLOCK_REQ		BIT(1)
#define SYSREG_PERIC1_USI13	0x10c21014
#define USI_SW_CONF_I2C		0x4
#define GPP25_CON		0x10c400a0
#define GPP25_I2C		0x22

/* i2c-exynos5.c */
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
#define HSI2C_READ_WRITE	BIT(16)
#define HSI2C_STOP_AFTER_TRANS	BIT(17)
#define HSI2C_MASTER_RUN	BIT(31)
#define HSI2C_CMD_SEND_STOP	BIT(2)
#define HSI2C_MASTER_ST_MASK	0xf
#define HSI2C_MASTER_ST_LOSE	0xc
#define HSI2C_SLV_ADDR_MAS(x)	(((x) & 0x3ff) << 10)
#define HSI2C_FIFO_DEPTH	64

#define FG_ADDR			0x36
#define FG_STATUS		0x00
#define FG_REPCAP		0x05
#define FG_REPSOC		0x06
#define FG_VCELL		0x09
#define FG_CURRENT		0x0a
#define FG_AVGCURRENT		0x0b
#define FG_FULLCAPREP		0x35
#define FG_TASKPERIOD		0x3c
#define FG_TTE			0x11
#define FG_DESIGNCAP		0x18
#define FG_AVGVCELL		0x19
#define FG_TTF			0x20
#define FG_DEVNAME		0x21
#define FG_DEVNAME_MAX77759	0x6200
#define FG_STATUS_POR		BIT(1)
/* 5.625 s per time LSB; current and charge scale with the 5 mOhm resistor */

#define CHG_ADDR		0x69
#define CHG_INT_OK		0xb4
#define CHG_DETAILS_00		0xb5
#define CHGIN_DTLS		GENMASK(6, 5)
#define CHGIN_DTLS_VALID	3
#define CHG_DETAILS_01		0xb6
#define BAT_DTLS		GENMASK(6, 4)
#define CHG_DTLS		GENMASK(3, 0)
#define CHG_DETAILS_02		0xb7
#define CHG_CNFG_00		0xb9
#define CHG_MODE		GENMASK(3, 0)
#define CHG_MODE_OFF		0x0
#define CHG_MODE_BUCK		0x4
#define CHG_MODE_CHG_BUCK	0x5
#define CHG_CNFG_02		0xbb
#define CHG_CC			GENMASK(5, 0)
#define CHG_CNFG_04		0xbd
#define CHG_CV_PRM		GENMASK(5, 0)
#define CHG_CNFG_06		0xbf
#define CHGPROT			GENMASK(3, 2)
#define CHGPROT_UNLOCKED	3
#define CHG_CNFG_09		0xc2
#define NO_AUTOIBUS		BIT(7)
#define CHGIN_ILIM		GENMASK(6, 0)
#define CHG_CNFG_19		0xcc

#define FG_AIN			0x27
#define FG_QH			0x4d	/* coulomb counter, capacity LSB */
#define FG_QL			0x4e	/* ... and its low 16 bits */
/* TGain and TOff of every battery model in the stock DT (fg-params) */
#define NTC_TGAIN		(-4783)
#define NTC_TOFF		7866
#define TEMP_STOP		450
#define TEMP_RESUME		400

/* google,battery step charging: STEP_TEMPS temperature limits bound the bands,
 * each band has a charge current per voltage tier, as a percentage of the
 * capacity. Never float a cell above FV_CEILING_UV, whatever the DT says.
 */
#define STEP_TEMPS		7
#define STEP_BANDS		(STEP_TEMPS - 1)
#define STEP_TIERS		3
#define STEP_MARGIN_UV		10000
#define FV_CEILING_UV		4450000

#define CHG_DETAILS_03		0xb8
#define THM_DTLS		GENMASK(2, 0)
enum { THM_COLD_SUSPEND, THM_COOL, THM_NORMAL, THM_WARM, THM_HOT_SUSPEND };

static uint input_limit_ma = 1500;
module_param(input_limit_ma, uint, 0644);
static bool input_off;
module_param(input_off, bool, 0644);
MODULE_PARM_DESC(input_off, "Measurement: charger mode 0 with USB present, so the system runs from the battery and the gauge reads its drain (clear to resume)");
MODULE_PARM_DESC(input_limit_ma, "USB input current limit to set when USB is present (100-1500, 0: leave)");

enum { CHG_DTLS_PREQUAL, CHG_DTLS_CC, CHG_DTLS_CV, CHG_DTLS_TO, CHG_DTLS_DONE,
       CHG_DTLS_TIMER_FAULT = 6, CHG_DTLS_SUSP_BATT_THM, CHG_DTLS_OFF,
       CHG_DTLS_OFF_WDOG = 11, CHG_DTLS_SUSP_JEITA };
enum { BAT_DTLS_NO_BATT, BAT_DTLS_DEAD, BAT_DTLS_TIMER_FAULT, BAT_DTLS_OKAY,
       BAT_DTLS_UNDERVOLTAGE, BAT_DTLS_OVERVOLTAGE, BAT_DTLS_OVERCURRENT,
       BAT_DTLS_ONLY };

struct pixel_battery {
	struct device *dev;
	void __iomem *regs;
	struct i2c_adapter adap;
	u32 saved[9];
	struct power_supply *battery;
	struct power_supply *usb;
	struct delayed_work poll;
	struct mutex lock;
	int last_status;
	int last_capacity;
	bool last_online;
	u8 boot_cnfg_00;
	u8 boot_cnfg_02;
	u8 boot_cnfg_04;
	u8 boot_cnfg_09;
	struct {
		s32 temp[STEP_TEMPS];
		u32 cv_uv[STEP_TIERS];
		u32 cc_pct[STEP_BANDS][STEP_TIERS];
		u32 fv_max_uv;
		bool valid;
	} step;
	int tier;
	bool cold;
	int end_threshold;
	bool limited;
	bool hot;
	bool forced_off;	/* input_off put the charger in mode 0 */
	bool model_failed;
};

static const u16 saved_regs[] = {
	HSI2C_CTL, HSI2C_TRAILIG_CTL, HSI2C_CONF, HSI2C_TIMEOUT, HSI2C_TIMING_FS1,
	HSI2C_TIMING_FS2, HSI2C_TIMING_FS3, HSI2C_TIMING_SLA, HSI2C_FIFO_CTL,
};

/* exynos5_i2c_reset(), restoring the bootloader's configuration instead of
 * recomputing timings from a clock rate this kernel does not know.
 */
static void hsi2c_reset(struct pixel_battery *b)
{
	unsigned int i;
	u32 ctl = readl(b->regs + HSI2C_CTL);

	writel(ctl | HSI2C_SW_RST, b->regs + HSI2C_CTL);
	writel(ctl & ~HSI2C_SW_RST, b->regs + HSI2C_CTL);
	for (i = 0; i < ARRAY_SIZE(saved_regs); i++)
		writel(b->saved[i], b->regs + saved_regs[i]);
}

/* exynos5_i2c_bus_check(): a lost arbitration state before a transfer means
 * SDA is held low; clock it out with stop commands.
 */
static void hsi2c_bus_check(struct pixel_battery *b)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(100);

	while (time_before(jiffies, timeout)) {
		u32 st = readl(b->regs + HSI2C_TRANS_STATUS);

		if ((st & HSI2C_MASTER_ST_MASK) != HSI2C_MASTER_ST_LOSE)
			return;
		writel(HSI2C_CMD_SEND_STOP, b->regs + HSI2C_MANUAL_CMD);
		usleep_range(100, 200);
	}
}

/* exynos5_i2c_message_start() plus the polled interrupt handling. */
static int hsi2c_message(struct pixel_battery *b, struct i2c_msg *msg, bool stop)
{
	bool read = msg->flags & I2C_M_RD;
	u32 ctl, fifo_ctl, int_en, auto_conf, st, fifo;
	unsigned long timeout;
	unsigned int ptr = 0, trig;
	bool done = false;

	if (!msg->len || msg->len > 0xffff || (msg->flags & I2C_M_TEN))
		return -EOPNOTSUPP;
	ctl = readl(b->regs + HSI2C_CTL) & ~(HSI2C_TXCHON | HSI2C_RXCHON);
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
	writel(HSI2C_SLV_ADDR_MAS(msg->addr), b->regs + HSI2C_ADDR);
	writel(fifo_ctl, b->regs + HSI2C_FIFO_CTL);
	writel(ctl, b->regs + HSI2C_CTL);
	hsi2c_bus_check(b);
	writel(readl(b->regs + HSI2C_INT_STATUS), b->regs + HSI2C_INT_STATUS);
	writel(int_en, b->regs + HSI2C_INT_ENABLE);
	if (stop)
		auto_conf |= HSI2C_STOP_AFTER_TRANS;
	writel(auto_conf | msg->len | HSI2C_MASTER_RUN, b->regs + HSI2C_AUTO_CONF);

	timeout = jiffies + msecs_to_jiffies(100);
	for (;;) {
		st = readl(b->regs + HSI2C_INT_STATUS);
		writel(st, b->regs + HSI2C_INT_STATUS);
		fifo = readl(b->regs + HSI2C_FIFO_STATUS);
		if (read) {
			unsigned int lvl = HSI2C_RX_FIFO_LVL(fifo);

			while (lvl-- && ptr < msg->len)
				msg->buf[ptr++] = readl(b->regs + HSI2C_RX_DATA);
		} else {
			unsigned int room = HSI2C_FIFO_DEPTH - HSI2C_TX_FIFO_LVL(fifo);

			while (room-- && ptr < msg->len)
				writel(msg->buf[ptr++], b->regs + HSI2C_TX_DATA);
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
	writel(0, b->regs + HSI2C_INT_ENABLE);

	if (st & (HSI2C_INT_NO_DEV_ACK | HSI2C_INT_NO_DEV))
		return -ENXIO;
	if (st & HSI2C_INT_TRANS_ABORT)
		return -EAGAIN;
	if (!(done && ptr == msg->len)) {
		dev_warn_ratelimited(b->dev, "transfer to %#x timed out (int %#x, trans %#x)\n",
				     msg->addr, st, readl(b->regs + HSI2C_TRANS_STATUS));
		hsi2c_reset(b);
		return -ETIMEDOUT;
	}
	return 0;
}

static int hsi2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct pixel_battery *b = i2c_get_adapdata(adap);
	int i, ret;

	for (i = 0; i < num; i++) {
		ret = hsi2c_message(b, &msgs[i], i == num - 1);
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

static int fg_read(struct pixel_battery *b, u8 reg, u16 *val)
{
	u8 data[2];
	struct i2c_msg msgs[] = {
		{ .addr = FG_ADDR, .len = 1, .buf = &reg },
		{ .addr = FG_ADDR, .flags = I2C_M_RD, .len = 2, .buf = data },
	};
	int ret = i2c_transfer(&b->adap, msgs, 2);

	if (ret != 2)
		return ret < 0 ? ret : -EIO;
	*val = data[0] | data[1] << 8;
	return 0;
}

static int chg_read(struct pixel_battery *b, u8 reg, u8 *val)
{
	struct i2c_msg msgs[] = {
		{ .addr = CHG_ADDR, .len = 1, .buf = &reg },
		{ .addr = CHG_ADDR, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	int ret = i2c_transfer(&b->adap, msgs, 2);

	return ret == 2 ? 0 : ret < 0 ? ret : -EIO;
}

static int chg_write(struct pixel_battery *b, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	struct i2c_msg msg = { .addr = CHG_ADDR, .len = 2, .buf = buf };
	u8 check;
	int ret = i2c_transfer(&b->adap, &msg, 1);

	if (ret != 1)
		return ret < 0 ? ret : -EIO;
	ret = chg_read(b, reg, &check);
	if (!ret && check != val)
		ret = -EIO;
	if (ret)
		dev_err(b->dev, "charger %#x <- %#x failed (%d)\n", reg, val, ret);
	return ret;
}

static bool usb_online(struct pixel_battery *b)
{
	u8 val;

	return !chg_read(b, CHG_DETAILS_00, &val) &&
	       FIELD_GET(CHGIN_DTLS, val) == CHGIN_DTLS_VALID;
}

static int battery_temp(struct pixel_battery *b, int *deci_c)
{
	u16 ain;
	int ret = fg_read(b, FG_AIN, &ain);

	if (ret)
		return ret;
	*deci_c = (int)((((s64)ain * NTC_TGAIN) >> 16) + NTC_TOFF) * 10 / 256;
	return 0;
}

static int battery_status(struct pixel_battery *b)
{
	u8 val;
	u16 cur;

	if (!chg_read(b, CHG_DETAILS_01, &val)) {
		switch (FIELD_GET(CHG_DTLS, val)) {
		case CHG_DTLS_PREQUAL:
		case CHG_DTLS_CC:
		case CHG_DTLS_CV:
		case CHG_DTLS_TO:
			/* charging, unless the load outruns the input limit */
			if (!fg_read(b, FG_AVGCURRENT, &cur) && (s16)cur < 0)
				return POWER_SUPPLY_STATUS_DISCHARGING;
			return POWER_SUPPLY_STATUS_CHARGING;
		case CHG_DTLS_DONE:
			return POWER_SUPPLY_STATUS_FULL;
		case CHG_DTLS_OFF:
			if (!usb_online(b))
				return POWER_SUPPLY_STATUS_DISCHARGING;
			if (b->limited || b->hot || b->cold)
				return POWER_SUPPLY_STATUS_NOT_CHARGING;
			fallthrough;
		case CHG_DTLS_TIMER_FAULT:
		case CHG_DTLS_SUSP_BATT_THM:
		case CHG_DTLS_OFF_WDOG:
		case CHG_DTLS_SUSP_JEITA:
			return POWER_SUPPLY_STATUS_NOT_CHARGING;
		}
	}
	if (!fg_read(b, FG_CURRENT, &cur))
		return (s16)cur > 0 ? POWER_SUPPLY_STATUS_CHARGING :
					POWER_SUPPLY_STATUS_DISCHARGING;
	return POWER_SUPPLY_STATUS_UNKNOWN;
}

static int battery_health(struct pixel_battery *b)
{
	u8 val;

	if (b->hot)
		return POWER_SUPPLY_HEALTH_OVERHEAT;
	if (b->cold)
		return POWER_SUPPLY_HEALTH_COLD;
	if (!chg_read(b, CHG_DETAILS_03, &val)) {
		switch (FIELD_GET(THM_DTLS, val)) {
		case THM_COLD_SUSPEND:
			return POWER_SUPPLY_HEALTH_COLD;
		case THM_COOL:
			return POWER_SUPPLY_HEALTH_COOL;
		case THM_WARM:
			return POWER_SUPPLY_HEALTH_WARM;
		case THM_HOT_SUSPEND:
			return POWER_SUPPLY_HEALTH_OVERHEAT;
		}
	}
	if (chg_read(b, CHG_DETAILS_01, &val))
		return POWER_SUPPLY_HEALTH_UNKNOWN;
	switch (FIELD_GET(BAT_DTLS, val)) {
	case BAT_DTLS_DEAD:
		return POWER_SUPPLY_HEALTH_DEAD;
	case BAT_DTLS_TIMER_FAULT:
		return POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE;
	case BAT_DTLS_UNDERVOLTAGE:
		return POWER_SUPPLY_HEALTH_UNDERVOLTAGE;
	case BAT_DTLS_OVERVOLTAGE:
		return POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	case BAT_DTLS_OVERCURRENT:
		return POWER_SUPPLY_HEALTH_OVERCURRENT;
	case BAT_DTLS_OKAY:
	case BAT_DTLS_ONLY:
		return POWER_SUPPLY_HEALTH_GOOD;
	}
	return POWER_SUPPLY_HEALTH_UNKNOWN;
}

/* 5 mOhm sense resistor: current 312.5 uA. Charge units depend on TaskPeriod. */
static int fg_current_ua(u16 val)
{
	return (s16)val * 625 / 2;
}

static int fg_charge_uah(struct pixel_battery *b, u16 val, int *uah)
{
	u16 period;
	int ret = fg_read(b, FG_TASKPERIOD, &period);

	if (ret)
		return ret;
	if (period != 0x1680 && period != 0x2d00)
		return -ENODATA;
	*uah = val * (period == 0x2d00 ? 2000 : 1000);
	return 0;
}

/* The gauge's coulomb counter QH:QL, 1/65536 of the capacity LSB, as the
 * raw value and in nAh: fine enough to measure minutes of sleep, where
 * charge_now (RepCap) moves in 2 mAh steps.
 */
static struct pixel_battery *coulomb_battery;

static int coulomb_get(char *buf, const struct kernel_param *kp)
{
	struct pixel_battery *b = READ_ONCE(coulomb_battery);
	u16 qh, ql, qh2, period;
	s32 raw;
	int ret;

	if (!b)
		return -ENODEV;
	ret = fg_read(b, FG_QH, &qh);
	ret = ret ?: fg_read(b, FG_QL, &ql);
	ret = ret ?: fg_read(b, FG_QH, &qh2);
	if (!ret && qh2 != qh) {
		qh = qh2;
		ret = fg_read(b, FG_QL, &ql);
	}
	ret = ret ?: fg_read(b, FG_TASKPERIOD, &period);
	if (ret)
		return ret;
	if (period != 0x1680 && period != 0x2d00)
		return -ENODATA;
	raw = (s32)((u32)qh << 16 | ql);
	return sysfs_emit(buf, "%d %lld\n", raw,
			  div_s64((s64)raw * (period == 0x2d00 ? 2000000 : 1000000), 65536));
}

static const struct kernel_param_ops coulomb_ops = { .get = coulomb_get };
module_param_cb(coulomb, &coulomb_ops, NULL, 0444);
MODULE_PARM_DESC(coulomb, "Coulomb counter QH:QL: raw and nAh (read-only)");

static int fg_time(struct pixel_battery *b, u8 reg, int *seconds)
{
	u16 val;
	int ret = fg_read(b, reg, &val);

	if (ret)
		return ret;
	if (val == 0xffff)
		return -ENODATA;
	*seconds = val * 5625 / 1000;
	return 0;
}

static int battery_get_locked(struct power_supply *psy, enum power_supply_property psp,
		       union power_supply_propval *out)
{
	struct pixel_battery *b = power_supply_get_drvdata(psy);
	u16 val;
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		out->intval = battery_status(b);
		return 0;
	case POWER_SUPPLY_PROP_HEALTH:
		out->intval = battery_health(b);
		return 0;
	case POWER_SUPPLY_PROP_PRESENT:
		out->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		out->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG:
		return fg_time(b, FG_TTE, &out->intval);
	case POWER_SUPPLY_PROP_TIME_TO_FULL_AVG:
		return fg_time(b, FG_TTF, &out->intval);
	default:
		break;
	}

	switch (psp) {
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = fg_read(b, FG_REPSOC, &val);
		out->intval = clamp((val + 128) >> 8, 0, 100);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = fg_read(b, FG_VCELL, &val);
		out->intval = val * 625 / 8;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		ret = fg_read(b, FG_AVGVCELL, &val);
		out->intval = val * 625 / 8;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = fg_read(b, FG_CURRENT, &val);
		out->intval = fg_current_ua(val);
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		ret = fg_read(b, FG_AVGCURRENT, &val);
		out->intval = fg_current_ua(val);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = battery_temp(b, &out->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = fg_read(b, FG_REPCAP, &val);
		if (!ret)
			ret = fg_charge_uah(b, val, &out->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = fg_read(b, FG_FULLCAPREP, &val);
		if (!ret)
			ret = fg_charge_uah(b, val, &out->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		ret = fg_read(b, FG_DESIGNCAP, &val);
		if (!ret)
			ret = fg_charge_uah(b, val, &out->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		out->intval = b->end_threshold;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD:
		out->intval = b->end_threshold < 100 ? b->end_threshold - 5 : 100;
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

static int battery_get(struct power_supply *psy, enum power_supply_property psp,
		       union power_supply_propval *out)
{
	struct pixel_battery *b = power_supply_get_drvdata(psy);
	int ret;

	mutex_lock(&b->lock);
	ret = battery_get_locked(psy, psp, out);
	mutex_unlock(&b->lock);
	return ret;
}

static const enum power_supply_property battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD,
};

static void charger_update(struct pixel_battery *b);

static int battery_set(struct power_supply *psy, enum power_supply_property psp,
		       const union power_supply_propval *in)
{
	struct pixel_battery *b = power_supply_get_drvdata(psy);

	if (psp != POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD)
		return -EINVAL;
	if (in->intval < 50 || in->intval > 100)
		return -EINVAL;
	b->end_threshold = in->intval;
	charger_update(b);
	power_supply_changed(b->battery);
	return 0;
}

static int battery_writeable(struct power_supply *psy, enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD;
}

static const struct power_supply_desc battery_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = battery_props,
	.num_properties = ARRAY_SIZE(battery_props),
	.get_property = battery_get,
	.set_property = battery_set,
	.property_is_writeable = battery_writeable,
};

/* The step-charging tables from the bootloader's device tree. Anything
 * unexpected leaves the bootloader's float voltage and charge current alone.
 */
static void step_profile_read(struct pixel_battery *b)
{
	struct device_node *bat = of_find_node_by_path("/google,battery");
	struct device_node *chg = of_find_node_by_path("/google,charger");
	unsigned int i, j;
	bool ok;

	ok = bat && chg &&
	     of_property_count_u32_elems(bat, "google,chg-temp-limits") == STEP_TEMPS &&
	     of_property_count_u32_elems(bat, "google,chg-cv-limits") == STEP_TIERS &&
	     of_property_count_u32_elems(bat, "google,chg-cc-limits") == STEP_BANDS * STEP_TIERS &&
	     !of_property_read_u32_array(bat, "google,chg-temp-limits", (u32 *)b->step.temp,
					 STEP_TEMPS) &&
	     !of_property_read_u32_array(bat, "google,chg-cv-limits", b->step.cv_uv, STEP_TIERS) &&
	     !of_property_read_u32_array(bat, "google,chg-cc-limits", &b->step.cc_pct[0][0],
					 STEP_BANDS * STEP_TIERS) &&
	     !of_property_read_u32(chg, "google,fv-max-uv", &b->step.fv_max_uv);
	of_node_put(bat);
	of_node_put(chg);
	/* no charging below 0 C, ascending limits, floats from 4.0 V to the ceiling */
	ok = ok && b->step.temp[0] >= 0 && b->step.fv_max_uv >= 4000000 &&
	     b->step.fv_max_uv <= FV_CEILING_UV;
	for (i = 1; ok && i < STEP_TEMPS; i++)
		ok = b->step.temp[i] > b->step.temp[i - 1];
	for (j = 0; ok && j < STEP_TIERS; j++)
		ok = b->step.cv_uv[j] >= 4000000 && b->step.cv_uv[j] <= FV_CEILING_UV &&
		     (!j || b->step.cv_uv[j] > b->step.cv_uv[j - 1]);
	for (i = 0; ok && i < STEP_BANDS; i++)
		for (j = 0; ok && j < STEP_TIERS; j++)
			ok = b->step.cc_pct[i][j] <= 150;
	b->step.valid = ok;
	if (ok)
		dev_info(b->dev, "step charging to %u mV, %d.%d-%d.%d C\n",
			 min(b->step.cv_uv[STEP_TIERS - 1], b->step.fv_max_uv) / 1000,
			 b->step.temp[0] / 10, b->step.temp[0] % 10,
			 b->step.temp[STEP_TEMPS - 1] / 10, b->step.temp[STEP_TEMPS - 1] % 10);
	else
		dev_warn(b->dev, "no usable step-charging tables: keeping the bootloader's %s\n",
			 "float voltage and charge current");
}

/* Google's step charging, simplified. The temperature picks a band and the
 * cell voltage a tier. Each tier charges at its band's current up to the
 * tier's voltage, then the next tier takes over. Tiers only rise until USB is
 * removed; a tier with no current in this band holds the one below at its
 * voltage. The current never exceeds the bootloader's. Returns false when the
 * band allows no charging at all; *raised says the float voltage went up.
 */
static bool charger_step(struct pixel_battery *b, int temp, bool *raised)
{
	int band, top, tier, cap_uah;
	u32 vcell_uv, fv, cc_ua;
	u8 cnfg, fv_code, cc_code;
	u16 vcell, cap;
	bool changed = false;

	*raised = false;
	if (!b->step.valid)
		return true;
	for (band = STEP_BANDS - 1; band >= 0; band--)
		if (temp >= b->step.temp[band])
			break;
	if (band < 0 || temp >= b->step.temp[STEP_TEMPS - 1] || !b->step.cc_pct[band][0]) {
		if (!b->cold)
			dev_warn(b->dev, "battery at %d.%d C: outside the charging range\n",
				 temp / 10, abs(temp % 10));
		b->cold = true;
		return false;
	}
	b->cold = false;
	for (top = 0; top + 1 < STEP_TIERS && b->step.cc_pct[band][top + 1]; top++)
		;
	if (fg_read(b, FG_VCELL, &vcell) || fg_read(b, FG_DESIGNCAP, &cap) ||
	    fg_charge_uah(b, cap, &cap_uah))
		return true;
	vcell_uv = vcell * 625 / 8;
	while (b->tier + 1 < STEP_TIERS && vcell_uv + STEP_MARGIN_UV >= b->step.cv_uv[b->tier])
		b->tier++;
	tier = min(b->tier, top);
	fv = min(b->step.cv_uv[tier], b->step.fv_max_uv);
	fv_code = (fv - 4000000) / 10000;
	cc_ua = (u64)cap_uah * b->step.cc_pct[band][tier] / 100;
	/* 200 mA at code 3, then 66.67 mA steps */
	cc_code = cc_ua > 200000 ? 3 + (cc_ua - 200000) / 66670 : 3;
	cc_code = min(cc_code, FIELD_GET(CHG_CC, b->boot_cnfg_02));
	/* the current comes down before the voltage goes up */
	if (!chg_read(b, CHG_CNFG_02, &cnfg) && FIELD_GET(CHG_CC, cnfg) != cc_code &&
	    !chg_write(b, CHG_CNFG_02, (cnfg & ~CHG_CC) | cc_code))
		changed = true;
	if (!chg_read(b, CHG_CNFG_04, &cnfg) && FIELD_GET(CHG_CV_PRM, cnfg) != fv_code &&
	    !chg_write(b, CHG_CNFG_04, (cnfg & ~CHG_CV_PRM) | fv_code)) {
		*raised = fv_code > FIELD_GET(CHG_CV_PRM, cnfg);
		changed = true;
	}
	if (changed)
		dev_info(b->dev, "step charging at %d.%d C, %u mV: tier %d, float %u mV, %u mA\n",
			 temp / 10, abs(temp % 10), vcell_uv / 1000, tier, fv / 1000,
			 200 + (cc_code - 3) * 6667 / 100);
	return true;
}

/* The charger's input limit and mode, from USB presence, the requested
 * input limit and the charge limit. Only modes 4 and 5 are ever changed:
 * charging off (0) or OTG boost stay as they are.
 */
static void charger_update(struct pixel_battery *b)
{
	u8 cnfg, mode, want, ilim, dtls;
	u16 soc;
	int capacity, temp;
	bool in_range, raised;

	mutex_lock(&b->lock);
	if (!usb_online(b)) {
		b->tier = 0;
		goto out;
	}
	if (input_limit_ma && !chg_read(b, CHG_CNFG_09, &cnfg)) {
		ilim = 3 + (clamp(input_limit_ma, 100U, 1500U) - 100) / 25;
		if (cnfg != (NO_AUTOIBUS | ilim) && !chg_write(b, CHG_CNFG_09, NO_AUTOIBUS | ilim))
			dev_info(b->dev, "USB input limit %u mA\n", 100 + (ilim - 3) * 25);
	}
	if (chg_read(b, CHG_CNFG_00, &cnfg) || fg_read(b, FG_REPSOC, &soc))
		goto out;
	mode = FIELD_GET(CHG_MODE, cnfg);
	if (READ_ONCE(input_off)) {
		if ((mode == CHG_MODE_BUCK || mode == CHG_MODE_CHG_BUCK) &&
		    !chg_write(b, CHG_CNFG_00, (cnfg & ~CHG_MODE) | CHG_MODE_OFF)) {
			b->forced_off = true;
			dev_info(b->dev, "input off: running from the battery\n");
		}
		goto out;
	}
	if (b->forced_off && mode == CHG_MODE_OFF &&
	    !chg_write(b, CHG_CNFG_00, (cnfg & ~CHG_MODE) | CHG_MODE_BUCK)) {
		b->forced_off = false;
		mode = CHG_MODE_BUCK;
		dev_info(b->dev, "input on again\n");
	}
	if (mode != CHG_MODE_BUCK && mode != CHG_MODE_CHG_BUCK)
		goto out;
	capacity = (soc + 128) >> 8;
	/* no temperature, no charging */
	if (battery_temp(b, &temp)) {
		if (!b->hot)
			dev_warn(b->dev, "battery temperature unreadable: charging paused\n");
		b->hot = true;
		temp = 0;
	} else if (temp >= TEMP_STOP) {
		if (!b->hot)
			dev_warn(b->dev, "battery at %d.%d C: charging paused\n",
				 temp / 10, temp % 10);
		b->hot = true;
	} else if (b->hot && temp <= TEMP_RESUME) {
		b->hot = false;
	}
	in_range = charger_step(b, temp, &raised);
	if (b->end_threshold >= 100)
		b->limited = false;
	else if (capacity >= b->end_threshold)
		b->limited = true;
	else if (capacity <= b->end_threshold - 5)
		b->limited = false;
	want = b->limited || b->hot || !in_range || b->model_failed ?
	       CHG_MODE_BUCK : CHG_MODE_CHG_BUCK;
	/* A finished charge restarts by itself only once the cell falls below
	 * the float voltage less the restart margin; after a raise, restart it.
	 */
	if (raised && want == CHG_MODE_CHG_BUCK && mode == want &&
	    !chg_read(b, CHG_DETAILS_01, &dtls) && FIELD_GET(CHG_DTLS, dtls) == CHG_DTLS_DONE &&
	    !chg_write(b, CHG_CNFG_00, (cnfg & ~CHG_MODE) | CHG_MODE_BUCK))
		mode = CHG_MODE_BUCK;
	if (mode != want && !chg_write(b, CHG_CNFG_00, (cnfg & ~CHG_MODE) | want))
		dev_info(b->dev, "charging %s at %d%%, %d.%d C (limit %d%%)\n",
			 want == CHG_MODE_BUCK ? "paused" : "resumed", capacity,
			 temp / 10, abs(temp % 10), b->end_threshold);
out:
	mutex_unlock(&b->lock);
}

static int usb_get(struct power_supply *psy, enum power_supply_property psp,
		   union power_supply_propval *out)
{
	struct pixel_battery *b = power_supply_get_drvdata(psy);
	u8 val;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		out->intval = usb_online(b);
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		ret = chg_read(b, CHG_CNFG_09, &val);
		if (ret)
			return ret;
		val = FIELD_GET(CHGIN_ILIM, val);
		/* 100 mA from code 3 in 25 mA steps; codes 0-2 are 100 mA too */
		out->intval = 100000 + (val > 3 ? val - 3 : 0) * 25000;
		return 0;
	default:
		return -EINVAL;
	}
}

static const enum power_supply_property usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static const struct power_supply_desc usb_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = usb_props,
	.num_properties = ARRAY_SIZE(usb_props),
	.get_property = usb_get,
};

static char *usb_supplies[] = { "battery" };

#include "pixel-battery-model.h"

/* Nothing interrupts on a change yet: poll, and tell userspace (uevents) when
 * the state or percentage moves.
 */
static void battery_poll(struct work_struct *work)
{
	struct pixel_battery *b = container_of(to_delayed_work(work), struct pixel_battery, poll);
	int status = battery_status(b);
	bool online = usb_online(b);
	u16 soc;
	int capacity = fg_read(b, FG_REPSOC, &soc) ? -1 : (soc + 128) >> 8;

	charger_update(b);
	if (status != b->last_status || capacity != b->last_capacity) {
		b->last_status = status;
		b->last_capacity = capacity;
		power_supply_changed(b->battery);
	}
	if (online != b->last_online) {
		b->last_online = online;
		power_supply_changed(b->usb);
	}
	schedule_delayed_work(&b->poll, 10 * HZ);
}

static void battery_log_state(struct pixel_battery *b)
{
	u16 status, devname, soc, vcell, cur;
	int temp;
	u8 regs[CHG_CNFG_19 - CHG_INT_OK + 1];
	unsigned int i;

	if (!fg_read(b, FG_DEVNAME, &devname) && !fg_read(b, FG_STATUS, &status) &&
	    !fg_read(b, FG_REPSOC, &soc) && !fg_read(b, FG_VCELL, &vcell) &&
	    !fg_read(b, FG_CURRENT, &cur) && !battery_temp(b, &temp))
		dev_info(b->dev, "gauge %#06x status %#06x%s: %u.%02u%%, %u mV, %d mA, %d.%d C\n",
			 devname, status, status & FG_STATUS_POR ? " (POR: stock model not loaded)" : "",
			 soc >> 8, (soc & 0xff) * 100 / 256, vcell * 625 / 8000,
			 fg_current_ua(cur) / 1000, temp / 10, abs(temp % 10));
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		if (chg_read(b, CHG_INT_OK + i, &regs[i]))
			return;
	dev_info(b->dev, "charger b4-cc: %*ph\n", (int)ARRAY_SIZE(regs), regs);
}

static int pixel_battery_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config cfg = {};
	struct pixel_battery *b;
	void __iomem *reg;
	u32 conf, con, gate, qch;
	u16 devname;
	u8 conf8;
	unsigned int i;
	int ret;

	/* Handoff checks: every register read here is documented and clocked. */
	reg = ioremap(SYSREG_PERIC1_USI13, 4);
	conf = reg ? readl(reg) : 0;
	if (reg)
		iounmap(reg);
	reg = ioremap(GPP25_CON, 4);
	con = reg ? readl(reg) : 0;
	if (reg)
		iounmap(reg);
	reg = ioremap(PERIC1_CMU, 0x4000);
	if (!reg)
		return -ENOMEM;
	gate = readl(reg + CMU_USI13_PCLK);
	qch = readl(reg + CMU_USI13_QCH);
	iounmap(reg);
	if (conf != USI_SW_CONF_I2C || (con & 0xff) != GPP25_I2C ||
	    ((gate & GATE_MANUAL) && !(gate & GATE_CG_VAL)) ||
	    (!(qch & QCH_ENABLE) && !(qch & QCH_CLOCK_REQ))) {
		dev_err(dev, "hsi2c_13 not as the bootloader leaves it: USI %#x pins %#x gate %#x qch %#x\n",
			conf, con, gate, qch);
		return -ENODEV;
	}

	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev;
	b->regs = devm_ioremap(dev, PERIC1_HSI2C13, 0x1000);
	if (!b->regs)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(saved_regs); i++)
		b->saved[i] = readl(b->regs + saved_regs[i]);
	if (!(b->saved[2] & BIT(31))) {
		dev_err(dev, "hsi2c_13 is not in auto mode (CONF %#x)\n", b->saved[2]);
		return -ENODEV;
	}

	b->adap.owner = THIS_MODULE;
	b->adap.algo = &hsi2c_algo;
	b->adap.dev.parent = dev;
	b->adap.retries = 2;
	strscpy(b->adap.name, "Pixel hsi2c_13", sizeof(b->adap.name));
	i2c_set_adapdata(&b->adap, b);
	ret = devm_i2c_add_adapter(dev, &b->adap);
	if (ret)
		return ret;

	ret = fg_read(b, FG_DEVNAME, &devname);
	if (ret || devname != FG_DEVNAME_MAX77759) {
		dev_err(dev, "no MAX77759 gauge at %#x (%d, %#x)\n", FG_ADDR, ret, devname);
		return ret ?: -ENODEV;
	}
	battery_log_state(b);
	if (chg_read(b, CHG_CNFG_00, &b->boot_cnfg_00) ||
	    chg_read(b, CHG_CNFG_02, &b->boot_cnfg_02) ||
	    chg_read(b, CHG_CNFG_04, &b->boot_cnfg_04) ||
	    chg_read(b, CHG_CNFG_09, &b->boot_cnfg_09) || chg_read(b, CHG_CNFG_06, &conf8)) {
		dev_err(dev, "no MAX77759 charger at %#x\n", CHG_ADDR);
		return -ENODEV;
	}
	if (FIELD_GET(CHGPROT, conf8) != CHGPROT_UNLOCKED) {
		dev_warn(dev, "charger settings locked (CNFG_06 %#x): read-only\n", conf8);
		input_limit_ma = 0;
		b->boot_cnfg_00 = 0xff;
	} else {
		step_profile_read(b);
	}
	mutex_init(&b->lock);
	b->end_threshold = 100;

	cfg.drv_data = b;
	b->battery = devm_power_supply_register(dev, &battery_desc, &cfg);
	if (IS_ERR(b->battery))
		return PTR_ERR(b->battery);
	cfg.supplied_to = usb_supplies;
	cfg.num_supplicants = ARRAY_SIZE(usb_supplies);
	b->usb = devm_power_supply_register(dev, &usb_desc, &cfg);
	if (IS_ERR(b->usb))
		return PTR_ERR(b->usb);

	platform_set_drvdata(pdev, b);
	WRITE_ONCE(coulomb_battery, b);
	ret = devm_device_add_group(dev, &pixel_model_group);
	if (ret)
		return ret;
	b->last_status = -1;
	INIT_DELAYED_WORK(&b->poll, battery_poll);
	schedule_delayed_work(&b->poll, 0);
	return 0;
}

static void pixel_battery_remove(struct platform_device *pdev)
{
	struct pixel_battery *b = platform_get_drvdata(pdev);
	u8 cnfg;

	WRITE_ONCE(coulomb_battery, NULL);

	cancel_delayed_work_sync(&b->poll);
	if (b->boot_cnfg_00 == 0xff)
		return;
	/* hand the charger back as the bootloader left it */
	if (!chg_read(b, CHG_CNFG_09, &cnfg) && cnfg != b->boot_cnfg_09)
		chg_write(b, CHG_CNFG_09, b->boot_cnfg_09);
	if (!chg_read(b, CHG_CNFG_04, &cnfg) && cnfg != b->boot_cnfg_04)
		chg_write(b, CHG_CNFG_04, b->boot_cnfg_04);
	if (!chg_read(b, CHG_CNFG_02, &cnfg) && cnfg != b->boot_cnfg_02)
		chg_write(b, CHG_CNFG_02, b->boot_cnfg_02);
	if (!chg_read(b, CHG_CNFG_00, &cnfg) && cnfg != b->boot_cnfg_00 &&
	    (FIELD_GET(CHG_MODE, cnfg) == CHG_MODE_BUCK ||
	     FIELD_GET(CHG_MODE, cnfg) == CHG_MODE_CHG_BUCK))
		chg_write(b, CHG_CNFG_00, b->boot_cnfg_00);
}

static struct platform_driver pixel_battery_driver = {
	.probe = pixel_battery_probe,
	.remove = pixel_battery_remove,
	.driver = { .name = "pixel-battery", .probe_type = PROBE_FORCE_SYNCHRONOUS },
};

static struct platform_device *pixel_battery_pdev;

static int __init pixel_battery_init(void)
{
	int ret;

	if (!of_machine_is_compatible("google,GS201 CHEETAH"))
		return -ENODEV;
	ret = platform_driver_register(&pixel_battery_driver);
	if (ret)
		return ret;
	pixel_battery_pdev = platform_device_register_simple("pixel-battery", PLATFORM_DEVID_NONE,
							     NULL, 0);
	if (IS_ERR(pixel_battery_pdev)) {
		platform_driver_unregister(&pixel_battery_driver);
		return PTR_ERR(pixel_battery_pdev);
	}
	if (!pixel_battery_pdev->dev.driver) {
		platform_device_unregister(pixel_battery_pdev);
		platform_driver_unregister(&pixel_battery_driver);
		return -ENODEV;
	}
	return 0;
}
module_init(pixel_battery_init);

static void __exit pixel_battery_exit(void)
{
	platform_device_unregister(pixel_battery_pdev);
	platform_driver_unregister(&pixel_battery_driver);
}
module_exit(pixel_battery_exit);

MODULE_DESCRIPTION("Pixel 7 Pro battery state and charge control over hsi2c_13");
MODULE_LICENSE("GPL");
