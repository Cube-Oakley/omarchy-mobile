// SPDX-License-Identifier: GPL-2.0-only
/* One-shot, read-only: sample the PMU DPU/DISP STATUS, OUT and IN registers
 * (the APM's power-down handshake: OUT bits 0/2/3 request, IN bit 5 ack,
 * IN bit 6 busy) every few microseconds for `ms` milliseconds and log each
 * change with its time. Never stays loaded. */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>

static unsigned int ms = 1000;
module_param(ms, uint, 0400);

#define MAXLOG 96
static struct { s64 us; u32 v[6]; } logbuf[MAXLOG];

static int __init dpu_in_init(void)
{
	void __iomem *pmu = ioremap(0x18062200, 0x100);
	static const u16 off[6] = { 0x04, 0x20, 0x24, 0x84, 0xa0, 0xa4 };
	unsigned int n = 0, nlog = 0, lost = 0, i, k;
	u32 prev[6] = {}, cur[6], bit6 = 0;
	ktime_t start, end;

	if (!pmu)
		return -ENOMEM;
	start = ktime_get();
	end = ktime_add_ms(start, ms);
	while (ktime_before(ktime_get(), end)) {
		bool changed = !n;

		for (k = 0; k < 6; k++) {
			cur[k] = readl(pmu + off[k]);
			changed |= cur[k] != prev[k];
			prev[k] = cur[k];
		}
		if (cur[2] & BIT(6))
			bit6++;
		if (changed) {
			if (nlog < MAXLOG) {
				logbuf[nlog].us = ktime_us_delta(ktime_get(), start);
				memcpy(logbuf[nlog].v, cur, sizeof(cur));
				nlog++;
			} else {
				lost++;
			}
		}
		n++;
		udelay(2);
	}
	iounmap(pmu);
	pr_info("dpu-in: %u samples, DPU IN bit6 set in %u, %u changes logged, %u lost\n",
		n, bit6, nlog, lost);
	for (i = 0; i < nlog; i++)
		pr_info("dpu-in: +%lld us DPU st %x out %x in %x | DISP st %x out %x in %x\n",
			logbuf[i].us, logbuf[i].v[0], logbuf[i].v[1], logbuf[i].v[2],
			logbuf[i].v[3], logbuf[i].v[4], logbuf[i].v[5]);
	return -EAGAIN;
}
module_init(dpu_in_init);
MODULE_DESCRIPTION("Read-only sampler for the DPU and DISP power handshake bits");
MODULE_LICENSE("GPL");
