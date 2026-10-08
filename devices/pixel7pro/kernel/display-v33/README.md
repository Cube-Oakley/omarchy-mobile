# Display v33: the panel's own 10 Hz idle at 120 Hz (2026-10-02)

v32 plus auto mode at 120 Hz. In normal use the S6E3HC4 was in manual mode
at 120 Hz, redrawing from its own memory 120 times a second even with a
static screen. The scanout sends almost no frames then (0.1 a second).

**Change (`pixel_panel.h`, `pixel_scanout.c`).** At 120 Hz, `panel_refresh()`
puts the panel in auto mode with early exit and a 10 Hz idle, as stock's
`s6e3hc4_set_panel_feat()` does (FEAT_FRAME_AUTO, FEAT_EARLY_EXIT, HS, not
HBM):

```
B0 00 0C BD / BD 00 00
B0 00 12 BD / BD 00 00 16 00 06 01
BD 23
```

That replaces `BD 21` and `60 00`. The panel watches for frame traffic
itself: it drops to 10 Hz when frames stop, and leaves the long frame at
the next one. The TE option at 120 Hz was already stock's fixed TE (0x51),
which stock also uses with a 10 Hz idle. 60 Hz, the 60 Hz group before AOD,
and AOD itself are unchanged.

`pixel_scanout.idle_refresh` (0, 10, 30, 60; default 10) changes it at run
time; 0 is the old manual mode.

**Measured (October 1–2,** `scripts/pixel-screen-power.sh`, two-minute coulomb
windows on battery, static app drawer, brightness 184):

| Setting | Round 1 | Round 2 |
| --- | --- | --- |
| 120 Hz manual | 497.4 mA | 484.5 mA |
| 120 Hz, 10 Hz idle | 428.3 mA | 428.3 mA |

About 63 mA saved, more than forcing 60 Hz (451 mA). Opening and closing
the app drawer by IPC sent 94 frames with no failures, late frames or DSI
underruns.

**Validated by RAM boot** (`power-image-10`, build ID `bc6fffc6…`):
- `BD 23` (auto) and 94@0x52 `00 0c 60 02` at boot, after two always-on
  clock cycles and after a screen-off/on;
- 538 transfers with 0 failures, late frames, recoveries or underruns.

**Installed in boot_a on October 2** (the user had pre-approved installs for
this work) through `scripts/install-pixel-boot.py`. Direct readback matched
`4f4ab1c8…`; the previous image was v32, `5d7e90c9…`. After the reboot:
- the build ID matched;
- `idle_refresh` read 10, and `BD 23` was set;
- `display_power` was on, with `suspend=all`.

To check by eye: the first touch after a still screen should feel as quick
as before.

Built with `scripts/build-pixel-shell.py --persistent-root --seconds 0 --cma
256M`. `kernel.config` matches v32's except for the initramfs path.
`display-v33.patch` is complete against `kernel-base.txt` (56 files).
