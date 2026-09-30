# GS201 GNSS: the BCM4776 on SPI5 (pixel-gnss)

`pixel-gnss.ko` drives the Pixel 7 Pro's GNSS receiver, a Broadcom BCM4776
(BCM4775x family), for the stock Broadcom daemons (`lhd`, `gpsd`). It is
Google's GPL "bbd" driver with its platform plumbing replaced:

| File | What |
| --- | --- |
| `bcm_gps_spi.c`, `bcm_gps_spi.h` | Google's SSI driver: the stream framing, the MCU_REQ/MCU_RESP handshake, the rx/tx worker, the ring buffers, `/dev/ttyBCM`, the `nstandby` and `sspmcureq` attributes |
| `bbd.c`, `bbd.h` | Google's bridge: `/dev/bbd_control`, `/dev/bbd_sensor`, `/dev/bbd_patch`, `/dev/bbd_pwrstat`, the control strings, the 1 Hz statistics |
| `bcm_gps_regs.c` | Google's direct and indirect chip-register access over SSI |
| `pixel-gnss-hw.c`, `pixel-gnss-hw.h` | New: the GS201 layer (supplies, lines, host_req interrupt, a polled SPI host) |

Google's changes are marked `pixel-gnss:` in the source and listed below.
It replaces `pixel-gnss-power.c`, whose rail switching is now part of
`pixel-gnss-hw.c`.

## Provenance

Google's gs201 kernel, `drivers/misc/bbdpl/`, from the LineageOS mirror at
commit `40ff93424549ffebfbba32e9435dfc58c40decb2`
(`https://raw.githubusercontent.com/LineageOS/android_kernel_google_gs201/40ff93424549ffebfbba32e9435dfc58c40decb2/drivers/misc/bbdpl/`).
The files are GPL-2.0, Copyright 2014-2015 Broadcom Corporation; their headers
are kept. The stock kernel loads the same driver as `bcm47765.ko`. SHA-256 of
the originals:

| File | SHA-256 |
| --- | --- |
| `bbd.c` | `0052ec84d2197ba93264f11ac717094a7c43a0b6182a7bde1e0158f5846f573c` |
| `bbd.h` | `f22c61038e0108701091a2174823f4b35057d9a94411ee00f65b3324f65bd734` |
| `bcm_gps_spi.c` | `1712de49d3126d6132de4dd375fe6a42cdfd7b5906b2ac02a82977dcbe76b873` |
| `bcm_gps_spi.h` | `24a2e98d4428bc639a5dda2ce68267eaf83918dee8f130aa8e81ba5ecaecfebd` |
| `bcm_gps_regs.c` | `83ace10ac4a6d16ae0524107942166016ad056c0a255918db743a76e9ea5484f` |

Register sources for the new layer: `pixel-spi.c` (the same SPI controller,
USI7), `pixel-gpio.c` (pin banks and wake-up EINTs), `pixel-pcie.c` (gph2),
Google's gs201 `cmucal-sfr.c` (CMU and sysreg offsets) and
`s2mpg13-register.h` (LDOs). The pin map is checked at probe against the
stock DT node `/spi@10940000/bcm4775@0`.

## Hardware

| Part | Where | Setting |
| --- | --- | --- |
| Supplies | S2MPG13 LDO9S (0x34, core, 1.20 V), LDO10S (0x35, RF, 1.80 V), LDO11S (0x36, aux, 1.80 V); ACPM PMIC channel 2, bank 1, PMIC 1 | Enable bit 7, switched on at probe in that order, about 1 ms apart, at the bootloader's voltage. The bootloader leaves them off; the stock DT keeps them always on. |
| nstandby | gph2-3 (HSI2 pinctrl 0x14440000, bank 0x0) | Output, low (standby) at probe. High runs the chip. |
| mcu_req | gpp4-3 (PERIC0 pinctrl 0x10840000, bank 0x80) | Output, low at probe. |
| mcu_resp | gph2-2 | Input, pull-down. |
| host_req | gpa6-4 (ALIVE pinctrl 0x180e0000, bank 0x0) | EINT function (0xf), no pull; EINT level high. Its own GIC line, SPI 48 (hwirq 80). |
| SPI | PERIC0 USI5, controller 0x10940000; sysreg PERIC0 0x10821000 SW_CONF +0x10 | SPI mode (SW_CONF 2). SCLK 24.96 MHz (PERIC0_IP 399.36 MHz, divider 4, IPCLK/4); mode 3; feedback delay 1 (stock `controller-data`); 64-byte FIFO. |
| SPI clock | CMU_PERIC0 0x10800000: user mux 0x690, divider 0x1820, IPCLK gate 0x2088, PCLK gate 0x208c | Gates checked on (auto); mux and divider set at probe. |
| SPI pins | gpp10-0/1/2 (bank 0x140): SCLK, MOSI, MISO | Function 3, no pull, drive 0 (stock). |
| Chip select | gpp10-3 | A GPIO output (stock uses the controller's slave select, function 3), high when idle, low for a whole transfer. |

The stock DT flags `nstandby-gpios` active-low, but Google's driver ignored
GPIO flags. The `nstandby` attribute is the raw line level, as on stock.

### SPI transport

`bcm_spi_sync()` is the only change to the protocol path: each call is one
transfer on the polled USI5 host (`pixel_gnss_spi_xfer()`), serialised by a
mutex. It runs from the rx/tx worker (or debugfs), both of which may sleep.
The chip select goes low, the data moves one 64-byte FIFO load at a time with
the controller's own slave select asserted per load (it only shifts then),
and the chip select goes high again. Frames are up to 8 KiB (about 3 ms at
24.96 MHz). Each load is busy-polled (about 20 us) and the worker yields
between loads, with the chip select held.

All transfers use 8-bit words. Google's host used 32-bit words for transfers
of 64 bytes and more, with the DT's `swap-mode = <1>`. spi-s3c64xx shifts
each FIFO word most significant bit first, so a 32-bit word loaded from a
little-endian buffer would put byte 3 on the wire first; the swap (TX/RX
byte and halfword) puts the bytes back in memory order. Mainline
spi-s3c64xx never swaps (`SWAP_CFG` 0). 8-bit words send every byte in
buffer order, MSB first, which is the same wire stream, so `SWAP_CFG` stays
0. Google's callers still round large frames to 4 bytes, so the chip sees
the frame lengths it saw on stock. `HS_EN` stays off: mainline sets it only
at 30 MHz and up without CPHA.

### host_req interrupt

Each gpa6 pin has its own GIC line in the stock `gpa6` node. Pin 4's is
mapped with `irq_of_parse_and_map()` and requested directly, level high,
`IRQF_NO_AUTOEN`, as stock requested it. `pixel-gpio.ko` owns pin 3's EINT
in the same bank, so the EINT registers are written only at probe and
remove:
- ECON nibble 4 is set to level high (1).
- The EMASK bit is cleared (unmasked).
- The EPEND bit is written 1 to clear.

From then on the driver gates only the GIC line, as Google's did
(`enable_irq()`, `disable_irq_nosync()`). The line follows the latched pend
bit, as `pixel-gpio`'s handling assumes, so the pend bit is cleared:
- in the handler, before it checks host_req. A level still high latches
  again, and the core masks the line once the handler has disabled it.
- before every enable (open, end of the worker, resume), so a level that
  has gone does not fire. `pixel-gpio` acks level EINTs before unmasking
  for the same reason.

### Handoff checks and restore

Probe refuses to load, with the reason in the log, if:
- the stock DT node is missing, or differs: compatible, chip select 0,
  mode 3, the four lines' banks and pins, the bus's FIFO size, or its USI
  offset;
- the clock chain differs from the one pixel-spi checks (PLL_SHARED2
  798.72 MHz, PERIC0_IP = PLL_SHARED2/2), or a USI5 gate is off;
- USI5 is in a mode other than unconfigured or SPI;
- a pin is on a function other than input or its own;
- a supply is outside its stock DT range.

The SPI block is touched only after USI5 is in SPI mode, and only
vendor-defined registers are used. Everything probe changes is saved and
restored after remove, in this order:
1. The EINT (mask, ECON, pin, EMASK).
2. The bus pins and the chip select.
3. USI5 (reset, SW_CONF), then the mux and the divider.
4. The chip to standby, then the rails it switched on, switched off in
   reverse order.
5. The line pins.

A rail that was already on is left on. System shutdown does the same, so a
warm reboot finds the rails off, as after a cold boot.

Shared registers: gph2 pins 0, 1, 4 and 5 belong to `pixel-pcie.ko` (Wi-Fi),
and gpa6 pin 3's EINT to `pixel-gpio.ko`. This module read-modify-writes only
its own fields and bits, under its own lock. A write here can still race
theirs to the same register. That can happen at probe and remove, and when
`nstandby` changes (gph2 DAT).

## Interfaces

| Path | What |
| --- | --- |
| `/dev/ttyBCM` | SSI packet stream (misc device), one opener. Opening arms the interrupt. |
| `/dev/bbd_control` | lhd's control strings in; 1 Hz `BBD:` statistics out |
| `/dev/bbd_sensor` | Sensor-hub packets (unused without SSP; lhd.conf has it commented out) |
| `/dev/bbd_patch` | Embedded patch: empty, as on stock; lhd reads `LhePatch` |
| `/dev/bbd_pwrstat` | GNSS on/off counters and durations (from `GPSD:CORE_ON/OFF`) |
| `/sys/devices/platform/pixel-gnss/nstandby` | Raw nstandby level; 1 runs the chip, 0 is standby |
| `/sys/devices/platform/pixel-gnss/sspmcureq` | Raw mcu_req level |
| `/sys/class/bbd/`, `/sys/devices/platform/pixel-gnss/misc/ttyBCM` | The char devices, under the platform device |
| `/sys/kernel/debug/pixel-gnss/pins` | Line levels and functions, the host_req EINT state, USI5, the three LDOs, the interrupt state |
| `/sys/kernel/debug/pixel-gnss/ssi` | Bring-up probe; needs `/dev/ttyBCM` closed. It does the MCU_REQ/MCU_RESP handshake, one SSI status read (`70 00 00`, the first transfer of `bcm_ssi_rx()`), and the HSI_STATUS (0x30) and HSI_ERROR_STATUS (0x2c) direct registers, then drops MCU_REQ. |

Module parameters:

| Parameter | Default | Meaning |
| --- | --- | --- |
| `spi_hz` | 24960000 | SCLK ceiling; the fastest IPCLK/4 at or under it (capped at the DT's 26 MHz) |
| `fb_delay` | -1 | RX feedback delay 0-3; -1 takes the DT's `samsung,spi-feedback-delay` (1) |
| `debug` | 0 | Hex-dump the first N bytes (at most 64) of each SPI transfer, both ways (writable at runtime) |

lhd's `SSI:DEBUG=1` on `/dev/bbd_control` still dumps whole transfers, as on
stock. `BBD:DEBUG=1` dumps the bbd devices' traffic.

## lhd and gpsd

On stock, `lhd` opens `/dev/ttyBCM` and `/dev/bbd_control`, raises
`nstandby` through sysfs, and writes `BBD:PassThru=1`. It then downloads
`SensorHub.patch` over `/dev/ttyBCM`, and about 0.8 s later writes
`BBD:PassThru=0` and `ESW:READY`. `gpsd` talks to lhd; it doesn't talk to
the kernel. `PassThru` needs nothing from the kernel: Google's `bbd_control()`
passes strings it doesn't know to the SSP sensor hub, and there is no SSP
here, so it only logs them, as stock did.

Changes to the stock configs:
- `lhd.conf`: `GpioNStdbyPath=/sys/devices/platform/pixel-gnss/nstandby`
  (stock: `/sys/devices/platform/10940000.spi/spi_master/spi5/spi5.0/nstandby`).
  Keep `LheBbdPacket=/dev/ttyBCM` and `LheBbdControl=/dev/bbd_control`.
- `gps.xml`: the same `GpioNStdbyPath`. `PortName=/dev/ttyBCM` is unchanged.
- PPS: `lhd.conf` `PpsEnable=true`, `gps.xml` `PpsDevice` and `init.gps.rc`
  (`/sys/devices/platform/bbd_pps/pps_assert`) expect Google's separate
  `bbd_pps` driver, which is not ported. Set `PpsEnable=false`, or expect
  errors.

On stock, lhd runs as user `gps`. The device nodes here are root-only. To run
it as another user, a udev rule is needed (for example `KERNEL=="ttyBCM|bbd_*",
GROUP="gps", MODE="0660"`), and a chown of the two sysfs attributes.

## Changes from Google's driver

Platform plumbing (the point of the port):
- `bcm_spi_sync()` uses the polled USI5 host (above) instead of `spi_sync()`.
  `bits_per_word` is ignored. That also neutralises `bcm_dreg_*()` passing
  `size + 3` as `bits_per_word`, a bug in Google's code.
- GPIO numbers become line indices. `gpio_get_value()` and
  `gpio_set_value()` become direct DAT accesses (`bcm_gpio_get()`,
  `bcm_gpio_set()`), and `gpio_is_valid()` becomes a range check. Only
  nstandby and mcu_req can be driven.
- `of_get_named_gpio()`, `gpio_request()`, `gpio_direction_*()` and
  `gpio_to_irq()` are replaced by `pixel_gnss_hw_init()`, which also switches
  on the supplies (stock: always-on regulators). The optional
  `gps-power-enable` GPIO is dropped; the DT has none.
- pinctrl is dropped. The DT defines only the `default` state, which
  `pixel-gnss-hw.c` sets. Google's code looked up `gps_active` and
  `gps_suspend`, which don't exist: stock logs "Can not get ts default
  pinstate".
- The interrupt clears the EINT pend bit in the handler and before each
  enable (above).
- The `spi_driver` becomes a platform driver and a `pixel-gnss` platform
  device. The attributes live on it, and `/dev/ttyBCM` and the bbd devices
  are its children (bbd's had no parent).

Fixes needed to load, unload and reload safely:
- `/dev/ttyBCM` is registered last, after the buffers, the locks, bbd and the
  interrupt. Google's registered it first, so an early `open()` could use
  them uninitialised.
- `remove` deregisters `/dev/ttyBCM`; Google's left it registered.
- `remove` and `shutdown` wait for a running handler before destroying the
  workqueue.
- `bbd_exit()` takes the `bbd_device`. Google's read it from the SPI device's
  driver data, which holds the `bcm_spi_priv`.
- The chrdev region is freed whole. Google's unregistered single minors of a
  five-minor region, which matches nothing, so every load leaked a major.
- `bbd_exit()` unregisters the PM notifier. Google's left it registered, so
  the next suspend after unload would call into freed code.
- `device_create()` failures are checked on the result; Google's checked the
  parent pointer.
- The error paths unwind what probe set up.

Other behaviour fixes:
- `BBD:DEBUG=1` logged `/dev/bbd_*` reads by hex-dumping the user buffer
  from the kernel (a fault with PAN); it now dumps the ring data copied.
- Control strings are NUL-terminated before `strstr()`: lhd writes them
  without one, and the buffer is reused.
- The 1 Hz statistics timer restarts after resume only if it was running.
  Google's started it after every resume, so an idle phone woke every second
  after its first suspend.
- Resume re-arms the interrupt only while `/dev/ttyBCM` is open. Google's
  armed it on any resume.
- `bcm_spi_priv` is vmalloc-backed (`kvzalloc`): its rings are 128 KiB.
- Shutdown also puts the chip in standby and switches off the rails it
  switched on.

Modernisation for 7.3:
- `class_create(name)`, `timer_delete_sync()`, `__poll_t`/`EPOLL*`,
  `file_inode()`, `sysfs_emit()`;
- `DEFINE_SIMPLE_DEV_PM_OPS()`, a `void` `remove`;
- helpers without prototypes made `static`, and no exports (one module);
- removed headers (`of_gpio.h`, `spidev.h`, pinctrl).

Left out:
- The Samsung SSP sensor-hub glue (`CONFIG_SENSORS_SSP`) and the legacy
  embedded patch (`CONFIG_SENSORS_BBD_LEGACY_PATCH`), both never set on Pixel.
  `CONFIG_REG_IO` and `CONFIG_TRANSFER_STAT` stay off, as in Google's build;
  DEBUG_1HZ_STAT, CONFIG_MCU_WAKEUP and BBD_PWR_STATUS stay on.
- DMA: every transfer is PIO.
- `bbd_pps` (PPS), a separate Google driver.
- `/sys/bbd/lk_enable`, which lhd's strings name; Google's bbd has no such
  file either.

New: the `debug` parameter and the debugfs files.

## Open questions

- **EINT semantics.** Two assumptions, from `pixel-gpio`'s model:
  - EPEND is write-1-to-clear, and host_req's GIC line follows EPEND & ~EMASK.
  - A level EINT re-latches while the level holds.

  If the GIC line followed the pad instead, clearing is a harmless no-op.
  Check with `/proc/interrupts`: one host_req-high event should add a few
  counts, not thousands.
- **host_req pull.** It is none, as stock. If the chip leaves host_req
  floating in standby while `/dev/ttyBCM` is open, spurious interrupts run
  the worker. Its handshake then times out (about 150 ms) and leaves the
  interrupt disabled until the next write (Google's behaviour).
- **Signal timing at 24.96 MHz.** Feedback delay 1 and no `HS_EN` are
  untested with this chip. If received bytes look shifted or all 0xff, try
  `fb_delay=0/2/3` or a lower `spi_hz` (12480000, 6144000).
- **Chip-select gaps.** The chip select stays low across the 64-byte loads
  with small SCLK gaps between them (larger if the worker is preempted). Stock
  DMA streamed without gaps.
- **Sleep.** USI5, pin and EINT state across s2idle is assumed retained
  (pixel-spi assumes the same). SICD or deeper states may need a resume
  hook. The pins' power-down fields (CONPDN, PUDPDN) are not set.
- **Standby power.** The rails stay on while the module is loaded, as on
  stock. The chip's standby draw is unmeasured.

## Build

Like the other modules, out of tree:

```
make -C <kernel> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=<copy of this directory> W=1 modules
```

It builds without warnings against the 7.3-rc2 tree in `mainline/linux`.
