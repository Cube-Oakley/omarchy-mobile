# CPH2655 first native probe

The initial probes described below were RAM-only USB shells. The current
probe 20 USB revision boots persistently; see [status](../docs/status.md).
Probe 3 runs an interactive root shell using BusyBox `setsid -c`, reports all
eight CPUs online and 15,218,088 KiB RAM, and returns to Android after its
three-minute timer. Probe 1 lacked cttyhack; probe 2 lost /init execute permission.
The packer now checks the actual built CPIO executable mode, content and shell
link before creating a boot image. The current build uses clean Linux
commit `5225b8eec4c9bb21aecff6295fab6346a3c3738e` (7.3-rc2), independently extracted
from the locally cached upstream tree. Pixel kernel modifications are excluded.

The board description enables USB2 peripheral mode, its M31 PHY, the PMIH0108
repeater and the necessary regulator parents. USB rail limits were compared
with this CPH2655's live stock device tree and regulator summary. The initial
probe leaves storage, remote processors, audio and GPU disabled. It keeps
unused clocks, power domains and regulators enabled during handoff.

`sm8750-oneplus13-probe.dts` derives its regulator mapping and clock definitions
from [index986's board](https://github.com/sm8750-mainline/linux/blob/1f2047e8663eda76c8b172fd3e040e3b3fa17950/arch/arm64/boot/dts/qcom/sm8750-oneplus-dodge.dts).
The community board's memory map is **not** reused. `prepare-probe-dtb.py`
replaces the reference reservations with the fixed carveouts from a captured
stock tree, retaining the verified command-db and SMEM bindings. Dynamic
Android allocations are not reproduced. Remote-processor reservation references
are not usable in this minimal tree; rebuild those bindings before enabling
their consumers.

`embedded-probe-dtb.patch` and `omarchy-oneplus13.S` embed the prepared tree in
the kernel, avoiding a vendor_boot/DTBO flash. The early handoff checks the
incoming `qcom,sun` compatible, copies the bootloader's current RAM banks and
reservation table, and selects the embedded description. The boot helper must
also validate the exact CPH2655 identity and firmware; `qcom,sun` alone does not
identify this board. The fixed carveouts must be recaptured after a firmware
change. The patch applies only to this experimental kernel.

`probe-init` runs from the embedded initramfs using the checksum-pinned static
AArch64 BusyBox already cached for the OnePlus 7 Pro. It mounts only RAM/kernel
filesystems, provides a supervised shell on USB ACM, and requests a forced
reboot after 180 seconds. This timer starts only if PID1 runs; an earlier hang
may require the phone's physical reset buttons. `CONFIG_INITRAMFS_FORCE`
prevents the installed Android/Magisk ramdisk from replacing that init.

## Build artifacts

Current ignored workspace: `../out/linux-7.3-rc2/`.
Probe output and build logs: `../out/native-probe1/` through `../out/native-probe7/`.
Stock capture: the `root_capture` entry in `../out/STOCK-BASELINE.json`.

To reconstruct the prepared DT after compiling the board against that source:

```sh
python devices/oneplus13/scripts/prepare-probe-dtb.py \
  --compiled-board PATH/arch/arm64/boot/dts/qcom/sm8750-oneplus13-probe.dtb \
  --stock-dtb devices/oneplus13/out/ROOT_CAPTURE/live.dtb \
  --output devices/oneplus13/out/NEW_PROBE/embedded.dtb
```

The helper refuses to overwrite an output and requires Git exclusion. Its
output reproduced the first prepared tree byte-for-byte. Copy that output to
`arch/arm64/kernel/omarchy-oneplus13.dtb`, apply the patch, and copy the assembly
source alongside it. Merge `probe.config` into arm64 defconfig. Supply a private
initramfs directory containing `probe-init` as executable `/init`, BusyBox as
`/bin/busybox`, `/bin/sh -> busybox`, and empty dev/proc/sys/tmp/run directories;
set INITRAMFS_SOURCE and the host file-owner UID/GID mappings. Build with
`ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-`.

The early RAM probe packer uses Android boot header v4 with OS 15.0.0 and
patch 2026-08. The captured stock boot header actually leaves the OS field at
zero and carries version properties in its AVB metadata. Do not flash the
minimal RAM wrapper: the probe 18 flash required restoring stock boot metadata
before even RAM boot worked again. See `pack-persistent.py` and the recovery
notes for the separate persistent wrapper, validated with `--metadata unsigned`
and the explicit USB2 UTMI clock selection. The native payload
is unsigned and requires the already
unlocked bootloader. Do not flash an untested probe: use `fastboot boot` and
retain the verified stock images. Probe 3 supplies native boot, USB shell and timed Android-return evidence.
The storage variant adds the stock UFS rail mapping and caps HS gear at 3;
its diagnostics set all exposed UFS block devices read-only before hashing
the active boot-related partitions. No filesystems are mounted.


## Storage, networking and first display attempt

Probe 5 enumerates all six UFS logical units. Eight active boot partitions
match the independently verified Android readbacks after setting the disks and
partitions read-only. The PHY and its QMP parent must both be built in.

Probe 6 replaces g_serial with a CDC Ethernet/ACM composite gadget, with
manufacturer `Omarchy-OnePlus13`, phone address `10.77.13.1/30` and host address
`10.77.13.2/30`. `69-oneplus13-linux-serial.rules` grants the active local user
access to this gadget only. Its 1200-second deadline permits RAM-only rootfs
transfer and testing. The shared Arch/mobile archive was hash-verified on the
phone and SSH into the tmpfs root succeeded; BusyBox remains PID1.

`start-native-arch.py` verifies the selected archive and uses a temporary
NetworkManager profile with no default route. It records the profile UUID for
cleanup and pins the phone's fresh SSH host key received over serial. All
credentials and logs must be Git-ignored. The cached Pixel userspace archive
contains hardware adapters: do not start Pixel-specific services on this board.

The first display attempt used the stock reserved splash region with the
community board's 1440 x 3168 geometry. Probe 6 had no framebuffer because
DRM remained modular. Probe 7 builds DRM and simpledrm into the kernel; the
packer now rejects a framebuffer test if those built-in dependencies are absent.
This remains an inherited-buffer experiment, not panel initialization or
accelerated scanout. Visible output requires owner confirmation.


Probe 7 produced visible color bars and the shared desktop, both confirmed by
the owner. Hyprland 0.56.2 used llvmpipe on simpledrm; the advertised 60 Hz mode
is not a physical refresh measurement. Current `overlay/mobile` installed in
RAM with `adapter/`. Optional camera/browser/link builds were skipped because
the cached rootfs lacked their development dependencies; the offline keyboard
dictionary download also failed. This is desktop bring-up, not application or
hardware parity.

## Touchscreen experiment

`sm8750-oneplus13-touch.dts` enables QUP1/SPI4 and its GPI DMA path, adding the
stock L4B 1.8 V rail and verified reset/attention/AVDD-enable GPIO mapping. SPI
starts at 1 MHz. `touch/oneplus13-tcm.c` is a small experimental module built
against the exact probe kernel. It only identifies the S3910 and reads its
application information by default. `input_enabled=1` additionally selects
volatile controller-local coordinate reporting; no persistent configuration,
flash or firmware-download commands are available. Reporting uses bounded
polling for bring-up, not a validated suspend-capable input driver.

The protocol reference is the official OnePlus source at
`d50b305f7da9e14715a25120a4ac7b1a4b8b97c3`, directory
`vendor/oplus/kernel/touchpanel/synaptics_hbp/tcm/`. Captured Android logs confirm
TouchComm v1, S3910PA0B0-15.2 application build 4569129, without appended CRC.
They also establish that the driver's legacy AVDD GPIO path drives physical
high on power-up despite the vendor DT's active-low flag.

Build the external module after Image and `modules_prepare`. For this kernel
with CONFIG_MODVERSIONS disabled, copy the generated `vmlinux.symvers` to
`Module.symvers` to provide the exact built-in exports, then use `make M=...`.
Keep module build outputs under ignored `out/`, copy the reviewed module into
the next initramfs, rebuild Image, and validate the actual CPIO module bytes.
Probe 8 identified the stock firmware and exposed ten multitouch slots. The
owner confirmed touch works in the shared desktop; recorded events include
nine contact starts and nine releases. Edge accuracy, simultaneous contacts,
latency and suspend remain unvalidated. The reviewed driver adds managed
power-off cleanup for subsequent probes. See `gpu/README.md` for GPU bring-up.
