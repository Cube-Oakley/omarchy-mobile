/* SPDX-License-Identifier: GPL-2.0-only */
/* Read-side helpers shared by pixel-sleep-audit and pixel-sleep: the stock
 * GS201 SYS_SLEEP tables (pixel-sleep-lists.h, generated), their register
 * blocks and power-domain gates, and the patch-0003 FLEXPMU counters.
 * Nothing in this file writes a register: the write paths live in
 * pixel-sleep.c only, so the audit module cannot contain one.
 *
 * Access rules (a stray access hangs GS201 without a watchdog bite):
 * - only registers named in the stock tables are touched, at their offsets;
 * - a block with a PMU power domain is read only while its STATUS bit 0 is
 *   set (stock COND test, pmucal_rae.c:67-77); CMU_CPUCL1/2 only while that
 *   cluster's NONCPU STATUS is on; CMU/SYSREG_CPUCL0 only from CPUs 0-3;
 * - PMU_ALIVE and PMU_INTR_GEN are always readable (pixel-cpupm reads them
 *   on every idle entry).
 */
#ifndef PIXEL_SLEEP_COMMON_H
#define PIXEL_SLEEP_COMMON_H

#include <linux/device.h>
#include <linux/io.h>
#include <linux/kernfs.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/smp.h>

/* enum pmucal_seq_acctype, pmucal_common.h:19-38: identical values. */
enum ps_type {
	PS_READ = 0,
	PS_WRITE,
	PS_COND_READ,
	PS_COND_WRITE,
	PS_SAVE_RESTORE,
	PS_COND_SAVE_RESTORE,
	PS_WAIT,
	PS_WAIT_TWO,
	PS_CHECK_SKIP,
	PS_COND_CHECK_SKIP,
	PS_WRITE_WAIT,
	PS_WRITE_RETRY,
	PS_WRITE_RETRY_INV,
	PS_WRITE_RETURN,
	PS_SET_BIT_ATOMIC,
	PS_CLR_BIT_ATOMIC,
	PS_DELAY,
	PS_CLEAR_PEND,
};

/* How stock writes a block (pmucal_rae.c:140-148). */
enum ps_class {
	PS_CLASS_PMU,		/* PMU_ALIVE 0x1806xxxx: secure SMC 0x82000504 */
	PS_CLASS_INTR_GEN,	/* PMU_INTR_GEN 0x18070000: writel */
	PS_CLASS_OTHER,		/* CMU, sysreg, DMC, UFS, TREX: writel */
};

struct ps_block {
	u32 pa;
	u32 size;
	u16 pd_status;		/* gating PMU STATUS offset; 0 = ALIVE/TOP */
	u8 class;
	s8 cluster;		/* 0-2 for CPUCL blocks, -1 otherwise */
	const char *name;
	const char *note;
};

struct ps_pd {
	u16 offset;
	u16 line;
	const char *name;
	const char *file;
};

#define PS_NO_BLOCK 0xff

#define PS_PD_WAITS	BIT(0)	/* stock list waits for a PLL: compare only */
#define PS_PD_NOC	BIT(1)	/* an interconnect domain */

/* A power domain's save list: ps_pd_save[first .. first + n). */
struct ps_pd_list {
	const char *name;
	u16 status;
	u16 first, n;
	u8 flags;
	u16 line;
};

struct ps_seq {
	const char *name;
	u32 mask, value;
	u32 cond_mask, cond_value;
	u16 offset, cond_offset;
	u16 line;
	u8 type, blk, cblk;
};

#define PS_SEQ(t, n, b, o, m, v, cb, co, cm, cv, l) {			\
	.name = n, .offset = o, .mask = m, .value = v,			\
	.cond_offset = co, .cond_mask = cm, .cond_value = cv,		\
	.line = l, .type = t, .blk = b, .cblk = cb }

#include "pixel-sleep-lists.h"

#define PS_PMU		PS_BLK_PMU_ALIVE
#define PS_INTR		PS_BLK_PMU_INTR_GEN
#define PS_PMU_PA	0x18060000

static void __iomem *ps_va[PS_NR_BLOCKS];

static void ps_unmap_blocks(void)
{
	unsigned int i;

	for (i = 0; i < PS_NR_BLOCKS; i++) {
		if (ps_va[i])
			iounmap(ps_va[i]);
		ps_va[i] = NULL;
	}
}

/* ioremap only creates page tables; it never touches the device. */
static int ps_map_blocks(void)
{
	unsigned int i;

	for (i = 0; i < PS_NR_BLOCKS; i++) {
		ps_va[i] = ioremap(ps_blocks[i].pa, ps_blocks[i].size);
		if (!ps_va[i]) {
			ps_unmap_blocks();
			return -ENOMEM;
		}
	}
	return 0;
}

static inline u32 ps_pmu_read(u32 offset)
{
	return readl(ps_va[PS_PMU] + offset);
}

static inline u32 ps_intr_read(u32 offset)
{
	return readl(ps_va[PS_INTR] + offset);
}

/* pixel-cpupm.c cpu_cluster[]: CPUs 0-3 little, 4-5 mid, 6-7 big. */
static inline int ps_cpu_cluster(unsigned int cpu)
{
	return cpu < 4 ? 0 : cpu < 6 ? 1 : 2;
}

static const u16 ps_noncpu_status[3] = { 0x1204, 0x1404, 0x1604 };

enum ps_skip {
	PS_OK,
	PS_SKIP_COND,		/* the stock condition is false */
	PS_SKIP_PD,		/* the block's power domain is off */
	PS_SKIP_CLUSTER,	/* CPU cluster off, or not on a CPU of it */
	PS_SKIP_EXCLUDED,	/* excluded by a module parameter */
};

static const char * const ps_skip_name[] __maybe_unused = {
	"ok", "cond", "pd-off", "cluster", "excluded",
};

#define PS_GATE_CPUCL_STATUS	BIT(0)	/* CPUCL1/2 only while NONCPU is on */
#define PS_GATE_CPUCL_LOCAL	BIT(1)	/* ... and only from a CPU in it */
#define PS_GATE_NO_G3D		BIT(2)	/* never touch G3D-gated blocks */

static unsigned int ps_gate_flags = PS_GATE_CPUCL_STATUS;

/* Whether block @blk may be accessed now. Callers that need the result to
 * hold across the access run with interrupts off on a fixed CPU; in the
 * syscore phase no other CPU or device can change a power domain.
 */
static int ps_block_gate(u8 blk)
{
	const struct ps_block *b = &ps_blocks[blk];
	unsigned int cpu = raw_smp_processor_id();

	if (b->cluster == 0)
		return ps_cpu_cluster(cpu) == 0 ? PS_OK : PS_SKIP_CLUSTER;
	if (b->cluster > 0) {
		if ((ps_gate_flags & PS_GATE_CPUCL_LOCAL) &&
		    ps_cpu_cluster(cpu) != b->cluster)
			return PS_SKIP_CLUSTER;
		if ((ps_gate_flags & PS_GATE_CPUCL_STATUS) &&
		    !(ps_pmu_read(ps_noncpu_status[b->cluster]) & BIT(0)))
			return PS_SKIP_CLUSTER;
		return PS_OK;
	}
	if (b->pd_status == 0x1e04 && (ps_gate_flags & PS_GATE_NO_G3D))
		return PS_SKIP_EXCLUDED;
	if (b->pd_status && !(ps_pmu_read(b->pd_status) & BIT(0)))
		return PS_SKIP_PD;
	return PS_OK;
}

/* pmucal_rae_check_condition(), pmucal_rae.c:67-77. Condition registers
 * are PMU_ALIVE or PMU_INTR_GEN only (checked by the generator).
 */
static bool ps_cond(const struct ps_seq *e)
{
	return (readl(ps_va[e->cblk] + e->cond_offset) & e->cond_mask) == e->cond_value;
}

static inline void __iomem *ps_reg(const struct ps_seq *e)
{
	return ps_va[e->blk] + e->offset;
}

static inline u32 ps_seq_pa(const struct ps_seq *e)
{
	return ps_blocks[e->blk].pa + e->offset;
}

/* One save-list read with stock pmucal_rae_save_seq() semantics
 * (pmucal_rae.c:225-258): COND_* entries read only when their condition
 * holds; SAVE_RESTORE and READ ignore any condition fields. The block gate
 * is applied on top. Returns PS_OK and the masked value, or a skip reason.
 */
static int ps_save_read(const struct ps_seq *e, u32 *val)
{
	int gate;

	if ((e->type == PS_COND_SAVE_RESTORE || e->type == PS_COND_READ) && !ps_cond(e))
		return PS_SKIP_COND;
	gate = ps_block_gate(e->blk);
	if (gate != PS_OK)
		return gate;
	*val = readl(ps_reg(e)) & e->mask;
	return PS_OK;
}

/* Patch 0003 exposes the ACPM FLEXPMU_DBG words only as the sysfs attribute
 * flexpmu_stats of the gs201-acpm-ipc device. Its show() does readl() and
 * sysfs_emit() only, so it is called directly (as dev_attr_show() does,
 * drivers/base/core.c) from atomic context into a private page. The ACPM
 * driver is built in and never unbinds.
 */
struct ps_counts {
	u32 soc_early, soc_down, mif_down;
	u32 sleep_early, sleep_soc_down, sleep_mif_down;
	u32 sicd_early, sicd_soc_down, sicd_mif_down;
	u32 mif_always_on, sw_flag0, sw_flag1;
	bool valid;
};

struct ps_flexpmu {
	struct device *dev;
	struct kernfs_node *kn;
	struct device_attribute *attr;
	char *page;
};

static void ps_flexpmu_close(struct ps_flexpmu *f)
{
	if (f->kn)
		kernfs_put(f->kn);
	if (f->dev)
		put_device(f->dev);
	if (f->page)
		free_page((unsigned long)f->page);
	memset(f, 0, sizeof(*f));
}

static int ps_flexpmu_open(struct ps_flexpmu *f)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct attribute *attr;

	memset(f, 0, sizeof(*f));
	np = of_find_compatible_node(NULL, NULL, "google,gs201-acpm-ipc");
	if (!np)
		return -ENODEV;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return -ENODEV;
	f->dev = &pdev->dev;
	f->kn = kernfs_find_and_get(f->dev->kobj.sd, "flexpmu_stats");
	if (!f->kn || kernfs_type(f->kn) != KERNFS_FILE || !f->kn->priv) {
		ps_flexpmu_close(f);
		return -ENOENT;	/* kernel without patch 0003 */
	}
	attr = f->kn->priv;
	f->attr = container_of(attr, struct device_attribute, attr);
	if (strcmp(attr->name, "flexpmu_stats") || !f->attr->show) {
		ps_flexpmu_close(f);
		return -ENOENT;
	}
	f->page = (char *)__get_free_page(GFP_KERNEL);
	if (!f->page) {
		ps_flexpmu_close(f);
		return -ENOMEM;
	}
	return 0;
}

/* Non-sleeping: safe with interrupts off. */
static int ps_flexpmu_read(struct ps_flexpmu *f, struct ps_counts *c)
{
	ssize_t n;

	memset(c, 0, sizeof(*c));
	if (!f->page)
		return -ENODEV;
	n = f->attr->show(f->dev, f->attr, f->page);
	if (n <= 0 || n >= PAGE_SIZE)
		return n < 0 ? n : -EIO;
	f->page[n] = 0;
	if (sscanf(f->page,
		   "soc_early %u\nsoc_down %u\nmif_down %u\n"
		   "sleep_early %u\nsleep_soc_down %u\nsleep_mif_down %u\n"
		   "sicd_early %u\nsicd_soc_down %u\nsicd_mif_down %u\n"
		   "mif_always_on %u\nsw_flags %x %x",
		   &c->soc_early, &c->soc_down, &c->mif_down,
		   &c->sleep_early, &c->sleep_soc_down, &c->sleep_mif_down,
		   &c->sicd_early, &c->sicd_soc_down, &c->sicd_mif_down,
		   &c->mif_always_on, &c->sw_flag0, &c->sw_flag1) != 12)
		return -EINVAL;
	c->valid = true;
	return 0;
}

#endif
