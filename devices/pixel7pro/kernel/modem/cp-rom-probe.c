// SPDX-License-Identifier: GPL-2.0-only
/* Disposable GS201 / S5300 ROM bring-up probe, not a PCI bus driver.
 *
 * Register sequences: Google's gs201 kernel, commit
 * 40ff93424549ffebfbba32e9435dfc58c40decb2, drivers/pci/controller/dwc/
 * pcie-exynos-{rc.c,gs201-rc-cal.c}, and drivers/soc/google/cpif/
 * {modem_ctrl_s5100.c,s51xx_pcie.c}. PHY tables adapted from pixel-pcie.c.
 *
 * phase=0 refuses before touching hardware (default).
 * phase=1 trains Gen1 x2; phase=2 also invokes the stock secure-ATU SMC
 * and reads the endpoint ID; phase=3 boots only the stock signed BOOT;
 * phase=4 also retrains the bootloader link at Gen3.
 * Every returning path powers CP down. Successful tests return -EAGAIN
 * deliberately: no resident module, aliases, autoload, or PCI enumeration.
 *
 * Arm wdtkick and stream dmesg on the host before loading. This probe has
 * no iATU mapping/access, no partition access, no NV, and no radio control.
 */
#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/spinlock.h>

static uint phase;
module_param(phase, uint, 0400);
MODULE_PARM_DESC(phase, "1: link, 2: secure ATU/EP ID, 3: ROM boot, 4: BL Gen3; 0 refuses");

#include "cp-probe-gpio.h"

#include "pcie-cp-hw.h"

/* CP ROM mailbox in cp_msi_rmem, modem_ctrl.h. Cached mapping only, the
 * same attributes as the existing linear map. IOCC is enabled before DMA.
 */
struct rom_mailbox {
	u32 msi_data, msi_check, err_report, reserved;
	u32 boot_stage, img_addr_lo, img_addr_hi, img_size;
};
static_assert(sizeof(struct rom_mailbox) == 32);
static_assert(offsetof(struct rom_mailbox, boot_stage) == 0x10);

static int boot_rom(struct pixel_pcie *p, struct rom_mailbox *mb,
		    const struct firmware *fw)
{
	u32 bar, v, last = ~0U;
	u16 msi_flags;
	unsigned int i;

	stage(p, "endpoint BAR0 and DMA enable");
	if (!link_up(p))
		return -ENOLINK;
	/* Like s51xx_pcie_probe(): writing the doorbell address rounds BAR0
	 * down to its hardware alignment. Refuse an unexpected aperture.
	 */
	writel(0x14e60000, p->config + PCI_BASE_ADDRESS_0);
	bar = readl(p->config + PCI_BASE_ADDRESS_0);
	dev_info(p->dev, "BAR0 readback=%#x (expect base 0x14e00000)\n", bar);
	if ((bar & PCI_BASE_ADDRESS_SPACE_IO) ||
	    (bar & PCI_BASE_ADDRESS_MEM_MASK) != 0x14e00000)
		return -EINVAL;
	if ((bar & PCI_BASE_ADDRESS_MEM_TYPE_MASK) == PCI_BASE_ADDRESS_MEM_TYPE_64)
		writel(0, p->config + PCI_BASE_ADDRESS_1);
	/* Vendor's fixed MSI target, without unhandled IRQs: all five blocks
	 * masked/disabled; the mailbox itself is polled from DRAM.
	 */
	writel(0xf6200000, p->dbi + 0x820);
	writel(0, p->dbi + 0x824);
	for (i = 0; i < 5; i++) {
		writel(0, p->dbi + 0x828 + i * 12);
		writel(~0U, p->dbi + 0x82c + i * 12);
	}
	/* MSI_CONTROL=0x50 in pcie-exynos-rc.h. Match cpif's four base
	 * vectors and the DWC MSI message (target page, first hwirq = 0).
	 * The ROM learns the mailbox address from the endpoint capability.
	 */
	if (readb(p->config + 0x50) != PCI_CAP_ID_MSI)
		return -EINVAL;
	msi_flags = readw(p->config + 0x50 + PCI_MSI_FLAGS);
	dev_info(p->dev, "endpoint MSI flags=%#x\n", msi_flags);
	if ((msi_flags & PCI_MSI_FLAGS_QMASK) < (2 << 1))
		return -EINVAL;
	writew(msi_flags & ~PCI_MSI_FLAGS_ENABLE, p->config + 0x50 + PCI_MSI_FLAGS);
	writel(0xf6200000, p->config + 0x50 + PCI_MSI_ADDRESS_LO);
	if (msi_flags & PCI_MSI_FLAGS_64BIT) {
		writel(0, p->config + 0x50 + PCI_MSI_ADDRESS_HI);
		writew(0, p->config + 0x50 + PCI_MSI_DATA_64);
	} else {
		writew(0, p->config + 0x50 + PCI_MSI_DATA_32);
	}
	writew((msi_flags & ~PCI_MSI_FLAGS_QSIZE) | (2 << 4) | PCI_MSI_FLAGS_ENABLE,
	       p->config + 0x50 + PCI_MSI_FLAGS);
	writew(readw(p->config + PCI_COMMAND) | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER,
	       p->config + PCI_COMMAND);
	/* Firmware was copied before power-on; the CPU and CP share it via
	 * IOCC. Do not overwrite boot_stage after the ROM has started.
	 */
	WRITE_ONCE(mb->img_addr_lo, 0xea410000);
	WRITE_ONCE(mb->img_addr_hi, 0);
	WRITE_ONCE(mb->img_size, fw->size);
	dma_wmb();
	stage(p, "ROM doorbell 0 with DOORBELL_INT_MASK");
	if (!link_up(p))
		return -ENOLINK;
	/* request_pcie_int(): DOORBELL_INT_MASK(int_ap2cp_msg), not raw 0. */
	writel(0x10000, p->doorbell);
	for (i = 0; i < 200; i++) {
		v = READ_ONCE(mb->boot_stage);
		dma_rmb();
		if (v != last) {
			dev_info(p->dev, "ROM boot_stage=%#x err_report=%#x wake=%d\n",
				 v, READ_ONCE(mb->err_report), cp_get(&cp2ap_wake));
			last = v;
		}
		if (v == 0x3fff)
			break;
		msleep(20);
	}
	if (v != 0x3fff)
		return -ETIMEDOUT;
	/* Stock shuts the ROM link down before waiting for BL1's wake. */
	turn_off_link(p);
	for (i = 0; i < 200 && !cp_get(&cp2ap_wake); i++)
		msleep(20);
	if (!cp_get(&cp2ap_wake))
		return -ETIMEDOUT;
	dev_info(p->dev, "G1 PASS: ROM=0x3fff and CP2AP_WAKEUP=1\n");
	return 0;
}

static int check_region(struct device_node *np, const char *name,
			phys_addr_t addr, resource_size_t size)
{
	struct resource r;
	int index = of_property_match_string(np, "reg-names", name);

	if (index < 0 || of_address_to_resource(np, index, &r) ||
	    r.start != addr || resource_size(&r) < size)
		return -EINVAL;
	return 0;
}

static int check_ram(const char *path, phys_addr_t addr, size_t size)
{
	struct device_node *np = of_find_node_by_path(path);
	struct reserved_mem *r;
	int ret = -EINVAL;

	if (!np)
		return ret;
	r = of_reserved_mem_lookup(np);
	if (r && r->base == addr && r->size == size &&
	    !of_property_read_bool(np, "no-map") &&
	    pfn_valid(PHYS_PFN(addr)) && PageReserved(pfn_to_page(PHYS_PFN(addr))))
		ret = 0;
	of_node_put(np);
	return ret;
}

static int __init cp_rom_probe_init(void)
{
	struct pixel_pcie p = { .isolation = 0x18063ec0, .diagnostic_delay_ms = 150 };
	struct device_node *np;
	const struct firmware *fw = NULL;
	struct rom_mailbox *mb = NULL;
	struct arm_smccc_res smc;
	void *ipc = NULL;
	u32 v;
	int i, ret = -EINVAL, cleanup_ret;
	bool bypass = false, powered = false, claimed = false;
	const char *secure;

	if (!phase || phase > 4 || !of_machine_is_compatible("google,GS201"))
		return -EINVAL;
	np = of_find_node_by_path("/pcie@11920000");
	if (!np)
		return -ENODEV;
	if (of_property_read_u32(np, "num-lanes", &v) || v != 2 ||
	    of_property_read_string(np, "use-secure-atu", &secure) || strcmp(secure, "true") ||
	    check_region(np, "elbi", 0x11920000, 0x2000) ||
	    check_region(np, "phy", 0x11950000, 0x2000) ||
	    check_region(np, "pcs", 0x11940000, 0x1000) ||
	    check_region(np, "ia", 0x11900000, 0x1000) ||
	    check_region(np, "sysreg", 0x11820000, 0x2000) ||
	    check_region(np, "dbi", 0x11c00000, 0x100040) ||
	    check_region(np, "config", 0x40ffe000, 0x1000)) {
		of_node_put(np);
		return -EINVAL;
	}
	of_node_put(np);
	/* Exclude any registered host owning this controller. Also do not run
	 * this disposable probe alongside the cpif driver's GPIO/DMA state.
	 */
	np = of_find_node_by_path("/cpif");
	if (np) {
		struct platform_device *cpdev = of_find_device_by_node(np);

		of_node_put(np);
		if (cpdev) {
			bool bound = cpdev->dev.driver != NULL;

			put_device(&cpdev->dev);
			if (bound)
				return -EBUSY;
		}
	}
	p.dev = root_device_register("cp-rom-probe");
	if (IS_ERR(p.dev))
		return PTR_ERR(p.dev);
	if (!request_mem_region(0x11920000, 0x2000, "cp-rom-probe")) {
		ret = -EBUSY;
		goto out;
	}
	claimed = true;
	if (phase >= 3) {
		if (check_ram("/reserved-memory/cp_rmem", 0xea400000, 0x800000) ||
		    check_ram("/reserved-memory/cp_msi_rmem", 0xf6200000, 0x1000))
			goto out;
		ret = request_firmware_direct(&fw, "modem-boot.bin", p.dev);
		if (ret)
			goto out;
		ret = -EINVAL;
		if (fw->size != 0x16800)
			goto out;
		ipc = memremap(0xea400000, 0x30000, MEMREMAP_WB);
		mb = memremap(0xf6200000, 0x1000, MEMREMAP_WB);
		if (!ipc || !mb) {
			ret = -ENOMEM;
			goto out;
		}
	}
	p.elbi = ioremap(0x11920000, 0x2000);
	p.phy = ioremap(0x11950000, 0x2000);
	p.pcs = ioremap(0x11940000, 0x1000);
	p.ia = ioremap(0x11900000, 0x1000);
	p.sysreg = ioremap(0x11820000, 0x2000);
	/* Narrow DBI mappings cannot reach the secure iATU at +0x300000. */
	p.dbi = ioremap(0x11c00000, 0x1000);
	p.dbi2 = ioremap(0x11d00000, 0x40);
	p.config = ioremap(0x40ffe000, 0x1000);
	p.doorbell = ioremap(0x40060000, 4);
	for (i = 0; i < NR_BANKS; i++)
		regs[i] = ioremap(bank_info[i].base, 0x20);
	p.gpio = regs[GPH0];
	ret = -ENOMEM;
	if (!p.elbi || !p.phy || !p.pcs || !p.ia || !p.sysreg || !p.dbi ||
	    !p.dbi2 || !p.config || !p.doorbell)
		goto out;
	for (i = 0; i < NR_BANKS; i++)
		if (!regs[i])
			goto out;

	stage(&p, "hold PERST and CP off; prepare reserved RAM");
	pin_set(&p, PIN_PERST, false);
	pin_config(&p, PIN_PERST, CON_OUTPUT, PUD_NONE, PUD_NONE);
	pin_config(&p, PIN_CLKREQ, CON_CLKREQ, PUD_UP, PUD_UP);
	cp_power_off();
	cp_set(&ap_wake, 0, 0);
	powered = true;
	if (phase >= 3) {
		memset(mb, 0, sizeof(*mb));
		WRITE_ONCE(*(u32 *)ipc, 0xbdbd);
		memcpy(ipc + 0x10000, fw->data, fw->size);
		dma_wmb();
	}
	stage(&p, "release PMU PHY isolation before controller access");
	ret = pixel_pcie_isolation(&p, true);
	if (ret)
		goto out;
	bypass = true;
	stage(&p, "first ELBI writes, all IRQs off");
	elbi_write(&p, ELBI_IRQ0_EN, 0);
	elbi_write(&p, ELBI_IRQ1_EN, 0);
	elbi_write(&p, ELBI_IRQ2_EN, 0);
	elbi_write(&p, ELBI_LTSSM_EN, 0);
	stage(&p, "PHY power-down");
	pixel_phy_power_down(&p);
	stage(&p, "CP GPIO power-on");
	cp_power_on();
	cp_print_lines("powered");
	stage(&p, "PHY power-down clear");
	pixel_phy_power_up(&p);
	ret = train_link(&p, 1);
	if (ret || phase == 1)
		goto out;
	stage(&p, "vendor secure-ATU SMC 0x820020d8");
	arm_smccc_smc(0x820020d8, 0, 0, 0, 0, 0, 0, 0, &smc);
	dev_info(p.dev, "secure-ATU result=%ld\n", (long)smc.a0);
	if (smc.a0) {
		ret = -EIO;
		goto out;
	}
	stage(&p, "first endpoint config read");
	if (!link_up(&p)) {
		ret = -ENOLINK;
		goto out;
	}
	v = readl(p.config + PCI_VENDOR_ID);
	dev_info(p.dev, "endpoint ID=%#x (expect 0xa5a5144d)\n", v);
	ret = v == 0xa5a5144d ? 0 : -ENODEV;
	if (!ret && phase >= 3)
		ret = boot_rom(&p, mb, fw);
	if (!ret && phase == 4) {
		/* boot_rom() stopped the ROM link and observed the BL1 wake.
		 * cpif now powers the same host up at Gen3 without cycling CP.
		 */
		stage(&p, "bootloader link: power-up clear, target Gen3");
		/* s5100_poweron_pcie(boot_on=false): wake CP before the host. */
		cp_set(&ap_wake, 1, 5);
		pixel_phy_power_up(&p);
		ret = train_link(&p, 3);
		if (ret)
			goto out;
		stage(&p, "bootloader secure ATU");
		arm_smccc_smc(0x820020d8, 0, 0, 0, 0, 0, 0, 0, &smc);
		if (smc.a0 || !link_up(&p)) {
			ret = -EIO;
			goto out;
		}
		stage(&p, "bootloader endpoint config read");
		v = readl(p.config + PCI_VENDOR_ID);
		dev_info(p.dev, "bootloader endpoint ID=%#x\n", v);
		ret = v == 0xa5a5144d ? 0 : -ENODEV;
	}
out:
	if (bypass) {
		stage(&p, "link shutdown and PHY power-down");
		turn_off_link(&p);
	}
	if (powered) {
		stage(&p, "CP power-off and AP GPIO release");
		cp_power_off();
		for (i = 0; i < ARRAY_SIZE(outputs); i++)
			cp_release(outputs[i]);
		cp_print_lines("off");
	}
	if (bypass) {
		stage(&p, "restore PHY isolation");
		cleanup_ret = pixel_pcie_isolation(&p, false);
		if (cleanup_ret)
			ret = cleanup_ret;
	}
	dev_info(p.dev, "phase=%u result=%d; probe exiting\n", phase, ret);
	for (i = 0; i < NR_BANKS; i++)
		if (regs[i])
			iounmap(regs[i]);
	if (p.elbi) iounmap(p.elbi);
	if (p.phy) iounmap(p.phy);
	if (p.pcs) iounmap(p.pcs);
	if (p.ia) iounmap(p.ia);
	if (p.sysreg) iounmap(p.sysreg);
	if (p.dbi) iounmap(p.dbi);
	if (p.dbi2) iounmap(p.dbi2);
	if (p.config) iounmap(p.config);
	if (p.doorbell) iounmap(p.doorbell);
	if (ipc) memunmap(ipc);
	if (mb) memunmap(mb);
	release_firmware(fw);
	if (claimed)
		release_mem_region(0x11920000, 0x2000);
	root_device_unregister(p.dev);
	return ret ?: -EAGAIN;
}
module_init(cp_rom_probe_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GS201 modem Gen1 link and signed ROM boot gate (manual, disposable)");
