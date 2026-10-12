// SPDX-License-Identifier: GPL-2.0-only
/* Exact-kernel development diagnostic: inspect the SMMU driver's cached
 * stream state under its lock and power-managed, architected registers.
 * No stream configuration writes.
 * This uses a private kernel header and must never cross kernel builds.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include "arm-smmu.h"

static int __init stream_state_init(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct arm_smmu_device *smmu;
	struct resource *res;
	unsigned int i, matches = 0;
	int ret = 0;

	if (!of_machine_is_compatible("oneplus,cph2655-omarchy-probe"))
		return -ENODEV;
	/* The GPU SMMU shares this compatible; select the exact application SMMU. */
	np = of_find_node_by_path("/soc@0/iommu@15000000");
	if (!np)
		return -ENODEV;
	if (!of_device_is_compatible(np, "qcom,sm8750-smmu-500")) {
		of_node_put(np);
		return -ENODEV;
	}
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return -ENODEV;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != 0x15000000 || resource_size(res) != 0x100000) {
		put_device(&pdev->dev);
		return -ENODEV;
	}
	device_lock(&pdev->dev);
	smmu = platform_get_drvdata(pdev);
	if (!pdev->dev.driver || !smmu || !smmu->smrs || !smmu->s2crs) {
		ret = -ENODEV;
		goto out;
	}
	mutex_lock(&smmu->stream_map_mutex);
	pr_info("oneplus13-smmu: groups=%u context_banks=%u\n",
		smmu->num_mapping_groups, smmu->num_context_banks);
	for (i = 0; i < smmu->num_mapping_groups; i++) {
		struct arm_smmu_smr *smr = &smmu->smrs[i];
		struct arm_smmu_s2cr *s2cr = &smmu->s2crs[i];
		if (!smr->valid || ((0x800 ^ smr->id) & ~smr->mask))
			continue;
		matches++;
		pr_info("oneplus13-smmu: index=%u id=%04x mask=%04x type=%u bypass=%u cb=%u count=%u\n",
			i, smr->id, smr->mask, s2cr->type,
			s2cr->type == S2CR_TYPE_BYPASS, s2cr->cbndx, s2cr->count);
	}
	mutex_unlock(&smmu->stream_map_mutex);
	pr_info("oneplus13-smmu: display SID 0800 matches=%u (cached software state only)\n", matches);
	/* Match arm_smmu_rpm_get: without a PM domain the owner keeps clocks on. */
	ret = pm_runtime_enabled(&pdev->dev) ? pm_runtime_resume_and_get(&pdev->dev) : 0;
	if (ret < 0)
		goto out;
	pr_info("oneplus13-smmu: SCR0=%08x\n",
		arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sCR0));
	/* Never probe the extra groups above the firmware-safe architected 128. */
	for (i = 0; i < min(smmu->num_mapping_groups, 128U); i++) {
		u32 smr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(i));
		if (!(smr & ARM_SMMU_SMR_VALID))
			continue;
		if (((smr & 0xffff) ^ 0x800) & ~((smr >> 16) & 0x7fff))
			continue;
		{
			u32 s2cr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_S2CR(i));
			u32 cb = s2cr & 0xff;
			pr_info("oneplus13-smmu: display hardware index=%u SMR=%08x S2CR=%08x\n",
				i, smr, s2cr);
			if (cb < smmu->num_context_banks)
				pr_info("oneplus13-smmu: CB%u SCTLR=%08x CBAR=%08x\n", cb,
					arm_smmu_cb_read(smmu, cb, ARM_SMMU_CB_SCTLR),
					arm_smmu_gr1_read(smmu, ARM_SMMU_GR1_CBAR(cb)));
		}
	}
	if (pm_runtime_enabled(&pdev->dev))
		pm_runtime_put(&pdev->dev);
	ret = 0;
out:
	device_unlock(&pdev->dev);
	put_device(&pdev->dev);
	return ret;
}
static void __exit stream_state_exit(void) {}
module_init(stream_state_init);
module_exit(stream_state_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OnePlus 13 cached display SMMU stream diagnostic");
