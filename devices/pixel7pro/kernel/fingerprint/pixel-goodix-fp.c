// SPDX-License-Identifier: GPL-2.0-only
/* The Pixel 7 Pro's under-display fingerprint sensor (Goodix, "Delmar") as
 * Goodix's HAL (libgf_hal.so) expects it: /dev/goodix_fp with the gf_spi
 * ioctls, and the sensor's interrupt as a netlink message. Goodix's trusted
 * app in Trusty drives the sensor over SPI; this side has only its reset line
 * and its interrupt. The stock DT node is /odm/goodixfp ("goodix,fingerprint"):
 * fp-gpio-reset gpp1-1 (an output, no pull, per goodixfp_pins) and fp-gpio-irq
 * gpa9-2.
 *
 * Power: the supply is S2MPG13 LDO15S (UDFPS_AVDD, PM bank register 0x3a in
 * Google's s2mpg13-register.h). Stock keeps it always on; the bootloader
 * leaves it off, at 3.3 V. ENABLE_POWER switches it on through ACPM (enable
 * field [7:6] = 3, stock's mode) at the voltage found, which must be in the
 * stock DT range (2.5-3.3 V), and waits 10 ms; DISABLE_POWER holds the
 * sensor in reset and switches it off. Unloading leaves it as found.
 *
 * ioctls, magic 'g', as libgf_hal.so issues them: INIT (_IOR 0, u8: the
 * netlink protocol), EXIT 1, RESET 2, ENABLE_IRQ 3, DISABLE_IRQ 4,
 * ENABLE_SPI_CLK (_IOW 5, u32) and DISABLE_SPI_CLK 6 (the SPI block is the
 * trusted app's; accepted, nothing done), ENABLE_POWER 7, DISABLE_POWER 8,
 * GET_FW_INFO (_IOR 11, u8) and REMOVE 12. Power, reset and the interrupt do
 * what their names say; the rest are accepted and do nothing.
 *
 * Netlink: the HAL opens a raw socket of the protocol INIT returned, binds it
 * to its pid and sends a message; that pid gets each interrupt as a 16-byte
 * payload whose first byte is the event (GF_NET_EVENT_IRQ, 1).
 *
 * Pins, from Google's pinctrl-gs201.c: gpp1 is PERIC0 (0x10840000) bank 0x20;
 * gpa9 is ALIVE (0x180d0000) bank 0xc0 with its wake-up EINT registers at
 * +0x18, and each gpa9 pin has its own GIC line in the bank's interrupts list
 * (pin 2: SPI 36, level high). The EINT is set to rising edge and unmasked
 * once, at load; afterwards only the GIC line is gated. Other pins of both
 * banks belong to other drivers: only this module's fields are changed, by
 * read-modify-write, at load and unload (and gpp1's DAT for a reset).
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netlink.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#define GF_IOC_MAGIC		'g'
#define GF_IOC_INIT		_IOR(GF_IOC_MAGIC, 0, u8)
#define GF_IOC_EXIT		_IO(GF_IOC_MAGIC, 1)
#define GF_IOC_RESET		_IO(GF_IOC_MAGIC, 2)
#define GF_IOC_ENABLE_IRQ	_IO(GF_IOC_MAGIC, 3)
#define GF_IOC_DISABLE_IRQ	_IO(GF_IOC_MAGIC, 4)
#define GF_IOC_ENABLE_SPI_CLK	_IOW(GF_IOC_MAGIC, 5, u32)
#define GF_IOC_DISABLE_SPI_CLK	_IO(GF_IOC_MAGIC, 6)
#define GF_IOC_ENABLE_POWER	_IO(GF_IOC_MAGIC, 7)
#define GF_IOC_DISABLE_POWER	_IO(GF_IOC_MAGIC, 8)
#define GF_IOC_GET_FW_INFO	_IOR(GF_IOC_MAGIC, 11, u8)
#define GF_IOC_REMOVE		_IO(GF_IOC_MAGIC, 12)

#define NETLINK_GF		25	/* free in this kernel's list */
#define GF_NL_PAYLOAD		16	/* the HAL's getNetlinkMsgDataLen() */
#define GF_NET_EVENT_IRQ	1

#define PERIC0			0x10840000
#define GPP1			0x20
#define ALIVE			0x180d0000
#define GPA9			0xc0
#define GPA9_EINT		0x18
#define PIN_CON			0x00	/* 4 bits a pin */
#define PIN_DAT			0x04
#define PIN_PUD			0x08	/* 4 bits a pin */
#define PIN_CONPDN		0x10	/* gpp1 only: 2 bits a pin */
#define PIN_PUDPDN		0x14	/* gpp1 only: 4 bits a pin */
#define ECON			0x700	/* 4 bits a pin: 3 is rising edge */
#define EMASK			0x900
#define EPEND			0xa00	/* write 1 to clear */
#define ECON_RISING		3
#define CON_OUTPUT		1
#define CON_EINT		0xf
#define PUD_NONE		0
#define CONPDN_PREV		3
#define RESET_PIN		1	/* gpp1-1 */
#define IRQ_PIN			2	/* gpa9-2 */

#define ACPM_PMIC_CHANNEL	2
#define PM_BANK			0x01
#define S2MPG13			1
#define L15S_CTRL		0x3a
#define LDO_ENABLE		GENMASK(7, 6)
#define LDO_ALWAYS_ON		(3 << 6)
#define LDO_VSEL		GENMASK(5, 0)	/* group 7: 1.8 V + 25 mV a step */
#define L15S_VSEL_MIN		0x1c		/* 2.5 V */
#define L15S_VSEL_MAX		0x3c		/* 3.3 V */

static void __iomem *peric0, *alive;
static DEFINE_SPINLOCK(pin_lock);
static u32 saved_reset_con, saved_reset_pud, saved_reset_conpdn, saved_reset_pudpdn;
static u32 saved_irq_con, saved_econ, saved_emask;
static int fp_irq;
static bool irq_enabled;
static DEFINE_MUTEX(fp_lock);
static struct sock *nl_sk;
static u32 hal_portid;
static struct device *fp_dev;
static struct acpm_handle *acpm;
static u8 saved_l15s;

static unsigned int irq_count;
module_param(irq_count, uint, 0444);
MODULE_PARM_DESC(irq_count, "Sensor interrupts since load");

static void pin_rmw(void __iomem *reg, unsigned int pin, unsigned int width, u32 val)
{
	u32 shift = pin * width, mask = GENMASK(shift + width - 1, shift);
	unsigned long flags;

	spin_lock_irqsave(&pin_lock, flags);
	writel((readl(reg) & ~mask) | (val << shift), reg);
	spin_unlock_irqrestore(&pin_lock, flags);
}

static u32 pin_field(void __iomem *reg, unsigned int pin, unsigned int width)
{
	return (readl(reg) >> (pin * width)) & GENMASK(width - 1, 0);
}

static void fp_reset_line(bool high)
{
	pin_rmw(peric0 + GPP1 + PIN_DAT, RESET_PIN, 1, high);
}

static int l15s_read(u8 *val)
{
	return acpm->ops->pmic.read_reg(acpm, ACPM_PMIC_CHANNEL, PM_BANK, L15S_CTRL, S2MPG13, val);
}

static int l15s_write(u8 val)
{
	return acpm->ops->pmic.write_reg(acpm, ACPM_PMIC_CHANNEL, PM_BANK, L15S_CTRL, S2MPG13, val);
}

static int fp_power(bool on)
{
	u8 val, vsel;
	int ret;

	if (!on)
		fp_reset_line(false);
	ret = l15s_read(&val);
	if (ret)
		return ret;
	vsel = val & LDO_VSEL;
	if (vsel < L15S_VSEL_MIN || vsel > L15S_VSEL_MAX)
		return -ERANGE;
	if (!!(val & LDO_ENABLE) == on)
		return 0;
	ret = l15s_write((val & ~LDO_ENABLE) | (on ? LDO_ALWAYS_ON : 0));
	if (!ret && on)
		msleep(10);
	return ret;
}

/* gf_hw_reset(): low for 3 ms, then high and the sensor's 3 ms to come up. */
static void fp_reset(void)
{
	fp_reset_line(false);
	usleep_range(3000, 3500);
	fp_reset_line(true);
	usleep_range(3000, 3500);
}

static void fp_irq_enable(bool enable)
{
	if (enable == irq_enabled)
		return;
	if (enable) {
		writel(BIT(IRQ_PIN), alive + GPA9_EINT + EPEND);
		enable_irq(fp_irq);
	} else {
		disable_irq(fp_irq);
	}
	irq_enabled = enable;
}

static void fp_netlink_send(u8 event)
{
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	u32 portid = READ_ONCE(hal_portid);

	if (!portid)
		return;
	skb = nlmsg_new(GF_NL_PAYLOAD, GFP_KERNEL);
	if (!skb)
		return;
	nlh = nlmsg_put(skb, 0, 0, 0, GF_NL_PAYLOAD, 0);
	if (!nlh) {
		kfree_skb(skb);
		return;
	}
	memset(nlmsg_data(nlh), 0, GF_NL_PAYLOAD);
	*(u8 *)nlmsg_data(nlh) = event;
	netlink_unicast(nl_sk, skb, portid, MSG_DONTWAIT);
}

/* The HAL registers the socket it reads by sending anything to it. */
static void fp_netlink_recv(struct sk_buff *skb)
{
	struct nlmsghdr *nlh = nlmsg_hdr(skb);

	if (skb->len >= NLMSG_HDRLEN && nlh->nlmsg_len >= NLMSG_HDRLEN)
		WRITE_ONCE(hal_portid, NETLINK_CB(skb).portid);
}

static irqreturn_t fp_irq_handler(int irq, void *data)
{
	writel(BIT(IRQ_PIN), alive + GPA9_EINT + EPEND);
	return IRQ_WAKE_THREAD;
}

static irqreturn_t fp_irq_thread(int irq, void *data)
{
	irq_count++;
	fp_netlink_send(GF_NET_EVENT_IRQ);
	return IRQ_HANDLED;
}

static long fp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	u8 value;
	long ret = 0;

	mutex_lock(&fp_lock);
	switch (cmd) {
	case GF_IOC_INIT:
		value = NETLINK_GF;
		if (copy_to_user(uarg, &value, sizeof(value)))
			ret = -EFAULT;
		break;
	case GF_IOC_RESET:
		fp_reset();
		break;
	case GF_IOC_ENABLE_IRQ:
		fp_irq_enable(true);
		break;
	case GF_IOC_DISABLE_IRQ:
		fp_irq_enable(false);
		break;
	case GF_IOC_GET_FW_INFO:
		value = 0;
		if (copy_to_user(uarg, &value, sizeof(value)))
			ret = -EFAULT;
		break;
	case GF_IOC_ENABLE_POWER:
		ret = fp_power(true);
		break;
	case GF_IOC_DISABLE_POWER:
		ret = fp_power(false);
		break;
	case GF_IOC_EXIT:
	case GF_IOC_ENABLE_SPI_CLK:
	case GF_IOC_DISABLE_SPI_CLK:
	case GF_IOC_REMOVE:
		break;
	default:
		ret = -ENOTTY;
	}
	mutex_unlock(&fp_lock);
	return ret;
}

/* The HAL closing the device, or dying, leaves the sensor off: it powers it
 * up again (ENABLE_POWER) when it opens it. */
static int fp_release(struct inode *inode, struct file *file)
{
	mutex_lock(&fp_lock);
	fp_irq_enable(false);
	fp_power(false);
	mutex_unlock(&fp_lock);
	return 0;
}

static const struct file_operations fp_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = fp_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.release = fp_release,
};

static struct miscdevice fp_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "goodix_fp",
	.fops = &fp_fops,
};

/* Tests without the HAL: power, a reset pulse, and the interrupt on or off. */
static int test_power_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int ret = kstrtobool(val, &on);

	if (ret)
		return ret;
	mutex_lock(&fp_lock);
	ret = fp_power(on);
	mutex_unlock(&fp_lock);
	return ret;
}
static int test_power_get(char *buf, const struct kernel_param *kp)
{
	u8 val;
	int ret = l15s_read(&val);

	return ret ?: sysfs_emit(buf, "L15S_CTRL %#04x (%s, %u mV)\n", val,
				 val & LDO_ENABLE ? "on" : "off", 1800 + 25 * (unsigned int)(val & LDO_VSEL));
}
static const struct kernel_param_ops test_power_ops = { .set = test_power_set, .get = test_power_get };
module_param_cb(test_power, &test_power_ops, NULL, 0644);
MODULE_PARM_DESC(test_power, "The sensor's supply on or off; reading shows L15S_CTRL");

static int test_reset_set(const char *val, const struct kernel_param *kp)
{
	mutex_lock(&fp_lock);
	fp_reset();
	mutex_unlock(&fp_lock);
	return 0;
}
static const struct kernel_param_ops test_reset_ops = { .set = test_reset_set };
module_param_cb(test_reset, &test_reset_ops, NULL, 0200);
MODULE_PARM_DESC(test_reset, "Write anything: one reset pulse");

static int test_irq_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int ret = kstrtobool(val, &on);

	if (ret)
		return ret;
	mutex_lock(&fp_lock);
	fp_irq_enable(on);
	mutex_unlock(&fp_lock);
	return 0;
}
static int test_irq_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%s, line %s, reset %s\n", irq_enabled ? "enabled" : "disabled",
			  pin_field(alive + GPA9 + PIN_DAT, IRQ_PIN, 1) ? "high" : "low",
			  pin_field(peric0 + GPP1 + PIN_DAT, RESET_PIN, 1) ? "high" : "low");
}
static const struct kernel_param_ops test_irq_ops = { .set = test_irq_set, .get = test_irq_get };
module_param_cb(test_irq, &test_irq_ops, NULL, 0644);
MODULE_PARM_DESC(test_irq, "The interrupt on or off; reading shows both lines");

/* The stock DT names this hardware; refuse anything else. */
static int fp_check_pin(struct device_node *np, const char *prop, const char *bank,
			u32 pin, phys_addr_t ctrl)
{
	struct of_phandle_args args;
	struct device_node *parent;
	struct resource res = {};
	bool ok;

	if (of_parse_phandle_with_args(np, prop, "#gpio-cells", 0, &args))
		return -ENODEV;
	parent = of_get_parent(args.np);
	ok = of_node_name_eq(args.np, bank) && args.args_count >= 1 && args.args[0] == pin &&
	     parent && !of_address_to_resource(parent, 0, &res) && res.start == ctrl;
	if (!ok)
		pr_err("pixel-goodix-fp: stock DT %s is not %s-%u\n", prop, bank, pin);
	of_node_put(parent);
	of_node_put(args.np);
	return ok ? 0 : -ENODEV;
}

static int __init fp_init(void)
{
	struct device_node *np, *bank;
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	np = of_find_node_by_path("/odm/goodixfp");
	if (!np || !of_device_is_compatible(np, "goodix,fingerprint")) {
		of_node_put(np);
		return -ENODEV;
	}
	ret = fp_check_pin(np, "fp-gpio-reset", "gpp1", RESET_PIN, PERIC0);
	if (!ret)
		ret = fp_check_pin(np, "fp-gpio-irq", "gpa9", IRQ_PIN, ALIVE);
	of_node_put(np);
	if (ret)
		return ret;

	fp_dev = root_device_register("pixel-goodix-fp");
	if (IS_ERR(fp_dev))
		return PTR_ERR(fp_dev);
	np = of_find_node_by_path("/power-management");
	acpm = np ? devm_acpm_get_by_node(fp_dev, np) : ERR_PTR(-ENODEV);
	of_node_put(np);
	if (IS_ERR(acpm)) {
		ret = PTR_ERR(acpm);
		goto unregister;
	}
	ret = l15s_read(&saved_l15s);
	if (ret)
		goto unregister;

	bank = of_find_node_by_path("/pinctrl@180D0000/gpa9");
	fp_irq = bank ? irq_of_parse_and_map(bank, IRQ_PIN) : 0;
	of_node_put(bank);
	if (!fp_irq) {
		ret = -ENXIO;
		goto unregister;
	}
	if (irq_get_trigger_type(fp_irq) != IRQ_TYPE_LEVEL_HIGH) {
		pr_err("pixel-goodix-fp: gpa9-2's GIC line is not level-high\n");
		ret = -EINVAL;
		goto dispose;
	}
	peric0 = ioremap(PERIC0, 0x1000);
	alive = ioremap(ALIVE, 0x1000);
	if (!peric0 || !alive) {
		ret = -ENOMEM;
		goto unmap;
	}

	/* Reset: an output at its present level, as goodixfp_pins configures it. */
	saved_reset_con = pin_field(peric0 + GPP1 + PIN_CON, RESET_PIN, 4);
	saved_reset_pud = pin_field(peric0 + GPP1 + PIN_PUD, RESET_PIN, 4);
	saved_reset_conpdn = pin_field(peric0 + GPP1 + PIN_CONPDN, RESET_PIN, 2);
	saved_reset_pudpdn = pin_field(peric0 + GPP1 + PIN_PUDPDN, RESET_PIN, 4);
	pin_rmw(peric0 + GPP1 + PIN_PUD, RESET_PIN, 4, PUD_NONE);
	pin_rmw(peric0 + GPP1 + PIN_CONPDN, RESET_PIN, 2, CONPDN_PREV);
	pin_rmw(peric0 + GPP1 + PIN_PUDPDN, RESET_PIN, 4, PUD_NONE);
	pin_rmw(peric0 + GPP1 + PIN_CON, RESET_PIN, 4, CON_OUTPUT);

	/* Interrupt: the wake-up EINT, rising edge, masked while it is set up. */
	saved_irq_con = pin_field(alive + GPA9 + PIN_CON, IRQ_PIN, 4);
	saved_econ = pin_field(alive + GPA9_EINT + ECON, IRQ_PIN, 4);
	saved_emask = pin_field(alive + GPA9_EINT + EMASK, IRQ_PIN, 1);
	pin_rmw(alive + GPA9_EINT + EMASK, IRQ_PIN, 1, 1);
	pin_rmw(alive + GPA9_EINT + ECON, IRQ_PIN, 4, ECON_RISING);
	pin_rmw(alive + GPA9 + PIN_CON, IRQ_PIN, 4, CON_EINT);
	writel(BIT(IRQ_PIN), alive + GPA9_EINT + EPEND);
	pin_rmw(alive + GPA9_EINT + EMASK, IRQ_PIN, 1, 0);

	ret = request_threaded_irq(fp_irq, fp_irq_handler, fp_irq_thread,
				   IRQF_ONESHOT | IRQF_NO_AUTOEN, "goodix_fp", NULL);
	if (ret)
		goto restore;

	{
		struct netlink_kernel_cfg cfg = { .input = fp_netlink_recv };

		nl_sk = netlink_kernel_create(&init_net, NETLINK_GF, &cfg);
	}
	if (!nl_sk) {
		ret = -ENOMEM;
		goto free_irq;
	}
	ret = misc_register(&fp_misc);
	if (ret)
		goto release_nl;
	pr_info("pixel-goodix-fp: /dev/goodix_fp, netlink %d, irq %d; reset %s, L15S_CTRL %#04x\n",
		NETLINK_GF, fp_irq, pin_field(peric0 + GPP1 + PIN_DAT, RESET_PIN, 1) ? "high" : "low",
		saved_l15s);
	return 0;

release_nl:
	netlink_kernel_release(nl_sk);
free_irq:
	free_irq(fp_irq, NULL);
restore:
	pin_rmw(alive + GPA9_EINT + EMASK, IRQ_PIN, 1, 1);
	pin_rmw(alive + GPA9 + PIN_CON, IRQ_PIN, 4, saved_irq_con);
	pin_rmw(alive + GPA9_EINT + ECON, IRQ_PIN, 4, saved_econ);
	writel(BIT(IRQ_PIN), alive + GPA9_EINT + EPEND);
	pin_rmw(alive + GPA9_EINT + EMASK, IRQ_PIN, 1, saved_emask);
	pin_rmw(peric0 + GPP1 + PIN_CON, RESET_PIN, 4, saved_reset_con);
	pin_rmw(peric0 + GPP1 + PIN_PUD, RESET_PIN, 4, saved_reset_pud);
	pin_rmw(peric0 + GPP1 + PIN_CONPDN, RESET_PIN, 2, saved_reset_conpdn);
	pin_rmw(peric0 + GPP1 + PIN_PUDPDN, RESET_PIN, 4, saved_reset_pudpdn);
unmap:
	if (alive)
		iounmap(alive);
	if (peric0)
		iounmap(peric0);
dispose:
	irq_dispose_mapping(fp_irq);
unregister:
	root_device_unregister(fp_dev);
	return ret;
}
module_init(fp_init);

static void __exit fp_exit(void)
{
	misc_deregister(&fp_misc);
	netlink_kernel_release(nl_sk);
	free_irq(fp_irq, NULL);
	pin_rmw(alive + GPA9_EINT + EMASK, IRQ_PIN, 1, 1);
	pin_rmw(alive + GPA9 + PIN_CON, IRQ_PIN, 4, saved_irq_con);
	pin_rmw(alive + GPA9_EINT + ECON, IRQ_PIN, 4, saved_econ);
	writel(BIT(IRQ_PIN), alive + GPA9_EINT + EPEND);
	pin_rmw(alive + GPA9_EINT + EMASK, IRQ_PIN, 1, saved_emask);
	/* The supply back as found. Without the driver the sensor is held in
	 * reset, as the boot leaves it: the line low, then its configuration
	 * back as found.
	 */
	if (!(saved_l15s & LDO_ENABLE))
		fp_power(false);
	pin_rmw(peric0 + GPP1 + PIN_DAT, RESET_PIN, 1, 0);
	pin_rmw(peric0 + GPP1 + PIN_CON, RESET_PIN, 4, saved_reset_con);
	pin_rmw(peric0 + GPP1 + PIN_PUD, RESET_PIN, 4, saved_reset_pud);
	pin_rmw(peric0 + GPP1 + PIN_CONPDN, RESET_PIN, 2, saved_reset_conpdn);
	pin_rmw(peric0 + GPP1 + PIN_PUDPDN, RESET_PIN, 4, saved_reset_pudpdn);
	iounmap(alive);
	iounmap(peric0);
	irq_dispose_mapping(fp_irq);
	root_device_unregister(fp_dev);
}
module_exit(fp_exit);

MODULE_DESCRIPTION("Pixel 7 Pro Goodix fingerprint sensor: reset, interrupt, /dev/goodix_fp");
MODULE_LICENSE("GPL");
