# AP watchdog across suspend

`pixel-wdt-pm` pauses the two GS201 AP watchdogs in `suspend_noirq` and
restores each previously enabled watchdog in `resume_noirq`, with its full
timeout. Userspace is frozen during this interval, so the modem supervisor's
`wdtkick` cannot feed it. Loading and unloading leave the watchdog untouched;
the existing feeder continues to own arming, feeding and clean shutdown.

The module checks the Pixel compatible and both stock DT register ranges.
It preserves the inherited divider/prescaler and configured timeout. Only
WTCON, WTDAT and WTCNT are used, following the in-tree s3c2410 watchdog's
save/stop/reload/restore sequence. A stop readback failure aborts suspend and
rearms the watchdogs. `/sys/module/pixel_wdt_pm/parameters/resumes` counts
restore callbacks.

The watchdog protects normal execution and most device transitions. It is
paused during the noirq callbacks and sleep itself; an RTC wake alarm is
still required during bring-up. This helper supports suspend, not hibernation.
It must not be used alongside another kernel driver owning these watchdogs.

The persistent modem bundle now includes this module in its checksum manifest.
`manager.py prepare` loads it after starting the watchdog feeder, before GPIO
and audio setup. This does not enable automatic suspend. An actual 44.8-second
RTC sleep exceeded the feeder's normal timeout and resumed in the same boot,
with watchdog control and reload values restored.

Build against the running kernel:

```sh
make -C devices/pixel7pro/mainline/linux ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- \
  M="$PWD/devices/pixel7pro/kernel/watchdog" modules
```

For short, supervised bring-up trials only, `keep_running=1` reloads the
already enabled watchdogs in the noirq callback without stopping them. It
never arms a stopped watchdog or changes its timeout. This can provide a
reset fallback if a new sleep mode hangs, but only if its watchdog clock
continues running. Use an RTC interval comfortably below the existing
watchdog timeout and restore `keep_running=0` afterwards; leaving it enabled
would reset the phone during an ordinary long sleep. The default is unchanged.
