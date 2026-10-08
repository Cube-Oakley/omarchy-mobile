// SPDX-License-Identifier: GPL-2.0-only
/* Pixel 7 Pro Bluetooth: power and UART18 for the BCM4389's Bluetooth core,
 * then a DT overlay that lets mainline samsung_tty and hci_bcm take over.
 *
 * From the stock DT and Google's GS201 sources:
 * - BT_REG_ON is gpp16-2 and BT device wake gpp16-3 (the nitrous node's
 *   shutdown and device-wakeup GPIOs), on the PERIC0 pin controller, bank
 *   offset 0x200 (pinctrl-gs201.c). Pins 0 and 1 of that bank are hsi2c_8's
 *   and are left alone.
 * - UART18 (0x181B0000) is CMU_APM's USI1_UART, its USI mode register is at
 *   sysreg_apm + 4 (samsung,usi-offset), and its pins are gpa3-0 to gpa3-3
 *   (TX, RX, CTS, RTS), function 2, on the ALIVE pin controller, bank
 *   offset 0x60.
 * - The vendor stack waits 100 ms after BT_REG_ON (BigHammerBtRegOnDelay).
 *
 * The bootloader leaves Bluetooth off (both GPIOs inputs), UART18's USI
 * unconfigured and in reset with its clocks running, and its pins inputs;
 * the module refuses any other state. It then powers the chip, holds device
 * wake asserted, puts USI18 in UART mode as mainline exynos-usi does, sets
 * the pins and applies the overlay (pixel-bt-overlay.dts). hci_uart binds to
 * the overlay's serdev node when loaded. Unloading reverses all of it.
 *
 * Sleep. When pixel-gpio provides device wake (gpp16-3) and host wake
 * (gpa6-6), the overlay hands them to hci_bcm, which then runs the chip's
 * low-power mode and owns device wake (sleep_ready=Y). Both GPIOs also get
 * the stock power-down state, "previous" (the nitrous pin states), so in
 * SYS_SLEEP, when PERIC0 is off and its pads are held, BT_REG_ON stays high
 * instead of the bootloader's state, which cut the chip's power;
 * pixel-sleep puts PERIC0's registers back before the pads are released.
 * USI18 and its pins are in the always-on APM domain; a syscore resume hook
 * checks that they kept their setup and restores it (counted in
 * resume_restored) before samsung_tty resumes the port.
 */
#include <linux/delay.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/syscore_ops.h>

#include "pixel_bt_overlay.h"

#define GPP16_BANK		0x10840200	/* PERIC0: CON, DAT, PUD */
#define BT_REG_ON		2
#define BT_DEV_WAKE		3
#define GPA3_BANK		0x180d0060	/* ALIVE: CON, DAT, PUD */
#define UART18			0x181b0000
#define USI_CON			0xc4
#define USI_CON_RESET		BIT(0)
#define USI_OPTION		0xc8
#define USI_OPTION_CLKREQ_ON	BIT(1)
#define USI_OPTION_CLKSTOP_ON	BIT(2)
#define SYSREG_APM_USI18	0x180204e4
#define USI_SW_CONF_UART	0x1
#define CMU_APM			0x18000000
#define USI1_UART_IPCLK		0x20ac
#define USI1_UART_PCLK		0x20b0
#define GATE_MANUAL		BIT(20)
#define GATE_CG_VAL		BIT(21)
#define GPIO_CON		0x0
#define GPIO_DAT		0x4
#define GPIO_PUD		0x8
#define GPIO_CON_PDN		0x10	/* 2 bits a pin: 0 low, 1 high, 2 input, 3 previous */
#define GPIO_PUD_PDN		0x14	/* 4 bits a pin: 0 none, 1 down, 3 up */
#define CON_OUTPUT		1
#define CON_UART		2
#define PDN_PREVIOUS		3
#define PUD_DOWN		1

static bool serdev = true;
module_param(serdev, bool, 0444);
MODULE_PARM_DESC(serdev, "Bluetooth serdev node for hci_uart (0: plain tty, for testing)");
static bool lpm = true;
module_param(lpm, bool, 0444);
MODULE_PARM_DESC(lpm, "Give hci_bcm the wake lines for the chip's low-power mode, when pixel-gpio provides them");
static bool sleep_ready;
module_param(sleep_ready, bool, 0444);
MODULE_PARM_DESC(sleep_ready, "Low-power mode wired: the chip can stay powered through SYS_SLEEP and wake the phone");
static unsigned int resume_restored;
module_param(resume_restored, uint, 0444);
MODULE_PARM_DESC(resume_restored, "Resumes after which USI18, its pins or BT_REG_ON had to be set up again");

static void __iomem *gpp16, *gpa3, *uart, *sw_conf, *cmu_apm;
static u32 gpp16_con, gpp16_dat, gpp16_con_pdn, gpp16_pud_pdn, gpa3_con, gpa3_pud, usi_con, usi_opt, conf;
static int overlay_id = -1;

static void rmw(void __iomem *reg, u32 mask, u32 val)
{
	writel((readl(reg) & ~mask) | (val & mask), reg);
}

static u32 nibbles(unsigned int first, unsigned int count, u32 val)
{
	u32 out = 0;

	while (count--)
		out |= val << (4 * (first + count));
	return out;
}

static bool gate_on(void __iomem *cmu, u32 off)
{
	u32 gate = readl(cmu + off);

	return !(gate & GATE_MANUAL) || (gate & GATE_CG_VAL);
}

static void bt_power(bool on)
{
	u32 bits = BIT(BT_REG_ON) | BIT(BT_DEV_WAKE);

	rmw(gpp16 + GPIO_DAT, bits, on ? bits : 0);
	rmw(gpp16 + GPIO_CON, nibbles(BT_REG_ON, 2, 0xf), nibbles(BT_REG_ON, 2, CON_OUTPUT));
}

static u32 pdn_mask(void)
{
	return (0x3 << (2 * BT_REG_ON)) | (0x3 << (2 * BT_DEV_WAKE));
}

static void set_pdn(void)
{
	rmw(gpp16 + GPIO_CON_PDN, pdn_mask(),
	    (PDN_PREVIOUS << (2 * BT_REG_ON)) | (PDN_PREVIOUS << (2 * BT_DEV_WAKE)));
	rmw(gpp16 + GPIO_PUD_PDN, nibbles(BT_REG_ON, 2, 0xf), PUD_DOWN << (4 * BT_DEV_WAKE));
}

static void usi_uart(void)
{
	writel(USI_SW_CONF_UART, sw_conf);
	writel(usi_con & ~USI_CON_RESET, uart + USI_CON);
	writel((usi_opt & ~USI_OPTION_CLKSTOP_ON) | USI_OPTION_CLKREQ_ON, uart + USI_OPTION);
	rmw(gpa3 + GPIO_PUD, 0xffff, 0);
	rmw(gpa3 + GPIO_CON, 0xffff, nibbles(0, 4, CON_UART));
}

/* Before devices resume: the sysreg and the GPIOs are always readable; the
 * USI's registers only with its clocks on, as at load.
 */
static void pixel_bt_resume(void *data)
{
	bool lost = readl(sw_conf) != USI_SW_CONF_UART ||
		    (readl(gpa3 + GPIO_CON) & 0xffff) != nibbles(0, 4, CON_UART) ||
		    (readl(gpp16 + GPIO_CON) & nibbles(BT_REG_ON, 1, 0xf)) != nibbles(BT_REG_ON, 1, CON_OUTPUT) ||
		    !(readl(gpp16 + GPIO_DAT) & BIT(BT_REG_ON));

	if (!gate_on(cmu_apm, USI1_UART_IPCLK) || !gate_on(cmu_apm, USI1_UART_PCLK))
		return;
	lost = lost || (readl(uart + USI_CON) & USI_CON_RESET) ||
	       !(readl(uart + USI_OPTION) & USI_OPTION_CLKREQ_ON);
	if (!lost)
		return;
	resume_restored++;
	rmw(gpp16 + GPIO_DAT, BIT(BT_REG_ON), BIT(BT_REG_ON));
	rmw(gpp16 + GPIO_CON, nibbles(BT_REG_ON, 1, 0xf), nibbles(BT_REG_ON, 1, CON_OUTPUT));
	usi_uart();
}

static const struct syscore_ops pixel_bt_syscore_ops = {
	.resume = pixel_bt_resume,
};

static struct syscore pixel_bt_syscore = {
	.ops = &pixel_bt_syscore_ops,
};

/* pixel-gpio's device-wake line and host-wake interrupt, if it provides them
 * (the overlay's references would otherwise leave hci_bcm deferring).
 */
static bool wake_lines(void)
{
	struct device_node *np;
	struct gpio_device *gdev = NULL;
	bool irq = false;

	np = of_find_node_by_path("/pinctrl@10840000/gpp16");
	if (np) {
		gdev = gpio_device_find_by_fwnode(of_fwnode_handle(np));
		of_node_put(np);
	}
	np = of_find_node_by_path("/pinctrl@180E0000/gpa6");
	if (np) {
		irq = irq_find_host(np) != NULL;
		of_node_put(np);
	}
	if (gdev)
		gpio_device_put(gdev);
	return gdev && irq;
}

static void restore(void)
{
	writel(usi_opt, uart + USI_OPTION);
	writel(usi_con, uart + USI_CON);
	writel(conf, sw_conf);
	rmw(gpa3 + GPIO_CON, 0xffff, gpa3_con);
	rmw(gpa3 + GPIO_PUD, 0xffff, gpa3_pud);
	/* Power off first: drive BT_REG_ON low, then hand the pins back. */
	bt_power(false);
	rmw(gpp16 + GPIO_CON, nibbles(BT_REG_ON, 2, 0xf), gpp16_con);
	rmw(gpp16 + GPIO_DAT, BIT(BT_REG_ON) | BIT(BT_DEV_WAKE), gpp16_dat);
	rmw(gpp16 + GPIO_CON_PDN, pdn_mask(), gpp16_con_pdn);
	rmw(gpp16 + GPIO_PUD_PDN, nibbles(BT_REG_ON, 2, 0xf), gpp16_pud_pdn);
}

static void unmap(void)
{
	if (gpp16)
		iounmap(gpp16);
	if (gpa3)
		iounmap(gpa3);
	if (uart)
		iounmap(uart);
	if (sw_conf)
		iounmap(sw_conf);
	if (cmu_apm)
		iounmap(cmu_apm);
}

static int __init pixel_bt_init(void)
{
	const unsigned char *overlay;
	size_t size;
	void *blob;
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	cmu_apm = ioremap(CMU_APM, 0x3000);
	if (!cmu_apm)
		return -ENOMEM;
	if (!gate_on(cmu_apm, USI1_UART_IPCLK) || !gate_on(cmu_apm, USI1_UART_PCLK)) {
		pr_err("pixel-bt: UART18 clocks are off\n");
		ret = -ENODEV;
		goto unmap;
	}

	ret = -ENOMEM;
	gpp16 = ioremap(GPP16_BANK, 0x20);
	gpa3 = ioremap(GPA3_BANK, 0x20);
	uart = ioremap(UART18, 0x100);
	sw_conf = ioremap(SYSREG_APM_USI18, 4);
	if (!gpp16 || !gpa3 || !uart || !sw_conf)
		goto unmap;

	gpp16_con = readl(gpp16 + GPIO_CON) & nibbles(BT_REG_ON, 2, 0xf);
	gpp16_dat = readl(gpp16 + GPIO_DAT) & (BIT(BT_REG_ON) | BIT(BT_DEV_WAKE));
	gpp16_con_pdn = readl(gpp16 + GPIO_CON_PDN) & pdn_mask();
	gpp16_pud_pdn = readl(gpp16 + GPIO_PUD_PDN) & nibbles(BT_REG_ON, 2, 0xf);
	gpa3_con = readl(gpa3 + GPIO_CON) & 0xffff;
	gpa3_pud = readl(gpa3 + GPIO_PUD) & 0xffff;
	conf = readl(sw_conf);
	usi_con = readl(uart + USI_CON);
	usi_opt = readl(uart + USI_OPTION);
	if (gpp16_con || gpa3_con || conf || !(usi_con & USI_CON_RESET)) {
		pr_err("pixel-bt: not as the bootloader leaves it (gpp16 %#x gpa3 %#x usi %#x/%#x)\n",
		       gpp16_con, gpa3_con, conf, usi_con);
		ret = -EBUSY;
		goto unmap;
	}

	set_pdn();
	bt_power(true);
	msleep(100);

	/* exynos_usi_configure() for USI v2, then the UART pins. */
	usi_uart();

	sleep_ready = serdev && lpm && wake_lines();
	if (sleep_ready) {
		overlay = pixel_bt_overlay;
		size = sizeof(pixel_bt_overlay);
	} else if (serdev) {
		overlay = pixel_bt_overlay_awake;
		size = sizeof(pixel_bt_overlay_awake);
	} else {
		overlay = pixel_bt_overlay_tty;
		size = sizeof(pixel_bt_overlay_tty);
	}
	ret = -ENOMEM;
	blob = kmemdup(overlay, size, GFP_KERNEL);
	if (!blob)
		goto restore;
	ret = of_overlay_fdt_apply(blob, size, &overlay_id, NULL);
	kfree(blob);
	if (ret) {
		pr_err("pixel-bt: overlay not applied: %d\n", ret);
		goto restore;
	}
	register_syscore(&pixel_bt_syscore);
	pr_info("pixel-bt: Bluetooth powered, UART18 at 197 MHz, %s\n",
		sleep_ready ? "low-power mode through pixel-gpio's wake lines" : "always awake");
	return 0;
restore:
	restore();
unmap:
	unmap();
	return ret;
}
module_init(pixel_bt_init);

static void __exit pixel_bt_exit(void)
{
	unregister_syscore(&pixel_bt_syscore);
	of_overlay_remove(&overlay_id);
	restore();
	unmap();
}
module_exit(pixel_bt_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pixel 7 Pro Bluetooth power and UART18");
