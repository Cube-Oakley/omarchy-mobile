// SPDX-License-Identifier: GPL-2.0
/*
 * Adds the GS201 video codec's device-tree node (pixel-mfc-overlay.dts) for
 * mainline s5p-mfc's "google,gs201-mfc" variant, and removes it at unload.
 * The kernel then creates the platform device, and s5p-mfc binds to it once
 * loaded. The codec's domain must be on: the boot leaves it on with
 * /var/lib/omarchy-mobile/mfc-dev (kernel/mfc/README.md).
 */
#include <linux/module.h>
#include <linux/of.h>

#include "pixel_mfc_overlay.h"

static int ovcs_id;

static int __init pixel_mfc_dt_init(void)
{
	return of_overlay_fdt_apply(pixel_mfc_overlay, sizeof(pixel_mfc_overlay), &ovcs_id, NULL);
}

static void __exit pixel_mfc_dt_exit(void)
{
	of_overlay_remove(&ovcs_id);
}

module_init(pixel_mfc_dt_init);
module_exit(pixel_mfc_dt_exit);
MODULE_DESCRIPTION("GS201 video codec device-tree node for s5p-mfc");
MODULE_LICENSE("GPL");
