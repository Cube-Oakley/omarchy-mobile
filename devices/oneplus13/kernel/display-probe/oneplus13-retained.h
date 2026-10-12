// SPDX-License-Identifier: GPL-2.0-only
#ifndef ONEPLUS13_RETAINED_H
#define ONEPLUS13_RETAINED_H
#include "arm-smmu.h"

static int verify_bypass(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct arm_smmu_device *smmu;
	struct resource *res;
	unsigned int i, matches = 0;
	bool okay = true;
	int ret = -ENODEV;

	np = of_find_node_by_path("/soc@0/iommu@15000000");
	if (!np)
		return ret;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return ret;
	device_lock(&pdev->dev);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	smmu = platform_get_drvdata(pdev);
	/* This exact owner keeps its clocks on without runtime PM. */
	if (!pdev->dev.driver || !res || res->start != 0x15000000 ||
	    !smmu || !smmu->smrs || !smmu->s2crs || pm_runtime_enabled(&pdev->dev))
		goto out;
	mutex_lock(&smmu->stream_map_mutex);
	for (i = 0; i < min(smmu->num_mapping_groups, 128U); i++) {
		struct arm_smmu_smr *smr = &smmu->smrs[i];
		u32 actual, s2cr, cb;
		if (!smr->valid || ((0x800 ^ smr->id) & ~smr->mask))
			continue;
		matches++;
		actual = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(i));
		s2cr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_S2CR(i));
		cb = s2cr & 0xff;
		okay &= smr->id == 0x800 && smr->mask == 2 &&
			smmu->s2crs[i].type == S2CR_TYPE_BYPASS &&
			smmu->s2crs[i].count == 0 && actual == 0x80020800 &&
			s2cr == 0x52 && cb < smmu->num_context_banks;
		if (okay)
			okay &= arm_smmu_cb_read(smmu, cb, ARM_SMMU_CB_SCTLR) == 0 &&
				arm_smmu_gr1_read(smmu, ARM_SMMU_GR1_CBAR(cb)) == 0x10000;
	}
	mutex_unlock(&smmu->stream_map_mutex);
	ret = matches == 1 && okay ? 0 : -EINVAL;
out:
	device_unlock(&pdev->dev);
	put_device(&pdev->dev);
	return ret;
}

#endif
