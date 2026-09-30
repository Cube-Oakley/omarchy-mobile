// SPDX-License-Identifier: GPL-2.0-only
/* Exercise pixel-pcie-cp through Linux PCI/MSI with only the signed BOOT.
 * Manual, one-shot, leaves CP off. No cpif, MAIN, NV or radio commands.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include "pcie-cp-api.h"
#include "cp-probe-gpio.h"

static bool run;
module_param(run, bool, 0400);
static atomic_t msi_count = ATOMIC_INIT(0);

static irqreturn_t probe_irq(int irq, void *data)
{
	atomic_inc(&msi_count);
	return IRQ_HANDLED;
}

static void progress(const char *text)
{
	pr_info("cp-host-probe: %s\n", text);
	msleep(150);
}

static bool ram_reserved(const char *path, phys_addr_t addr, size_t size)
{
	struct device_node *np = of_find_node_by_path(path);
	struct reserved_mem *r;
	bool ok = false;

	if (!np)
		return false;
	r = of_reserved_mem_lookup(np);
	if (r && r->base == addr && r->size == size &&
	    !of_property_read_bool(np, "no-map") &&
	    pfn_valid(PHYS_PFN(addr)) && PageReserved(pfn_to_page(PHYS_PFN(addr))))
		ok = true;
	of_node_put(np);
	return ok;
}

static int __init cp_host_probe_init(void)
{
	struct device *dev;
	const struct firmware *fw = NULL;
	struct pci_dev *ep = NULL;
	struct device_node *np;
	void *ipc = NULL;
	u32 *mb = NULL;
	void __iomem *doorbell = NULL;
	int ret = -EINVAL, nirq = 0, i;
	bool powered = false, vectors = false, enabled = false;
	u32 v = 0, last = ~0U;
	u16 link_status = 0;

	if (!run || !of_machine_is_compatible("google,GS201"))
		return -EINVAL;
	np = of_find_node_by_path("/cpif");
	if (np) {
		struct platform_device *cpdev = of_find_device_by_node(np);

		of_node_put(np);
		if (cpdev) {
			bool busy = cpdev->dev.driver != NULL;

			put_device(&cpdev->dev);
			if (busy)
				return -EBUSY;
		}
	}
	if (!ram_reserved("/reserved-memory/cp_rmem", 0xea400000, 0x800000) ||
	    !ram_reserved("/reserved-memory/cp_msi_rmem", 0xf6200000, 0x1000))
		return -EINVAL;
	/* Refuse to share a modem already owned by cpif or any PCI driver. */
	ep = pci_get_device(0x144d, 0xa5a5, NULL);
	if (ep) {
		bool busy = ep->driver != NULL;

		pci_dev_put(ep);
		ep = NULL;
		if (busy)
			return -EBUSY;
	}
	dev = root_device_register("cp-host-probe");
	if (IS_ERR(dev))
		return PTR_ERR(dev);
	ret = request_firmware_direct(&fw, "modem-boot.bin", dev);
	if (ret)
		goto out;
	ret = -EINVAL;
	if (fw->size != 0x16800)
		goto out;
	ipc = memremap(0xea400000, 0x30000, MEMREMAP_WB);
	mb = memremap(0xf6200000, 0x1000, MEMREMAP_WB);
	doorbell = ioremap(0x40060000, 4);
	for (i = 0; i < NR_BANKS; i++)
		regs[i] = ioremap(bank_info[i].base, 0x20);
	ret = -ENOMEM;
	if (!ipc || !mb || !doorbell)
		goto out;
	for (i = 0; i < NR_BANKS; i++)
		if (!regs[i])
			goto out;
	progress("CP off, prepare signed BOOT in reserved RAM");
	ret = exynos_pcie_poweroff(0);
	if (ret)
		goto out;
	cp_power_off();
	cp_set(&ap_wake, 0, 0);
	powered = true;
	memset(mb, 0, 32);
	WRITE_ONCE(*(u32 *)ipc, 0xbdbd);
	memcpy(ipc + 0x10000, fw->data, fw->size);
	dma_wmb();
	cp_power_on();
	progress("host poweron Gen1 and PCI enumeration");
	ret = exynos_pcie_poweron(0, 1);
	if (ret)
		goto out;
	ep = pci_get_device(0x144d, 0xa5a5, NULL);
	ret = -ENODEV;
	if (!ep || ep->driver || pci_resource_start(ep, 0) != 0x40000000)
		goto out;
	progress("allocate four PCI MSI vectors");
	ret = pci_enable_device(ep);
	if (ret)
		goto out;
	enabled = true;
	ret = pci_alloc_irq_vectors(ep, 4, 4, PCI_IRQ_MSI);
	if (ret != 4) {
		if (ret >= 0) {
			vectors = true;
			ret = -EINVAL;
		}
		goto out;
	}
	vectors = true;
	for (i = 0; i < 4; i++) {
		ret = request_irq(pci_irq_vector(ep, i), probe_irq, 0, "cp-host-probe", dev);
		if (ret)
			goto out;
		nirq++;
	}
	pci_set_master(ep);
	WRITE_ONCE(mb[5], 0xea410000);
	WRITE_ONCE(mb[6], 0);
	WRITE_ONCE(mb[7], fw->size);
	dma_wmb();
	progress("ROM doorbell with interrupt mask");
	writel(0x10000, doorbell);
	for (i = 0; i < 200; i++) {
		v = READ_ONCE(mb[4]);
		dma_rmb();
		if (v != last) {
			pr_info("cp-host-probe: ROM=%#x error=%#x wake=%d MSI_count=%d\n",
				v, READ_ONCE(mb[2]), cp_get(&cp2ap_wake), atomic_read(&msi_count));
			last = v;
		}
		if (v == 0x3fff)
			break;
		msleep(20);
	}
	ret = -ETIMEDOUT;
	if (v != 0x3fff)
		goto out;
	progress("power off ROM link and wait for BL wake");
	ret = exynos_pcie_poweroff(0);
	if (ret)
		goto out;
	for (i = 0; i < 200 && !cp_get(&cp2ap_wake); i++)
		msleep(20);
	if (!cp_get(&cp2ap_wake)) {
		ret = -ETIMEDOUT;
		goto out;
	}
	cp_set(&ap_wake, 1, 5);
	progress("host poweron Gen3 and restore PCI state");
	ret = exynos_pcie_poweron(0, 3);
	if (!ret) {
		pci_read_config_dword(ep, PCI_VENDOR_ID, &v);
		if (v != 0xa5a5144d)
			ret = -ENODEV;
		/* First L0 can still be Gen1 while automatic speed change is
		 * in progress. Read the settled link after poweron completes.
		 */
		pcie_capability_read_word(pci_upstream_bridge(ep), PCI_EXP_LNKSTA, &link_status);
		if ((link_status & PCI_EXP_LNKSTA_CLS) != 3 ||
		    ((link_status & PCI_EXP_LNKSTA_NLW) >> PCI_EXP_LNKSTA_NLW_SHIFT) != 2)
			ret = -EIO;
		pr_info("cp-host-probe: BL endpoint=%#x link_status=%#x MSI_count=%d result=%d\n",
			v, link_status, atomic_read(&msi_count), ret);
	}
out:
	for (i = 0; i < nirq; i++)
		free_irq(pci_irq_vector(ep, i), dev);
	if (vectors)
		pci_free_irq_vectors(ep);
	if (enabled) {
		pci_clear_master(ep);
		pci_disable_device(ep);
	}
	pci_dev_put(ep);
	if (powered) {
		progress("host and CP shutdown");
		cp_set(&ap_wake, 0, 5);
		exynos_pcie_poweroff(0);
		cp_power_off();
		for (i = 0; i < ARRAY_SIZE(outputs); i++)
			cp_release(outputs[i]);
		cp_print_lines("off");
	}
	for (i = 0; i < NR_BANKS; i++)
		if (regs[i])
			iounmap(regs[i]);
	if (doorbell) iounmap(doorbell);
	if (ipc) memunmap(ipc);
	if (mb) memunmap(mb);
	release_firmware(fw);
	root_device_unregister(dev);
	pr_info("cp-host-probe: result=%d (success deliberately returns EAGAIN)\n", ret);
	return ret ?: -EAGAIN;
}
module_init(cp_host_probe_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("One-shot signed ROM and BL test through pixel-pcie-cp PCI/MSI host");
