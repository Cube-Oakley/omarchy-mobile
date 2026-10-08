# V28: the always-on display in the panel's low-power mode

`display-v28.patch` is a complete patch against the pinned base, as v27's
is. It differs from v27 in `pixel_scanout.c` and `pixel_panel.h`.

**The panel (`pixel_panel.h`)**
- `panel_display_set()` sends stock's display off and on sets, with the VREG
  and AMP changes.
- `panel_lp()` enters and leaves AOD as `s6e3hc4_panel_set_lp_mode()` and
  `s6e3hc4_set_nolp_mode()` do on a PVT panel:
  - fixed 30 Hz TE;
  - auto mode, with the panel refreshing itself at 10 Hz from its memory;
  - AOD low (10 nit) or high (50 nit) brightness;
  - the DSIM's 30 Hz command timers (UNDERRUN 21827, TE timer `0x680f787d`).
- Leaving AOD restores the 60 Hz group, the brightness and display-on.

**The driver (`pixel_scanout.c`)**
- `pixel_scanout.ambient` (0 off, 1 low, 2 high) enters AOD while the
  screen is on. It drops to 60 Hz first and releases the 120 Hz bandwidth
  floor. Clearing it, or screen-off, leaves AOD and restores the CRTC's
  refresh rate.
- In AOD, as stock's hibernation does, the pipeline powers off 100 ms after
  each frame (v27's `pixel_power.h`) and back on for the next one; the
  panel keeps showing its memory. A frame in AOD may wait for the 10 Hz
  idle refresh, so the frame-done wait there is 250 ms.
- Brightness changes wait until AOD ends.
- Debugfs shows `ambient_lp` and `ambient_hibernations`.

**Outside the kernel**
- **`pixel-display-hook ambient`** (from `omarchy-mobile-display`), once the
  clock shows, enters AOD and sets `pixel_pmic_opmode.panel_on=1`: the
  panel's VCI and VDDD, which otherwise follow PWREN_MIF and drop in sleep,
  stay on. `ambient-exit` runs before the clock fades out.
- **The shell's sleep loop** sleeps under the clock when the device profile
  sets `ambientSleep`. It wakes a second after each minute, redraws the
  clock (IPC `ambientTick`: Qt's timers stop in suspend), and sleeps again
  after 1 s. The adapter brings nothing up for these wakes
  (`OMARCHY_MOBILE_WAKE_PURPOSE=ambient`). Every `sleepCheck` minutes the
  minute wake is also the background check.

**Validated by RAM boot on October 1**
- AOD entry and exit while awake: 0 failures. Over a minute the pipeline
  powered off after every frame (5 cycles).
- Exit restored 120 Hz with its bandwidth floor.
- Three minute-long deep sleeps under the clock. Each wake drew the new
  minute in one or two frames, and the pipeline powered off again.
- The user watched the clock stay up and update at each minute.

Built with `scripts/build-pixel-shell.py --persistent-root --seconds 0 --cma
256M`. `kernel.config` matches v27's except for the initramfs path.

**Installed in boot_a on October 1** with the user's approval, through
`scripts/install-pixel-boot.py` from a RAM boot of the same image (readback
`260ec8bf…`; the previous image was v27, `a92d0e7e…`). Unplugged on the
installed kernel, the user found the screen stuck after quick power-key
presses between the clock and the lit screen: a frame timed out 0.27 s after
leaving AOD and the driver failed closed until a reboot. v29 recovers from
that.
