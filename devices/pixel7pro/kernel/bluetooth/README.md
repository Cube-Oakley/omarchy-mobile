# Bluetooth

The BCM4389's Bluetooth core runs Google's patch firmware over UART18 at
3 Mbaud, under mainline `hci_uart`/`btbcm` and BlueZ:

```
Bluetooth: hci0: BCM4389C1 ES1PX_GG_C10  FW:2c4fc4e15c CFG:dc5f4d77d4 [Baseline: 0391]
Bluetooth: hci0: BCM (003.002.009) build 0203
```

An LE scan found 24 devices nearby. The controller's address is its own
(locally administered, stable across power cycles); no address is set.

## Power and UART

`pixel-bt.c` sets up everything below and reverses it on unload. It checks the
bootloader's hand-off first: Bluetooth off, USI18 unconfigured and in reset
with its clocks running, and its pins as inputs.

1. **Power.** BT_REG_ON (gpp16-2) goes high and device wake (gpp16-3) is held
   asserted. These are the stock `nitrous` node's shutdown and
   device-wakeup GPIOs. It then waits 100 ms, as the vendor stack does.
2. **UART18.** 0x181B0000 is CMU_APM's USI1_UART. The module sets the USI to
   UART mode (sysreg_apm + 4 = 1), releases its reset as exynos-usi does, sets
   `USI_OPTION` CLKREQ_ON (without it, the USI's auto clock gating stops the
   clock while idle and received bytes are lost), and switches gpa3-0 to
   gpa3-3 (TX, RX, CTS, RTS) to function 2.
3. **Overlay.** `pixel-bt-overlay.dts` adds a new UART node for mainline
   `samsung_tty` (`google,gs101-uart`, 197 MHz fixed clock, GIC SPI 69) with a
   `brcm,bcm4389-bt` serdev child. The stock node can't be used: it names pin
   states on a pin controller that has no mainline driver. With `serdev=0` the
   overlay leaves out the child, so the port is a plain tty for testing.

The UART clock is APM_FUNC, which the bootloader leaves on PLL_ALV/2
(394 MHz), divided by DIV_CLK_APM_USI1_UART (ratio 2): 197 MHz. At 3 Mbaud
the divisor is 3 + 1/16 (3.03 Mbaud).

## Kernel changes

In the kernel tree (`drivers/bluetooth`):
- **`btbcm`: unacknowledged Write RAM.** The BCM4389's download minidriver
  never answers Write RAM (0xfc4c) records. Google's HAL streams them and
  waits only for Launch RAM (0xfc4e). Waiting for each record made the first
  one time out, which looked like a hung chip. For `brcm,bcm4389-bt`, btbcm
  now sends them without waiting, at most 200 bytes per millisecond. The
  records queue ahead of Launch RAM, whose reply must come within the HCI
  core's 2 s command timeout, so the queue is kept short.
- **`hci_bcm`: `brcm,bcm4389-bt`.** Without a shutdown GPIO, hci_bcm drops to
  115200 baud, because it can't reset the chip to its default speed. Here
  `pixel-bt` powers the chip up for every load, so the new
  `bcm4389_device_data` sets `host_powered` and keeps `max-speed`. At
  115200 baud the 478 KB download took about 40 s, well past the timeout.
- **`btbcm`: quirks for chip id 170.** The ROM rejects Get MWS Transport
  Config, so the BCM4387's quirks apply (broken MWS transport config, broken LE
  Coded PHY, the extended-advertising PHY fix-up).

The fix follows the GS101 (Pixel 6, also BCM4389) mainline work in
[gs101-mainline](https://github.com/m8l8th814n-eng/gs101-mainline), commit
13982bd.

## Firmware

Google's vendor image has the firmware as `/firmware/brcm/BCM.hcd`
("BCM4389C1 ES1PX_GG_C10", baseline 0391, 3,059 write-RAM records and a
launch record); `BTFW_B.hcd` is an older build (baseline 0389). The build
copies `BCM.hcd` from the ignored `out/stock-vendor/bt/` into the initramfs.
The download takes about 3 s.

## Starting

The boot script loads rfkill, kpp, ecc, ecdh_generic, bluetooth, btbcm, btqca,
pixel-bt and hci_uart from the image, then starts `bluetoothd`. PipeWire,
WirePlumber and pipewire-pulse (BlueZ and PipeWire packages from Arch Linux
ARM) run as root in the desktop session and provide Bluetooth audio.

## Building

`gen-overlay-header.py` compiles the overlay with `dtc -@` into
`pixel_bt_overlay.h`, which holds two C arrays:
- `pixel_bt_overlay`: the whole overlay;
- `pixel_bt_overlay_tty`: the overlay without the `bluetooth` child.

The image build runs it, then builds the Bluetooth modules from the kernel
tree with `M=`, in the order crypto, net/bluetooth, drivers/bluetooth.
`KBUILD_MODPOST_WARN=1` is needed because of unrelated modules in those
directories.

## Not done

- **Low-power mode.** Device wake stays asserted and the host-wake interrupt
  isn't used, because neither GPIO has a mainline pin controller driver.
- **Hands-free calls (HFP/SCO).** SCO audio is routed through the AOC on stock.
