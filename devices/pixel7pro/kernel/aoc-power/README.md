# Supplies for the AoC's microphones and sensors

`pixel-aoc-power.c` switches on the PMIC rails that the stock DT gives the
AoC for its peripherals. Nothing on this kernel drives the S2MPG12/S2MPG13
regulators, and the bootloader leaves all five rails off.

| Rail | PMIC register (PM bank) | Stock DT | Feeds |
| --- | --- | --- | --- |
| LDO20M | S2MPG12 0x3F | always on, 1.6-1.95 V | DMIC1 |
| LDO19S | S2MPG13 0x3E | always on, 1.6-1.95 V | DMIC3 |
| LDO20S | S2MPG13 0x3F | always on, 1.6-1.95 V | DMIC4/5 |
| LDO7S | S2MPG13 0x32 | `L7S_SENSORS`, the AoC's `sensor_1v8` | IMU, magnetometers, barometer, light sensors (1.8 V) |
| LDO5S | S2MPG13 0x30 | `L5S_PROX`, the AoC's `sensor_3v3` | proximity (3.3 V) |

With the microphone rails off, the AoC records a short burst and then zeros.
With the sensor rails off, every sensor probe fails with an I/O error on the
AoC's I2C/I3C buses.

The module switches the rails on through ACPM, in the table's order. The
sensor rails go 1.8 V before 3.3 V, as the AoC driver orders
`sensor_power_list`. Per Google's `s2mpg12`/`s2mpg13` regulator drivers:
- the microphone LDOs are enabled by opmode ON (bits 7:6 = 3);
- the sensor LDOs are enabled by bit 7.

The voltage (bits 5:0) is left as the bootloader set it: 1.8 V for all but
LDO5S at 3.3 V. A rail whose voltage is outside the stock DT range is left
alone. The rails stay on when the module unloads.

The AoC probes its sensors when its registry is loaded
([sensors](../../sensors/README.md)), so this module must be loaded first. The
PMIC keeps its state across a warm reboot, so a rail may already be on.
