// SPDX-License-Identifier: GPL-2.0-only
/* Load before the ADSP starts (guacamole_adsp_test). Reboot to remove. */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include "voice_services_dtbo.h"

static int overlay_id = -1;
static int __init voice_services_init(void)
{
    struct device_node *adsp;
    bool started;
    int ret;
    if (!of_machine_is_compatible("oneplus,guacamole"))
        return -ENODEV;
    adsp = of_find_compatible_node(NULL, NULL, "qcom,sm8150-adsp-pas");
    if (!adsp)
        return -ENODEV;
    /* An enabled ADSP has already listed its APR services. */
    started = of_device_is_available(adsp);
    of_node_put(adsp);
    if (started)
        return -EBUSY;
    ret = of_overlay_fdt_apply(voice_services_dtbo, sizeof(voice_services_dtbo),
                               &overlay_id, NULL);
    if (ret && overlay_id >= 0)
        of_overlay_remove(&overlay_id);
    return ret;
}
module_init(voice_services_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Guacamole ADSP voice services (MVM, CVS, CVP); reboot to remove");
