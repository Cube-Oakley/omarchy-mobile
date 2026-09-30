// SPDX-License-Identifier: GPL-2.0-only
/* GS201 MCT global comparator as the tick broadcast device.
 *
 * A CPU in C2 loses its arch timer comparator, so idle CPUs need a broadcast
 * device to wake them. Without one the kernel falls back to its hrtimer
 * broadcast, which runs on one CPU that then must stay awake: that CPU's C2
 * entries are rejected, and its cluster can never power down. Stock used the
 * Exynos MCT's global comparator. Mainline exynos_mct can't use the stock
 * node (its clocks come from a vendor provider), so the kernel skips it.
 *
 * The bootloader starts the MCT's free-running counter and leaves its clock
 * on: CMU_MISC's MCT PCLK gate (0x10010000 + 0x209c) under hardware control,
 * with the MCT Q-channel (0x3088) disabled. The counter runs at 24.576 MHz,
 * the arch timer's count. This module leaves the counter alone and drives
 * comparator 0 as mainline exynos_mct does (exynos4_mct_comp0_start), with
 * interrupt G0 through the stock node's interrupt map (GIC SPI 785).
 *
 * The comparator fires on an exact match, so a target the counter has already
 * passed never fires. set_next_event reads the counter back and returns
 * -ETIME in that case, and the clockevents core retries with a longer delta.
 *
 * Before registering, one event 100 us ahead must interrupt exactly once.
 * Registering replaces the hrtimer broadcast device (rating 0). The tick core
 * then holds a reference to this module, so it has no exit.
 *
 * MISC, the MCT's block, loses power in SYS_SLEEP, and the counter comes back
 * stopped: the comparator would never match, and a CPU in C2 would never wake.
 * The clockevent resume callback restarts it as mainline exynos4_frc_resume()
 * does (G_TCON START). clockevents_resume() is the first step of
 * timekeeping_resume(), before the clocksources are read and before
 * tick_resume() puts this device back to work; stock's MCT clocksource resume
 * ran at the same point, before exynos-pm restored CMU_MISC. A restart is
 * followed by a polled one-shot self-test. The same callback runs on every
 * s2idle wake, where the counter is still running and it only reads G_TCON.
 *
 * The polls count MCT reads, not time: udelay() runs on the arch counter,
 * and on Exynos the MCT counter feeds the arch timer (mainline 6282edb72bed;
 * here both read the same count).
 */
#include <linux/clockchips.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/suspend.h>

#define GS201_MCT		0x10050000
#define MCT_G_CNT_L		0x100
#define MCT_G_CNT_U		0x104
#define MCT_G_COMP0_L		0x200
#define MCT_G_COMP0_U		0x204
#define MCT_G_TCON		0x240
#define MCT_G_INT_CSTAT		0x244
#define MCT_G_INT_ENB		0x248
#define MCT_G_WSTAT		0x24c
#define G_TCON_COMP0_ENABLE	BIT(0)
#define G_TCON_COMP0_AUTO_INC	BIT(1)
#define G_TCON_START		BIT(8)
#define G_WSTAT_COMP0_L		BIT(0)
#define G_WSTAT_COMP0_U		BIT(1)
#define G_WSTAT_TCON		BIT(16)
#define MCT_RATE		24576000

#define GS201_CMU_MISC		0x10010000
#define MCT_PCLK_GATE		0x209c
#define MCT_QCH_CON		0x3088
#define GATE_MANUAL		BIT(20)
#define GATE_CG_VAL		BIT(21)
#define QCH_ENABLE		BIT(0)

#define MCT_POLL_READS		100000

static void __iomem *mct;
static bool registered;
static unsigned int selftest_hits;
static unsigned int frc_restarts, resume_checks, resume_errors;
module_param(frc_restarts, uint, 0444);
MODULE_PARM_DESC(frc_restarts, "Resumes that found the counter stopped (MISC power lost) and restarted it");
module_param(resume_checks, uint, 0444);
MODULE_PARM_DESC(resume_checks, "Comparator self-tests passed on resume");
module_param(resume_errors, uint, 0444);
MODULE_PARM_DESC(resume_errors, "Comparator self-tests failed on resume");
static bool resume_selftest;
module_param(resume_selftest, bool, 0644);
MODULE_PARM_DESC(resume_selftest, "Diagnostic: self-test on every deep (mem) resume, also when the counter kept running");

/* exynos4_mct_write(): the write lands in the MCT's clock domain, and the
 * status bit, cleared by writing it back, says when.
 */
static void mct_write(u32 val, u32 off, u32 wstat)
{
	int i;

	writel_relaxed(val, mct + off);
	for (i = 0; i < 10000; i++) {
		if (readl_relaxed(mct + MCT_G_WSTAT) & wstat) {
			writel_relaxed(wstat, mct + MCT_G_WSTAT);
			return;
		}
	}
	pr_warn_ratelimited("pixel-mct: write %#x to %#x not applied\n", val, off);
}

static u64 mct_count(void)
{
	u32 hi, lo, hi2 = readl_relaxed(mct + MCT_G_CNT_U);

	do {
		hi = hi2;
		lo = readl_relaxed(mct + MCT_G_CNT_L);
		hi2 = readl_relaxed(mct + MCT_G_CNT_U);
	} while (hi != hi2);
	return (u64)hi << 32 | lo;
}

static int mct_shutdown(struct clock_event_device *evt)
{
	u32 tcon = readl_relaxed(mct + MCT_G_TCON);

	mct_write(tcon & ~(G_TCON_COMP0_ENABLE | G_TCON_COMP0_AUTO_INC), MCT_G_TCON,
		  G_WSTAT_TCON);
	writel_relaxed(0, mct + MCT_G_INT_ENB);
	return 0;
}

static int mct_set_next_event(unsigned long cycles, struct clock_event_device *evt)
{
	u64 target = mct_count() + cycles;
	u32 tcon = readl_relaxed(mct + MCT_G_TCON);

	mct_write(lower_32_bits(target), MCT_G_COMP0_L, G_WSTAT_COMP0_L);
	mct_write(upper_32_bits(target), MCT_G_COMP0_U, G_WSTAT_COMP0_U);
	writel_relaxed(1, mct + MCT_G_INT_ENB);
	if (!(tcon & G_TCON_COMP0_ENABLE))
		mct_write(tcon | G_TCON_COMP0_ENABLE, MCT_G_TCON, G_WSTAT_TCON);
	if ((s64)(mct_count() - target) >= 0)
		return -ETIME;
	return 0;
}

/* One event must match exactly once. Only from syscore resume: interrupts are
 * off on the only online CPU, and the level interrupt is cleared before they
 * return. Bounded by MCT_POLL_READS, so a stopped counter fails it.
 */
static bool mct_poll_selftest(struct clock_event_device *evt)
{
	unsigned long delta = MCT_RATE / 10000;
	unsigned int reads = 0, hits = 0;
	bool pending, was_pending = false;
	u64 until;

	writel_relaxed(1, mct + MCT_G_INT_CSTAT);
	while (mct_set_next_event(delta, evt)) {
		if (delta >= MCT_RATE / 100)
			goto out;
		delta *= 2;
	}
	until = mct_count() + 3 * delta;
	while ((s64)(mct_count() - until) < 0 && ++reads < MCT_POLL_READS) {
		pending = readl_relaxed(mct + MCT_G_INT_CSTAT) & 1;
		if (pending) {
			hits += !was_pending;
			writel_relaxed(1, mct + MCT_G_INT_CSTAT);
		}
		was_pending = pending;
	}
out:
	mct_shutdown(evt);
	writel_relaxed(1, mct + MCT_G_INT_CSTAT);
	return hits == 1 && reads < MCT_POLL_READS;
}

/* clockevents_resume(): exynos4_frc_resume(), when power was lost. */
static void mct_resume(struct clock_event_device *evt)
{
	u32 tcon = readl_relaxed(mct + MCT_G_TCON);
	bool restart = !(tcon & G_TCON_START);

	if (!restart && !(READ_ONCE(resume_selftest) &&
			  pm_suspend_target_state == PM_SUSPEND_MEM))
		return;
	if (restart) {
		frc_restarts++;
		/* As at load: stale write-status bits would end mct_write()'s wait. */
		writel_relaxed(readl_relaxed(mct + MCT_G_WSTAT), mct + MCT_G_WSTAT);
		writel_relaxed(0, mct + MCT_G_INT_ENB);
		writel_relaxed(1, mct + MCT_G_INT_CSTAT);
		mct_write(tcon | G_TCON_START, MCT_G_TCON, G_WSTAT_TCON);
	}
	if (mct_poll_selftest(evt)) {
		resume_checks++;
	} else {
		resume_errors++;
		pr_err("pixel-mct: comparator self-test failed on resume (G_TCON %#x)\n",
		       readl_relaxed(mct + MCT_G_TCON));
	}
}

static struct clock_event_device mct_comp = {
	.name			= "mct-comp",
	.features		= CLOCK_EVT_FEAT_ONESHOT,
	.rating			= 250,
	.set_next_event		= mct_set_next_event,
	.set_state_shutdown	= mct_shutdown,
	.set_state_oneshot	= mct_shutdown,
	.tick_resume		= mct_shutdown,
	.resume			= mct_resume,
	.owner			= THIS_MODULE,
};

static irqreturn_t mct_comp_isr(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	writel_relaxed(1, mct + MCT_G_INT_CSTAT);
	if (likely(registered))
		evt->event_handler(evt);
	else
		WRITE_ONCE(selftest_hits, selftest_hits + 1);
	return IRQ_HANDLED;
}

static bool mct_clock_on(void)
{
	void __iomem *cmu = ioremap(GS201_CMU_MISC, 0x4000);
	u32 gate, qch;

	if (!cmu)
		return false;
	gate = readl(cmu + MCT_PCLK_GATE);
	qch = readl(cmu + MCT_QCH_CON);
	iounmap(cmu);
	return (!(gate & GATE_MANUAL) || (gate & GATE_CG_VAL)) && !(qch & QCH_ENABLE);
}

static int __init pixel_mct_init(void)
{
	struct device_node *np;
	u64 first;
	u32 tcon;
	int irq, ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	if (!mct_clock_on()) {
		pr_err("pixel-mct: MCT clock is not as the bootloader leaves it\n");
		return -ENODEV;
	}
	np = of_find_compatible_node(NULL, NULL, "samsung,exynos4210-mct");
	if (!np)
		return -ENODEV;
	irq = irq_of_parse_and_map(np, 0);
	of_node_put(np);
	if (irq <= 0)
		return -ENXIO;

	mct = ioremap(GS201_MCT, 0x800);
	if (!mct) {
		ret = -ENOMEM;
		goto dispose;
	}
	tcon = readl(mct + MCT_G_TCON);
	first = mct_count();
	udelay(10);
	if (!(tcon & G_TCON_START) || mct_count() == first) {
		pr_err("pixel-mct: MCT counter is stopped\n");
		ret = -ENODEV;
		goto unmap;
	}
	if (tcon & (G_TCON_COMP0_ENABLE | G_TCON_COMP0_AUTO_INC)) {
		pr_err("pixel-mct: comparator 0 is in use (G_TCON %#x)\n", tcon);
		ret = -EBUSY;
		goto unmap;
	}
	/* The bootloader leaves its own write-status bits set. */
	writel(readl(mct + MCT_G_WSTAT), mct + MCT_G_WSTAT);
	writel(0, mct + MCT_G_INT_ENB);
	writel(1, mct + MCT_G_INT_CSTAT);

	mct_comp.irq = irq;
	mct_comp.cpumask = cpu_possible_mask;
	ret = request_irq(irq, mct_comp_isr, IRQF_TIMER | IRQF_IRQPOLL, "mct-comp", &mct_comp);
	if (ret)
		goto unmap;

	/* One event 100 us ahead must interrupt before the tick core relies on it. */
	mct_set_next_event(MCT_RATE / 10000, &mct_comp);
	mdelay(2);
	mct_shutdown(&mct_comp);
	if (READ_ONCE(selftest_hits) != 1) {
		pr_err("pixel-mct: self-test event interrupted %u times\n", selftest_hits);
		free_irq(irq, &mct_comp);
		ret = -EIO;
		goto unmap;
	}
	registered = true;
	clockevents_config_and_register(&mct_comp, MCT_RATE, 0x80, 0xffffffff);
	pr_info("pixel-mct: comparator 0 on hwirq %lu is the tick broadcast device\n",
		irqd_to_hwirq(irq_get_irq_data(irq)));
	return 0;
unmap:
	iounmap(mct);
dispose:
	irq_dispose_mapping(irq);
	return ret;
}
module_init(pixel_mct_init);

MODULE_DESCRIPTION("GS201 MCT tick broadcast device");
MODULE_LICENSE("GPL");
