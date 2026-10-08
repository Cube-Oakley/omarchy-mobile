# Display v34: the memory bus at stock's 1352 MHz at 120 Hz (2026-10-02)

v33 plus a lower memory-bus (MIF) floor. Nothing else scales MIF on this
kernel. The scanout's bandwidth floor held the higher of 1352 MHz and the
boot rate, so after a normal boot MIF stayed at the bootloader's 2028 MHz
whenever the screen was on. Stock's `mif_int_map` pairs 120 Hz with 1352
MHz.

**Change (`pixel_bandwidth.h`, `pixel_scanout.c`).** The 120 Hz floor is now
1352 MHz. `pixel_scanout.mif_floor_mhz` (1352 or 2028) changes it at run
time; 2028 is the old behaviour. The release at screen-off (421 MHz) is
unchanged.

**Measured (October 2, `scripts/pixel-screen-power.sh`):** two-minute
coulomb windows on battery, with the 10 Hz idle, a static app drawer and
brightness 184:

| Floor | Round 1 | Round 2 |
| --- | --- | --- |
| 2028 MHz | 486.8 mA | 457.9 mA |
| 1352 MHz | 417.3 mA | 421.9 mA |

About 50 mA saved. At 1352 MHz, five rounds of opening and closing the app
drawer and the shade sent 417 frames with no failures, late frames or DSI
underruns.

**Validated by RAM boot** (`power-image-11`, build ID `95b86c72…`):
- MIF 1352 MHz on, 421 MHz on the always-on clock;
- `BD 23` and 94@0x52 `00 0c 60 02` after two clock cycles and a
  screen-off/on;
- 1147 transfers with 0 failures, late frames, recoveries or underruns.

**Installed in boot_a on October 2** (pre-approved). Direct readback matched
`b4341700…`; the previous image was v33, `4f4ab1c8…`. After the reboot:
- the build ID matched;
- `idle_refresh` was 10 and `mif_floor_mhz` 1352;
- `display_power` was on, with `suspend=all`.

**Tried and dropped (October 2):** releasing MIF to 421 MHz 300 ms after
the last frame, and raising it (about 4 ms) before the next. It worked: idle
at 421, 1352 within 0.1 s of opening the drawer, 530 frames without an
underrun. But it saved nothing measurable:

| Idle MIF | Round 1 | Round 2 |
| --- | --- | --- |
| held at 1352 | 431.9 mA | 416.9 mA |
| 421 after 300 ms | 429.8 mA | 426.4 mA |

The saving was 2028 to 1352 MHz. The code was taken out again.

Built with `scripts/build-pixel-shell.py --persistent-root --seconds 0 --cma
256M`. `kernel.config` matches v33's except for the initramfs path.
`display-v34.patch` is complete against `kernel-base.txt` (56 files).
