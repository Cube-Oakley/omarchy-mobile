# Omarchy Mobile · Pixel 7 Pro

Bringing native Linux and the Omarchy desktop experience to the Pixel 7 Pro
(`cheetah`, Tensor G2 / GS201). The OnePlus 7 Pro project is the reference for
bring-up methods and the eventual mobile interface.

**Status: accelerated shared mobile shell and working CRT screen power;
persistent installation in progress, September 26, 2026.** Mainline Linux runs Arch Linux
ARM and the shared Hyprland/Quickshell mobile shell from installed ext4 storage. Mali-G710 rendering,
clean 120 Hz scanout, bounded CPU scaling and seven thermal zones are verified.
The new S2MPG12 input driver and DRM panel off/on path have a user-confirmed
sleep/wake cycle with clean CRT transitions and no console flash.
The Pixel uses the same CRT animation and power-key policy as the OnePlus.

This phone is dedicated to Linux. Android userdata has been replaced with ext4
after verified UFS reads and explicit authorization. The write/readback check
passes and the installed Arch desktop has been validated; boot_a now contains the tested kernel with matching direct readback. Image H embeds the required boot parameters and passed RAM validation; its normal reboot remains untested.
Recovery uses the bootloader and saved host images. See the
[active implementation record](docs/persistence-power-20260925.md).

## What works

| Area | Verified result |
|---|---|
| Recovery baseline | Unlocked bootloader and saved factory images; boot partitions checked by SHA256 before installation. |
| Native boot | Linux 7.3.0-rc2, embedded initramfs, all eight CPUs online. |
| Display | Native 1440×3120 DMA scanout, real page flips, validated 60/120 Hz modes and memory-bandwidth floor. |
| Wayland | Hyprland on Mali-G710 MC7, shared mobile shell and clean fullscreen animation at 119.6–120.2 fps; older software-rendered fallback preserved. |
| CPU/thermal | Three bounded cpufreq policies, schedutil, seven thermal zones and cooling tests. |
| Keys/display sleep | Power and volume keys on wake-up interrupts ([keys](kernel/keys/README.md)); panel off/on with clean CRT transitions, and the panel sleeps in while the screen is off. |
| System sleep | Guarded s2idle while unplugged; calls and SMS wake the AP, and power-key/RTC wake work. Repeated Wi-Fi resume is fixed. Power savings remain modest; deep SoC sleep is unfinished. See [suspend results](docs/suspend-20260929.md). |
| Brightness | Panel DBV as a backlight device; the shell's slider works and its level is restored. |
| Flashlight | LM3644 on hsi2c_15 ([torch](kernel/torch/README.md)). |
| Vibration | CS40L26A ROM effects on hsi2c_8 ([haptics](kernel/haptics/README.md)). |
| Internal storage | All UFS logical units discovered; boot hashes match and ext4 write/remount/readback passes. The installed Arch desktop runs from userdata; proof files survive reset and recovery RAM boot. |
| Physical touch | S3908 on the SPI0 controller with its attention interrupt: about 240 Hz, user-confirmed smooth. |
| USB | DWC3 peripheral; the kernel brings up the USB 2 PHY on a normal boot (fastboot's is inherited); concurrent USB2 CDC-ACM and CDC-ECM Ethernet. |
| Shell | Native root BusyBox shell, job control, RAM files, shell restart after exit, command exit-status reporting. |
| Arch userspace | Same cached Arch Linux ARM base as OnePlus; native Bash, glibc, pacman and OpenSSH. |
| Network transfer | Private USB link, key-only SSH, 8 MiB SFTP roundtrip with matching hashes. |
| Wi-Fi | BCM4389 on PCIe channel 1 with mainline brcmfmac and the stock firmware; starts at boot under NetworkManager. 5 GHz, about 90 Mbit/s both ways (the home network's limit). Firmware deep sleep and L1.2 cut its idle cost to about 0.1 W. See [v24](kernel/wifi-v24/README.md) and [v25](kernel/sleep-v25/README.md). |
| Audio | Built-in microphones and both speakers through the AoC audio DSP, with Google's AoC drivers ported ([aoc](kernel/aoc/README.md)). Both CS35L41 amplifiers run their protection firmware with the factory calibration and the stock gain ([audio](kernel/audio/README.md)). PipeWire uses a UCM profile. Starts at boot. |
| Sensors | Accelerometer, gyroscope, magnetometers, barometer, proximity and light through the AoC's sensor framework, with a USF client written from the stock library; iio-sensor-proxy's D-Bus API for the shell, whose rotation works ([sensors](sensors/README.md)). Starts at boot. The display-synced light and proximity sensor stopped converting on 2026-09-29, so automatic brightness waits on it (see the [status](docs/status.md)). |
| Bluetooth | BCM4389 on UART18 with Google's patch firmware at 3 Mbaud; BlueZ and PipeWire ([bluetooth](kernel/bluetooth/README.md)). Pairing is still to be tested by hand. |
| Boot watchdogs | Both inherited AP watchdogs stopped; two-minute runtime verified before the longer shell test. |
| Recovery | Power + Volume Down reaches the bootloader; verified host images are retained. Android userdata has been replaced. |

Autonomous boot validation, faster UFS, complete panel rail/PHY control,
battery/charging and CPU suspend remain unfinished. Image H is RAM-tested and installed with full checksum readback.
The current session shows the persistent-root terminal, but the top shell stays transparent after cold startup; this remains unresolved. Earlier G normal boots did not restore USB. Slot B is **not** a recovery fallback.

## Connect

Use the current connection and installation state in [status](docs/status.md).
The following commands describe the historical RAM recovery image, from
`devices/pixel7pro/` on the computer with the phone in fastboot.
The helper needs the handset serial, which stays local: set `PHONE_SERIAL` or
put it on one line in ignored `out/device.serial`.

```sh
python scripts/boot-pixel-shell.py
python scripts/pixel-shell.py
```

The boot helper verifies the image hash, phone identity and slot state, then
uses `fastboot boot`. The default v9 image reboots after ten minutes.
`Ctrl-]` disconnects the terminal; `exit` restarts the phone's shell; `reboot`
restarts the phone. Android userdata is no longer present.

For a single command:

```sh
python scripts/pixel-shell.py --command 'id; uname -a; cat /sys/devices/system/cpu/online'
```

The host's USB serial access rule is installed. Full setup, build instructions,
test evidence and recovery steps: [native shell bring-up](docs/native-shell-20260925.md).
For the network image and repeatable Arch/SSH bootstrap, follow
[native Arch userspace](docs/native-arch-20260925.md).

## Documentation

- [Persistent install and power button](docs/persistence-power-20260925.md): current implementation, CRT requirement, storage decision and validation.

- [Hardware pipeline](docs/hardware-pipeline-20260925.md): CPU/GPU/display dependency order, stock inventory and initial ACPM patches.

- [Physical touch checkpoint](docs/touch-bringup-20260925.md): user-confirmed GPIO SPI experiment and its limitations.

- [Hyprland and shared mobile shell](docs/hyprland-mobile-20260925.md): renderer fix, shared-source provenance, screenshots and replay.
- [Current checkpoint](docs/status.md): working image, exact state, next steps.
- [Native DRM display, September 25](docs/display-drm-20260925.md): retained-panel bridge, visual confirmation and page-flip evidence.
- [USB networking and native Arch, September 25](docs/native-arch-20260925.md): shared base, SSH, repeatable RAM provisioning.
- [Shared OS architecture proposal](../../docs/shared-os-20260925.md): monorepo/device boundaries, docking and upstream updates.
- [Native shell, September 25](docs/native-shell-20260925.md): implementation, reproduction, validation, recovery.
- [Recovery and first native boot, September 24](docs/restart-20260924.md): chronological investigation and failed experiments.
- [Kernel patch and config](kernel/README.md): source checkpoint, independent of built images.
- [Historical README](README-legacy-20260913.md) and [historical recovery notes](STATUS-legacy-20260913.md): preserved for reference; their state and procedures are superseded.

Dated build artifacts, source snapshots, checksums and test transcripts live in
`out/checkpoints/`; earlier diagnostics remain in `out/restart-20260924/`.
