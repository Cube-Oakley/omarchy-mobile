# GS201 SPI7: the speaker amplifiers' bus

`pixel-spi.c` is a polled SPI host for PERIC0 USI7 (0x10960000). On this bus
sit the two Cirrus Logic CS35L41 speaker amplifiers:
- `cs35l41@0`, the left amplifier, which drives the top (earpiece) speaker;
- `cs35l41@1`, the right amplifier, prefix `R`, which drives the bottom
  speaker.

The module binds to the stock DT's `spi@10960000` device, and only to it, by
setting that device's `driver_override`. Matching the stock compatible,
`samsung,exynos-spi`, would also take the other SPI controllers, which then
sit in the deferred-probe list. The SPI core creates both amplifiers from the
node's children. Mainline `cs35l41` (built out of tree, see
[audio](../audio/README.md)) then drives them.

What the bootloader leaves:
- USI7 unconfigured (sysreg SW_CONF 0);
- the bus pins as inputs;
- both amplifiers in reset.

What the module sets up:

| Part | Setting |
| --- | --- |
| Clock | USI7's user mux picks OSCCLK (24.576 MHz) or PERIC0_IP (PLL_SHARED2/2, 399.36 MHz), then a 1-16 divider. SCLK is IPCLK/4: the fastest at or under each transfer's speed. Mainline `cs35l41` asks for 4 MHz, which gives OSCCLK/2 = 3.072 MHz. |
| USI | SPI mode (SW_CONF 2), out of reset, clock requested continuously |
| Pins | gpp14-0/1/2 on the SPI function. MISO pulled down; CLK and MOSI at drive 1 (the stock states). |
| Chip selects | gpp14-3 (left) and gpp22-1 (right), GPIO outputs driven by `set_cs` |
| Transfers | polled, 8-bit words, one 64-byte FIFO load at a time |

The controller only shifts while its own slave select is asserted (manual
mode), so each FIFO load asserts it too, as mainline `spi-s3c64xx` does with
`SPI_CONTROLLER_GPIO_SS`. That pin is a GPIO here, so nothing reaches it.

The chip selects are not taken through `cs-gpios`. The stock DT also names
them in each amplifier's `controller-data`, which makes them shared GPIOs to
gpiolib, and its shared lookup resolves only the first entry of a property.
Resets and interrupts come from [pixel-gpio](../gpio/README.md).

While the module is loaded, it adjusts the stock DT with an `of_changeset`:
- **Pin states.** The states of the bus and amplifier nodes get
  `pinctrl-use-default`. There is no pin controller driver to apply them;
  this module and pixel-gpio set those pins.
- **cs-gpios** is dropped.
- **Interrupt output.** The amplifiers get the mainline properties for the
  stock `cirrus,gpio-config2` values: `cirrus,gpio2-src-select = 5`, a
  push-pull active-high interrupt, and `cirrus,gpio2-output-enable`.
- **VA-supply** is dropped, so the amplifiers get a dummy supply instead of
  deferring. VA is S2MPG13 BUCKA, the always-on 1.8 V IO rail, read back on at
  1.85 V; it has no regulator driver here.
- **TDM slots**, from the stock `mixer_paths.xml`, for the AoC card to set
  (the AoC card patch in [aoc](../aoc/README.md)):

  | | `google,tdm-rx-slots` | `google,tdm-tx-slots` |
  | --- | --- | --- |
  | left | `<0 1>` | `<0 2 4 6>` |
  | right | `<1 0>` | `<1 3 5 7>` |

  Playback puts left in slot 0 and right in slot 1. The V/I sense coming back
  is interleaved.
- **Subsystem ID.** `cirrus,subsystem-id = "cheetah"`, so wm_adsp loads each
  amplifier's own protection tuning, the right one with `-r`.

Unloading restores the pins, the USI mode, the clock mux and the divider,
clears the override, and reverts the DT changes.

Validated on 2026-09-29:
- Both amplifiers probe as CS35L41 rev B2, each with its interrupt.
- Tones on either channel reach only that amplifier's speaker, measured with
  the built-in microphones.
- With the whole audio stack loaded, the staged suspend tests (`pm_test`
  devices and platform) pass.

Register layout comes from mainline `spi-s3c64xx.c` (the gs101 variant) and
`exynos-usi.c`. CMU and sysreg offsets come from Google's gs201
`cmucal-sfr.c`.
