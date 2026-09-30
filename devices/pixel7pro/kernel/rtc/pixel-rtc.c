// SPDX-License-Identifier: GPL-2.0-only
/* S2MPG12 real-time clock through firmware's ACPM PMIC protocol: the time,
 * read-only, and alarm 0, which can wake the system.
 * GS201 stock DT: ACPM IPC channel 2, PMIC bus 0; RTC bank 2 (vendor
 * s2mpg12-core.c I2C_ADDR_RTC). Registers 0x00-0x1b match S2MPG10's
 * (Google gs android-gs-pantah-6.1-android16, rtc-s2mpg12.h).
 *
 * Reading latches the counters with RTC_UPDATE.RUDR, exactly as the vendor
 * s2m_rtc_update() does, with the write and freeze latch bits cleared, so the
 * time never changes: no set_time, WTSR/SMPL or oscillator changes; Android
 * keeps the battery-backed RTC in UTC.
 *
 * Alarm 0 (0x0d-0x13, bit 7 of each field enables it) is written and latched
 * with RTC_UPDATE.AUDR, as the vendor s2m_rtc_set_alarm() does. Its interrupt,
 * RTCA0 (PM INT2 bit 2), is unmasked in PM INT2M only while the alarm is
 * enabled; the PMIC's other interrupts stay masked. The PMIC signals over its
 * I3C bus as an in-band interrupt, which the SoC turns into GIC SPI 72 (the
 * s2mpg12mfd node's interrupt) through SYSREG_VGPIO2AP. As in the vendor
 * s2mpg12_irq_thread(), the handler clears INTC0_IPEND there and reads INT1
 * to INT5 in one block, which acknowledges the PMIC: a read of INT2 alone
 * returns only its live 1-second tick and leaves the interrupt asserted. The
 * interrupt is a wake-up source.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of_irq.h>
#include <linux/pm_wakeup.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/rtc.h>

#define ACPM_PMIC_CHAN	2
#define RTC_BANK	2
#define PM_BANK		1
#define PMIC_BUS	0

#define RTC_CTRL	0x00
#define RTC_UPDATE	0x01
#define RTC_SEC		0x06	/* SEC, MIN, HOUR, WEEK, DAY, MON, YEAR */
#define RTC_A0SEC	0x0d	/* alarm 0, same layout */
#define RTC_REGS	7
#define ALARM_EN	BIT(7)
#define HOUR_PM		BIT(6)

#define PM_INT1		0x00	/* INT1..INT5: read as a block, which clears them */
#define PM_INTS		5
#define PM_INT2M	0x06
#define INT2_RTCA0	BIT(2)

#define VGPIO_MONITOR	0x18101704	/* VGPIO_I3C_BASE + VGPIO_MONITOR_ADDR */
#define VGPIO_IPEND	0x182f0290	/* SYSREG_VGPIO2AP + INTC0_IPEND */

#define CTRL_BCD_EN	BIT(0)
#define CTRL_MODEL24	BIT(1)
#define UPDATE_RUDR	BIT(0)
#define UPDATE_WUDR	BIT(1)
#define UPDATE_FREEZE	BIT(2)
#define UPDATE_AUDR	BIT(4)

static bool alarm = true;
module_param(alarm, bool, 0444);
MODULE_PARM_DESC(alarm, "Alarm 0 with its wake-up interrupt (off: time only)");

static struct device *rtcdev;
static struct rtc_device *rtc;
static struct acpm_handle *acpm;
static DEFINE_MUTEX(rtc_lock);
static void __iomem *ipend;
static int irq;
static bool alarm_enabled;

static int rtc_reg_read(u8 reg, u8 *val)
{
	return acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHAN, RTC_BANK, reg, PMIC_BUS, val);
}

static int rtc_reg_write(u8 reg, u8 val)
{
	return acpm->ops->pmic.write_reg(acpm, ACPM_PMIC_CHAN, RTC_BANK, reg, PMIC_BUS, val);
}

static int pm_reg_read(u8 reg, u8 *val)
{
	return acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHAN, PM_BANK, reg, PMIC_BUS, val);
}

/* Read (and so clear) the PMIC's INT1..INT5; returns INT2. */
static int pm_ints_ack(u8 *int2)
{
	u8 ints[PM_INTS];
	int ret;

	ret = acpm->ops->pmic.bulk_read(acpm, ACPM_PMIC_CHAN, PM_BANK, PM_INT1, PMIC_BUS,
					PM_INTS, ints);
	if (!ret)
		*int2 = ints[1];
	return ret;
}

static int pm_reg_write(u8 reg, u8 val)
{
	return acpm->ops->pmic.write_reg(acpm, ACPM_PMIC_CHAN, PM_BANK, reg, PMIC_BUS, val);
}

/* Pulse one latch bit: RUDR copies the counters to the readable time
 * registers, AUDR moves the written alarm into effect. The time write (WUDR)
 * and freeze bits are always cleared.
 */
static int rtc_latch_bit(u8 bit)
{
	u8 update;
	int ret;

	ret = rtc_reg_read(RTC_UPDATE, &update);
	if (ret)
		return ret;
	update &= ~(UPDATE_RUDR | UPDATE_WUDR | UPDATE_FREEZE | UPDATE_AUDR);
	ret = rtc_reg_write(RTC_UPDATE, update);
	if (ret)
		return ret;
	usleep_range(50, 60);
	ret = rtc_reg_write(RTC_UPDATE, update | bit);
	if (ret)
		return ret;
	usleep_range(1000, 1100);
	return 0;
}

static int rtc_latch(void)
{
	return rtc_latch_bit(UPDATE_RUDR);
}

static void data_to_tm(const u8 *data, struct rtc_time *tm)
{
	/* Binary, 24-hour (checked at load); the hour's PM flag is informational. */
	tm->tm_sec = data[0] & 0x7f;
	tm->tm_min = data[1] & 0x7f;
	tm->tm_hour = data[2] & 0x1f;
	tm->tm_wday = data[3] & 0x7f ? __fls(data[3] & 0x7f) : 0;
	tm->tm_mday = data[4] & 0x1f;
	tm->tm_mon = (data[5] & 0x0f) - 1;
	tm->tm_year = (data[6] & 0x7f) + 100;
}

static int pixel_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	u8 data[RTC_REGS];
	int ret, i;

	mutex_lock(&rtc_lock);
	ret = rtc_latch();
	for (i = 0; !ret && i < RTC_REGS; i++)
		ret = rtc_reg_read(RTC_SEC + i, &data[i]);
	mutex_unlock(&rtc_lock);
	if (ret)
		return ret;
	data_to_tm(data, tm);
	return rtc_valid_tm(tm);
}

/* RTCA0 in PM INT2M: unmasked only while the alarm is enabled. */
static int rtca0_unmask(bool on)
{
	u8 mask;
	int ret;

	ret = pm_reg_read(PM_INT2M, &mask);
	if (ret)
		return ret;
	mask = on ? mask & ~INT2_RTCA0 : mask | INT2_RTCA0;
	ret = pm_reg_write(PM_INT2M, mask);
	if (!ret)
		alarm_enabled = on;
	return ret;
}

static int pixel_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	u8 data[RTC_REGS];
	int ret, i;

	mutex_lock(&rtc_lock);
	ret = rtc_latch();
	for (i = 0; !ret && i < RTC_REGS; i++)
		ret = rtc_reg_read(RTC_A0SEC + i, &data[i]);
	alrm->enabled = alarm_enabled;
	mutex_unlock(&rtc_lock);
	if (ret)
		return ret;
	data_to_tm(data, &alrm->time);
	return 0;
}

static int pixel_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct rtc_time *tm = &alrm->time;
	u8 data[RTC_REGS] = {
		tm->tm_sec, tm->tm_min, tm->tm_hour | (tm->tm_hour >= 12 ? HOUR_PM : 0),
		BIT(tm->tm_wday), tm->tm_mday, tm->tm_mon + 1, tm->tm_year - 100,
	};
	int ret, i;

	if (tm->tm_year < 100 || tm->tm_year > 199)
		return -EINVAL;
	mutex_lock(&rtc_lock);
	ret = rtca0_unmask(false);
	for (i = 0; !ret && i < RTC_REGS; i++)
		ret = rtc_reg_write(RTC_A0SEC + i, data[i] | ALARM_EN);
	if (!ret)
		ret = rtc_latch_bit(UPDATE_AUDR);
	if (!ret && alrm->enabled)
		ret = rtca0_unmask(true);
	mutex_unlock(&rtc_lock);
	return ret;
}

static int pixel_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	int ret;

	mutex_lock(&rtc_lock);
	ret = rtca0_unmask(enabled);
	mutex_unlock(&rtc_lock);
	return ret;
}

static irqreturn_t pixel_rtc_irq(int irq, void *data)
{
	u8 int2 = 0;
	int ret;

	writel(readl(ipend), ipend);
	mutex_lock(&rtc_lock);
	ret = pm_ints_ack(&int2);
	mutex_unlock(&rtc_lock);
	if (!ret && (int2 & INT2_RTCA0)) {
		pm_wakeup_event(rtcdev, 500);
		rtc_update_irq(rtc, 1, RTC_IRQF | RTC_AF);
	}
	return IRQ_HANDLED;
}

static const struct rtc_class_ops pixel_rtc_ro_ops = {
	.read_time = pixel_rtc_read_time,
};

static const struct rtc_class_ops pixel_rtc_ops = {
	.read_time = pixel_rtc_read_time,
	.read_alarm = pixel_rtc_read_alarm,
	.set_alarm = pixel_rtc_set_alarm,
	.alarm_irq_enable = pixel_rtc_alarm_irq_enable,
};

/* The alarm's interrupt, with RTCA0 masked and any stale event cleared. */
static int pixel_rtc_alarm_init(struct device_node *pmic)
{
	u8 int2;
	int ret;

	ret = rtca0_unmask(false);
	if (!ret)
		ret = pm_ints_ack(&int2);
	if (ret)
		return ret;
	ipend = devm_ioremap(rtcdev, VGPIO_IPEND, 4);
	if (!ipend)
		return -ENOMEM;
	writel(readl(ipend), ipend);
	irq = irq_of_parse_and_map(pmic, 0);
	if (!irq)
		return -EINVAL;
	ret = request_threaded_irq(irq, NULL, pixel_rtc_irq, IRQF_ONESHOT, "pixel-rtc", NULL);
	if (ret) {
		irq_dispose_mapping(irq);
		irq = 0;
		return ret;
	}
	device_init_wakeup(rtcdev, true);
	enable_irq_wake(irq);
	return 0;
}

static int __init pixel_rtc_init(void)
{
	struct device_node *np;
	struct rtc_time tm;
	u8 ctrl;
	int ret;

	struct device_node *pmic;

	pmic = of_find_compatible_node(NULL, NULL, "samsung,s2mpg12mfd");
	if (!pmic)
		return -ENODEV;
	rtcdev = root_device_register("pixel-rtc");
	if (IS_ERR(rtcdev))
		return PTR_ERR(rtcdev);
	np = of_find_node_by_path("/power-management");
	if (!np) {
		ret = -ENODEV;
		goto unregister;
	}
	acpm = devm_acpm_get_by_node(rtcdev, np);
	of_node_put(np);
	if (IS_ERR(acpm)) {
		ret = PTR_ERR(acpm);
		goto unregister;
	}
	ret = rtc_reg_read(RTC_CTRL, &ctrl);
	if (ret)
		goto unregister;
	if ((ctrl & (CTRL_BCD_EN | CTRL_MODEL24)) != CTRL_MODEL24) {
		pr_err("pixel-rtc: unexpected RTC_CTRL %#x (want binary 24-hour)\n", ctrl);
		ret = -EINVAL;
		goto unregister;
	}
	ret = pixel_rtc_read_time(rtcdev, &tm);
	if (ret) {
		pr_err("pixel-rtc: first read failed %d\n", ret);
		goto unregister;
	}
	if (alarm) {
		ret = pixel_rtc_alarm_init(pmic);
		if (ret) {
			pr_err("pixel-rtc: no alarm interrupt (%d); time only\n", ret);
			alarm = false;
		}
	}
	rtc = devm_rtc_allocate_device(rtcdev);
	if (IS_ERR(rtc)) {
		ret = PTR_ERR(rtc);
		goto unregister;
	}
	rtc->ops = alarm ? &pixel_rtc_ops : &pixel_rtc_ro_ops;
	rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	rtc->range_max = RTC_TIMESTAMP_END_2099;
	ret = devm_rtc_register_device(rtc);
	if (ret)
		goto unregister;
	of_node_put(pmic);
	pr_info("pixel-rtc: S2MPG12 RTC, time read-only%s, now %ptRs UTC\n",
		alarm ? ", alarm 0 wakes" : "", &tm);
	return 0;
unregister:
	if (irq) {
		free_irq(irq, NULL);
		irq_dispose_mapping(irq);
	}
	of_node_put(pmic);
	root_device_unregister(rtcdev);
	return ret;
}

static void __exit pixel_rtc_exit(void)
{
	if (irq) {
		disable_irq_wake(irq);
		free_irq(irq, NULL);
		irq_dispose_mapping(irq);
		mutex_lock(&rtc_lock);
		rtca0_unmask(false);
		mutex_unlock(&rtc_lock);
	}
	root_device_unregister(rtcdev);
}

module_init(pixel_rtc_init);
module_exit(pixel_rtc_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pixel GS201 S2MPG12 RTC: time read-only, alarm 0");
