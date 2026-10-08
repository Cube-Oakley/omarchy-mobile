# V31: the panel's own settings survive screen-off

`display-v31.patch` is a complete patch against the pinned base, as v30's
is. It differs from v30 in `pixel_panel.h` and `pixel_scanout.c`.

**The bug.** The user saw dark themes turn black and the always-on clock
turn bluish after a screen-off; a reboot put it right. The shell drew the
right colours (a screenshot read Tokyo Night's `#1a1b26`), so the panel was
rendering them wrong.

At screen-off the panel sleeps (`panel_sleep`, since September 28). Sleep out
puts the panel's own settings back to their factory defaults, and only the
bootloader had set them. v30's TE fix (B9) was one case. A new debug dump
(`pixel_scanout.panel_regs`) reads every register stock's init and display
sets write:

| Register | After boot | After one screen-off/on (v30) |
|---|---|---|
| VLIN1 (`B1`@0x12) | `08` (7.9 V) | `48` |
| VGH (`F4`@0x0e) | `18` ×5 (7.4 V) | `13` ×5 |
| Black insertion (`BB`) | `0b 04 04 41 3d 0c` | `0b 04 04 45 31 0c` |
| AOD low (`94`@0x52) | `00 07 98 02` | `00 0c 60 02` |
| Toggle (`94`@0x58) | `0c 60 0c 60` | `01 70 01 70` |
| SP (`68`@0x2f3) | `09 09 09 0a 0a 0a` | `0a 0f 0a 0a 0a 0a` |
| TSP sync (`B9`@0x3c, @0x40) | `19 09`, `30 03` | `09 19`, `00 00` |
| `F2`@0x05 | `c8 c0` | `c0 c0` |
| Frame insertion (`BD`@0x10) | `00` | `24` |

VREG, the AMP type, FFC, the NS flicker setting, AOD SAP, `C3` and WRCTRLD
kept their values. VGH and VLIN1 are drive voltages; at their defaults the
darkest greys rendered black.

The panel has slept at screen-off since September 28, yet the user saw the
change only today. v30 now sets the 120 Hz group again after every wake;
before, the panel likely came back in a default mode that hid the wrong
voltages. That explanation is inferred, not measured.

**The fix (`panel_wake`).** It follows stock's `s6e3hc4_enable()` and
`s6e3hc4_init_cmds` after a power-up, without the reset:
- DSC and PPS, then sleep out;
- 10 ms;
- black insertion, toggle, VLIN1 and VGH;
- 110 ms for the voltages;
- TE on;
- the AOD low bytes, SP, TSP sync, `F2`, the frame insertion count and v30's
  TE block;
- `F7 0F`;
- the refresh group.

Each register gets the value read after boot (stock's init values, and the
AOD low bytes as the bootloader left them). The wake still takes about
120 ms in all.

**Validated by RAM boot on October 1** (`power-image-7`, build ID
`41159657…`): all 17 registers read the same after boot, after a
screen-off/on, after AOD in and out, and after a second screen-off/on, with
0 display failures.

Built with `scripts/build-pixel-shell.py --persistent-root --seconds 0 --cma
256M`. `kernel.config` matches v30's except for the initramfs path.

**Installed in boot_a on October 1** with the user's approval, through
`scripts/install-pixel-boot.py` from a RAM boot of the same image (direct
readback matched `4990f937…`; the previous image was v30, `c291657d…`).
Reboot validation:
- the build ID matched;
- `display_power` and `display_off_required` were on, with `suspend=all`;
- the 17 panel registers read identically after boot, after a
  screen-off/on and after AOD in and out;
- 0 display failures.
