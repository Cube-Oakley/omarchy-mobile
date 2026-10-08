# Fingerprint sensor driver (`/dev/goodix_fp`)

`pixel-goodix-fp.c` gives Goodix's fingerprint HAL (`libgf_hal.so`, the
"Delmar" sensor) the kernel interface it expects. Goodix's trusted app in
Trusty drives the sensor itself over SPI; the kernel side has only the reset
line, the interrupt and the supply. See [the lab notes](../../fingerprint/README.md)
for the trusted app and secure storage.

## Interface (from `libgf_hal.so`'s calls, October 2)

ioctls, magic `'g'`:

| Number | Name | Here |
| --- | --- | --- |
| `_IOR 0, u8` | INIT | returns the netlink protocol (25) |
| 1 | EXIT | accepted |
| 2 | RESET | reset low 3 ms, high, 3 ms |
| 3, 4 | ENABLE_IRQ, DISABLE_IRQ | gate the interrupt |
| `_IOW 5, u32`, 6 | ENABLE_SPI_CLK, DISABLE_SPI_CLK | accepted (the SPI block is the trusted app's) |
| 7, 8 | ENABLE_POWER, DISABLE_POWER | LDO15S on (10 ms settle) or off, with reset held low |
| `_IOR 11, u8` | GET_FW_INFO | 0 |
| 12 | REMOVE | accepted |

Netlink:
- The HAL opens `socket(AF_NETLINK, SOCK_RAW, 25)`, binds it to its pid and
  sends a message; the driver records that port.
- Each interrupt reaches it as a 16-byte payload (`getNetlinkMsgDataLen()`)
  whose first byte is the event: 1, GF_NET_EVENT_IRQ.

## Hardware (stock DT `/odm/goodixfp`, Google's `pinctrl-gs201.c`)

- **Reset:** gpp1-1, PERIC0 (0x10840000) bank 0x20, an output with no pull
  (`goodixfp_pins`).
- **Interrupt:** gpa9-2, ALIVE (0x180d0000) bank 0xc0 with its EINT
  registers at +0x18, on its own GIC line (SPI 36, level high). Set to
  rising edge and unmasked once at load, then gated at the GIC.
- **Supply:** S2MPG13 LDO15S (`L15S_UDFPS_AVDD`, PM register 0x3a). Stock
  keeps it always on, from 2.5 to 3.3 V.

Only this module's pin fields are changed; the banks' other pins belong to
pixel-gpio, pixel-keys and others. The load checks the stock DT node and
both pins' banks first. Unloading leaves the sensor held in reset (line low)
and the pins as found.

The power ioctls switch LDO15S through ACPM: the enable field [7:6] of
L15S_CTRL becomes 3 (stock's mode) or 0, at the voltage found, which must be
within 2.5–3.3 V. That is register 0x3a only, never L14S (the NFC rail), and
the same mechanism pixel-gnss uses for its LDOs. The sensor is powered only
while the fingerprint service wants it. Unloading switches it off again if it
was off at load.

Test parameters:
- `test_power`: on or off; reading shows L15S_CTRL;
- `test_reset`: a pulse;
- `test_irq`: on or off; reading shows both lines;
- `irq_count`.

## State (October 2)

Loaded on the phone, `/dev/goodix_fp` and the netlink socket came up and the
interrupt registered. The bootloader leaves LDO15S off (L15S_CTRL 0x3c, 3.3 V)
and the reset line low, so at first no reset drew an interrupt.

With the user's approval the power ioctls now switch LDO15S. L15S_CTRL read
0xfc (on, 3300 mV). Each reset pulse then drew 16 sensor interrupts within
300 ms, three pulses in a row: the sensor is alive and answers on gpa9-2. It
signals until Goodix's trusted app reads it over SPI.

Next: the HAL with the trusted app loaded (Trusty storage, the lab), then
LHBM on the panel during capture.
