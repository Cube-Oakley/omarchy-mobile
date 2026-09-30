# Camera power

`pixel-camera-power.c` powers the four cameras the way the stock DT's LWIS
sensor nodes do. It is milestone (a) of the
[camera plan](../../docs/camera-plan-20260930.md): power a sensor and read its
chip ID. Nothing here streams yet. The camera power domains (pd_csis, pd_pdp)
stay off, and the module touches no register inside them.

`cam-id.py` reads a chip ID over i2c-dev. The camera buses come from
`pixel-hsi2c-cam` ([i2c](../i2c/README.md)).

The module replaces the first test module, `pixel-cam-pmic`. That module only
raised the SLG51002's enable lines.

## Hardware

| | Ultrawide | Front | Main | Tele |
| --- | --- | --- | --- | --- |
| sysfs name, stock node | `uw`, sensor-sandworm | `front`, sensor-dokkaebi | `main`, sensor-nagual | `tele`, sensor-kraken |
| Part, chip ID (register) | Sony IMX386, 0x0386 (0x0016) | Samsung 3J1, 0x30A1 (0x0000) | Samsung GN1, 0x08E1 (0x0000) | Samsung GM5, 0x08D5 (0x0000) |
| Bus, address | hsi2c_3, 0x1a | hsi2c_2, 0x10 | hsi2c_1, 0x3d | hsi2c_4, 0x2d |
| Bus pins (SCL/SDA) | gpp6-0/1 | gpp4-0/1 | gpp2-0/1 | gpp8-0/1 |
| Reset (high = run) | gpp6-3 | gpp4-2 | gpp2-2 | gpp6-2 |
| MCLK clock, pad | CIS_CLK0, gpp3-0 | CIS_CLK1, gpp5-0 | CIS_CLK3, gpp9-0 | CIS_CLK2, gpp7-0 |
| Supplies | L12S, SLG LDO6 1.1 V, LDO2 2.85 V | L12S, LDO8 1.1 V, SLG GPIO4 | L12S, LDO4 2.9 V, GPIO3, LDO8 1.1 V, LDO1 2.85 V | L12S, LDO7 1.0 V, LDO3 2.25 V |
| Default | enabled | `experimental=1` | `experimental=1` | `experimental=1` |

The supplies:
- **L12S** is S2MPG13 LDO12S (`L12S_CAMIO`, 1.8 V), the camera IO rail. All
  four sensors share it. The module reaches it through ACPM (PMIC channel 2,
  S2MPG13), as [aoc-power](../aoc-power/README.md) does.
- **The SLG51002** camera PMIC, revision AB (ID 0xB13103), sits at 0x75 on
  hsi2c_8. It provides LDO1-8 and GPIO1-4. LDO8 is shared by the main and
  front sensors. Three S2MPG13 GPIOs hold the chip in reset: GPIO1 (bb),
  GPIO3 (buck) and GPIO0 (cs).

## Power sequences (stock DT)

Each delay runs after its step.

| Camera | Power-up | Power-down |
| --- | --- | --- |
| Ultrawide | L12S → LDO6 → LDO2, 1 ms → MCLK on, 1 ms → reset high, 10 ms | reset low, 1 ms → MCLK off, 1 ms → LDO2 → LDO6 → L12S, 1 ms |
| Front | L12S → LDO8 → GPIO4, 1 ms → MCLK on, 1 ms → reset high, 8 ms | reset low → MCLK off, 1 ms → GPIO4 → LDO8 → L12S, 1 ms |
| Main | L12S → LDO4, 1 ms → GPIO3 → LDO8 → LDO1, 2 ms → reset high, 1 ms → MCLK on, 9 ms | MCLK off, 1 ms → reset low, 1 ms → LDO1 → LDO8 → GPIO3, 6 ms → LDO4 → L12S |
| Tele | L12S, 0.3 ms → LDO7 → LDO3 → MCLK on, 1 ms → reset high, 10 ms | reset low → MCLK off → LDO7, 1 ms → L12S → LDO3 |

Around each sequence the module follows LWIS (`lwis_dev_power_up_locked`,
`lwis_i2c_device_enable`):

| When | What |
| --- | --- |
| Before power-up | SLG51002 out of reset if it is the first sensor, then the CIS clock |
| After power-up | bus pins to `on_i2c`, then 2 ms |
| Before power-down | bus pins back |
| After power-down | CIS clock restored, then the SLG51002 back into reset if it was the last sensor |

## What the module does

| Part | Registers | Action |
| --- | --- | --- |
| L12S | S2MPG13 PM bank 0x01, CTRL 0x37 | Requires the bootloader's 1.8 V (bits 5:0 = 0x2C). Sets or clears bit 7, reads it back, then waits 128 µs (`S2MPG13_ENABLE_TIME_LDO`). If L12S is already on at load, the module leaves it alone. |
| SLG51002 enable lines | S2MPG13 GPIO bank 0x0C, GPIOn_SET 0x05 + n | Require output enable (bit 6) and a low value at load. Raised in Google's probe order: bb, 2 ms; buck, 2 ms; cs, 10 ms. Lowered in Google's remove order: cs, 10 ms; buck, 1 ms; bb, 1 ms. |
| SLG51002 check | PATN_ID 0x1105-0x1107, MATRIX_CONF_A 0x110D, GPIO1-4_CTRL 0x1710-0x1713, FAULT_LOG1 0x1115 | After each release from reset: ID must be 0xB13103 and every output off. |
| SLG51002 LDO voltage | LDOn_VSEL; MINV/MAXV at VSEL + 0x60/+0x61 | The stock DT voltage's selector, written only if it lies inside the chip's MINV-MAXV and differs from VSEL. Read back. |
| SLG51002 LDO enable | MATRIX_CONF_A bit n-1 | Read-modify-write, read back. |
| SLG51002 GPIO output | GPIOn_CTRL bit 0 | Read-modify-write, read back. Rev AB needs no software test mode. |
| SLG51002 status | LDOn_STATUS (VSEL + 0xC1) | Read after power-up. VOUT_OK (bit 1) is expected and ILIM (bit 0) is not; either one off is logged. |
| CIS_CLKn | CMU_TOP mux 0x1010, div 0x1814, gate 0x2038, DFTMUX Q-channel 0x3004 (each + 4n) | Mux 0 (OSCCLK), divider 0: 24.576 MHz, the EXTCLK every stock sensor table assumes. Mux and divider are written only if they differ, and BUSY is polled. The gate is made to pass as Google's `ra_set_gate` does. The Q-channel gets ENABLE 0 and CLOCK_REQ 1, as stock's `GATE_DFTMUX_CMU_CIS_CLKn` enable leaves it. All four are restored at power-down. |
| Reset pin | gppN CON/DAT | Value, then output, as `gpiod_direction_output` does. |
| MCLK pad | gppN CON/DAT/PUD/DRV, pin 0 | On: function 2, no pull (`sensor-mclkN-fn`). Off: driven low, pull-down (`-out`). |
| Bus pins | gppN CON/PUD/DRV, pins 0/1 | Function 3 with pull-up (`hsi2cN-bus`) while the sensor is on. Otherwise as found at load. |

Every pin a sensor uses must be an input at load, as the bootloader leaves
it. Otherwise that sensor is marked unavailable. Its pins are restored as
found when it powers down. The camera bus adapter (`Pixel hsi2c_N`) must be
registered before a sensor powers up. The module holds that adapter while the
sensor is on.

Shared rails are reference counted: L12S, every SLG output, and the SLG51002
itself. A failed power-up undoes exactly the steps it took, in power-down
order. Unloading powers every sensor down.

## Interface

```
/sys/devices/platform/pixel-camera-power/uw/power     0 or 1
/sys/devices/platform/pixel-camera-power/front/power  (experimental=1)
/sys/devices/platform/pixel-camera-power/main/power   (experimental=1)
/sys/devices/platform/pixel-camera-power/tele/power   (experimental=1)
/sys/devices/platform/pixel-camera-power/status
```

`status` shows:
- SLG51002: in reset or on, with MATRIX_CONF_A and FAULT_LOG1;
- the L12S CTRL register and its users;
- each SLG rail's users, with VSEL/range/STATUS while the chip is on;
- each sensor's state, CIS_CLK registers and pin fields.

Parameters:
- `experimental=1` allows powering the front, main and tele sensors.
- `takeover=1` is for a warm reboot with a camera on. The S2MPG13 keeps its
  state across a warm reboot, so the SLG51002 lines stay high and the module
  refuses to load. With `takeover=1`, it lowers them and switches L12S off at
  load.

## Test: ultrawide chip ID

Verified on 2026-09-30: `pixel-hsi2c-cam buses=3` configured hsi2c_3 from
scratch, the ultrawide's stock power-up ran (SLG51002 out of reset, L12S,
SLG LDO6 1.1 V and LDO2 2.85 V, CIS_CLK0 24.576 MHz, reset high), and the
IMX386 answered register 0x0016 with `03 86`. Power-down returned the SLG51002
to reset, L12S to 0x2c and USI3 to unconfigured.

Prerequisites:
- `pixel-hsi2c buses=15,8` is loaded (boot);
- `i2c-dev` is loaded;
- `pixel-cam-pmic` is not loaded (`rmmod pixel_cam_pmic`; its unload lowers
  the lines);
- `wdtkick` is armed and `dmesg -w` is streamed to the host.

```
insmod pixel-hsi2c-cam.ko buses=3
insmod pixel-camera-power.ko
cat /sys/devices/platform/pixel-camera-power/status
echo 1 > /sys/devices/platform/pixel-camera-power/uw/power
python3 cam-id.py uw
cat /sys/devices/platform/pixel-camera-power/status
echo 0 > /sys/devices/platform/pixel-camera-power/uw/power
rmmod pixel_camera_power
rmmod pixel_hsi2c_cam
```

Expected results:

| Step | Expected |
| --- | --- |
| `pixel-hsi2c-cam` | `hsi2c_3 as found: USI 0 mux 0x10 div 0 gates 0x200000/0x200000 qch …`, then `hsi2c_3 configured: …` and `hsi2c_3 at 0x10920000` |
| `pixel-camera-power` | the three lines high, `SLG51002 up: MATRIX_CONF_A 0 …`, the lines low again (cs, buck, bb), then `… LDO12S off at load; sensors: uw` |
| `uw/power` = 1 | lines high, SLG up, `CIS_CLK0 at 24576000 Hz`, `S2MPG13 LDO12S on`, LDO6 and LDO2 VSEL set if needed (0x8C, 0xA5), then `on`, and finally `uw (Sony IMX386) on` |
| `cam-id.py uw` | `… 0x1a register 0x0016: 03 86` and `chip ID 0x0386: as expected` |
| `uw/power` = 0 | LDO2, LDO6 and LDO12S off, lines low, `uw off` |
| `status` afterwards | SLG51002 in reset, LDO12S CTRL 0x2c, every pin and CIS_CLK0 register as at load |

A read-only second check is the module EEPROM:
`cam-id.py --bus "Pixel hsi2c_3" --addr 0x50 --reg 0 --len 16`.

What a failure means:
- **`ENXIO`**: the sensor NACKed. The bus runs, but the sensor is not
  answering. Check `status`: STATUS VOUT_OK, the reset DAT and the MCLK CON.
- **`ETIMEDOUT`**: the bus pins or the controller are not driving the bus.

## Safety

- The module touches only registers named above: CMU_TOP, PERIC0 pins, the
  S2MPG13 through ACPM, and the SLG51002 at 0x75 on hsi2c_8. All of them are
  in always-on or PERIC0 blocks. None is in a camera power domain.
- `cam-id.py` refuses address 0x08 on every bus. `pixel-hsi2c` refuses it on
  hsi2c_8, where it is the ST54J NFC controller and the eSIM.
- The camera buses refuse writes to the module EEPROMs (0x51 on hsi2c_2, 0x50
  on hsi2c_3), except the address bytes ahead of a read.
- **gpp8** holds the tele bus pins and the torch's HWEN (gpp8-2).
  `pixel-torch` has its own lock, so a tele power toggle racing a torch
  toggle can lose one of the two CON updates. The result is the torch off or
  the tele bus dead, not a hang. This is one reason the tele is experimental.
- L12S also feeds the laser AF (`stmvl53l1`) and the OIS/AF devices. Nothing
  else drives them on this kernel.
- Power sensors down before suspending. The module has no suspend handling.
- Registers and sequences come from:
  - Google's gs201 `slg51002-core.c`, `slg51002-regulator.c`, `slg51002.h`;
  - `s2mpg13-regulator.c`, `s2mpg13-register.h`, `s2mpg1x-gpio-gs201.c`;
  - `cmucal-sfr.c`, `ra.c`, `drivers/clk/samsung/composite.c`,
    `pinctrl-gs201.c`;
  - LWIS `lwis_device.c`, `lwis_device_i2c.c`, `lwis_gpio.c`;
  - the stock DT's sensor, `slg51002@75` and pin state nodes.

## Build

```
make -C <kernel> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=<copy of this directory> modules
```
