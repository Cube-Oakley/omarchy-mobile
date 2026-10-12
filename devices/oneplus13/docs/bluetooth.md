# OnePlus 13 Bluetooth bring-up

Experimental Bluetooth now starts automatically from the installed native
image. Stock firmware, LE discovery, BlueZ/mobile-shell discovery and three
off/on cycles passed alongside Wi-Fi traffic. Pairing, accessory data, audio,
range, suspend and factory address retrieval remain unverified.

The installed image is `out/native-persistent20-bt-uart/boot-persistent.img`,
SHA256 `f981a3532097eb8bf2978b35dc1af999b08e32078d467771e895b2683eb4c8ea`.
It adds only a boot-time UART alias to the validated Wi-Fi device tree.
Bluetooth firmware is loaded into chip RAM from the saved stock backup;
the firmware/calibration partitions remain untouched.

## Stock evidence

The exact saved `bluetooth_a` partition is 12 MiB, SHA256
`dcb7856c74e5273ce95695a0cb58698530c61fbb6d4c68406fd6c54aef5e0eda`.
It contains a FAT16 filesystem whose header declares a 64 MiB volume. The
original backup remains untouched; a private analysis copy was zero-padded
to the declared size for extraction. All 214 files extracted successfully;
private manifests record individual hashes. The Ganges 2.0 firmware family
includes `gngbtfw20.mbn`, board-specific `gngbtnv20` files, and peripheral/UWB
images. The Bluetooth patch is an ELF image. No firmware was installed on the
phone by the identification probe.

The stock live device tree and OnePlus's
[Peach Bluetooth description](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8750/blob/d50b305f7da9e14715a25120a4ac7b1a4b8b97c3/vendor/qcom/opensource/bt-devicetree/sun-peach-bt-v8.dtsi)
identify UART14 at `0x898000`, TX/RX GPIO26/27, CTS/RTS GPIO24/25,
BT_EN on PM8550VE-F GPIO3 and SW_CTRL on TLMM GPIO18. The chip shares the
six physical Wi-Fi rails and RF_CLK1. The saved stock kernel log reports
SoC version `40210200`. GPIO16 is WLAN_EN and must stay owned by Wi-Fi.

## Identification probe

- `kernel/peach-bluetooth-version-probe.patch` adds a private CPH2655 QCA UART
  profile with explicit `peach_version_probe=1` opt-in. It takes independent
  votes on the existing stock-constrained rails, follows the 50 ms BT_EN
  sequence, reads ROM identity at 115200 baud, and powers down. It makes one
  attempt per module load, with no firmware loading, radio setup or automatic
  retry. Its power-off path releases its own regulator votes.
- `kernel/peach-bluetooth-probe-overlay.dts` adds the UART/pins and private
  serdev node. The loader checks the handset, exact base phandles, RF clock,
  bound Wi-Fi endpoint and boot-time UART alias. It has no unload hook;
  reboot removes the entire probe. Compile this overlay without `dtc -@`:
  local phandle fixups are needed, exported symbols are not. The base tree
  has no symbol table and the kernel rejects an overlay exporting symbols.
- `kernel/peach-bluetooth-alias-overlay.dts` is applied on the host before
  building a RAM test image. GENI's driver requires a serial alias cached at
  boot; dynamically enabling the UART without it returned `Invalid line -19`.
  Adding only `aliases/serial0` preserves every existing node and phandle.

The ROM query returned product `0x21`, SoC `0x40210200`, ROM `0x0200` and
patch `0x6d58`, matching stock evidence. After power-off all six Bluetooth
regulator votes returned to zero, BT_EN went low, and Wi-Fi still passed
HTTPS traffic. The kernel had only its existing out-of-tree-module taint.

## Firmware probe

`kernel/peach-bluetooth-firmware-probe.patch` applies after the identification
patch. Both `peach_version_probe=1` and `peach_firmware_probe=1` are required.
It verifies the exact ROM identity, loads the saved Ganges ELF patch, reads
the firmware board ID and selects its corresponding stock NVM file. A
missing board-specific NVM is an error; there is no generic-file fallback.
The downloaded NVM is volatile controller configuration, not a write to any
factory calibration partition. Each attempt ends by powering off.

The probe retains the verified 115200 baud rate. The first unpaced transfer
queued the entire image ahead of the final acknowledged command and hit
the HCI core's two-second transmit timeout. Retrying then stalled in UART
close with pending bytes; controlled reboot to fastboot recovered normally.
The revised probe paces unacknowledged segments and flushes the UART before
dropping BT_EN. No kernel command timeout is globally extended. This revision
passed: board ID `0000` selected the stock `gngbtnv20.bin`, and the chip
reported `BTFW.GANGES.2.0.0-00343-PATCHZ-1_DEF` for firmware and NVM. All six
Bluetooth votes returned to zero and Wi-Fi HTTPS still passed. The HCI core
logged a reset timeout after the probe deliberately powered the chip off;
this is separate from the successful firmware/HCI-reset/build-info sequence.

## Experimental controller

`kernel/peach-bluetooth-radio-probe.patch` adds a third explicit opt-in,
`peach_radio_probe=1`, on top of both earlier patches. It keeps the successfully
initialized controller powered, enables the existing QCA in-band sleep
protocol and lets Linux initialize HCI. Errors still power down without
automatic retry or firmware fallback. The installed startup uses this profile
with the separately validated high-speed option described below.

BlueZ recognizes the controller after assigning a private test address; the
factory address retrieval path is still pending. The address is kept in a
mode-0600 file on the phone, outside version control. No factory identity
partition was changed. Basic initialization and repeated power sequences
have passed. At
115200 baud, the first LE discovery command (`0x202d`) timed out even though
the IBS wake acknowledgement arrived. That path is not usable Bluetooth.

The installed kernel configuration lacks Bluetooth LE, RFCOMM and BNEP.
An isolated copy of the same kernel build applies `kernel/bluetooth.config`;
its configuration diff changes those Bluetooth options and enables the UHID
module needed by BlueZ for Bluetooth LE keyboards and mice. The complete
Bluetooth core, UART drivers and protocol modules are rebuilt together to
keep their conditional structures consistent. They can replace the old
Bluetooth modules in RAM; the Wi-Fi and other kernel modules are unchanged.
The original kernel build tree remains intact. The
[ROCKNIX WCN7860 patch](https://github.com/ROCKNIX/distribution/blob/bcf3b5bc574990b96543484575b06f912153a715/projects/ROCKNIX/devices/SM8750/patches/linux/0045-Bluetooth-qca-add-WCN7860-support.patch)
provides the Ganges peripheral baud-command sequence. Its generic power
profile was not adopted.

`kernel/peach-bluetooth-speed-probe.patch` adds a separately opted-in
`peach_highspeed_probe=1` path: peripheral wake/baud command, HCI baud command,
host switch to 3 Mbaud, then a second ROM identity read before downloading
firmware. Raw-command TX is explicitly kicked and waits are bounded. The
firmware-only test passed at 3 Mbaud in approximately 1.5 seconds and powered
off cleanly, with Wi-Fi still working. Peripheral startup can emit an
unrecognized frame and an IBS acknowledgement outside the ordinary state;
these remain visible in logs. At 3 Mbaud, a ten-second LE scan found 27 unique
advertisers after an idle interval, and BlueZ/shared-shell scanning passed.
Three mobile-shell off/on cycles passed (about 0.2/1.7 seconds), with Wi-Fi
HTTPS after every cycle. Automatic startup passed a fresh RAM boot and a
normal boot after flashing; full image readback matched and the seven other
checked boot partitions were unchanged. The kernel retained only its expected
out-of-tree module taint. Accessory pairing and audio remain pending.
Further normal boots passed, including automatic UHID creation. The existing
mobile pairing agent's D-Bus interface successfully registered and unregistered
with BlueZ without initiating any remote pairing. Its `python-gobject`
dependency was installed from signed Arch Linux ARM packages alongside
`bluez` and `bluez-utils`.

## Startup and recovery

The verified package lives in `/opt/oneplus13-bluetooth`. Its manifest covers
all modules (including UHID), the exact stock patch/NVM and kernel release. Optional
`start-native-bluetooth.sh` waits for Wi-Fi initialization, applies the guarded
UART overlay, loads the complete LE stack, verifies the private test address
through Linux MGMT, and starts BlueZ. The existing mobile Bluetooth UI is used.

An interrupted initialization leaves
`/var/lib/omarchy-mobile/oneplus13/bluetooth-starting`, suppressing another
attempt on the next boot. Inspect `/run/oneplus13-bluetooth.log` and
`/run/oneplus13-bluetoothd.log` before clearing it. The previous Wi-Fi boot image
has no UART alias, so this helper exits before powering Bluetooth or setting
the failure marker. The dynamic overlay has no removal hook; reboot to remove
it. Normal driver teardown has been tested, but never force-unload the overlay.

Private source copies, extracted firmware, build manifests and probe logs
are under `out/bluetooth-survey`; the alias-only image is under
`out/native-persistent20-bt-uart`. Firmware, identities, nearby-device records
and binary packages are private ignored artifacts.
