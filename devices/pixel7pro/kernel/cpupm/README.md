# CPU power-down idle (C2 and cluster power-down)

Two modules make the stock DT's C2 idle states (PSCI CPU_SUSPEND power down,
one per cluster) work:
- `pixel-mct.c` makes the MCT the tick broadcast device;
- `pixel-cpupm.c` gives the firmware the PMU hints it needs for C2 and for
  cluster power-down.

The boot script loads both right after ACPM activation, the MCT first.

## SICD prerequisite audit

`pixel-sicd-audit.ko` is an optional read-only diagnostic, not loaded at boot.
It never changes CPU hints or power registers. In the system-suspend noirq
window it samples the stock GS201 status registers when the last little CPU
enters CPU_PM. The `counts` module parameter contains:

1. Last-little-CPU attempts with all eight CPUs marked idle.
2. Attempts where another CPU had not yet powered down.
3. Attempts with a larger cluster still on.
4. Attempts without the UFS internal Hibern8 clock-stop bits.
5. Attempts with the debug idle-IP busy.
6. Attempts with AUR powered, requiring additional clock save/restore.
7. Attempts with the modem PCIe host powered.
8. Attempts with Wi-Fi PCIe still running.
9. ACPM snapshot helper unavailable.
10. ACPM queues busy.
11. Candidates passing these partial checks.

Wi-Fi ELBI is read only while its PMU isolation is released: even a read of
isolated ELBI hangs the SoC. The stock device tree makes both PCIe hosts
SICD blockers while powered. The native modem host powers down during
suspend, but the Wi-Fi host currently retains its link in L1. A 17.833-second
RTC test recorded nine Wi-Fi blockers. Temporarily unloading Wi-Fi and its
host removed that blocker. With the ACPM helper below, a further 10.273-second
sleep recorded four partial candidates and no busy firmware queues.

The optional firmware helper comes from
[0002-acpm-idle-snapshot.patch](../suspend/0002-acpm-idle-snapshot.patch).
It uses the driver's validated shared queues without waiting or writing.
Older kernels can still run the audit but report ACPM unavailable. Candidate
counts are not residency and do not validate the full device idle-IP list.

## Guarded SICD experiment

`pixel-sicd.h` adds a deliberately disabled experimental path to `pixel-cpupm`.
It requires load-time `permit_sicd=1` and a nonzero `sicd_budget`; each attempt
consumes one budget unit before programming. Normal boots permit neither.
The extra mappings and PM device are created only when permitted.

Entry requires the system s2idle noirq window, all eight CPUs online and
in the frozen-tick idle callback, every other core actually powered down, both larger clusters
off, all unused domains off (including AUR), UFS Hibern8 clock-stop bits,
idle debug hardware, quiescent PCIe hosts, drained ACPM queues and all next
timer deadlines at least 10 ms away. Existing nonzero system wake masks
prevent entry. Only a little CPU can request the mode.

The experiment follows Google's `exynos-cpupm` and GS201 flexpmu sequences:
CPU_INFORM hint 3 through the secure register API; stock CPU/GIC wake mask;
EINT wake for the power key and modem; first-CPU wake cleanup; and the stock
cancelled-entry cleanup when the reserved NSCODE page reports CANCEL_FLAG.
It restores the previous EINT masks. It never accesses APM SRAM through
`/dev/mem`. Firmware return errors exhaust the remaining budget.

The `sicd_*` parameters record hints, exits, cancellations, errors, timer and
firmware blockers. `sicd_blocked` records the first hardware blocker in this
order: software CPU, powered CPU, larger cluster, unused domain, debug, UFS,
modem PCIe, Wi-Fi PCIe. `sicd_last_pd` identifies a blocking domain status
offset. `sicd_idle_returns` counts returning PSCI calls; `sicd_elapsed_ns`
and `sicd_longest_ns` measure uncancelled hint-to-first-wake intervals.
The intervals use the architectural counter and CNTFRQ, since Linux freezes
monotonic time during s2idle. Neither these intervals nor successful PSCI
returns prove firmware residency without power measurements.

`permit_sicd=1 sicd_defer=1` additionally permits CPU 0 to refuse up to 64
frozen-idle entries while sibling CPUs finish entering sleep. Each refusal
happens before any C2 hint or interrupt bookkeeping and uses the normal
CPU_PM failure unwind. A 50 us delay after the PSCI callback returns, outside
both notifier locks, lets siblings progress. Total deliberate delay is
bounded to 3.2 ms per system sleep; zero budget skips it. This diagnostic
coordination is disabled by default. `sicd_first_last_cpu` records the first
CPU to find all cores in frozen idle; `sicd_deferrals` counts these refusals.

Without this coordination, CPU 7 entered last in the measured trial. No
little CPU was left to request SICD until wakeup. With paced CPU 0 retries,
a 9.148-second RTC sleep recorded one uncancelled 9.147-second hint-to-wake
interval, CPU 0 entering last, 16 refusals, no errors, an intact test file
and restored Wi-Fi. Two further hint intervals of 9.308 and 9.325 seconds
also resumed cleanly. However, their metered totals were 861.4 and 866.2 mW,
versus controls of 863.1 and 863.2 mW; CPU, interconnect and memory rails were
unchanged. These partial-rail measurements establish no power saving. The
counters alone cannot distinguish a firmware fallback. A subsequent
read-only firmware-counter image confirmed one SICD SoC-down sequence during
an 8.369-second sleep, with zero early wakeups. The MIF-down count remained
zero and `mif_always_on` was 1. Thus SoC entry is confirmed, while memory
sleep and a meaningful battery-life saving remain unresolved. This stays an
opt-in RAM-boot experiment.

The initial trials kept the existing watchdog running and used a 10-second
RTC alarm. Two trials were refused by CPU timing guards. A subsequent trial
issued one hint, returned once with no cancellation or errors, woke by RTC
after 8.923 seconds, preserved a synced 16 MiB file and restored Wi-Fi.
Those initial transition hints and monotonic-clock timings did not establish
sustained entry. The later frozen-callback and architectural-counter fixes
are required to interpret the experiment. No battery-life claim follows from
these counters alone.

Wi-Fi was normally unloaded for these trials and restored afterwards. That
is a diagnostic prerequisite, not a shipped suspend policy. Proper PCIe host
PM, Wi-Fi wake behavior and power measurements remain necessary before this
mode can be enabled in ordinary use. No active SICD configuration is flashed.

## Why

Without the hints, the el3mon/ACPM firmware rejects every C2 entry. The menu
governor keeps choosing C2, the entry fails at once, and the idle loop asks
again: the idle CPUs spin on SMCs instead of sleeping. In ten minutes cpu0
counted about 76 million rejections
(`/sys/devices/system/cpu/cpu0/cpuidle/state1/rejected`). With the screen off,
disabling C2 alone took the phone from 2.77 W to 2.12 W at the USB input.

## C2 hints

Mainline `exynos-pmu` does the same bookkeeping for GS101, and Google's
`flexpmu_cal_cpu_gs201.h` shows GS201 uses the same registers. Around every C2
entry (CPU PM notifier):

- **Before power down:** PMU `CPU_INFORM` (0x18060000 + 0x860 + 4 × cpu) is set
  to C2 (1) through the secure register SMC (0x82000504). The CPU's GRP2
  wake-up interrupt in `pmu_intr_gen` (0x18070000 + 0x200) is enabled, and its
  GRP1 pending bits (cpu and cpu + 8) are cleared.
- **After wake-up, or a failed entry:** the hint is cleared, the GRP2 enable
  bit dropped, and its pending bit cleared.
- **Once a restart starts:** C2 entries are refused, as mainline does.

## Cluster power-down (CPD)

This follows the vendor's `exynos-cpupm`. The last CPU of a cluster to go idle
hints CPD (2) instead of C2, and the firmware powers the cluster down with it.
A CPU may do so only when:
- every online sibling is idle;
- every sibling's next timer, and its own, is at least `cpd_residency_us`
  (10 ms, the stock `target-residency`) away;
- no sibling has hinted CPD and not yet powered down (PMU
  `CLUSTERx_CPUy_STATUS`, 0x18060000 + 0x1004 onwards).

Each CPU's next timer is `dev->next_hrtimer`, which cpuidle sets just before it
calls the state's enter function. Stock read it through an Android vendor
hook. Here each C2 state's enter callback is wrapped to record it. Every CPU
has its own PSCI driver, and each CPU wraps and restores its own, so no CPU is
ever inside a wrapper being changed.

The separate `enter_s2idle` callback must also be wrapped: the DT idle-state
parser initializes it independently of `enter`. During its frozen-tick
interval, `next_wake` is `KTIME_MAX` rather than the stale ordinary
`next_hrtimer` prediction. This lets cluster checks reflect the stopped
local timers. Both callback pointers are restored on unload. Per-CPU
`s2idle_entries` and `s2idle_last` count frozen entries and all-cores-idle
observations. Ordinary C2 timer handling is unchanged.

Parameters (all in `/sys/module/pixel_cpupm/parameters/`):
- `cpd`: a bit per cluster. It defaults to 0x6, the mid and big clusters,
  because the stock DT allows no CPD entry for the little cluster. Writable at
  run time.
- `cpd_residency_us`: the minimum time to the next timer.
- `cpd_entries`: CPD hints given, per cluster.

With `cpd=6` the PMU reports the mid and big clusters' non-CPU domains off
(`CLUSTER1/2_NONCPU_STATUS` = 0); with `cpd=0` they stay on. Screen off, each
cluster takes about 15 CPD hints a second.

## CPU hotplug

Deep sleep offlines CPUs 1–7 before the firmware call, so hotplug needs the
same PMU bookkeeping as C2. This follows mainline GS101 (`exynos-pmu.c`
`gs101_cpuhp_pmu_offline/online`) and stock `exynos-cpupm.c:1105-1152`
(`cpuhp_cpupm_offline/online`, `cal_cpu_disable`, `cal_cluster_disable`):

- **Going offline:** a `CPUHP_AP_ONLINE_DYN` teardown, the state both mainline
  and stock use, runs on the dying CPU in its hotplug thread. The CPU is still
  in `cpu_online_mask` then; `CPUHP_TEARDOWN_CPU` removes it, and the idle
  loop then calls PSCI CPU_OFF. The teardown writes `CPU_INFORM` = C2, enables
  the CPU's GRP2 wake-up interrupt and clears its GRP1 pending bits. The last
  online CPU of cluster 1 or 2 writes CPD (2) instead, if `cpd` allows that
  cluster. A module can only register the dynamic states, and AP_ONLINE_DYN
  is the only one that runs on the dying CPU. The later DYING states, which
  run with interrupts off just before CPU_OFF, are static and reserved for
  core code.
- **Between the hint and CPU_OFF**, the CPU's idle entries are refused
  (`in_cpuhp`, as mainline). Otherwise a C2 exit would clear the hint and the
  wake-up interrupt again.
- **Coming online:** a `CPUHP_BP_PREPARE_DYN` startup on the control CPU,
  before PSCI CPU_ON, disables the GRP2 interrupt, clears its pending bit and
  lets the CPU use C2 again. Once the CPU runs, the `AP_ONLINE_DYN` startup
  clears its own `CPU_INFORM`, as a C2 exit does, and rejoins the tick
  broadcast. The same startup undoes an offline that failed later on. Stock
  and mainline clear the *control* CPU's hint in the BP step instead: that
  write is a no-op and is skipped, so CPU0's hint is never written during deep
  sleep.
- Hotplug hints check the SMC result and read `CPU_INFORM` back (as
  `pixel-reboot` does). Failures count in `hotplug_errors`.
- The idle path's CPD rules are unchanged. They already consider online
  siblings only, like stock `cpus_busy()`, and a dying sibling (online, idle
  refused) blocks CPD until it is gone.

**Loading and unloading.** Loading fails unless every CPU is online, since
each CPU wraps its own idle states. Each offline CPU holds a module reference,
so `rmmod` reports the module in use. Once removal has begun, offlining is
refused (`-EBUSY`). Unloading still needs C2 disabled in sysfs first.

Parameters: `hotplug_offlines` (per CPU), `hotplug_cpd` (per cluster),
`hotplug_errors`, and `pmu_status`. `pmu_status` is a read-only snapshot of
the ALIVE registers used here: the online mask, `CPUx_STATUS` (`cpu_on`),
`CLUSTERx_NONCPU_STATUS` (`cluster_on`, 0x1204/0x1404/0x1604), `in_cpuhp`, the
GRP2 enable and the eight `CPU_INFORM` values. An offline CPU always reads
`cpu_on` 0, GRP2 set and `inform` 1 or 2. An online CPU shows the same values
while it is in C2.

## System sleep handover

`pixel_cpupm_system_sleep(bool on)` (exported GPL; callers declare it) is for
`pixel-sleep`. Call it with `true` in syscore suspend, just before writing
CPU0's `CPU_INFORM` = 4. Call it with `false` in syscore resume, after
clearing CPU0's hint. In between, the CPU_PM notifier writes nothing and
changes no idle state, like stock `system_suspended`
(`exynos-cpupm.c:974-976`). Otherwise `cpu_pm_suspend()`'s CPU_PM_ENTER (the
last syscore suspend step) would overwrite CPU0's 4 with C2, and its
CPU_PM_EXIT would clear it before `pixel-sleep` reads the wake state.
`sleep_ignored` counts these events: 2 per cycle. The hotplug callbacks do not
check the flag, because CPU hotplug is disabled for that whole window. A call
with `true` while secondaries are online warns once.

## The MCT across power loss

MISC, which holds the MCT, loses power in SYS_SLEEP, so the counter restarts
stopped and the broadcast comparator would never match. `pixel-mct` now has a
clockevent `resume` callback. `clockevents_resume()` is the first step of
`timekeeping_resume()`. It runs before the clocksources are read, and before
`tick_resume()` puts the broadcast device back to work. The callback does:

- If G_TCON START is clear, it restarts the counter as mainline
  `exynos4_frc_resume()` does: stale write-status bits are cleared and the
  interrupt is disabled first. It counts `frc_restarts`.
- It then runs a polled one-shot self-test (interrupts are off on the only
  CPU): one match 100 µs ahead, exactly once, then shut down with the status
  cleared. It counts `resume_checks` or `resume_errors`.

Stock's MCT clocksource resume ran at the same point, before exynos-pm
restored CMU_MISC (stock `drivers/clocksource/exynos_mct.c:167-174, 217-229`).
On every s2idle wake the counter is still running, so the callback only reads
G_TCON. The polls are bounded by MCT read counts, not `udelay()`: on Exynos
the arch timer shares this counter (mainline commit 6282edb72bed, and the
two counts match here). `resume_selftest=1` also runs the self-test on deep
(`mem`) resumes where the counter kept running, to exercise it under
`pm_test=core`.

## Testing hotplug and deep sleep

Results on 2026-09-30 (module swapped in live, `cpd=6`):
- 1,000 rounds of the randomized offline/online stress passed: 10,266 CPU
  kills, `hotplug_errors` 0, CPD hints exactly one per emptied mid and big
  cluster, the emptied cluster's NONCPU status off, no kernel warnings. The
  firmware adds bit 16 to an offline CPU's CPU_INFORM (0x10001, 0x10002).
- `pm_test=processors` with `mem_sleep=deep`: three of five runs took all
  secondaries through the new hotplug path and back (+1 offline each, CPD +1
  for clusters 1 and 2); the other two were refused by cpif (the modem was
  waking, -EBUSY), as designed.

Do not load or test on a phone without the watchdog feeder running. `P` is
`/sys/module/pixel_cpupm/parameters`.

1. **Hotplug, per CPU:** do 1000 cycles for each of CPUs 1–7. Offline, wait
   20–50 ms, check, online, and wait 20–50 ms of idle. Mix single CPUs with
   emptying a cluster (4+5 and 6+7). While a CPU is offline, its `cpu_on` bit
   must be 0 in `$P/pmu_status`. With `cpd=6`, `cluster_on` for an empty
   cluster 1 or 2 must be 0; with `cpd=0` it stays 1. `hotplug_errors` must
   stay 0, and `in_cpuhp` must be 0 when all CPUs are online. The kernel log
   must show no "may not have shut down cleanly", "failed to come online",
   BUG or RCU stall. Afterwards, `cpd_entries` and every CPU's
   `cpuidle/state1/usage` must still increase, and a pinned
   `sleep 0.5` must return on time on each CPU.
2. **`pm_test=processors`:** `cat /sys/power/mem_sleep` must list `deep`.
   Then run `echo deep > /sys/power/mem_sleep; echo processors >
   /sys/power/pm_test; echo mem > /sys/power/state`. Pass: it returns 0 after
   about 5 s, `hotplug_offlines` rises by 1 for each of CPUs 1–7, and
   `hotplug_cpd` rises by 1 for clusters 1 and 2 (CPUs 5 and 7 go last).
   Restore with `echo none > /sys/power/pm_test; echo s2idle >
   /sys/power/mem_sleep`.
3. **`pm_test=core`:** set `pixel_mct` `resume_selftest=1` and run the same
   sequence with `core`. Pass: `resume_checks` +1, `resume_errors` 0,
   `frc_restarts` 0 (power is kept), all `inform` 0 afterwards, and pinned
   sleeps on every CPU. With `pixel-sleep` loaded, also `sleep_ignored` +2.

## The local timer and the MCT

A CPU in C2 loses its arch timer comparator. The stock DT's idle states do not
say so (no `local-timer-stop`); stock used the Exynos MCT as the broadcast
timer. On the first test, cpu3 went into C2 once and never took a timer
interrupt again. Every timer queued on it stayed pending, and the next restart
hung in its sync.

So, before registering the notifier, `pixel-cpupm` does two things on every
CPU:
- flags each C2 state `CPUIDLE_FLAG_TIMER_STOP`;
- enables the tick broadcast.

The order is safe because the firmware rejects C2 until the notifier is
registered, so no CPU powers down early.

**The broadcast device.** Without `pixel-mct`, the kernel's hrtimer broadcast
runs the broadcast. The CPU that holds its hrtimer can't enter C2, so one CPU
was always in WFI, and its cluster could never power down. Mainline
`exynos_mct` can't bind to the stock MCT node (its clocks come from a vendor
provider), so the kernel skips it at boot.

`pixel-mct` drives the MCT's global comparator 0 as a broadcast device (rating
250, replacing the hrtimer's 0):
- **Hand-off.** The bootloader starts the MCT's 64-bit free-running counter at
  24.576 MHz, the same count as the arch timer (it reads uptime plus the
  bootloader's 11 s). It leaves the MCT's clock on: CMU_MISC's MCT PCLK gate
  (+0x209c) under hardware control, and the Q-channel (+0x3088) off. The
  module checks all of this and refuses otherwise.
- **The comparator.** It is programmed as mainline's
  `exynos4_mct_comp0_start()` does: COMP0 low and high words, the interrupt
  enable, then G_TCON. Each write waits for its G_WSTAT bit.
- **The interrupt.** G0 is GIC SPI 785, resolved through the stock node's
  interrupt map.
- **Missed targets.** The comparator fires on an exact match, so a target
  already passed would never fire. `set_next_event` reads the counter back
  and returns `-ETIME`, and the clockevents core retries with a longer delta.
- **Self-test.** Before registering, one event 100 µs ahead must interrupt
  exactly once.

The tick core keeps a reference to the module, so it has no unload path.

## Checked

- **C2 in use:** every CPU entered it tens of times a second. Each CPU's tick
  deadline kept advancing, and a timed sleep pinned to each CPU returned.
- **The MCT broadcast:** 500 ms sleeps pinned to each CPU returned in 509 to
  516 ms. It ran for 40 minutes (77,000 interrupts) with no warnings, across
  module reloads and CPD changes.
- **Restarts:** a warm restart to fastboot and a cold boot both work.
- **Unloading `pixel-cpupm`:** disable the C2 states in sysfs first. Without
  the hints, C2 entries are rejected again. Every CPU must be online.
- **Not yet checked on the phone:** the hotplug callbacks, the system-sleep
  handover and the MCT resume path. These are built with W=1 and have no
  warnings.

## Power

Screen off, idle on Wi-Fi, at the USB input:

| Configuration | Draw |
| --- | --- |
| hrtimer broadcast, no CPD (image E) | 1.47 W |
| MCT broadcast, no CPD | 1.45–1.51 W |
| MCT broadcast, CPD on clusters 1 and 2 | 1.47–1.48 W |

C2 itself saves little beyond WFI (2.12 W vs 2.13 W). CPD saves about 5 to
10 mW on the mid and big rails, below the input meter's noise.

The mid and big rails still draw about 60 and 50 mW with every core and the
cluster logic off. That draw follows the cluster clock, not the load: with
the mid cluster's cores idle, its rail drew 62 mW at 400 MHz, 81 mW at
1.2 GHz and 125 mW at 2.35 GHz. The cluster PLLs and clock trees keep running
in CPD. Stock stops them in SICD, the SoC's clock-down idle. The guarded experiment
above is not enabled in ordinary use.

Clock gating is not the gap:
- every active CMU already has automatic clock gating on
  (`CONTROLLER_OPTION` = 0xf1000000);
- enabling the vendor's `pmucal_lpm_init` bus-component gating (DRCG, all zero
  after the bootloader) in 15 active sysregs changed nothing measurable;
- nor did NOCL1A and NOCL2A root-clock gating (HCHGEN).
