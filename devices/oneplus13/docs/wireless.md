# OnePlus 13 wireless bring-up

This is experimental CPH2655 station support. Scanning, association and traffic
now work from the installed native image, including automatic reconnect after
normal boot. Keep the previous earlyflip image and stock
backups described in [recovery](recovery.md).

## Verified on the handset

1. The private Peach power-sequencing profile votes the six stock rails and
   RF_CLK1 before raising GPIO16. PCIe0 and its PHY enumerate `17cb:110e`,
   revision 01, at Gen1 x2. AER counters were zero before firmware loading.
2. GIC ITS routing accepted the initial firmware stage but delivered no MHI
   interrupts. Removing only PCIe0's `msi-map` and `msi-map-mask` selects the
   existing DWC internal MSI interrupts. The IOMMU mapping stays enabled.
3. With DWC MSI, the stock `peach/amss20.bin` reaches MHI mission mode in about
   0.6 seconds. QMI returns chip ID 2, family 4, board ID ff, SoC 40210200 and
   firmware version 10097fff. Mainline MHI already handles this image's second
   ELF header; no MHI image-format patch is needed.
4. The stock-init probe accepts the exact US `bdwlan.b0a`, `regdb.bin`,
   `phy_ucode20.elf`, `aux_ucode20.elf` and `qdss_trace_config_v2.cfg` and receives
   firmware-ready. The optional TME RAM patch returns result 1/error 83, also
   present in both saved Android boots. Only that observed failure is tolerated.

The firmware-only probe blocks all WLAN-mode commands and ignores firmware-ready
for normal driver startup. The separate opt-in calibration probe now completes
stock calibration in approximately 8.4 seconds, sends OFF, and unloads cleanly.
It reports zero downloaded calibration-file bytes, matching the stock BDF
CalDB=0 path; no factory calibration is modified. QMI transactions may return
a positive decoded-message length on success, which the probe normalizes.

The station probe adds an MHI power cycle after calibration/OFF, retaining the
DMA buffers and advertising cal_done=1 on the next firmware boot, as CNSS does.
Without that restart, the firmware times out on the next INI command. With it,
the normal firmware, CE and WMI startup reaches Linux interface registration.
The initial station-only profile had an invalid interface-combination list;
a corrected profile omits combinations for a single station. The borrowed
WCN7850 radio-kill GPIO was also disabled: the phone has no such switch.

The first passive scan found 63 BSS entries across 2.4, 5 and 6 GHz. The driver
retains the stock self-managed US regulatory domain and uses signed Linux
regulatory data. It logs a rejected country update and a regulatory update
timeout during association; the existing firmware domain remains in force.
No country or power restrictions were overridden.

On the owner's selected network, 5 GHz WPA2 association, DHCP, DNS and HTTPS
passed. The initial DHCP attempt stalled; retrying with NetworkManager restarted
succeeded, and a subsequent disconnect/reconnect also passed. A 32 MiB transfer
in each direction verified matching SHA256 hashes, at about 93 Mbps download
and 82 Mbps upload on this test path. This is not a maximum-throughput result.
There were no new kernel faults during scanning, association or transfers.
A subsequent clean RAM boot and installed normal boot both have taint 4096
(out-of-tree modules only), with no kernel WARN/OOPS. Screen-off/wake preserved
12 consecutive ping replies and a later HTTPS request. Deep suspend remains
disabled and is not covered by that test.
Physical power-off followed by the owner's Power-button boot also passed:
Wi-Fi reconnected automatically, HTTPS passed and the desktop stayed responsive
through its normal idle screen-off and wake. No kernel warning taint was added.

Factory MAC retrieval through modem DMS is not implemented. Firmware returns
a placeholder; before interface-up, the probe replaces it with a random local
unicast identity saved privately on the handset. NetworkManager's default for
new profiles is a stable address per network. Password entry uses the existing
on-phone helper; credentials are not copied into this repository.

Twenty selected TCL/REO/WBM register offsets match the official Peach v2
headers. The basic TCL/REO/WBM descriptors are 32 bytes and CE source descriptors
16 bytes, matching the current HAL. Other ring/descriptor details remain under
validation. Stock exposes eight TCL banks; the borrowed profile uses seven,
a conservative subset. Station probes disable AP/monitor support, idle power
saving, suspend and automatic crash recovery.

## Firmware-only shutdown

Removing the initial capabilities-only driver skipped hardware-group power-down
because that group never started. Reloading left a stale MHI IPCR child and
produced a QMI/MHI use-after-free fault and an IOMMU fault. The test was recovered
by syncing and remounting the native filesystem read-only before bootloader
restart. Do not repeat an unload/reload cycle with that probe.

The first correction still ran cleanup for an uninitialized hardware data path,
and the normal power-down guard skipped MHI because CE initialization had not
completed. Removal reset the phone, which recovered into installed Omarchy.
The corrected probe skips that group cleanup and checks MHI power-on state
independently of CE initialization. Two firmware-ready/unload cycles now pass
in the same boot, with no stale IPCR child or new kernel taint. These fixes are
scoped to the firmware-only Peach profile; revisit them when enabling its normal
data path. The station version uses normal removal once registered. A registered
interface unload passed before connection testing; removal after connected
traffic also passed in a clean boot, without new taint or a stale MHI child.

## Source and artifact map

- `kernel/sm8750-oneplus13-pcie.dts`: optional rails, power sequence and PCIe PHY.
- `kernel/sm8750-oneplus13-pcie-msi.dts`: DWC MSI selection.
- `kernel/peach-pwrseq-enumeration.patch`: private PMU binding and PCI endpoint.
- `kernel/peach-ath12k-bootprobe.patch`: capabilities-only ath12k profile.
- `kernel/peach-ath12k-stock-init-probe.patch`: incremental stock firmware setup
  and experimental shutdown changes. Apply after the bootprobe patch.
- `kernel/peach-ath12k-calibration-probe.patch`: opt-in one-shot calibration
  extension; completion/OFF/unload verified. Mission mode remains blocked.
- `kernel/peach-ath12k-station-probe.patch`: experimental station startup after
  calibration and MHI restart. Apply after the calibration patch. Both module
  parameters must be enabled explicitly. All four patches apply cleanly to the
  original ath12k sources and reproduce the nine modified files that were built.
- `scripts/start-native-wifi.sh`: optional startup with handset, kernel,
  DWC MSI, IOMMU and file-hash checks. A persistent interrupted-start marker
  prevents repeating a failed initialization on the following boot.
- `adapter/NetworkManager-oneplus13.conf`: keeps USB unmanaged and disables
  Wi-Fi power saving until it is validated.

The validated MSI image, retained as a Wi-Fi rollback, is
`out/native-persistent20-pcie-msi/boot-persistent.img`, SHA256
`3aba4fe36fecfa44e53701d14423a1c40bf6b0666b337400ce42b23f6bb1ef85`.
Its actual embedded DTB was produced by deleting only the two MSI properties
from the already-tested enumeration DTB. Files prefixed `compiled-` in that
directory are separate, untested build variants.
The current installed Bluetooth revision adds only `aliases/serial0` to that
tree and preserves this Wi-Fi path; its image and hash are in
[recovery](recovery.md).

The verified package is installed at `/opt/oneplus13-wifi` on the handset.
`native-persistent-session.sh` starts the shared system bus before launching
the optional Wi-Fi helper after desktop readiness. An initial normal-boot test
exposed this bus dependency; correcting the ordering restored automatic startup.
The helper checks SHA256SUMS and exposes firmware to the outer initramfs:
the kernel firmware worker cannot use the Arch chroot's path directly.
All modules match the probe20 ABI. A failure leaves
`/var/lib/omarchy-mobile/oneplus13/wifi-starting`; inspect the log and clear
that marker explicitly before retrying in a clean boot. The older earlyflip
image lacks the required DT nodes and safely fails the helper's initial guard.

Private evidence is under `out/native-persistent20-pcie`,
`out/native-persistent20-pcie-msi` and `out/wireless-survey`. Firmware comes from
the verified handset backup: `firmware.tar` and `odm_a` extracted from `super.img`.
Stock-init manifests record original paths, sizes and hashes. These files,
calibration data, logs and network credentials must remain uncommitted.

## References

The primary reference is OnePlus's
[SM8750 vendor source](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8750/tree/d50b305f7da9e14715a25120a4ac7b1a4b8b97c3/vendor/qcom/opensource/wlan),
including CNSS2, its QMI protocol definitions, Peach HAL and stock device tree.
The local source manifests pin individual URLs and hashes.

The related [pagani port](https://github.com/renhiyama/pagani-linux/commit/c49d202f6c16e2b0f9e4d762a031ebf082a7f2cc)
provided starting points for the six-rail profile and ath12k additions. Its
TME/QDSS QMI layouts match the official source. Its PDC voltage overrides and
skipped PHY firmware were not adopted: this probe follows the CPH2655 stock
rail ranges and firmware sequence. Its presence does not establish usable
OnePlus 13 Wi-Fi.
