# GS201 HSI2C buses

`pixel-hsi2c.c` registers two I2C buses that the bootloader leaves configured
as polled adapters. `buses=15,8` selects them.

| Bus | Controller | Pins | Devices |
| --- | --- | --- | --- |
| `Pixel hsi2c_15` | PERIC1 USI15, 0x10DA0000 | gpp24-0/1 | LM3644 flash (0x63), P9412 wireless charger (0x3C), battery EEPROM (0x50) |
| `Pixel hsi2c_8` | PERIC0 USI8, 0x10970000 | gpp16-0/1 | CS40L26A haptics (0x43), ST54J NFC (0x08) |

The bootloader leaves both buses like hsi2c_13, which the
[battery driver](../battery/README.md) uses:
- USI in I2C mode (sysreg SW_CONF 4);
- pins on their I2C function (2 on gpp24, 3 on gpp16);
- CMU clock mux on the shared PLL, undivided, with the gates on;
- the controller in auto mode with the same 400 kHz timings.

The module checks all of that and refuses any bus that differs. It changes one
setting: it clears the controller's timeout enable, as mainline i2c-exynos5
does. Register offsets come from Google's GS201 `cmucal-sfr.c` and
`pinctrl-gs201.c` and the stock DT's bus nodes.

Two addresses are guarded:
- **Battery EEPROM** (0x50 on hsi2c_15): writes are refused. It must never
  change.
- **NFC controller** (0x08 on hsi2c_8): every transfer is refused. The ST54J
  also holds the eSIM.

Transfers use the battery driver's i2c-exynos5 sequence, with one addition
taken from mainline: the controller is reset after any failure. A NACKed
write leaves its bytes in the TX FIFO, ahead of the next message, and the
CS40L26 NACKs the transfer that wakes it.
