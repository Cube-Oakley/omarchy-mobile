# UI smoothness — September 28, 2026

The user reported that the Pixel felt less smooth than the OnePlus. Five
limits were found and fixed:

1. the display ran at 60 Hz;
2. the GPU was pinned at its lowest clock;
3. touch arrived at about 21 Hz;
4. the CPUs were capped below half speed;
5. a startup race broke the app switcher.

Image S in `boot_a` carries all five fixes; the kernel changes are in
checkpoint [v21](../kernel/smooth-v21/README.md).

## Measuring

`pixel_scanout`'s debugfs `starts` counter counts frames sent to the panel,
and the driver only sends frames when something changes. The measurement
used:

- **A frame sampler:** reads the counter every millisecond while a script
  opens and closes the launcher and the shade over quickshell IPC
  (`apps`, `controls`, `dismiss`).
- **GPU time:** from Panthor's `drm-cycles` in each process's fdinfo, once
  `profiling` is set to 1.
- **Touch timing:** recorded from the input device's `SYN_REPORT` timestamps.
- **Synthetic touches:** a uinput touchscreen (uinput rebuilt as an
  out-of-tree module) for swipes, and for replaying recorded gestures with
  their original timing.

| | 60 Hz, GPU 302 MHz | 120 Hz, GPU scaling, 603 MHz floor |
|---|---|---|
| Mean frame rate, launcher and shade loop | 61 fps | 111–115 fps (warm) |
| Median interval | 16.6 ms | 8.3 ms |
| Intervals under 10 ms | 9% | 97% |
| p99 | 33 ms | 17 ms |

## 1. The display ran at 60 Hz

Hyprland asked for 120 Hz, but the driver's atomic check refused it.
`pixel_bandwidth.h` only accepted the memory clock a fastboot RAM boot leaves
(421 MHz). A normal boot leaves 2028 MHz, which already exceeds the 1352 MHz
that 120 Hz needs, so the check now accepts that rate and holds it. The
Pixel's `adapter/mobile.json` also sets `displayMode` to `1440x3120@120`.

## 2. The GPU was pinned at 302 MHz

Hyprland and Qt were not the limit: a fullscreen `weston-simple-egl` and a
one-window quickshell animation both ran at a steady 120 fps. The shell's
full-screen translucent layers cost about 8.3 ms of Hyprland GPU time plus
2.5 ms of quickshell GPU time per frame at 302 MHz, over the 8.33 ms budget.

The GPU now scales over the stock `gpu_dvfs_table` top clocks (302, 470,
603, 750 and 885 MHz):
- **Clock pairing:** `pixel_gpu.c` keeps the shader-stack clock at the stock
  pair (202, 351, 471, 572 and 701 MHz). The 996/848 MHz level, which needs
  the maximum memory clock, is not exposed.
- **Power-gated GPU:** writing devfreq's `min_freq` while the GPU was
  power-gated asked the firmware to reclock it and reset the phone. The
  pairing notifier now refuses any change unless the GPU is runtime-active.
- **Thermal cap:** a PM QoS maximum on the G3D zone gives 603 MHz from 60 C,
  302 MHz from 65 C, and releases below 55 C. It was checked with emulated
  temperatures.
- **Clock floor:** the boot script sets a 603 MHz floor. Without it, each
  animation began at 302 MHz until devfreq's first 50 ms poll. The GPU still
  power-gates when idle.

## 3. Touch arrived at 21 Hz

The temporary GPIO SPI driver (`mainline/pixel-touch-input.c`) delivered a
report every 45 ms. It spent 70 µs per byte bit-banging, clocking a 157-byte
vendor report 0xc3 with every touch report. It also slept 5–10 ms after each
of 1,106 not-ready reads, and polled only every 8–12 ms. Now:

- **Faster transfers:** a shadow of the output register and no read-backs
  between clock edges, about three times faster.
- **Shorter retries:** 0.5 ms with the same 100 ms budget (`retry_us`).
- **Fast polling while touched:** 1 ms polls for a second after any report
  (`active_poll_us`), then back to 8 ms when idle.

Recorded while the user dragged: 95 Hz mean, median 10.7 ms. The user reports
that the shade and launcher now drag smoothly.

**Image T: heat map off, and recovery.** Touch later stopped for good while
the phone was unplugged. The old worker exited on any error: a protocol error,
a controller reset (identify report 0x10), or AOC taking the bus. Its log was
lost with the reboot. The driver now:

- **turns off the heat map:** after each controller start it sends
  DISABLE_REPORT (0x06) for report 0xc3, a runtime setting that Google's
  driver also toggles. Recorded rate with it off: 201 Hz mean, median 3.8 ms.
- **recovers:** after any error it releases contacts, waits if AOC holds the
  bus, resets the controller and continues. Retries back off from 0.5 s to
  30 s.
- **can be restarted by hand:** writing 1 to the `restart` parameter runs the
  same recovery. It was tested: back in 0.3 s with the heat map off again.

Hardware SPI with the touch interrupt remains the real fix. SPI0 is PERIC1
USI0: vendor clocks `GATE_PERIC1_TOP0_USI0_USI` (1401) and
`VDOUT_CLK_PERIC1_USI0_USI` (1412), USI offset 0, interrupt 690, GPIO chip
select, 10 MHz mode 0.

## 4. The CPUs were capped

The bring-up exposed only rates below the boot clocks. The full stock tables
reset the phone within a second of load. A BIG core at 2850 MHz read 87 C on
its sensor, and the bring-up thermal policy reboots at 80 C. It was a thermal
reboot, not a crash. Stock's BIG zone controls at 100 C, with emergency
limits at 110 and 120 C, and uses sensor interrupts rather than polling.

- **Caps:** LITTLE up to its stock maximum of 1803 MHz, MID to 1999 MHz and
  BIG to 2048 MHz.
- **LITTLE floor:** follows the stock MID and BIG dependency tables.
- **CPU thermal zones:** throttle from 75 C and reboot at 95 C, polling every
  250 ms and every 100 ms while throttling.
- **Result:** one BIG core under full load peaks at 68 C. All eight cores hold
  74–75 C by throttling (BIG 1582, MID 1663, LITTLE 1704 MHz), with no
  reboot.
- **Cooler starts:** the boot script now waits up to five minutes for 58 C
  before starting CPU scaling and the GPU. A restart while hot had left the
  desktop unstarted, because the GPU guard's refusal was fatal.

Opening the switcher from home had a 92 ms first frame; it is 67 ms now on
first use after boot and 17.5 ms after that.

**Image U: full rates under a fast cap.** A polled zone cannot follow a BIG
core at 2850 MHz, but a thread reading the BIG and MID sensors every 20 ms
can. It is `pixel-cpu-thermal` in [v22](../kernel/smooth-v22/README.md).
- **Levels:** it allows the stock maxima below 75 C, and caps at 2048/1999,
  1582/1663 or 1106/1197 MHz as the sensors pass 75, 85 and 92 C.
- **Hold:** a level holds for at least a second before it lifts, because the
  sensor swings about 10 C within one poll.
- **Result:** bursts from cool get about 0.75 s at 2850 MHz, which covers app
  launches and first opens. Sustained single-core load settles at 2048 MHz
  (61–69 C), and all eight cores stay at 75–81 C. No thermal reboot.

## 5. The app switcher could not minimize the terminal

The session starts kitty right after the shell. The window opened before
Quickshell had loaded the workspace list, and its toplevel kept a null
workspace for good. The shell therefore treated the phone as the home screen:
a bottom swipe did nothing to the terminal, and the switcher could not handle
it. The shared `shell.qml` now re-queries workspaces and windows until every
window has a workspace. A synthetic home swipe, the switcher card tap, and a
21 Hz home swipe all work.

## Holding for the switcher from the home screen

From an app the switcher was smooth. From the home screen, the user saw it
open only partway while the finger was held, then jump fully open with a
hitch on release.

- **Cause:** holding from an open app calls `motion.animateTo(1)`, which ends
  the drag. The home path set `motion.progress = 1` directly, and the drag
  that `beginDrawer()` started stayed active. Any finger jitter then reached
  `motion.update()` and pulled the switcher back to the finger's distance
  (about 0.2) until release.
- **Fix:** the home path now also ends the drag. A synthetic hold from home,
  with jitter every frame, shows the switcher fully open mid-hold.
- **Result:** once warm, the worst frame is 18–25 ms; the first open after the
  shell starts still costs about 60 ms.

**Gradual reveal from home.** After that fix, the switcher opened from home
at full size at once, which the user found abrupt. The backdrop now dims in
while the cards grow and fade in over 210 ms: `present` runs from 2 to 1, the
minimize animation reversed. Side cards also fade for `present` above 1, so
they no longer pop at the start of the reveal or the end of a minimize.

## Current shared shell on the Pixel

The Pixel's shell source had nothing of its own to lose. Of 237 files, 214
matched the repo, 21 were older repo versions, and the other two held only the
switcher fixes. `overlay/mobile/install.sh` with the Pixel adapter installed
the current shell: alphabetical launcher, the shade's new layout with weather
and stats, and Pixel `device.json` with 120 Hz. The old configuration was
backed up. The Pixel adapter has no `power-suspend.sh`, so the sleep policy
only waits and never suspends. The launcher and shade loop now runs at a
steady 120 fps: mean 8.3 ms, p99 under 10 ms.

## Black screen that swallowed touch

The user found the screen dark with touch dead. The power key then played the
off animation before a second press lit the screen, and touch worked. The
cause was test tooling:
- **Out-of-band `on`:** frame measurements woke the screen by running
  `omarchy-mobile-display on` over SSH, without `WAYLAND_DISPLAY`.
- **Failed CRT request:** `quickshell ipc` then finds no shell ("No running
  instances"), so the CRT `on` request failed.
- **Fallback:** the script's fallback enabled DPMS under the CRT's parked
  black cover. The panel was lit but covered and eating touches, while the
  power key's toggle, which reads DPMS, took the screen for on.

`display-power.sh` now finds `WAYLAND_DISPLAY` from `hyprctl instances` when
it is unset, and an SSH-run `off` then `on` plays both animations. The power
key and screen timeout were never affected: they run inside the session.
The Pixel's Hyprland configuration also hides the pointer now
(`cursor.invisible`), which had shown the default Hyprland cursor.

## Remaining

- Opening the switcher for the first time after the shell starts costs one
  slow frame (about 60–67 ms).
- Touch now runs on the SPI0 controller with its interrupt (image V): see
  the [SPI touch record](touch-spi-20260928.md).
- Sustained CPU speed above 2048 MHz would need higher trips, closer to
  stock's 100 C control point, with a controller proven against them.
- **App-in-front measurements:** the "long frames" measured with an app in
  front are an idle gap of 67–109 ms after each shade animation, before one
  settling frame. They are not a stall: the animation itself runs at 8–9 ms
  per frame, with one 17–19 ms frame as the shade first maps.
