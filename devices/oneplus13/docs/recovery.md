# Stock recovery preparation — October 11, 2026

**Current native checkpoint:** `boot_a` contains the verified probe 20 PCIe/DWC
MSI/UART-alias revision with native station Wi-Fi and experimental Bluetooth;
the installed ext4 root boots directly. Slot A success is marked and
confirmed by fastboot. Stock `vbmeta_a` remains byte-identical. The stock Android
observations below are historical; do not boot stock recovery and format data.
See the final section for the tested native image and recovery path.

The CPH2655 bootloader is unlocked. Both Android boot properties and fastboot
confirm it; stock Android still boots on slot A, with region `US`, project
`23893`, and the original OxygenOS `16.0.10.501(EX01)` installation.

## Verified stock images

The exact-build OTA is an incremental package, so its boot images cannot simply
be extracted. A complete chain was found from a full `15.0.0.832` package through
seven incremental updates to `16.0.10.501`. Reconstruction happened entirely on
the host. **No older image was booted or flashed.**

| Stage | OxygenOS target | Catalog metadata |
|---|---|---|
| 2125 | 15.0.0.832 | [full package](https://otachecker.com/ota/2125) |
| 2126 | 15.0.0.850 | [incremental](https://otachecker.com/ota/2126) |
| 2127 | 16.0.2.403 | [incremental](https://otachecker.com/ota/2127) |
| 2128 | 16.0.5.703 | [incremental](https://otachecker.com/ota/2128) |
| 2155 | 16.0.7.201 | [incremental](https://otachecker.com/ota/2155) |
| 2222 | 16.0.8.301 | [incremental](https://otachecker.com/ota/2222) |
| 2518 | 16.0.9.401 | [incremental](https://otachecker.com/ota/2518) |
| 2541 | 16.0.10.501 | [incremental](https://otachecker.com/ota/2541) |

Downloads were HTTP range reads from the official `android.googleapis.com` OTA
package URLs. The catalog supplies discovery/build-chain metadata, not the
cryptographic trust anchor. The final target fingerprint exactly matches the
phone's `ro.build.fingerprint`:

```text
OnePlus/CPH2655/OP5D55L1:16/BP2A.250605.015/V.R4T3.26d97ac-1c1394-1d9854:user/release-keys
```

Trust and integrity checks:

- Public certificates were pulled from this stock installation's
  `/system/etc/security/otacerts.zip`; OpenSSL extracted their public keys.
- Every stage's payload metadata signature was verified against those keys.
- Each delta's full source partition size/hash was checked before patching.
- Operation data and source hashes were checked where applicable.
- Every reconstructed partition matched its signed target size and SHA256.
- `avbtool verify_image` passed for final boot, init_boot, vendor_boot, recovery
  and DTBO images. This supplements the authenticated payload hashes.

Final images are in ignored `out/reconstructed-stock/2541/`:

| Image | Bytes | SHA256 |
|---|---:|---|
| boot | 100663296 | `bbb4980f9efc63371268b8f7fbf78d0516e19c78a2c6a5f254170a409ebea983` |
| init_boot | 8388608 | `612d604e894c477e4980d03b31ea1ecb0d9eee21f78473a15b22a3dfbcd10b29` |
| vendor_boot | 100663296 | `9ab90aaf0464d72ad9a27746f45c2772f34a3653882e4613f764f6e209545d80` |
| recovery | 104857600 | `429a51af38b2b7a01f59402875fa754da796e14700b14286946d257604e55d8e` |
| dtbo | 25165824 | `c3f0982c8694acc81602a869cdded433116f97c9971aa057d760bb2ad8d5fce6` |
| vbmeta | 12288 | `0bb08f0c8434679dd6b9a19dfcb1a2dd17168fd97f003b52b80cd1e939cf2930` |
| vbmeta_system | 4096 | `474a208e87bde5a4813066e4b80c7f891ecab35e9c574ee4fe4b73f2809d7a66` |
| vbmeta_vendor | 4096 | `f5c0c53ff1cee29b62f2fff0e3bc1fe0108de03d7cc6d2878a50430af1ef2542` |

These are authenticated stock distribution images. Subsequent rooted readback
matches boot, vendor_boot, recovery, DTBO and all three active VBMeta images
(the larger VBMeta partitions contain zero padding after the OTA image).
init_boot_a matches the intentionally flashed Magisk image. Stock init_boot is
retained from the authenticated distribution; it was not read before patching.

The rooted capture contains 130 raw partitions, 3,275,976,704 bytes, all matching
phone hashes before and after transfer and their expected lengths. This covers
persist, modemst1/2, fsg/fsc, vendor NV copies, both slots’ boot/recovery/firmware,
and boot-chain images. Persist files, live DT and memory map were also captured.
The 13,329,498,112-byte stock super partition also matches its before/host/after
hashes. Primary and backup GPT regions for all six UFS logical units are saved;
all twelve header and partition-table CRC checks pass. Userdata, Android encryption
metadata and swap are excluded. This is a live, non-atomic backup and no raw
calibration/NV restoration is proposed. Neither slot is a tested fallback.

## Reproduction

The host-only helper [reconstruct-stock.py](../scripts/reconstruct-stock.py)
uses the existing ZIP/protobuf reader from the OnePlus 7 Pro workspace. It adds
signed-metadata verification and SOURCE_COPY / BSDIFF / BROTLI_BSDIFF support.
It accepts only official Google-hosted OTA URLs, a continuous CPH2655 build
chain, trusted keys, supported operations and bounded boot-image extents.
Unsupported formats or failed checks stop reconstruction.

The local chain file records source URLs and pre/post fingerprints. To resume
or recheck (existing images are rehashed before being reused):

```sh
python devices/oneplus13/scripts/reconstruct-stock.py \
  devices/oneplus13/out/stock-ota/chain.json \
  --key devices/oneplus13/out/stock-ota/stock-ota-key-0.pem \
  --key devices/oneplus13/out/stock-ota/stock-ota-key-1.pem
python devices/oneplus13/tests/test_reconstruct_stock.py
```

Nine host tests cover both compression formats, signed source seeks, byte
addition and literals, malformed patches and partition-boundary rejection.
The actual eight-stage reconstruction additionally validates all 64 partition
outputs against their signed hashes. No command in the helper accesses a phone.

## Boot format and readback limitations

Boot, init_boot, vendor_boot and recovery use header v4. Boot has no ramdisk;
init_boot carries the generic ramdisk. Vendor boot has a 6,821,771-byte bundle
containing 13 base DTBs; DTBO contains six overlays. The boot properties select
base DTB index 2 and DTBO index 3. That overlay's `oplus,project-id` includes
23893, and applying it to base DTB 2 with `fdtoverlay` succeeds locally. The
merged tree is saved as `out/reconstructed-stock/2541/stock-board-23893.dts`.
This is the packaged board tree, not the live tree after bootloader fixups. The
community PJZ110 example selects a different overlay. Do not use its selection
or memory map without comparing it with this handset's stock tree.

The official OnePlus module/device-tree tree was inspected at
`d50b305f7da9e14715a25120a4ac7b1a4b8b97c3`. Its
[`dodge-23893-sun-overlay.dts`](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8750/blob/d50b305f7da9e14715a25120a4ac7b1a4b8b97c3/kernel_platform/qcom/opensource/devicetree/oplus/dodge-23893-sun-overlay.dts)
explicitly covers projects 23893/23894/23895, including regional charging and
eSIM wiring. This is a source reference, not proof that the source revision
exactly reproduces the installed kernel.

Both fastboot modes were reached and returned successfully to Android:

- Bootloader: `unlocked: yes`, slot `a`; `max-fetch-size` unavailable and
  `fetch boot_a` unsupported.
- Userspace fastboot: `is-userspace: yes`; reports `fetch not supported on user
  builds`. The attempted vendor_boot export produced no valid backup.

## RAM boot and root checkpoint

`fastboot boot` successfully loaded the authenticated, unchanged stock boot
image and returned to Android. A second RAM-only image combined the same stock
kernel with a Magisk-patched generic ramdisk; Android booted, but root did not
initialize. This is consistent with the installed separate init_boot supplying
the ramdisk. It does not establish that an arbitrary Linux image can boot.

The owner explicitly authorized device flashing with recoverability preserved.
Magisk v30.7 was downloaded from its official GitHub release, its release digest
checked, and the authenticated stock init_boot patched on this same handset.
Patch options kept verity and force encryption and did not change VBMeta flags.
At that checkpoint only `init_boot_a` was flashed. Android completed boot, Magisk reports 30.7,
and its daemon is running. After a separate explicit approval, the Shell root
grant was enabled and `/debug_ramdisk/su -c id` returned uid 0. The binary is
outside the normal ADB PATH.

- Stable APK SHA256: `e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5`
- Patched init_boot SHA256: `161e4178fddfefd6b61b0d6011df9372806d6739cbd3cce42049816aa577a981`
- Local evidence: `out/stock-ram-boot-probe.json`, `out/root-ram-probe/`,
  `out/magisk/install-log.json` and `out/root-stock-*/manifest.json`.

No boot-chain or firmware downgrade was performed. Both slots retain their
stock boot kernels and recovery images; neither is claimed as a tested fallback.
The rooted capture helper only reads the phone and stores sensitive output in
Git-ignored directories. It hashes each partition before and after transfer;
`stable_verified` requires both hashes, the host hash and byte count to agree.
Live-changing partitions are explicitly marked rather than called verified.

To undo this specific root flash from the bootloader, first verify the serial,
product `sun`, unlocked state, slot `a`, and the original init_boot digest above,
then flash the saved exact-build `init_boot.img` to **init_boot_a** and reboot.
The exact stock init_boot restoration has now booted Android successfully,
with Magisk absent from the process list. Do not relock the bootloader or
restore factory/NV partitions as part of that operation. Removing root will
remove the privileged read path needed for the remaining capture work.

## Native installation checkpoint, October 12

The owner authorized all required flashing on this dedicated project handset.
Android userdata (`sda15`, 493,792,325,632 bytes) now contains the native ext4
root, label `omarchy-op13`. The stock photos were independently backed up
before unlocking. Native installation verified a remounted 16 MiB write,
copied the prepared Arch system, passed `e2fsck -fn`, checked installed file
hashes and confirmed all eight boot-chain partition hashes remained unchanged.
Probe 18 starts this storage root and desktop automatically from a RAM boot.

The minimal probe 18 wrapper was flashed to `boot_a` once. Normal boot fell
back to fastboot, and RAM boot then failed at the bootloader's load/authentication
step. The original exact-build 96 MiB stock boot image was restored, after
which the same native RAM image booted successfully again. No VBMeta,
vendor_boot, DTBO, recovery, radio or calibration partition was flashed.

Probe 19's compressed, stock-metadata image was **only RAM-booted**. The
bootloader accepted it but native USB never appeared. An early boot hang can
precede the userspace recovery timer; no remote reset is available without a
USB transport. The owner subsequently recovered it with a physical reset. Before any new
recovery attempt, inspect bootloader identity and slot state. Do not accept a recovery data-format prompt: that would remove
the installed Linux root. Do not relock.

The known working recovery image is `out/native-persistent18/boot.img`, SHA256
`d5e978195b145d9667b2500704c277c9f39fb7de9eb5097f25f072f8f62a9fb5`.
It is a **RAM-boot image only**, and depends on the installed native root.
The standalone original RAM rootfs archive and earlier probes remain saved.
The kernel in this image is probe 18, even though its root is on storage.
The newer probe 20 USB image below is also a tested RAM recovery path.

Clean native reboot must run outside the Arch chroot. The reviewed
`scripts/native-reboot.sh` stops chroot processes, syncs and unmounts all native
root mounts before requesting bootloader/restart/poweroff. Bootloader return
and restart were exercised, as was clean poweroff followed by the owner
pressing Power to boot the installed desktop. Do not invoke inherited Pixel or systemd
shutdown helpers on this bootstrap.

`pack-persistent.py` requires the exact stock boot SHA and refuses to cross its
36,753,408-byte original-image boundary. It preserves the stock AVB metadata
with a deliberately stale kernel digest: this is for unlocked development,
not a signed or relockable image. Raw Image is now the default; LZ4 remains an
explicit unvalidated experiment. The `--metadata unsigned` mode instead creates
a correct boot payload hash with stock build properties, without changing the
separate VBMeta partition. This mode is used in the validated installed image.


### Uncompressed probe 20 and diagnostic revision

Probe 20 (`out/native-persistent20/boot-persistent.img`, SHA256
`bebd4ad4bb0d8edba85faf44890ed6ba8f7be5273073a616c893258a01c611ca`)
RAM boots successfully. Its normal boot test returned to fastboot without USB.
The exact stock boot was restored and native RAM boot succeeded again.

The diagnostic image `out/native-persistent20-logging/rehashed/boot.img`, SHA256
`9872a64e89716f379ebd81e99f65ae280f89e2d00aaa7ba5f9b6f576eb670553`, adds
UFS-enumeration protection and early durable logs. Its AVB metadata is newly
unsigned, with a correct hash over the padded payload and the stock OS/build
properties. `avbtool verify_image` passes its internal hash check; this is not
an OEM signature or proof of acceptance by the bootloader. Its normal boot also
returned to fastboot, while RAM boot succeeds. It retains the probe 20 module
ABI. Stock `boot_a` restoration is verified by full partition readback.

A separate, **unflashed** test changes byte 123 of the exact 64 KiB active
`vbmeta_a` from 0 to 2, setting only AVB `VERIFICATION_DISABLED`. Automatic
approval review initially rejected that flash. The owner subsequently approved
the specific test, but early logs proved that Linux had booted and USB reset
was the failure. The AVB experiment was abandoned without flashing it.
Stock VBMeta readback remains
`3f8e42997ebad9db1104af3146060cebe69c8db398620e01330df150d4b73792`.
The prepared modified file hashes to
`ce71296350865d0ffbf18d01f1a85ac2e5abe6f022bee25dfc63e31c8c6904f3`.
Both full-size files are retained under the diagnostic output directory for
audit; the modified VBMeta is unnecessary and is not part of the installation.

The flag's behavior is defined in upstream
[libavb's verification path](https://android.googlesource.com/platform/external/avb/+/master/libavb/avb_slot_verify.c).
The diagnostic normal-boot log rules out AVB policy as the cause of that
startup failure. No changes to ABL, UEFI, recovery, DTBO, vendor boot,
radio firmware or calibration are needed for this proposed experiment.

### Installed native boot and slot success

The current image is `out/native-persistent22-audio/boot-persistent.img`, SHA256
`31c8b49bf190b10c5f3b3fcf33814f88e6037fa12deaee5a60601c94daf12c11`, built from
the `out/linux-op13` tree. It adds audio ([audio](audio.md)) and describes
Bluetooth in the base tree, so the Bluetooth startup no longer loads the
runtime overlay (older images still do). Its DTB is packed with 4 KiB of
padding for the early handoff's in-place edits.
The previous image is `out/native-persistent21-panel/boot-persistent.img`, SHA256
`e2a48233f8ae412739c6ca97e896ef41382d0da6a3fcc489f36b939b4f2fb40c`.
It adds DSI panel control (backlight device, panel sleep while the screen is
off; see [panel](panel.md)) to the Bluetooth image, with the same kernel and
device tree. `scripts/op13-boot.py` performs the identity-checked
fastboot/RAM-boot/flash cycle hands-off.
The one before is `out/native-persistent20-bt-uart/boot-persistent.img`, SHA256
`f981a3532097eb8bf2978b35dc1af999b08e32078d467771e895b2683eb4c8ea`.
It adds only the boot-time UART alias to the tested Peach PCIe/DWC MSI tree.
Normal boot starts native Wi-Fi and experimental Bluetooth from verified
packages on the installed root, and reconnects the saved Wi-Fi network.
Full boot readback matches; the seven other
checked active boot partitions still match their original baseline.
The previous Wi-Fi image is `out/native-persistent20-pcie-msi/boot-persistent.img`,
SHA256 `3aba4fe36fecfa44e53701d14423a1c40bf6b0666b337400ce42b23f6bb1ef85`.
It lacks the UART alias, so Bluetooth startup is safely skipped.
The earlier image is `out/native-persistent20-earlyflip/boot-persistent.img`, SHA256
`c25e21e52f951db43c2e0af6c5c6177fc609818a30ae238214a41f84e6952313`.
It adds tested 120 Hz flip notification after TE/address latch, preserving DMA
buffer lifetime until transfer completion. An earlier working image remains
`out/native-persistent20-usb/boot-persistent.img`, SHA256
`660133e3fd406c79b55c8be5a2ae80205f705dbe2dd99f87ac5f768e19b08fab`.
All four are tested RAM recovery images.
RAM boot and repeated normal boot pass with stock VBMeta unchanged. The
USB2-only DT now explicitly selects `qcom,select-utmi-as-pipe-clk`; otherwise
DWC3 resets timed out on normal boot despite working after fastboot had
initialized the controller. The local desktop no longer requires USB SSH.

Only native `boot_a`, the previously patched `init_boot_a`, and native userdata
are intentional payload changes. Full active boot readbacks verify the native
image and seven unchanged baseline partitions. The successful-slot update
changes `boot_a` attribute bit 54 in both GPT copies and their dependent CRCs.
It does not change partition addresses or the other slot. Fastboot confirms
`slot-successful:a: yes`, `slot-unbootable:a: no`, retry count 6 after the Bluetooth
image's normal-boot validation. Flashing reset the success flag; it was marked
again only after verifying the new image boots normally.

[mark-boot-successful.py](../scripts/mark-boot-successful.py) defaults to a dry
run. It requires the exact private original GPT capture and installed boot
hash, checks both header/table CRCs, requires active bootable slot A, and rejects
layout or unrelated byte changes. Before writing it saves and fsyncs the
current metadata; it updates and verifies the backup copy first, then primary,
and restores the disk read-only flag. Seven host tests cover corruption,
copy disagreement, wrong slot state, byte preservation and idempotence. The
actual first update's readback equals the original stock GPT capture byte for
byte. This follows Qualcomm's
[boot-control success operation](https://android.googlesource.com/platform/hardware/qcom/bootctrl/+/refs/heads/android14-qpr2-s5-release/boot_control.cpp).
Never mark an unvalidated normal boot successful based only on a RAM test.

For recovery, enter fastboot, verify identity and slot A, and RAM boot the
known image above. If an experimental boot image prevents even RAM boot,
restore the exact authenticated stock `boot_a` first, then RAM boot the native
recovery image. Restoring stock boot alone does not restore Android userdata.
Do not flash ABL/UEFI, change slots, relock, or restore calibration for this
native recovery procedure. Physical power-off/on validation also passed with
the installed Wi-Fi image; automatic reconnect and HTTPS passed, and the owner
confirmed the desktop and idle/wake response.

Wi-Fi is optional and starts after the desktop. Its early guard requires the
Peach device-tree nodes, DWC MSI, and the normal IOMMU mapping. Booting the
earlier rollback image therefore skips wireless initialization. An interrupted
Wi-Fi initialization leaves `/var/lib/omarchy-mobile/oneplus13/wifi-starting`
on the handset, preventing retries on subsequent boots until explicitly cleared.
Inspect `/run/oneplus13-wifi.log` before clearing it. Do not remove factory
calibration or change rail settings to recover a failed Wi-Fi start.

Bluetooth startup is also optional. It waits for Wi-Fi initialization and
requires the UART alias, exact base DT references and verified module/firmware
hashes. An interrupted start leaves
`/var/lib/omarchy-mobile/oneplus13/bluetooth-starting`; inspect
`/run/oneplus13-bluetooth.log` and `/run/oneplus13-bluetoothd.log` before clearing
it. Reboot removes the retained UART overlay; do not force-unload it.
Bluetooth initialization, discovery and UI power cycling are tested, but
accessory pairing/audio remain pending. See [Bluetooth details](bluetooth.md).
