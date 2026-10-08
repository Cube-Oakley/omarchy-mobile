# USB-C port temperature

**Superseded on October 2 by `../odpm`**, which owns the S2MPG13 meter for the
power readings and these NTC channels together. This module stays as the boot
script's fallback when `pixel-odpm` is missing.

`pixel-port-temp.c` reads the S2MPG13 sub-PMIC's NTC channels through ACPM,
as Google's `s2mpg13_spmic_thermal` does:
- channel 3, `usb_pwr_therm`, at the port;
- channel 4, `usb_pwr_therm2`;
- channel 0, `neutral_therm`, the board.

Stock cuts the port above 42 °C when it is also 2.5 °C over
`usb_pwr_therm2` or 5 °C over `neutral_therm` (the thermal HAL's
`VIRTUAL-USB-THROTTLING`).

**On demand.** The bootloader leaves the NTC engine off: `METER_CTRL3` 0,
the meter off (`METER_CTRL1` 0x0c). The first call to `pixel_port_temp_mc()`
turns the three channels on with stock's sequence:
- pulse the meter reset (`PMETER_MRST`, trim bank 0x0e, register 0x34), clear
  `METER_CTRL3`, then pulse it again;
- set the channels;
- turn the meter on with the NTCs at 0.625 Hz.

60 s without a call puts `METER_CTRL3` and `METER_CTRL1` back. pixel-battery
calls it once a poll while USB power is there, so the engine runs only then.
Reading `parameters/temps` also turns it on, and lists all eight channels.
Do not load `pixel-odpm` at the same time: both drive `METER_CTRL1` and the
meter reset.

**First reading** (October 1, 23:00, on the PC's port at 500 mA): port
32.7 °C, second port sensor 32.9 °C, board 34.0 °C, battery 22.8 °C. The
channels turned off 70 s after the last read.

The boot script (`scripts/pixel-persistent-session.sh`) loads
`/usr/local/lib/omarchy-mobile/pixel-port-temp.ko` after `pixel-usb-switch`.
