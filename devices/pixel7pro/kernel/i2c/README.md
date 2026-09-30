# GS201 HSI2C buses

`pixel-hsi2c.c` is a polled HSI2C adapter driver. It builds two modules:

| Module | Buses | How |
| --- | --- | --- |
| `pixel-hsi2c.ko` | 15, 8 (`buses=15,8`) | adopts buses the bootloader leaves configured |
| `pixel-hsi2c-cam.ko` | camera buses 1-4 (`buses=3` by default) | configures them from scratch |

`pixel-hsi2c-cam.c` is three lines: it defines `PIXEL_HSI2C_CAM` and includes
`pixel-hsi2c.c`, which then uses its camera bus table and the driver name
`pixel-hsi2c-cam`. It is a module of its own so that the camera buses load and
unload beside the running `pixel-hsi2c`, whose buses serve the torch, the
haptics and the camera PMIC. Both register adapters named `Pixel hsi2c_N`.

## Adopted buses

| Bus | Controller | Pins | Devices |
| --- | --- | --- | --- |
| `Pixel hsi2c_15` | PERIC1 USI15, 0x10DA0000 | gpp24-0/1 | LM3644 flash (0x63), P9412 wireless charger (0x3C), battery EEPROM (0x50) |
| `Pixel hsi2c_8` | PERIC0 USI8, 0x10970000 | gpp16-0/1 | CS40L26A haptics (0x43), SLG51002 camera PMIC (0x75), ST54J NFC (0x08) |

The bootloader leaves both buses like hsi2c_13, which the
[battery driver](../battery/README.md) uses:
- USI in I2C mode (sysreg SW_CONF 4);
- pins on their I2C function (2 on gpp24, 3 on gpp16);
- CMU clock mux on the shared PLL, with the gates on;
- the controller in auto mode with the same 400 kHz timings.

The module checks all of that and refuses any bus that differs. It changes one
setting: it clears the controller's timeout enable, as mainline i2c-exynos5
does.

## Camera buses, from scratch

| Bus | Controller | SW_CONF | CMU_PERIC0 mux / div / gates / Q-channel | Pins | Devices (stock DT) |
| --- | --- | --- | --- | --- | --- |
| `Pixel hsi2c_1` | USI1, 0x10900000 | 0x10821000 | 0x650 / 0x1810 / 0x2068, 0x206c / 0x3074 | gpp2-0/1 | main GN1 (0x3d), AF (0x0c), OIS (0x24), laser AF (0x29) |
| `Pixel hsi2c_2` | USI2, 0x10910000 | 0x10821004 | 0x660 / 0x1814 / 0x2070, 0x2074 / 0x3078 | gpp4-0/1 | front 3J1 (0x10), EEPROM (0x51) |
| `Pixel hsi2c_3` | USI3, 0x10920000 | 0x10821008 | 0x670 / 0x1818 / 0x2078, 0x207c / 0x307c | gpp6-0/1 | ultrawide IMX386 (0x1a), AF (0x0f), EEPROM (0x50) |
| `Pixel hsi2c_4` | USI4, 0x10930000 | 0x1082100c | 0x680 / 0x181c / 0x2080, 0x2084 / 0x3080 | gpp8-0/1 | tele GM5 (0x2d), AF/OIS/EEPROM (0x34) |

The bootloader leaves USI1-4 unconfigured (SW_CONF 0). For each bus the module:

1. Reads the reference, hsi2c_8. It must be as `pixel-hsi2c` adopts it: USI
   in I2C mode, user mux on the shared PLL, gates and Q-channel on, controller
   in master auto mode with timings set. The module takes its user mux
   select, divider, CONF and TIMING_FS1/FS2/FS3/SLA. It reads them twice and
   refuses if they differ, since `pixel-hsi2c` resets hsi2c_8 after a failed
   transfer.
2. Requires the camera USI unconfigured (SW_CONF 0) and its gates and
   Q-channel on (Q-channel enabled or clock requested, as the battery driver
   checks). The controller is not touched otherwise.
3. Gives the USI hsi2c_8's user mux select and divider, polling each CMU
   register's BUSY bit. hsi2c_8's timings then give the same 400 kHz.
4. Puts the USI in I2C mode (SW_CONF 4), takes it out of reset (USI_CON
   +0xC4 bit 0) and has it request its clock continuously (USI_OPTION +0xC8:
   CLKREQ_ON set, CLKSTOP_ON clear), in [pixel-spi](../spi/README.md)'s order.
5. Initialises the controller as Google's i2c-exynos5 `exynos5_i2c_reset` and
   `exynos5_i2c_init` do: soft reset, master, trailing count 0xFFFFFF, auto
   mode. It then applies hsi2c_8's CONF and timings and clears the timeout
   enable. It reads the setup back.
6. Applies the stock DT's `samsung,no_lose_arbitration` (CTL bit 22) and
   `samsung,reset-before-trans` (a controller reset before each transfer).

The module does not touch the bus pins. On stock, LWIS switches them from
`hsi2cN-bus-in` (input) to `on_i2c` (function 3, pull-up) only while a device
on the bus is powered. That keeps the pull-ups from feeding an unpowered
sensor. [pixel-camera-power](../camera/README.md) does the same with each
sensor's power. Until a sensor is on, transfers time out after 100 ms and the
controller is reset.

Unloading puts each USI back in reset and restores its USI_OPTION, SW_CONF,
divider and mux. `pixel-camera-power` holds a camera bus's adapter while that
sensor is on, so the module cannot be unloaded under a powered sensor.

## Guarded addresses

- **Battery EEPROM** (0x50 on hsi2c_15): writes are refused. It must never
  change. A one-byte address write ahead of a read is allowed.
- **Camera module EEPROMs** (0x51 on hsi2c_2, 0x50 on hsi2c_3): writes are
  refused except the two address bytes ahead of a read.
- **NFC controller** (0x08 on hsi2c_8): every transfer is refused. The ST54J
  also holds the eSIM.

## Transfers

Transfers use the battery driver's i2c-exynos5 sequence, with one addition
taken from mainline: the controller is reset after any failure. A NACKed
write leaves its bytes in the TX FIFO, ahead of the next message, and the
CS40L26 NACKs the transfer that wakes it.

Register offsets come from Google's GS201 `cmucal-sfr.c` and
`pinctrl-gs201.c`, the stock DT's bus nodes (`reg`, and `samsung,usi-offset`
into sysreg_peric0 at 0x10821000), and Google's `i2c-exynos5.c`.

## Build

Build both modules out of tree:

```
make -C <kernel> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=<copy of this directory> modules
```
