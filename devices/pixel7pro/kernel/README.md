# Pixel native kernel checkpoints

Current baseline: [v26](audio-v26/README.md): audio, sensors and
Bluetooth. Google's AoC drivers run the audio DSP, which drives the
microphones, the two speaker amplifiers ([spi](spi/README.md),
[gpio](gpio/README.md)) and the sensors ([aoc-power](aoc-power/README.md),
[sensors](../sensors/README.md)); Bluetooth starts at boot. The kernel image's
code is v25's apart from the Bluetooth drivers.

Previous baseline: [v25](sleep-v25/README.md): sleep, Wi-Fi power and
brightness. Wi-Fi firmware deep sleep, panel sleep while the screen is off, a
panel backlight device and s2idle as the default system sleep. With the
[L1 substates](pcie/README.md), the screen-off idle draw falls from 2.11 W to
1.47 W. New modules: [keys](keys/README.md), [i2c](i2c/README.md),
[torch](torch/README.md) and [haptics](haptics/README.md).

Older baseline: [v24](wifi-v24/README.md): Wi-Fi. brcmfmac drives the
BCM4389 with Google's stock firmware, over PCIe channel 1 from the
[PCIe module](pcie/README.md). The kernel image's code is v23's; the changes
are in the Wi-Fi modules, which the image now carries with their firmware.

Older baseline: [v23](power-v23/README.md): idle power (memory clock
released while the screen is off, a slower thermal poll when cool), with the
[C2 idle](cpupm/README.md) and [power domain](pd/README.md) modules and the
[ODPM power meters](odpm/README.md).

Previous baseline: [v22](smooth-v22/README.md): full CPU rates under a 20 ms
thermal cap, on top of [v21 smoothness](smooth-v21/README.md) (120 Hz on a
normal boot, GPU and CPU frequency scaling, retuned CPU thermal trips).

Previous baseline: [v20 native boot](native-boot-v20/README.md), a complete patch
that boots from `boot_a` without fastboot. It brings up the USB 2 PHY on a
normal boot and accepts that boot's CPU clock state. Together with the
[UFS driver](storage/README.md)'s HS gear 4 link, this makes the persistent
desktop start on its own. Out-of-tree modules loaded by the persistent boot:
[UFS](storage/README.md) (HS gear 4, device reset), [RTC](rtc/README.md),
[battery and charge control](battery/README.md),
[power key](powerkey/README.md) and [reboot target](reboot/README.md)
(reboot to fastboot works with a warm reset). The history below predates it.

Previous display baseline: [v19](panel-v19/README.md), applied after the cumulative
v18 image D patch. It adds clean shared CRT screen off/on. The
[power-key module](powerkey/README.md) supplies Linux input events; the
[UFS handoff driver](storage/README.md) and `--persistent-root` build option are
under installation validation. The Arch root is installed on ext4 userdata and
the shared desktop runs from it. [Reboot target selection](reboot/README.md) is
experimental: its bootloader-mode test entered Android recovery, where ADB
successfully restored fastboot. Full CPU suspend remains unimplemented. Earlier
checkpoints below are preserved for reproduction. Persistent builds now embed
their required command line before early parsing; image H tests whether normal
ABL boot was omitting the boot-header arguments. H is not hardware-validated yet.

`kernel-base.txt` records the exact upstream commit. `native-bringup-v9.patch`
contains all local source changes relative to it, including the preexisting
embedded initramfs/bootconfig changes and the new framebuffer, USB and watchdog
code. `native-bringup-v9.config` is the tested configuration; the build helper
replaces its absolute initramfs path with the selected output directory.
Recorded configurations name this workspace as `@PIXEL_ROOT@` in place of the
local absolute path; the helpers set both paths again at build time.

Apply one checkpoint patch to a clean checkout of that commit. The v11 checkpoint
used the complete patch below. Reverse
`git apply --check` was tested against that working source, including added files.
Do not stack checkpoint patches or apply twice.

The low-level instrumentation is specific to this Pixel's verified bootloader
handoff; this is a bring-up patch, not an upstream-ready multi-device driver.
The framebuffer and reserved-RAM startup markers use fixed addresses. Normal
DRM, USB PHY/clock, watchdog and power drivers remain future integration work.

Use `scripts/build-pixel-shell.py` from the project root to rebuild. See
[the dated milestone](../docs/native-shell-20260925.md) for dependencies,
commands, artifact hashes and on-device evidence.

The v10 network variant uses the same hardware patch, disables `USB_G_SERIAL`
and enables `USB_CDC_COMPOSITE`. Build it with `--usb-network`; the helper selects
the composite gadget and adds the USB network setup script to the initramfs.
See [network/Arch validation](../docs/native-arch-20260925.md).

## v11 retained-display bridge

`native-bringup-v11.patch` is a complete alternative patch against the same pinned
upstream commit, including the v9 work plus the guarded DRM bridge and console
handoff. Do not stack it on top of v9. The v11 working kernel passed
reverse-apply validation. `native-bringup-v11.config` records the tested
v11d configuration. The build helper still starts with v9's config and applies
the display options when passed `--drm`.

The native DRM test card is user-confirmed visible, with successful modeset,
eight page-flip events and advancing DECON frame counters. This is fixed-mode
CPU-copy display support, not GPU acceleration or a complete native panel driver.
See [display milestone](../docs/display-drm-20260925.md).
