# GS201 power and volume keys

`pixel-keys.c` reports `KEY_POWER`, `KEY_VOLUMEDOWN` and `KEY_VOLUMEUP` from
the keys' GPIO pins, using the pins' wake-up external interrupts. The power
key is a system wake-up source. That makes it the key that brings the phone
out of s2idle; the polled [power key](../powerkey/README.md) cannot wake a
suspended CPU.

| Key | Pin | Pin controller | GIC SPI |
| --- | --- | --- | --- |
| Power | gpa10-0 (`PMIC_PWRON_OD_L`, the S2MPG12's open-drain copy) | ALIVE, 0x180D0000 | 42 |
| Volume down | gpa10-1 | ALIVE, 0x180D0000 | 43 |
| Volume up | gpa8-5 | FAR_ALIVE, 0x180E0000 | 61 |

All three are active low on the always-on pin controllers. Each pin has its
own interrupt, taken from the stock DT's per-pin `gpa10`/`gpa8` interrupt
lists. The register layout (CON/DAT at the bank, then ECON 0x700, FLTCON
0x800, EMASK 0x900 and EPEND 0xa00 plus the bank's EINT offset) is from
Google's `pinctrl-gs201.c`.

Each pin becomes an EINT on both edges, with the digital filter on. An
interrupt starts a 15 ms debounce, and then the pin level is reported. The
module takes a pin only in the state the bootloader hands it over in (an
input with its EINT masked). Otherwise it refuses with -EBUSY and the boot
script falls back to `pixel-powerkey`. Unloading restores each pin's
function, trigger, filter and mask.

Validated on image A (2026-09-29):
- Presses and releases of all three keys arrived as clean down/up pairs.
- The power key toggled the screen through the shell as before.
- One press woke the phone from a 23 s s2idle, and the screen came back on.

The volume overlay appears but the level does not move: the Pixel has no
audio driver yet.

Build it externally like the other modules, against the configured kernel,
with `make -C <kernel> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
M=<output> modules`.
