# Per-rail power meters (ODPM) and the USB-C port's temperature

`pixel-odpm.c` reads the on-device power meters in both PMICs (S2MPG12 and
S2MPG13), 12 channels each, through ACPM. Since October 2 it also reads the
S2MPG13 meter's NTC channels for the USB-C port's temperature, which
`pixel-port-temp` did alone. The two share that meter (its CTRL1 and its
reset), so one module now owns it. The boot script loads it from
`/usr/local/lib/omarchy-mobile/pixel-odpm.ko` and falls back to
`pixel-port-temp` without it.

- `parameters/power`: each channel's low-pass filtered power, now.
- `parameters/energy`: each rail's energy (µJ) since the previous read, after
  a `seconds PMIC S` line per PMIC with the time its meter covered. The first
  read takes the baseline. It is read once per battery-history line by the
  sleep policy (`overlay/mobile/sleep.py`), its only reader.
- `parameters/temps` and `pixel_port_temp_mc()`: the port, the second port
  sensor and the board (see `../port-temp/README.md`). Each energy read resets
  the meter, which also clears the NTC samples for about 1.6 s. Within 5 s of
  a reset the function returns the last good reading, so the charger does not
  drop to its unknown-temperature limit.

## What it measures

The channels cover the rails that matter for idle power:
- **Main PMIC:** MIF, the three CPU clusters, INT and INT_M, the LLDO1 and
  LLDO3 feeds, and SLC.
- **Sub PMIC:** camera, GPU, both DRAM rails (VDD2H, VDDQ), the LLDO1/2/3
  feeds, MLDO, and the SA and SD bucks.
- **External (VSYS shunts):** display, modem, mmWave, RF front end and
  WLAN/BT.

Each read returns the low-pass filtered power per channel and a total.

`survey=1`, `2` or `3` at load meters other rails instead (diagnostics):
1 the LDOs left on in SYS_SLEEP with the MLDO, SC, SD and buck-boost feeds;
2 the LLDO feeds and the remaining LDOs, among them L14S_ALIVE; 3 every buck
left on in sleep (the AoC's among them), the largest LDOs and the shunts.
Only the meter's input is selected; no rail is switched.

For energy spanning system suspend, read `parameters/energy`. The first
read configures accumulator power mode and starts a baseline; each following
read latches the sample sums and the sample count, and starts a new
interval. Every interval starts with GS201's meter software
reset (`s2mpg1x_meter_sw_reset`): a pulse on PMETER_MRST, bit 7 of the
MT_TRIM bank's common register (S2MPG12 0x29, S2MPG13 0x34), then METER_EN.
Without it the accumulators read back frozen: the sample count is pinned at
0xfffff and every interval returns the same sums. Neither ASYNC_RD nor
turning the meters off and on clears them. The meters accumulate while the AP sleeps, so
read immediately before and after a bounded sleep to include its transitions.
These are independent intervals, not cumulative counters to subtract.
Use intervals shorter than one hour (the 20-bit count wraps after about
2.3 hours at 125 Hz). Do not mix readers during a measurement.

## How

The meters run at 125 Hz, with LPF power mode on all channels. Registers,
mux codes and resolutions come from Google's `s2mpg1x-meter.h`,
`s2mpg12-powermeter.c`, `s2mpg13-powermeter.c` and `odpm.c`.
- **Internal rails:** resolution per mux code.
- **External rails:** VRAIL × VSHUNT × TRIM × 10 / shunt, using the stock DT's
  shunts (10 mΩ for mmWave, display and WLAN/BT; 5 mΩ for the modem and RF
  front end).

Only the meter bank (0x0A) and PMETER_MRST in the MT_TRIM bank (0x0E) are
written:
- CTRL1 (the meter enables and internal rate; the NTC rate bits are kept);
- CTRL2 (external channel enables and rate);
- MUXSEL0–11;
- BUCKEN1/2 (0x0F, 0x10): a buck channel measures only with its buck's bit
  set (the stock DT's channel-en-byte-offset and channel-en-index; stock's
  odpm.c sets them for every metered buck);
- the LPF mode registers.
- on the first interval read, accumulator mode; each interval read strobes
  CTRL2's ASYNC_RD and waits for the hardware to clear it.

Unloading restores CTRL1, CTRL2, BUCKEN1/2 and, if used, the accumulator
mode.

## Caveats

- **Coverage:** the metered total is about half the USB input. The rest is
  charger and buck conversion loss, plus loads the 24 channels do not cover.
- **Noise:** single readings of INT and the CPU rails swing by tens of mW.
- **Buck readings before October 3 are not power:** the bootloader leaves
  BUCKEN1/2 at 0, and until the module set them every buck channel read a
  constant (the switched-off GPU buck 49 mW). With the bits set the GPU and
  camera bucks read 0 while off, and over a deep sleep the metered rails add
  up to the battery's power (about 190 mW at 44 mA and 4.3 V). The shunt
  channels were never affected. SD still reads a near-constant 20 mW,
  asleep and awake, although its opmode follows PWREN: treat it with care.

## What the meters found in SYS_SLEEP (October 3)

Charger input off, policy paused, two 150 s deep sleeps
per survey set, cable in (USB's L8M-L10M draw about 18 mW only because of
that). Before the fix below, deep sleep read 44 mA:
- **MLDO 80 mW, of which the touch controller's DVDD (L25M) 30 mW.** Awake
  with the screen dark (touch held in reset) L25M reads 0.004 mW. In
  SYS_SLEEP the pins take their power-down states, which the bootloader sets
  to input with pull-down for every PERIC pin; the touch reset and SPI lines
  with it. Stock's states (reset as it was, SPI unpulled, chip select pulled
  up; `kernel/suspend/pixel-sleep-gpio.h`) bring L25M to 0, MLDO to 46 mW and
  deep sleep to 34.7 mA.
- Off as intended: CPU, INT, MIF, GPU, camera bucks (0-0.5 mW).
- Small: the PCIe PHY LDOs (L16M, L18M, L3S, L18S) under 1 mW each, though
  the firmware puts them back to always-on after every sleep; microphones,
  sensors, fingerprint, GSC, GNSS, UWB each 0.0-0.3 mW.
- Remaining: AoC retention (L3M) 10.5 mW, L1M_ALIVE 8, VDD2H 11, the modem
  shunt 12-15, and the LLDO feeds (S6M 21.5, S8M 16, S3S 14-18, S9M 12,
  S6S 10.5), whose loads the LDO channels do not account for.
