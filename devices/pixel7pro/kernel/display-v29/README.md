# V29: a display fault recovers itself

`display-v29.patch` is a complete patch against the pinned base. Unlike
v26–v28's, it carries everything the image was built from. Those patches
left out changes recorded only as separate patches, and the images had them:
- `btbcm.c`'s unacknowledged Write RAM (in `audio-v26`);
- `kernel/suspend/0001` (brcmfmac D3 ack), `0002` (ACPM idle snapshot) and
  `0004` (earlycon and ramoops ownership: `head.S`, `pixel_earlycon.c`).

The patch applies cleanly to a clean worktree of the base. Each of its 56
files then matches the build tree byte for byte.

**The driver (`pixel_scanout.c`, `pixel_power.h`)**
- **Faults recover themselves.** A failed frame used to fail closed until the
  next screen-off. The always-on clock never turns the screen off, so after
  a frame timed out on the installed v28 (0.27 s after leaving AOD, with
  quick power-key presses) the display stayed dead until a reboot.
- **How recovery works.** A fault now schedules a recovery 100 ms later:
  - the pipeline powers off (`pixel_power_down(pw, force)`, which skips the
    idle checks), so the DMA can no longer read a retained buffer;
  - the fault clears and the failed buffer is released;
  - the pipeline powers back on, or in AOD stays off until the next frame.
- **Limit.** After three recoveries in a minute it stays failed closed until
  a screen-off, which recovers it the same way.
- **Late frames.** A frame done that comes late (a panel mode change can hold
  TE back) is counted, not failed: after the 100 ms (AOD 250 ms) wait, it
  waits another 500 ms.
- **Logging.** A failure logs the DECON and DSIM interrupt state.
- **Test hook.** `pixel_scanout.test_fault` (write-only) fails closed on
  demand.
- **Debugfs** shows `late_frames` and `recoveries`.

**Outside the kernel**
- **The shell's screen timeout.** Quickshell's `IdleMonitor` stops firing
  when its timeout changes: neither the old nor the new one fires until it
  is restarted. A test config showed this. After Settings changed the
  timeout, the screen stayed lit until the next screen-on. `shell.qml` now
  restarts the monitor when the timeout changes.

**Validated by RAM boot on October 1** (`power-image-3`, build ID
`5164d05d…`):
- A timeout change from 60 s to 15 s with the screen lit: it went to the
  clock 17 s later.
- An injected fault with the screen lit: recovered in 104 ms, and the next
  redraw completed 120 frames.
- An injected fault under the clock: recovered in 101 ms, the minute redraw
  completed, and leaving AOD restored the lit screen.
- 14 power-key presses through `omarchy-mobile-power` (1–3 s apart), between
  the clock and the lit screen: 0 failures, about 1,360 frames.

The v28 frame timeout itself did not recur, here or in 10 tries on v28.

Built with `scripts/build-pixel-shell.py --persistent-root --seconds 0 --cma
256M`. `kernel.config` matches v28's except for the initramfs path.

**Installed in boot_a on October 1** with the user's approval, through
`scripts/install-pixel-boot.py` from a RAM boot of the same image (direct
readback matched `93ddc7eb…`; the previous image was v28, `260ec8bf…`).
Reboot validation:
- the build ID matched;
- `display_power` and `display_off_required` were on, with `suspend=all`;
- frames completed;
- AOD entry (pipeline off, `panel_on`) and exit (back to the lit screen)
  ran without a failure.
