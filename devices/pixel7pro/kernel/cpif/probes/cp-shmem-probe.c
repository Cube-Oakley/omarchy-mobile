// SPDX-License-Identifier: GPL-2.0-only
/* Manual mapping/lifetime check with CP and cpif off. Does not access memory
 * contents. Exercises both cached and vmap paths and rejects non-DRAM ranges.
 */
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/vmalloc.h>

extern unsigned long cp_shmem_get_base(u32 cp, u32 idx);
extern u32 cp_shmem_get_size(u32 cp, u32 idx);
extern void __iomem *cp_shmem_get_region(u32 cp, u32 idx);
extern void cp_shmem_release_region(u32 cp, u32 idx);
extern void __iomem *cp_shmem_get_nc_region(unsigned long base, u32 size);

static bool run;
module_param(run, bool, 0400);

static int __init cp_shmem_probe_init(void)
{
	/* SHMEM_IPC, PKTPROC, PKTPROC_UL, MSI in Google's DT bindings. */
	static const u32 indices[] = { 3, 7, 8, 11 };
	void __iomem *addr;
	unsigned long physical, expected;
	unsigned int pass, i;
	int ret = 0;

	if (!run)
		return -EINVAL;
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < ARRAY_SIZE(indices); i++) {
			expected = cp_shmem_get_base(0, indices[i]);
			addr = cp_shmem_get_region(0, indices[i]);
			if (!addr) {
				ret = -ENOMEM;
				goto out;
			}
			physical = is_vmalloc_addr(addr) ?
				PFN_PHYS(vmalloc_to_pfn((void *)addr)) : virt_to_phys(addr);
			if (!expected || physical != expected ||
			    !cp_shmem_get_size(0, indices[i]))
				ret = -EINVAL;
			cp_shmem_release_region(0, indices[i]);
			/* Cached linear aliases must not be vunmapped or reused stale. */
			cp_shmem_release_region(0, indices[i]);
			if (ret)
				goto out;
		}
	}
	/* No conversion of AoC SRAM, invalid indices or ordinary RAM to pages. */
	if (cp_shmem_get_region(0, 1) || cp_shmem_get_region(~0U, 3) ||
	    cp_shmem_get_region(0, ~0U) || cp_shmem_get_base(0, ~0U) ||
	    cp_shmem_get_size(~0U, 3))
		ret = -EINVAL;
	addr = cp_shmem_get_nc_region(0x80000000, PAGE_SIZE);
	if (addr) {
		vunmap(addr);
		ret = -EINVAL;
	}
	addr = cp_shmem_get_nc_region(0x197fd000, PAGE_SIZE);
	if (addr) {
		vunmap(addr);
		ret = -EINVAL;
	}
out:
	for (i = 0; i < ARRAY_SIZE(indices); i++)
		cp_shmem_release_region(0, indices[i]);
	pr_info("cp-shmem-probe: mapping/release cycles and invalid-range checks result=%d\n", ret);
	return ret ?: -EAGAIN;
}
module_init(cp_shmem_probe_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Manual CP reserved-memory mapping and lifetime test");
