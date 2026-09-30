# GS201 GPIO lines and wake-up interrupts

`pixel-gpio.c` lets drivers bound from the stock device tree use a few pins.
This kernel has no GS201 pin controller driver, so DT lookups of
`reset-gpios`, `cs-gpios` or `interrupts` on the stock pin banks defer
forever. The module registers a GPIO chip or an interrupt domain on individual
bank nodes, and exposes only the lines it lists. Every other line in those
banks stays invalid and untouched.

| Bank | Controller | Lines | Use |
| --- | --- | --- | --- |
| gpp17 | PERIC0, 0x10840000 | 1 (GPIO) | left CS35L41 reset |
| gpp25 | PERIC1, 0x10C40000 | 2 (GPIO) | right CS35L41 reset |
| gpa6 | FAR_ALIVE, 0x180E0000 | 3 (interrupt) | left CS35L41 interrupt |
| gpa8 | FAR_ALIVE, 0x180E0000 | 6 (interrupt) | right CS35L41 interrupt |

GPIO lines are plain inputs or outputs: CON function 0 or 1, and DAT.

Wake-up EINT pins become interrupts, each chained from its own GIC line, taken
from the bank node's per-pin `interrupts` list. When a pin is requested, it is
switched to its EINT function (0xf). Amplifier pulls are disabled as in the
stock pinctrl states; modem input pulls are preserved. Freeing the interrupt
restores both. IRQ users pin the module and lock GPIO inputs against output
use. GPIO-to-IRQ lookup shares the existing per-bank domain. Probe failures
and module removal dispose mappings and parent handlers before unmapping
registers. Bind/unbind attributes are suppressed while consumers own pins.

The irqchip follows the Samsung EINT model:
- ECON sets the trigger;
- EMASK masks the pin;
- EPEND is write-one-to-clear;
- a level interrupt is acked before it is unmasked, so a level that has gone
  does not fire.

The stock DT's interrupt specifier is `<pin type 0>`. gpiolib's own domain
would read three cells as `<instance hwirq type>`, so the banks get their own
domain instead of a gpiolib irqchip.

Register layout is from Google's `pinctrl-gs201.c`:
- CON, DAT and PUD at the bank offset (+0, +4, +8);
- ECON at 0x700, EMASK at 0x900 and EPEND at 0xa00, each plus the bank's
  EINT offset.

The chip selects of the amplifiers' SPI bus are not here. The stock DT names
them twice (`cs-gpios` and each amplifier's `controller-data`), so gpiolib
treats them as shared GPIOs, and its shared lookup resolves only the first
entry of a property. `pixel-spi` drives them itself.

Build it externally like the other modules, with `make -C <kernel>
ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=<output> modules`.

## Optional modem pins (manual tests only)

The default remains audio-only. Explicit `modem=1` additionally exposes:

| Bank | GPIO lines | Interrupts |
| --- | --- | --- |
| gph0 | 2, 3 | none |
| gph1 | 1, 2, 3, 6 | none |
| gpa0 | 2, 3 | none |
| gpa8 | 2, 3, 4 | 3 (CP wake), 4 (phone active) |

The HSI1 banks have 6 and 7 pins respectively. Layout is verified against
Google's pinned gs201 source (hash in [modem/sources.json](../modem/sources.json)).
PCIe gph0-0/1, AoC gph1-0 and speaker gpa8-6 retain their separate owners.

On 2026-09-29, both CS35L41 drivers unbound/rebound successfully with this
module, and the [CP GPIO probe](../modem/cp-gpio-probe.c) passed repeated
power cycles with one high and one low wake interrupt each. The wake API
propagates to the parent GIC interrupt; system suspend wake is untested.
Phone-active was mapped/requested successfully but has no transition until
firmware boots.

**Preserve CP wake's bootloader pull-up.** Applying the stock Android DT's
pull-down made the ROM-stage signal read zero and suppressed all interrupts.
Keeping the existing pull-up fixed both GPIO readback and interrupt delivery.
Do not apply Android's later pin state during this ROM bring-up stage.

For manual replacement, unbind both amplifier devices from `cs35l41`, unload
`pixel_gpio`, load `/root/tools/pixel-gpio.ko modem=1`, then rebind the amps.
Do not replace it while cpif owns pins. Arm the watchdog and stream diagnostics
as described in the modem README. Reboot afterward to restore normal startup;
no boot script enables the modem option.
