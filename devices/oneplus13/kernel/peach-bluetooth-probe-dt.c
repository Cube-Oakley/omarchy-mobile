// SPDX-License-Identifier: GPL-2.0
/* Guarded runtime UART/BT overlay. Reboot to remove it. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pci.h>

#include "peach_bluetooth_overlay.h"

static bool apply;
module_param(apply, bool, 0444);
MODULE_PARM_DESC(apply, "Apply the exact CPH2655 probe20 Bluetooth identification overlay");
static int overlay_id;

static const struct {
	const char *path;
	u32 phandle;
} required_nodes[] = {
	{ "/soc@0/pinctrl@f100000", 0x5f },
	{ "/soc@0/spmi@c400000/pmic@5/gpio@8800", 0xb9 },
	{ "/soc@0/rsc@16500000/regulators-2/ldo3", 0x146 },
	{ "/soc@0/rsc@16500000/regulators-2/ldo2", 0x147 },
	{ "/soc@0/rsc@16500000/regulators-1/smps4", 0x148 },
	{ "/soc@0/rsc@16500000/regulators-2/smps5", 0x149 },
	{ "/soc@0/rsc@16500000/regulators-3/smps3", 0x120 },
	{ "/soc@0/rsc@16500000/regulators-4/smps7", 0x122 },
};

static int __init peach_bt_dt_init(void)
{
	struct device_node *node;
	struct pci_dev *wlan;
	const __be32 *clocks;
	int i, len;
	bool valid;

	if (!apply || !of_machine_is_compatible("oneplus,dodge"))
		return -EPERM;
	for (i = 0; i < ARRAY_SIZE(required_nodes); i++) {
		node = of_find_node_by_path(required_nodes[i].path);
		valid = node && node->phandle == required_nodes[i].phandle;
		of_node_put(node);
		if (!valid)
			return -EINVAL;
	}
	node = of_find_node_by_path("/peach-pmu");
	clocks = of_get_property(node, "clocks", &len);
	valid = clocks && len == 8 && be32_to_cpu(clocks[0]) == 0x6a &&
		be32_to_cpu(clocks[1]) == 6;
	of_node_put(node);
	if (!valid)
		return -EINVAL;
	/* The stock BT power sequence's normal path assumes WLAN is already on.
	 * Keep its own driver and regulator votes intact during this probe.
	 */
	wlan = pci_get_device(0x17cb, 0x110e, NULL);
	valid = wlan && wlan->driver;
	pci_dev_put(wlan);
	if (!valid)
		return -ENODEV;
	node = of_find_node_by_path("/soc@0/geniqup@8c0000/serial@898000");
	valid = node && !of_device_is_available(node) &&
		of_alias_get_id(node, "serial") == 0;
	of_node_put(node);
	if (!valid)
		return -EBUSY;
	return of_overlay_fdt_apply(peach_bluetooth_overlay,
				    sizeof(peach_bluetooth_overlay), &overlay_id, NULL);
}

module_init(peach_bt_dt_init);
/* No module_exit: live UART/serdev references must not outlive this overlay. */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Guarded CPH2655 Bluetooth ROM identification device tree");
