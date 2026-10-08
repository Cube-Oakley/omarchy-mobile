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
   The child carries the wake lines (see Sleep) only when `pixel-gpio`
   provides both; otherwise pixel-bt applies it without them.

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
- **`hci_bcm`: links kept through system sleep**
  ([patches/0001](patches/0001-hci_bcm-keep-the-BCM4389-links-through-system-sleep.patch),
  on top of the v37 tree; not in an image yet). The HCI core's suspend
  notifier is for laptops: at every system sleep it disconnects every link
  and pauses scanning and advertising. With a host-wake interrupt, the
  BCM4389 now registers without it, as `hci_h5` does for some Realtek parts,
  so a watch, headphones or a car key stay connected while the phone sleeps.

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
pixel-bt and hci_uart from the image (pixel-bt and hci_uart from
`/usr/local/lib/omarchy-mobile/bluetooth` when installed there), then starts
`bluetoothd`. PipeWire, WirePlumber and pipewire-pulse (BlueZ and PipeWire
packages from Arch Linux ARM) run in the desktop session and provide
Bluetooth audio.

## Building

`gen-overlay-header.py` compiles the overlay with `dtc -@` into
`pixel_bt_overlay.h`, which holds three C arrays:
- `pixel_bt_overlay`: the whole overlay, wake lines included (their
  `&gpp16` and `&gpa6` resolve against the stock tree's `__symbols__`);
- `pixel_bt_overlay_awake`: without the wake lines;
- `pixel_bt_overlay_tty`: without the `bluetooth` child.

The image build runs it, then builds the Bluetooth modules from the kernel
tree with `M=`, in the order crypto, net/bluetooth, drivers/bluetooth.
`KBUILD_MODPOST_WARN=1` is needed because of unrelated modules in those
directories. For `install-lpm.sh`, the modules are built the same way:
- pixel-bt and pixel-gpio: `make -C <kernel> ARCH=arm64
  CROSS_COMPILE=aarch64-linux-gnu- M=<dir> modules`, with the header
  generated into the same directory;
- hci_uart: with `M=<kernel>/drivers/bluetooth` and the patch applied;
- all three stripped with `aarch64-linux-gnu-strip --strip-debug` into
  `kernel/bluetooth/` and `kernel/gpio/`.

## Sleep

The stock `nitrous` node names the chip's wake lines: device wake gpp16-3
(host to chip, active high) and host wake gpa6-6 (chip to host, an always-on
wake-up interrupt, pulled down). `pixel-gpio` provides both, and the overlay
gives them to mainline `hci_bcm`, which then runs the chip's low-power mode:
- it sends Broadcom's Set Sleep Mode (0xfc27: UART sleep, active-high wake
  lines, pulsed host wake);
- after 5 s without traffic it drops device wake and deasserts RTS, and the
  chip sleeps;
- the chip pulses host wake when it has something to send: the interrupt
  resumes the port, and in system sleep it wakes the phone.

pixel-bt reports this as `sleep_ready=Y` (else `N`, and the chip stays awake
as before).

**Deep sleep (SYS_SLEEP).** PERIC0 powers down and its pads take their
power-down states. The bootloader's made BT_REG_ON an input, so the chip lost
power and came back without its patch firmware. pixel-bt now sets the stock
state for both GPIOs ("previous", device wake pulled down), so BT_REG_ON
stays high; pixel-sleep puts PERIC0's registers back before the pads are
released. UART18 and its pins are in the always-on APM domain. A syscore
resume hook checks the USI mode, its reset and clock request, the pins and
BT_REG_ON before samsung_tty resumes the port, and restores them if they
were lost (`resume_restored` counts it).

`pixel-suspend` keeps Bluetooth through deep sleep when BlueZ has it on and
the low-power mode is wired. It adds the host wake to pixel-sleep's wake
EINTs (bit 51) and names such a wake `bluetooth`, which the shell holds
like any other. Otherwise, with Bluetooth off or without the wake lines, it
unloads hci_uart and pixel-bt, and the display hook loads them again through
`pixel-bt-restart` when the screen lights (`docs/suspend-20260930.md`).

**Not yet measured on the phone** (October 2): the chip's idle draw in deep
sleep, the host wake, and the links across a sleep.

## Installing

The image's pixel-bt and hci_uart predate the low-power mode. The boot script
and `pixel-bt-restart` load newer builds for the running kernel from
`/usr/local/lib/omarchy-mobile/bluetooth` first. The boot now starts
Bluetooth after the GPIO provider, which is the modem bundle's `pixel-gpio`
while the modem is on (`modem/manager.py prepare`), else the image's.
`install-lpm.sh` (run from the host) does the following:
- installs both modules and the scripts;
- replaces the bundle's pixel-gpio with the build that carries the wake
  lines, with the manifest's checksum;
- keeps the old files in `/root/*.before-lpm`.

The change takes effect at the next boot.

## Testing

`ble-check.py` exercises BlueZ end to end over D-Bus, with this laptop's
adapter as the other side:
- `peripheral` (on the phone) advertises a test GATT service: a counter
  (read, notify), an inbox (write) and a secret readable only over an
  authenticated, encrypted link;
- `central` (on the laptop) finds it and connects;
- the central reads, writes and subscribes, then pairs with LE Secure
  Connections, where both sides print the six-digit code;
- it reads the secret, then removes the pairing.

Each step is printed as a JSON line. Run as root, it also prints the link's
RSSI while connected, read through the kernel's management socket (Get
Connection Information). BlueZ's D-Bus API gives RSSI only while scanning. The roles can be swapped. A car's phone
key works the other way round: the car advertises and the phone connects as
the central.

## Not done

- **Hands-free calls (HFP/SCO).** SCO audio is routed through the AOC on stock.
- **Controller-side scan filters.** An app's unfiltered LE scan keeps
  reporting through sleep and wakes the phone for every advertisement.
  Android offloads such filters to the chip (Broadcom's APCF commands), which
  BlueZ does not use. BlueZ's auto-connect scan for bonded devices filters in
  the controller (its accept list).
