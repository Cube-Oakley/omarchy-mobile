# The display's stats are read under its lock (2026-10-08)

**The crashes.** On October 7 at about 14:00 (v42) and October 8 at about
12:41 (v43) the phone restarted by itself while it lay idle on the always-on
display: a synchronous external abort in `pixel_stats+0x144` (the debugfs
file `dri/*/pixel_scanout`), read by a root `python3`, 0.16 s after the
phone woke from deep sleep, then panic and the watchdog (pstore
`/var/log/pstore/20261007-140011` and `20261008-124310`, the second boot's
console at line ~28104). Both times the faulting load is `readl(p->dma +
0x440)` (`x2 = ffff80008005d440`). The reader is the root sleep helper
(`pixel-suspend.py`): its `minute_tick()` polls the file every 50 ms while the
AOD clock's new minute is sent and the pipeline powers off again.

**The cause.** `pixel_stats()` read the DMA register when `power.off` was
clear, without `hw_lock`. `pixel_power_down()` (always under `hw_lock`: the
AOD hibernation, screen-off and the fault recovery) turns the DPU and DISP
domains off first and sets `power.off` only afterwards, so a read in that
window reached a powered-off block. The poll lands in it a few times a day at
most; the phone sleeps through hundreds of minute ticks a day.

**The fix (v44).** `0001-pixel-scanout-stats-read-under-hw-lock.patch` reads
`power.off` and the register under `hw_lock`; the rest of the file is
counters and needs no lock. The kernel is v43's otherwise: built with
`scripts/build-pixel-shell.py --persistent-root --seconds 0 --cma 256M
--mfc-cma 128M` (`out/scanout-stats-v44`, build ID `50741e4b…`, image
`29bf2acf…`), same release string, configuration and command line; the only
source file changed since v43 is `pixel_scanout.c`.

**The test.** `scanout-race-test.sh N` reads the stats file in a tight loop
(about 14,000 reads a second) while `kernel/apm-watchdog/dpu-cycle-test.sh`
puts the pipeline through N AOD hibernations. On v43 the first cycle panicked
(Asynchronous SError in `pixel_stats+0x14c`, 0.16 s after the cycle began,
pstore `20261008-125117`). On v44, RAM-booted, 300 cycles ran clean: 297
hibernations, 0 failures, 1.44 million reads. Then **installed in boot_a**
(readback matched; the previous image was v43, `d41f736b…`) and an orderly
reboot came up on it with the modem and mobile data back.
