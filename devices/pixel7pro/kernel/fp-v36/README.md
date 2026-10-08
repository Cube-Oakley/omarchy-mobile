# v36: binder and the fingerprint circle (2026-10-02)

v35 plus the two kernel pieces the under-display fingerprint sensor needs from
outside its trusted app. See [the fingerprint notes](../../fingerprint/README.md).

**Binder.** `CONFIG_ANDROID_BINDER_IPC` and `CONFIG_ANDROID_BINDERFS`
(`scripts/build-pixel-shell.py`). Goodix's HAL is an AIDL service on NDK
binder, registered with `servicemanager`, and calls the display, power and
thermal HALs over binder. `mount -t binder binder /dev/binderfs` gives
`binder`, `hwbinder` and `vndbinder`. Nothing mounts it at boot; the
fingerprint lab does.

**LHBM: the fingerprint circle** (`pixel_panel.h`, `pixel_scanout.c`).
`pixel_scanout.lhbm`: 1 lights it, 0 puts it out.
- **The panel side:** stock's `s6e3hc4_lhbm_extra_cmds` (from Google's
  `panel-samsung-s6e3hc4.c`, `android-gs-pantah-android14-release`) set the
  circle's area, centre (0x2d0, 0x939) and 6 mm radius. Then
  `s6e3hc4_write_display_mode()` adds WRCTRLD's local HBM bit (0x10) to
  brightness control (0x20). Off writes WRCTRLD back to 0x20.
- **Bounds:**
  - It lights only on a lit screen outside AOD.
  - It goes out by itself 3 s after lighting (stock's
    LOCAL_HBM_MAX_TIMEOUT_MS), to spare the panel; asking again while lit does
    not extend that.
  - Screen-off puts it out first, and sleep out and AOD entry rewrite WRCTRLD.
  - A failed put-out fails the pipeline closed, and its recovery resets the
    panel.

**Validated by RAM boot** (`power-image-14`, build ID `beeb4450…`):
- binderfs mounted with its three devices;
- `lhbm` lit and put out by hand, then went out by itself after 3 s,
  ignoring a second request while lit;
- Wi-Fi runtime suspend, the modem's data link, `idle_refresh` 10 and
  `mif_floor_mhz` 1352 as in v35.

**Installed in boot_a on October 2** (pre-approved). Readback matched
`63bd689b…`; the previous image was v35, `e6ad7373…`. After the reboot the
same checks passed.

Built with `scripts/build-pixel-shell.py --persistent-root --seconds 0 --cma
256M`. `kernel.config` matches v35's except for the initramfs path and the
three binder options. `fp-v36.patch` is complete against `kernel-base.txt`
(58 files).
