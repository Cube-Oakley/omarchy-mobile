# Display v32: correct colours after the always-on clock (2026-10-01)

v31 plus one panel register. After the first time the screen dimmed to the
always-on clock, or woke from deep sleep, the whole picture's tones were off
until the next boot. Dark greys went black, so the dark themes' blue-grey UI
showed as black and the wallpaper's colours shifted.

**Cause.** Register 0x94 at offset 0x52 (B0 global parameter) holds AOD low
mode and three bytes the mode uses. Entering AOD low (10 nit), stock writes
`94 01 07 98 02`. Leaving it, stock writes only `94 00`, so `07 98 02` stays.
Those bytes shift the picture in normal mode too. v31's wake sequence also
wrote `94 00 07 98 02`, taken from stock's AOD sequence.
- Right after boot it reads `00 0c 60 02`, with correct colours.
- After a clock cycle it read `00 07 98 02`, with crushed darks.
- Every other register in `pixel_scanout.panel_regs` matched between the
  two states.
- The SoC's display quality enhancer (DQE, 0x1c2a0000) was bypassed and at
  its reset values in both. The DECON data path was 0xb1 in both.

**Fix (`pixel_panel.h`).** Both the wake and the AOD exit write the full
normal value, `94 00 0c 60 02`.

**Validated on October 1**
- **RAM boot** (`power-image-8`, build ID `7301cdf0…`):
  - 94@0x52 read `00 0c 60 02` at boot;
  - it read the same after four always-on clock cycles through
    `omarchy-mobile-display ambient`/`wake` (`pixel_scanout.ambient` was 1
    during each);
  - it read the same after a screen-off/on;
  - the user saw correct colours.
- **Installed in boot_a** with the user's approval, through
  `scripts/install-pixel-boot.py` from that RAM boot. Direct readback matched
  `5d7e90c9…`; the previous image was v31, `4990f937…`.
- **Reboot validation:**
  - the build ID matched;
  - `display_power`, `display_off_required` and `suspend=all` were on;
  - 94@0x52 read `00 0c 60 02` at boot and after a clock cycle.

Built with `scripts/build-pixel-shell.py --persistent-root --seconds 0 --cma
256M`. `kernel.config` matches v31's except for the initramfs path.
`display-v32.patch` is complete against `kernel-base.txt` (56 files).
