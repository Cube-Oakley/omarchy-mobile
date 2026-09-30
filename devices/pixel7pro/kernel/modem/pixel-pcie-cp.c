// SPDX-License-Identifier: GPL-2.0-only
/* Manual GS201 channel-0 host for cpif. No OF alias: never autoload.
 *
 * Uses only DWC's MSI component. pci_host_probe() gets custom config ops
 * on EL3's fixed CFG0 window. Never calls dw_pcie_host_init/setup_rc or
 * any iATU helper. The hardware sequence has passed the disposable G1
 * probe, including the BOOT-to-Gen3 x2 transition.
 *
 * Loading maps resources but leaves the CP off. cpif owns CP power GPIOs;
 * this driver owns PERST/CLKREQ, PCIe power, and the six controller IRQs.
 * ASPM is disabled for bring-up. No unload: cpif and its PCI devices have
 * no safe hot-remove lifecycle yet; recovery is an orderly reboot.
 */
#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/workqueue.h>
#include "pcie-designware.h"
#include "pcie-cp-api.h"
#include "pcie-cp-hw.h"

struct cp_host {
	struct pixel_pcie hw;
	struct dw_pcie dw;
	struct pci_dev *ep;
	struct pci_host_bridge *bridge;
	struct mutex power_lock;
	struct mutex event_lock;
	raw_spinlock_t config_lock;
	struct exynos_pcie_register_event *event;
	struct work_struct event_work;
	atomic_t pending;
	int irq[6];
	unsigned int queues;
	unsigned int speed;
	u32 split_mask;
	irq_handler_t queue_handler[4];
	void *queue_context[4];
	bool powered, enumerated, failed;
	bool cpl_timeout, linkdown;
};
static struct cp_host *host;
static struct platform_device *cp_host_pdev;

/* CPIF owns this link's complete power/config lifecycle, including while
 * the AP sleeps. Generic PCI PM must not save an isolated controller's
 * all-ones config or retrain it before CPIF processes CP2AP_WAKEUP on resume.
 * This domain covers only our private root port and the validated modem EP.
 */
static int cp_pm_noop(struct device *dev)
{
	return 0;
}

static int cp_pm_busy(struct device *dev)
{
	return -EBUSY;
}

static void cp_pm_complete(struct device *dev) {}

static struct dev_pm_domain cp_pm_domain = {
	.ops = {
		.prepare = cp_pm_noop,
		.complete = cp_pm_complete,
		.suspend = cp_pm_noop,
		.suspend_late = cp_pm_noop,
		.suspend_noirq = cp_pm_noop,
		.resume_noirq = cp_pm_noop,
		.resume_early = cp_pm_noop,
		.resume = cp_pm_noop,
		.freeze = cp_pm_busy,
		.poweroff = cp_pm_busy,
		.runtime_suspend = cp_pm_busy,
		.runtime_resume = cp_pm_noop,
		.runtime_idle = cp_pm_busy,
	},
};

/* Slow log checkpoints are useful for a first MMIO probe, but adding them
 * to every runtime wake stalls modem traffic for about 1.7 seconds. Keep
 * the hardware reset/PERST delays in train_link; only checkpoint pacing
 * is optional. The disposable ROM probe retains its 150 ms checkpoints.
 */
static unsigned int diagnostic_delay_ms;
module_param(diagnostic_delay_ms, uint, 0444);
MODULE_PARM_DESC(diagnostic_delay_ms, "Optional delay after MMIO log checkpoints (default 0 ms)");

/* DWC's MSI callbacks may be invoked while the link is off. Guard all
 * accesses to the isolated controller, including deferred MSI masking.
 */
static u32 cp_dbi_read(struct dw_pcie *pci, void __iomem *base, u32 reg, size_t size)
{
	struct cp_host *h = container_of(pci, struct cp_host, dw);
	unsigned long flags;
	u32 val = 0;

	raw_spin_lock_irqsave(&h->config_lock, flags);
	if (h->powered)
		dw_pcie_read(base + reg, size, &val);
	raw_spin_unlock_irqrestore(&h->config_lock, flags);
	return val;
}

static void cp_dbi_write(struct dw_pcie *pci, void __iomem *base, u32 reg, size_t size, u32 val)
{
	struct cp_host *h = container_of(pci, struct cp_host, dw);
	unsigned long flags;

	raw_spin_lock_irqsave(&h->config_lock, flags);
	if (h->powered)
		dw_pcie_write(base + reg, size, val);
	raw_spin_unlock_irqrestore(&h->config_lock, flags);
}

static const struct dw_pcie_ops cp_dw_ops = {
	.read_dbi = cp_dbi_read,
	.write_dbi = cp_dbi_write,
};

static void set_powered(struct cp_host *h, bool on)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&h->config_lock, flags);
	WRITE_ONCE(h->powered, on);
	raw_spin_unlock_irqrestore(&h->config_lock, flags);
}

/* The IOCC registers make DMA coherent even though the stock host node
 * lacks dma-coherent. DWC MSI does not allocate or map any packet buffers.
 */
static void __iomem *cp_map_bus(struct pci_bus *bus, unsigned int devfn, int where)
{
	struct cp_host *h = bus->sysdata;

	if (devfn || where < 0 || where >= SZ_4K || !READ_ONCE(h->powered))
		return NULL;
	if (pci_is_root_bus(bus))
		return h->hw.dbi + where;
	if (bus->number != 1 || !link_up(&h->hw))
		return NULL;
	return h->hw.config + where;
}

static int cp_config_read(struct pci_bus *bus, unsigned int devfn, int where,
			  int size, u32 *val)
{
	struct cp_host *h = bus->sysdata;
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&h->config_lock, flags);
	ret = pci_generic_config_read(bus, devfn, where, size, val);
	raw_spin_unlock_irqrestore(&h->config_lock, flags);

	/* ROM and BL1 advertise class 0. Normalize only the class fields to
	 * PCI_CLASS_OTHERS so PCI enables memory decoding for this endpoint.
	 * The vendor/device match remains exact in cpif.
	 */
	if (!ret && !pci_is_root_bus(bus) && where == PCI_CLASS_REVISION && size == 4 &&
	    !(*val >> 8))
		*val |= PCI_CLASS_OTHERS << 24;
	return ret;
}

static int cp_config_write(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 val)
{
	struct cp_host *h = bus->sysdata;
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&h->config_lock, flags);
	/* Keep the ROM at Gen1 even if PCI's generic link-speed recovery
	 * tries to remove the restriction during enumeration.
	 */
	if (pci_is_root_bus(bus) && where == RC_EXP_CAP + PCI_EXP_LNKCTL2)
		val = (val & ~PCI_EXP_LNKCTL2_TLS) | h->speed;
	ret = pci_generic_config_write(bus, devfn, where, size, val);
	raw_spin_unlock_irqrestore(&h->config_lock, flags);
	return ret;
}

static struct pci_ops cp_config_ops = {
	.map_bus = cp_map_bus,
	.read = cp_config_read,
	.write = cp_config_write,
};

static irqreturn_t cp_queue_irq(int irq, void *data)
{
	struct cp_host *h = data;
	unsigned int i;
	irq_handler_t handler;

	if (!READ_ONCE(h->powered))
		return IRQ_NONE;
	for (i = 0; i < 4; i++) {
		if (h->irq[i + 2] != irq)
			continue;
		writel(1, h->hw.dbi + 0x830 + (i + 1) * 12);
		handler = smp_load_acquire(&h->queue_handler[i]);
		if (handler)
			handler(irq, h->queue_context[i]);
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

static void cp_event_work(struct work_struct *work)
{
	struct cp_host *h = container_of(work, struct cp_host, event_work);
	struct exynos_pcie_register_event *reg;
	unsigned int pending = atomic_xchg(&h->pending, 0), event;

	mutex_lock(&h->event_lock);
	reg = h->event;
	for (event = 1; reg && event <= EXYNOS_PCIE_EVENT_CPL_TIMEOUT; event <<= 1) {
		if (!(event & pending & reg->events))
			continue;
		reg->notify.event = event;
		reg->notify.user = reg->user;
		reg->notify.options = reg->options;
		if (reg->mode == EXYNOS_PCIE_TRIGGER_CALLBACK && reg->callback)
			reg->callback(&reg->notify);
	}
	mutex_unlock(&h->event_lock);
}

static irqreturn_t cp_event_irq(int irq, void *data)
{
	struct cp_host *h = data;
	u32 v0, v1, v2;
	unsigned int event = 0;

	/* IRQs are disabled and synchronized before the PHY can be isolated. */
	if (!READ_ONCE(h->powered))
		return IRQ_NONE;
	v0 = elbi_read(&h->hw, ELBI_IRQ0);
	v1 = elbi_read(&h->hw, ELBI_IRQ1);
	v2 = elbi_read(&h->hw, ELBI_IRQ2);
	elbi_write(&h->hw, ELBI_IRQ0, v0);
	elbi_write(&h->hw, ELBI_IRQ1, v1);
	elbi_write(&h->hw, ELBI_IRQ2, v2);
	if (v1 & IRQ1_LINK_DOWN) {
		WRITE_ONCE(h->linkdown, true);
		event |= EXYNOS_PCIE_EVENT_LINKDOWN;
	}
	if (v2 & IRQ2_CPL_TIMEOUT) {
		WRITE_ONCE(h->cpl_timeout, true);
		event |= EXYNOS_PCIE_EVENT_CPL_TIMEOUT;
	}
	if (event) {
		atomic_or(event, &h->pending);
		schedule_work(&h->event_work);
	}
	return v0 || v1 || v2 ? IRQ_HANDLED : IRQ_NONE;
}

static void cp_msi_program(struct cp_host *h)
{
	unsigned int i;

	dw_pcie_msi_init(&h->dw.pp);
	/* Only block 0 uses DWC's domain; blocks 1..4 belong to pktproc.
	 * The vendor assigns each queue the low bit of its own block.
	 */
	for (i = 1; i < 5; i++) {
		writel(h->split_mask & BIT(i) ? 1 : 0, h->hw.dbi + 0x828 + i * 12);
		writel(h->split_mask & BIT(i) ? ~1U : ~0U, h->hw.dbi + 0x82c + i * 12);
		writel(~0U, h->hw.dbi + 0x830 + i * 12);
	}
}

static void cp_resume_rx_queues(struct cp_host *h)
{
	unsigned long flags;
	unsigned int i;
	irq_handler_t handler;

	/* Shared packet descriptors survive link power collapse, but the MSI
	 * controller does not. A packet arriving around PERST can therefore
	 * remain queued without an interrupt after MSI reinitialization. Let
	 * each registered pktproc handler check its ring and schedule NAPI.
	 * Empty rings are ignored by that handler; this does not fabricate an
	 * MSI status bit or acknowledge a concurrent hardware interrupt.
	 */
	local_irq_save(flags);
	for (i = 0; i < ARRAY_SIZE(h->queue_handler); i++) {
		handler = smp_load_acquire(&h->queue_handler[i]);
		if (handler)
			handler(h->irq[i + 2], h->queue_context[i]);
	}
	local_irq_restore(flags);
}

static void cp_irq_disable(struct cp_host *h)
{
	unsigned int i;

	elbi_write(&h->hw, ELBI_IRQ0_EN, 0);
	elbi_write(&h->hw, ELBI_IRQ1_EN, 0);
	elbi_write(&h->hw, ELBI_IRQ2_EN, 0);
	for (i = 0; i < ARRAY_SIZE(h->irq); i++)
		disable_irq(h->irq[i]);
}

static void cp_irq_enable(struct cp_host *h)
{
	unsigned int i;

	elbi_write(&h->hw, ELBI_IRQ0, elbi_read(&h->hw, ELBI_IRQ0));
	elbi_write(&h->hw, ELBI_IRQ1, elbi_read(&h->hw, ELBI_IRQ1));
	elbi_write(&h->hw, ELBI_IRQ2, elbi_read(&h->hw, ELBI_IRQ2));
	elbi_write(&h->hw, ELBI_IRQ1_EN, IRQ1_LINK_DOWN);
	elbi_write(&h->hw, ELBI_IRQ2_EN, IRQ2_CPL_TIMEOUT); /* MSI bit 17 stays off */
	for (i = 0; i < ARRAY_SIZE(h->irq); i++)
		enable_irq(h->irq[i]);
}

static int cp_enumerate(struct cp_host *h)
{
	struct pci_bus *bus;
	int ret;

	ret = pci_host_probe(h->bridge);
	if (ret)
		return ret;
	h->enumerated = true;
	bus = pci_find_bus(pci_domain_nr(h->bridge->bus), 1);
	if (!bus)
		return -ENODEV;
	h->ep = pci_get_slot(bus, 0);
	if (!h->ep || h->ep->vendor != 0x144d || h->ep->device != 0xa5a5)
		return -ENODEV;
	if (pci_resource_start(h->ep, 0) != 0x40000000 ||
	    pci_resource_len(h->ep, 0) <= 0x60000) {
		dev_err(h->hw.dev, "unexpected EP BAR0: %pR\n", &h->ep->resource[0]);
		return -EINVAL;
	}
	if (!bus->self || h->ep->dev.pm_domain || bus->self->dev.pm_domain)
		return -EINVAL;
	dev_pm_domain_set(&bus->self->dev, &cp_pm_domain);
	dev_pm_domain_set(&h->ep->dev, &cp_pm_domain);
	dev_info(h->hw.dev, "modem enumerated; BAR0=%pR\n", &h->ep->resource[0]);
	return 0;
}

int exynos_pcie_poweron(int ch_num, int spd)
{
	struct cp_host *h = host;
	struct arm_smccc_res smc;
	int ret;
	bool irqs_on = false;

	if (ch_num || !h || (spd != 1 && spd != 3))
		return -ENODEV;
	mutex_lock(&h->power_lock);
	if (h->failed) {
		ret = -EIO;
		goto unlock;
	}
	if (h->powered) {
		ret = link_up(&h->hw) ? 0 : -ENOLINK;
		goto unlock;
	}
	h->speed = spd;
	h->hw.diagnostic_delay_ms = diagnostic_delay_ms;
	stage(&h->hw, "host release PHY isolation");
	ret = pixel_pcie_isolation(&h->hw, true);
	if (ret)
		goto unlock;
	elbi_write(&h->hw, ELBI_IRQ0_EN, 0);
	elbi_write(&h->hw, ELBI_IRQ1_EN, 0);
	elbi_write(&h->hw, ELBI_IRQ2_EN, 0);
	pin_set(&h->hw, PIN_PERST, false);
	pin_config(&h->hw, PIN_PERST, CON_OUTPUT, PUD_NONE, PUD_NONE);
	pin_config(&h->hw, PIN_CLKREQ, CON_CLKREQ, PUD_UP, PUD_UP);
	elbi_write(&h->hw, ELBI_LTSSM_EN, 0);
	pixel_phy_power_down(&h->hw);
	pixel_phy_power_up(&h->hw);
	ret = train_link(&h->hw, spd);
	if (ret)
		goto powerdown;
	stage(&h->hw, "host secure ATU");
	arm_smccc_smc(0x820020d8, 0, 0, 0, 0, 0, 0, 0, &smc);
	if (smc.a0 || !link_up(&h->hw)) {
		ret = -EIO;
		goto powerdown;
	}
	if (readl(h->hw.config) != 0xa5a5144d) {
		ret = -ENODEV;
		goto powerdown;
	}
	set_powered(h, true);
	WRITE_ONCE(h->cpl_timeout, false);
	WRITE_ONCE(h->linkdown, false);
	cp_msi_program(h);
	cp_irq_enable(h);
	irqs_on = true;
	if (!h->enumerated) {
		stage(&h->hw, "Linux PCI enumeration");
		ret = cp_enumerate(h);
		if (ret) {
			h->failed = true; /* clean reboot after partial enumeration */
			goto powerdown;
		}
	} else {
		/* Restore the root-port forwarding window and the EP state
		 * saved before PERST. cpif also restores its own saved copy.
		 */
		writel(0x153014e0, h->hw.dbi + PCI_MEMORY_BASE);
		pci_restore_state(h->ep);
		cp_resume_rx_queues(h);
	}
	ret = 0;
	goto unlock;
powerdown:
	if (irqs_on)
		cp_irq_disable(h);
	set_powered(h, false);
	turn_off_link(&h->hw);
	pixel_pcie_isolation(&h->hw, false);
unlock:
	if (ret)
		dev_err(h->hw.dev, "PCIe power-on failed: %d\n", ret);
	mutex_unlock(&h->power_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos_pcie_poweron);

int exynos_pcie_poweroff(int ch_num)
{
	struct cp_host *h = host;
	int ret = 0;

	if (ch_num || !h)
		return -ENODEV;
	mutex_lock(&h->power_lock);
	if (h->powered) {
		if (h->ep && link_up(&h->hw))
			pci_save_state(h->ep);
		cp_irq_disable(h);
		set_powered(h, false);
		turn_off_link(&h->hw);
		ret = pixel_pcie_isolation(&h->hw, false);
	}
	mutex_unlock(&h->power_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos_pcie_poweroff);

int exynos_pcie_rc_chk_link_status(int ch_num)
{
	struct cp_host *h = host;
	unsigned long flags;
	bool up;

	if (ch_num || !h)
		return 0;
	/* cpif calls from atomic contexts; serialize with isolation. */
	raw_spin_lock_irqsave(&h->config_lock, flags);
	up = h->powered && !h->linkdown && !h->cpl_timeout && link_up(&h->hw);
	raw_spin_unlock_irqrestore(&h->config_lock, flags);
	return up;
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_chk_link_status);

void exynos_pcie_set_perst_gpio(int ch_num, bool on)
{
	if (!ch_num && host)
		pin_set(&host->hw, PIN_PERST, on);
}
EXPORT_SYMBOL_GPL(exynos_pcie_set_perst_gpio);

int exynos_pcie_rc_l1ss_ctrl(int enable, int id, int ch_num)
{
	/* L1 substates intentionally disabled until boot/runtime is stable. */
	return ch_num || !host ? -ENODEV : 0;
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_l1ss_ctrl);

int exynos_pcie_register_event(struct exynos_pcie_register_event *reg)
{
	if (!host || !reg || reg->mode != EXYNOS_PCIE_TRIGGER_CALLBACK || !reg->callback)
		return -EINVAL;
	mutex_lock(&host->event_lock);
	if (host->event && host->event != reg) {
		mutex_unlock(&host->event_lock);
		return -EBUSY;
	}
	host->event = reg;
	mutex_unlock(&host->event_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(exynos_pcie_register_event);

int exynos_pcie_deregister_event(struct exynos_pcie_register_event *reg)
{
	if (!host)
		return -ENODEV;
	mutex_lock(&host->event_lock);
	if (host->event == reg)
		host->event = NULL;
	mutex_unlock(&host->event_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(exynos_pcie_deregister_event);

int register_separated_msi_vector(int ch_num, irq_handler_t handler, void *context, int *irq_num)
{
	struct cp_host *h = host;
	unsigned int block;
	int ret;

	if (ch_num || !h || !handler || !irq_num)
		return -EINVAL;
	mutex_lock(&h->power_lock);
	if (!h->powered || h->queues >= 4) {
		ret = -ENOSPC;
		goto out;
	}
	block = h->queues + 1;
	h->queue_context[block - 1] = context;
	smp_store_release(&h->queue_handler[block - 1], handler);
	h->queues++;
	h->split_mask |= BIT(block);
	writel(1, h->hw.dbi + 0x828 + block * 12);
	writel(~1U, h->hw.dbi + 0x82c + block * 12);
	*irq_num = h->irq[block + 1];
	ret = block * 32; /* vendor API returns the MSI data value */
out:
	mutex_unlock(&h->power_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(register_separated_msi_vector);

int exynos_pcie_rc_set_outbound_atu(int ch_num, u32 target_addr, u32 offset, u32 size)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_set_outbound_atu);

bool exynos_pcie_rc_get_cpl_timeout_state(int ch_num)
{
	return !ch_num && host && READ_ONCE(host->cpl_timeout);
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_get_cpl_timeout_state);
void exynos_pcie_rc_set_cpl_timeout_state(int ch_num, bool recovery)
{
	if (!ch_num && host)
		WRITE_ONCE(host->cpl_timeout, recovery);
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_set_cpl_timeout_state);
bool exynos_pcie_rc_get_sudden_linkdown_state(int ch_num)
{
	return !ch_num && host && READ_ONCE(host->linkdown);
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_get_sudden_linkdown_state);
void exynos_pcie_rc_set_sudden_linkdown_state(int ch_num, bool recovery)
{
	if (!ch_num && host)
		WRITE_ONCE(host->linkdown, recovery);
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_set_sudden_linkdown_state);
void exynos_pcie_set_ready_cto_recovery(int ch_num)
{
	if (!ch_num && host) {
		WRITE_ONCE(host->linkdown, false);
		WRITE_ONCE(host->cpl_timeout, false);
	}
}
EXPORT_SYMBOL_GPL(exynos_pcie_set_ready_cto_recovery);
void exynos_pcie_rc_force_linkdown_work(int ch_num)
{
	if (!ch_num && host) {
		WRITE_ONCE(host->linkdown, true);
		atomic_or(EXYNOS_PCIE_EVENT_LINKDOWN, &host->pending);
		schedule_work(&host->event_work);
	}
}
EXPORT_SYMBOL_GPL(exynos_pcie_rc_force_linkdown_work);
void exynos_pcie_rc_register_dump(int ch_num) { }
EXPORT_SYMBOL_GPL(exynos_pcie_rc_register_dump);
void exynos_pcie_rc_dump_all_status(int ch_num) { }
EXPORT_SYMBOL_GPL(exynos_pcie_rc_dump_all_status);
void exynos_pcie_rc_print_msi_register(int ch_num) { }
EXPORT_SYMBOL_GPL(exynos_pcie_rc_print_msi_register);

static int cp_host_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cp_host *h;
	struct resource_entry *win;
	unsigned int i;
	int ret;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->hw.dev = dev;
	h->hw.isolation = 0x18063ec0;
	mutex_init(&h->power_lock);
	mutex_init(&h->event_lock);
	raw_spin_lock_init(&h->config_lock);
	INIT_WORK(&h->event_work, cp_event_work);
	atomic_set(&h->pending, 0);
	if (!devm_request_mem_region(dev, 0x11920000, 0x2000, "pixel-pcie-cp"))
		return -EBUSY;
#define MAP(field, addr, size) do { \
	h->hw.field = devm_ioremap(dev, addr, size); \
	if (!h->hw.field) return -ENOMEM; \
} while (0)
	MAP(elbi, 0x11920000, 0x2000);
	MAP(phy, 0x11950000, 0x2000);
	MAP(pcs, 0x11940000, 0x1000);
	MAP(ia, 0x11900000, 0x1000);
	MAP(sysreg, 0x11820000, 0x2000);
	MAP(dbi, 0x11c00000, 0x1000);
	MAP(dbi2, 0x11d00000, 0x40);
	MAP(config, 0x40ffe000, 0x1000);
	MAP(gpio, 0x11840000, 0x20);
#undef MAP
	for (i = 0; i < ARRAY_SIZE(h->irq); i++) {
		h->irq[i] = of_irq_get(dev->of_node, i);
		if (h->irq[i] <= 0)
			return h->irq[i] ?: -EINVAL;
	}
	ret = devm_request_irq(dev, h->irq[0], cp_event_irq, IRQF_NO_AUTOEN,
			       "cp-pcie-events", h);
	if (ret)
		return ret;
	for (i = 2; i < ARRAY_SIZE(h->irq); i++) {
		ret = devm_request_irq(dev, h->irq[i], cp_queue_irq, IRQF_NO_AUTOEN,
				       "cp-pcie-queue", h);
		if (ret)
			return ret;
	}
	h->dw.dev = dev;
	h->dw.ops = &cp_dw_ops;
	h->dw.dbi_base = h->hw.dbi;
	h->dw.pp.num_vectors = 32;
	h->dw.pp.msi_irq[0] = h->irq[1];
	h->dw.pp.cfg0_base = 0xf6200000; /* MSI helper selects this fixed 32-bit target */
	h->dw.pp.use_imsi_rx = true;
	raw_spin_lock_init(&h->dw.pp.lock);
	ret = dw_pcie_msi_host_init(&h->dw.pp);
	if (ret)
		return ret;
	/* No controller access until poweron. The MSI helper only registered
	 * the domain and chained handler; keep its GIC parent disabled.
	 */
	disable_irq(h->irq[1]);
	h->bridge = devm_pci_alloc_host_bridge(dev, 0);
	if (!h->bridge) {
		dw_pcie_free_msi(&h->dw.pp);
		return -ENOMEM;
	}
	h->dw.pp.bridge = h->bridge;
	h->bridge->sysdata = h;
	h->bridge->ops = &cp_config_ops;
	h->bridge->child_ops = &cp_config_ops;
	h->bridge->native_aer = 0;
	h->bridge->native_pme = 0;
	h->bridge->native_dpc = 0;
	h->bridge->native_pcie_hotplug = 0;
	dev_set_msi_domain(&h->bridge->dev, h->dw.pp.irq_domain);
	resource_list_for_each_entry(win, &h->bridge->windows) {
		if (resource_type(win->res) == IORESOURCE_MEM)
			win->res->end = win->res->start + SZ_4M + SZ_2M - 1;
		if (resource_type(win->res) == IORESOURCE_BUS)
			win->res->end = 1;
	}
	platform_set_drvdata(pdev, h);
	host = h;
	dev_info(dev, "manual channel-0 host ready, CP remains off\n");
	return 0;
}

static struct platform_driver cp_host_driver = {
	.probe = cp_host_probe,
	.driver = { .name = "pixel-pcie-cp", .suppress_bind_attrs = true },
};

static int __init cp_host_init(void)
{
	struct device_node *np;
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	np = of_find_node_by_path("/pcie@11920000");
	if (!np)
		return -ENODEV;
	ret = platform_driver_register(&cp_host_driver);
	if (ret) {
		of_node_put(np);
		return ret;
	}
	cp_host_pdev = platform_device_alloc("pixel-pcie-cp", -1);
	if (!cp_host_pdev) {
		ret = -ENOMEM;
		of_node_put(np);
		goto unregister;
	}
	cp_host_pdev->dev.of_node = np;
	dev_set_of_node_reused(&cp_host_pdev->dev);
	ret = platform_device_add(cp_host_pdev);
	if (ret) {
		platform_device_put(cp_host_pdev);
		goto unregister;
	}
	if (!host) {
		platform_device_unregister(cp_host_pdev);
		ret = -ENODEV;
		goto unregister;
	}
	return 0;
unregister:
	platform_driver_unregister(&cp_host_driver);
	return ret;
}
module_init(cp_host_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Manual GS201 modem PCIe host, fixed secure windows, DWC MSI only");
