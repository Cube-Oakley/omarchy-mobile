# Claude Opus handoff: OnePlus 13 native Omarchy Mobile

Prepared October 12, 2026, for the repository root.
Paths below are relative to `devices/oneplus13/` unless explicitly stated.
This is a handoff of the last verified state, not a fresh hardware retest.

## User direction

The user bought a dedicated used OnePlus 13 for this project and wants native,
persistent Omarchy Mobile with hardware support matching or exceeding the
Pixel 7 Pro port. They explicitly authorized required flashing, provided the
device remains recoverable. Do not ask again for blanket flashing permission.
They also explicitly approved an AVB flag experiment, but it proved unnecessary
and **was never flashed**. Stock VBMeta remains unchanged.

The latest request is to prepare this handoff for Claude Opus. **Bluetooth
accessory testing is deferred until the user is ready.** Do not immediately ask
them to provide a keyboard or headphones. When work resumes, independent work
on the remaining hardware can proceed within the existing authorization.

Original camera samples, including extreme zoom examples, were saved privately
before unlocking. The user particularly values Android's usable 120x zoom.
Preserve the samples as a quality baseline; do not promise comparable native
camera quality before implementing and measuring the pipeline.

## Read first

- [Current status](status.md), [recovery](recovery.md), and [device README](../README.md).
- [Bluetooth](bluetooth.md) and [Wi-Fi](wireless.md) contain the latest bring-up details.
- [Display timing](display-timing.md), [RTC](rtc.md), [camera baseline](camera-baseline.md).
- Private machine checkpoint: `out/NATIVE-CHECKPOINT.json`.
- Last Bluetooth health result: `out/bluetooth-survey/final-installed-health.json`.

Older sections of the recovery document describe historical Android states.
The current installation is native Linux on userdata, not stock Android.

## Current installation and recovery boundaries

Device is the US **CPH2655**, project 23893, fastboot product `sun`, unlocked,
active slot **a**. Original OxygenOS was `16.0.10.501(EX01)`.

Current image, installed in **boot_a**:

```text
out/native-persistent20-bt-uart/boot-persistent.img
SHA256 f981a3532097eb8bf2978b35dc1af999b08e32078d467771e895b2683eb4c8ea
Size   100663296 bytes
Kernel 7.3.0-rc2-omarchy-oneplus13-probe20
Last verified boot ID ebcdcf39-cc08-4b64-9dfd-3a3d14eac7f4
```

Repeated normal boots passed, full boot readback matched, and seven other
checked active boot partitions matched baseline. Fastboot independently
confirmed slot A successful=yes, unbootable=no, retry-count=6. Last health:
Bluetooth powered, UHID present, pairing agent registration passed, Wi-Fi
HTTPS returned 200, kernel taint 4096 (out-of-tree modules only).

The native root is ext4 on `/dev/sda15`, label `omarchy-op13`, UUID
`5ab004b1-1f45-4805-8649-2d800b7a0693`, size 493792325632 bytes.
**Do not format userdata or accept an Android recovery data-format prompt.**
Do not relock, switch slots, downgrade firmware, or modify boot-chain or
factory calibration/NV partitions as an ordinary bring-up/recovery step.
Neither alternate slot is a tested fallback.

Full-size stock `vbmeta_a` remains:
`3f8e42997ebad9db1104af3146060cebe69c8db398620e01330df150d4b73792`.
The earlier normal-boot failure was USB clock selection, not AVB. The fix is
`qcom,select-utmi-as-pipe-clk`; fastboot's retained USB state had masked it.

Verified rollback images (also usable for RAM recovery):

| Image under `out/` | SHA256 | Notes |
| --- | --- | --- |
| `native-persistent20-pcie-msi/boot-persistent.img` | `3aba4fe36fecfa44e53701d14423a1c40bf6b0666b337400ce42b23f6bb1ef85` | Previous Wi-Fi image; no UART alias, so Bluetooth startup safely skips |
| `native-persistent20-earlyflip/boot-persistent.img` | `c25e21e52f951db43c2e0af6c5c6177fc609818a30ae238214a41f84e6952313` | Earlier 120 Hz image |
| `native-persistent20-usb/boot-persistent.img` | `660133e3fd406c79b55c8be5a2ae80205f705dbe2dd99f87ac5f768e19b08fab` | Earlier 60 Hz USB image |
| `root-stock-20261012T054257.055213Z/partitions/boot_a.img` | `bbb4980f9efc63371268b8f7fbf78d0516e19c78a2c6a5f254170a409ebea983` | Exact original stock boot; restoring this alone does not restore Android userdata |

Authenticated stock reconstruction is also under `out/reconstructed-stock/2541/`.
Private backups include 130 raw partitions, stock super, and both GPT copies
for all six UFS LUNs. Preserve them. Do not restore calibration speculatively.

If an experimental flash prevents RAM boot, the tested recovery sequence is:
verify device identity/slot, restore exact stock **boot_a**, then RAM boot a
known native image. Read `recovery.md` before executing it. The user previously
reached fastboot by holding Volume Up + Power until black, then Volume Down +
Power. Early boot hangs can require physical intervention; do not assume a
userspace recovery timer can always recover an image.

Flashing can reset the slot-success flag. `scripts/mark-boot-successful.py`
defaults to dry run and requires the original GPT capture plus expected boot
hash. It narrowly changes the success bit and dependent CRCs, with backups and
readback. **Never mark success on RAM-boot evidence alone.** The original
combined primary/backup GPT baseline hash is
`4683b0c7d7eaf9903d61abde0539be9a43027fe08d8dc9ea10d07da0884a9f10`;
see `out/bluetooth-survey/stock-gpt-sde.bin` and the original stock GPT capture.

## Access and clean reboot

From the repository root, the existing pinned-key USB SSH wrapper is:

```sh
devices/oneplus13/out/native-probe17/arch-session/ssh 'uname -r; cat /proc/sys/kernel/random/boot_id'
```

It connects as root to `10.77.13.1`. The desktop also runs as **root**, HOME
`/root`; the unused `alarm` account is not the active desktop. Host USB is
`enp198s0f0u2`, address `10.77.13.2/30`. Existing NetworkManager profile:

```sh
nmcli connection up uuid d83e85f0-b43f-4a26-a597-26efa606b885 ifname enp198s0f0u2
```

Do not alter the host uplink `enp198s0f4u1u1`. Network/USB access may require
the execution environment's normal escalation mechanism.

Outer PID 1 is BusyBox; Arch is a chroot mounted at `/run/arch`. **This is not
a systemd-booted Arch installation.** Kernel firmware loading sees the outer
filesystem. Debugfs can be inspected from Arch through
`/proc/1/root/sys/kernel/debug`.

Serial is `/dev/ttyACM0`; use only one serial reader at a time. Clean reboot
must run in the outer environment, not via inherited Pixel/systemd helpers:

```sh
python3 devices/oneplus13/scripts/native-shell.py --wait 5 --timeout 15 \
  --log devices/oneplus13/out/handoff-reboot.log \
  --command '/bin/native-reboot --bootloader'
```

The helper also supports `--restart` and `--poweroff`; it stops chroot
processes, syncs, and unmounts the root. A serial disconnect/exit 1 during
reboot is expected. Fastboot serial is privately stored in `out/device.serial`.
Verify serial, product, unlocked state and slot before every flash; target
partitions explicitly. Do not run raw forced reboots as a routine shortcut.

## What works and what remains

| Area | Last verified state / limitation |
| --- | --- |
| Native boot | Persistent root and desktop; repeated normal boots; USB SSH independent of local desktop |
| GPU/display | Adreno 830 v2, Mesa 26.2.3; GPU capped at 342 MHz, 8 CPU cores at 2 GHz; retained bootloader DMA KMS path presents about 120.7 fps |
| Panel | Full native initialization, brightness and deep-off still pending; 120 Hz presentation is not proof of a complete native panel driver |
| Touch/buttons | S3910 SPI touch, 8 ms polling, 1 MHz, ten slots; owner confirmed volume, power, timeout/wake |
| Rotation/sensors | ADSP FastRPC accel/gyro/compass/proximity, four orientations and touch alignment verified; ambient light fails; slight transition flicker remains |
| RTC | PMK8550 software anchor survives power-off; no hardware clock write |
| Battery/USB | Telemetry, about 5 V charging, 113-second unplug/replug and responsive unplugged desktop verified; no full charging-cycle validation |
| Wi-Fi | Native autostart, 5 GHz WPA2/DHCP/DNS/HTTPS/reconnect; around 93/82 Mbps with 32 MiB hash-checked transfers; 2.4/5/6 GHz scan |
| Bluetooth | Native autostart, stock firmware, LE and BlueZ/UI discovery, three UI power cycles; UHID and D-Bus pairing-agent registration ready |
| Missing | Camera, audio, modem, GNSS, NFC, IR, fingerprint, haptics; full suspend/deep idle |

Wi-Fi physical cold boot and USB unplug/replug passed. The Bluetooth revision
has repeated normal restart validation, not a separate physical cold-power
test. Bluetooth accessory pairing, input/data, reconnect, audio, range and
suspend are **not** validated. Agent registration is not successful pairing.

## Wi-Fi implementation checkpoint

Installed package `/opt/oneplus13-wifi`; startup source
`scripts/start-native-wifi.sh`; interface `wlp1s0`; NetworkManager config
`adapter/NetworkManager-oneplus13.conf`. The owner entered the Wi-Fi password
on the phone. Keep the saved mode-0600 connection private; do not read or log
credentials. Autoconnect already works.

Private PCI `17cb:110e` Peach profile uses stock-constrained six rails,
stock firmware/US board data/regdb/PHY/AUX/QDSS, firmware-ready, mode-7
calibration (~8.4 s), then MHI restart and station mode. No factory calibration
partition writes. Four patches are `kernel/peach-ath12k-{bootprobe,stock-init-probe,calibration-probe,station-probe}.patch`.
The DWC MSI tree removes ITS `msi-map` while retaining the IOMMU mapping.
Tested module build: `out/native-persistent20-pcie-msi/ath12k-mission-probe/ath12k`.

Station only; AP/monitor, power saving and suspend are disabled/unvalidated.
Stock US regulatory domain remains; regulatory-update timeouts are unresolved.
Factory MAC retrieval remains pending; a persistent private local address is
used. Association on 2.4/6 GHz is untested despite successful scanning.

## Bluetooth implementation checkpoint

Four incremental patches apply in this order and match the tested final
driver source:

1. `kernel/peach-bluetooth-version-probe.patch`
2. `kernel/peach-bluetooth-firmware-probe.patch`
3. `kernel/peach-bluetooth-radio-probe.patch`
4. `kernel/peach-bluetooth-speed-probe.patch`

Private compatible `oneplus,cph2655-peach-bt`, enum `QCA_CPH2655_PEACH`.
All four opt-ins are required for the installed path:
`peach_version_probe=1 peach_firmware_probe=1 peach_radio_probe=1 peach_highspeed_probe=1`.
Defaults are false. Error paths power down without automatic retry or generic
firmware fallback.

UART14 is `0x898000`, live path `/soc@0/geniqup@8c0000/serial@898000`,
`ttyHS0` / `serial0-0`. GPIO24 CTS, 25 RTS, 26 TX, 27 RX; BT_EN is PM8550VE-F
GPIO3, SW_CTRL TLMM18. **GPIO16 WLAN_EN belongs to Wi-Fi.** Bluetooth takes
independent votes on six shared rails and RF_CLK1 using stock constraints.

`kernel/peach-bluetooth-alias-overlay.dts` supplies the boot-time serial alias.
GENI requires it at boot; adding it dynamically does not work. Runtime UART
overlay source/loader: `kernel/peach-bluetooth-probe-overlay.dts` and
`kernel/peach-bluetooth-probe-dt.c`. Compile the runtime overlay **without
`dtc -@`**; the base tree has no exported symbol table. The loader validates
machine, phandles, clock, bound Wi-Fi and alias. It has **no module_exit**;
reboot to remove it, never force-unload it.

ROM identity: product `0x21`, SoC `0x40210200`, ROM `0x0200`, patch `0x6d58`.
Exact stock firmware is `gngbtfw20.mbn`; board ID `0000` selects
`gngbtnv20.bin`, build `BTFW.GANGES.2.0.0-00343-PATCHZ-1_DEF`. A nonzero board
ID must select its exact file, never silently fall back to generic NVM.
NVM is loaded into controller RAM, not written to calibration storage.
Private extraction: `out/bluetooth-survey/firmware/image/`. The original 12 MiB
partition has a FAT header declaring 64 MiB; only an analysis copy was padded
for extraction. Preserve the original backup unchanged.

Important transport lessons:

- Unpaced firmware at 115200 queued behind the HCI core's two-second timer.
  Pacing unacknowledged segments 25–27 ms fixed firmware-only loading.
- Pending serial bytes then stalled close/retry. Flush serdev writes before
  dropping BT_EN. Reboot recovered the stalled experiment.
- Firmware initialization at 115200 succeeded, but idle LE discovery timed
  out despite IBS wake ACK. Do not regress to that path as working Bluetooth.
- Working 3 Mbaud path sends the Ganges peripheral baud command before the
  HCI baud command, explicitly kicks TX, uses bounded waits, switches host
  baud, and re-reads ROM identity before loading firmware (~1.5 s).
- Pinned reference patch is saved with provenance at
  `out/bluetooth-survey/rocknix-wcn7860.patch`; its generic power profile was
  not adopted. See `bluetooth.md` for the upstream link.
- Startup can log `Received HCI_IBS_WAKE_ACK in tx state 0` or
  `Frame reassembly failed (-84)` around that handshake. These remain visible;
  validated 3 Mbaud scans/power cycles had no new WARN/OOPS taint or command timeout.
- Version/firmware-only modes deliberately power off and return EOPNOTSUPP;
  the HCI core may subsequently log a reset timeout. Interpret the stage log.
- `btmgmt` may exit zero while reporting a failed command. Check status/output.
  Its `--timeout` may wait the full duration even after command completion.

## Installed Bluetooth userspace and guards

Package `/opt/oneplus13-bluetooth` contains a SHA256 manifest, KERNELRELEASE,
config diff, matching crypto/Bluetooth core/UART/protocol modules, UHID,
runtime overlay and exact stock firmware. Outer `/lib/firmware/qca` points to
`/run/arch/opt/oneplus13-bluetooth/firmware/qca`. A pre-UHID package backup is
retained at `/opt/oneplus13-bluetooth-pre-uhid`.

Source helpers: `scripts/start-native-bluetooth.sh`,
`scripts/configure-bluetooth.py`, `scripts/native-persistent-session.sh`.
Installed helpers live under `/usr/local/lib/omarchy-mobile/oneplus13/`, with
the main session named `session.sh`. Session startup creates the system bus,
starts the desktop, and runs optional helpers; Bluetooth waits for Wi-Fi.
The old session is backed up at `/var/log/oneplus13/session-before-bluetooth.sh`.

Guard files under `/var/lib/omarchy-mobile/oneplus13/`:

- `wifi-starting`: retained after interrupted Wi-Fi initialization.
- `bluetooth-starting`: retained after interrupted Bluetooth initialization.
- `bluetooth-test-address`: private mode-0600 stable test address; factory
  Bluetooth address retrieval is pending.

A retained startup marker intentionally suppresses automatic retries. Inspect
`/run/oneplus13-wifi.log`, `/run/oneplus13-bluetooth.log`, and
`/run/oneplus13-bluetoothd.log` before deciding whether to clear a marker.
The final healthy boot had no Bluetooth failure marker.

The MGMT helper checks binary response statuses and waits for index
reconfiguration/readback after setting the address. Command completion alone
does not mean the normal controller index is ready.

Installed signed packages include BlueZ/bluez-utils 5.87-2,
python-gobject 3.58.1-1, libgirepository and gobject-introspection-runtime
1.86.0-2. Gio agent registration/unregistration passed without pairing any
remote device. Shared UI source is repository-root `overlay/mobile/bluetooth.py`;
installed entrypoint `/root/.local/bin/omarchy-mobile-bluetooth`.

Pacman has Required DatabaseOptional signature policy, but its mirrorlist
still points at the old bootstrap localhost:8000 server. Do not blindly run a
full upgrade. Recent dependencies were downloaded as signed official Arch ARM
packages on the host and installed with pacman -U; caches/manifests are under
`out/bluetooth-survey/packages/` and `pairing-packages/`.

Hyprland now uses Lua. Old `hyprctl dispatch dpms off` syntax failed parsing;
do not reuse it as a verified power test. Existing timeout/power-key paths work.
`XDG_RUNTIME_DIR=/run/user/0 hyprctl -i 0 monitors -j` works.

## Build reproducibility and evidence

Base kernel directory: `out/linux-7.3-rc2`, source base commit
`5225b8eec4c9bb21aecff6295fab6346a3c3738e`. **It is an extracted tree, not a
Git checkout**; `git -C` can misleadingly return the parent repository HEAD.

The original config omitted LE/RFCOMM/BNEP/UHID. An isolated reflink/copy at
`out/bluetooth-survey/linux-le-build` applies `kernel/bluetooth.config`.
Bluetooth core, protocols and UART drivers were rebuilt together with matching
conditional headers. Do not mix old and new Bluetooth structures/modules.
Other kernel/Wi-Fi modules were left unchanged.

Authoritative final artifacts under `out/bluetooth-survey/`:

- `speed-probe/bluetooth/`: tested installed UART/QCA driver build.
- `le-probe/`: matching LE-enabled stack build/package stages.
- `uhid-probe/uhid.ko`: matching UHID module.
- `native-package/`: final package matching the phone.
- `patch-check/drivers/bluetooth/`: four patches applied to pristine sources;
  the three modified driver files matched final build sources.
- `speed-probe/le-scan.txt`, `bluez-scan.txt`, `cycles-and-adapter.txt`.
- `startup-ram-health.txt`, `startup-ui-scan.json`.
- `installed-normalboot-health.txt`, `installed-ui-scan.json`.
- `flash-bluetooth.log`, `mark-installed-success.txt`, `installed-slot-success.txt`.
- `final-installed-health.json`.

Installed image/DTB/manifests live in `out/native-persistent20-bt-uart/`.
Original initramfs source is `out/native-persistent20-pcie-msi/root`.
The original kernel tree's embedded DTB source was restored to the Wi-Fi DTB
after building the Bluetooth alias image, but cached `arch/arm64/boot/Image`
may still contain the alias image. Be explicit about build inputs and copied
artifacts; do not infer consistency from filenames or timestamps alone.

One-off builders `/tmp/op13-build-bt-*.py` and `/tmp/op13-build-uhid.py` may
remain, but are not a durable, idempotent build workflow. Some assert fresh
directories; the initial firmware builder predates pacing/flush fixes. Do not
blindly rerun them or treat them as more authoritative than final patches and
validated build copies. The final overlay source changed only in a comment
after its binary build.

Recent validation: all four Bluetooth patches applied cleanly and matched
built driver sources; startup shell syntax, Python compile, device-status JS
syntax and git diff whitespace checks passed. No active build sessions or
subagents were left running at handoff.

## Suggested continuation

1. Read this checkpoint and confirm transport/current boot with read-only
   checks before changing anything. Do not reflash merely to establish context.
2. Respect the deferred accessory test. Independent investigation can focus
   on audio, cameras, modem or completing native panel support. Compare the
   Pixel 7 Pro architecture, but do not copy board-specific rails/drivers blindly.
3. Audio's AW882xx speakers need correct protection/calibration; do not
   randomly unmute amplifiers. Haptics uses PMIH0108 and needs a real driver.
   Camera reference consists of 13 originals (~60.5 MB) and ~1.92 GB of saved
   stock camera assets; follow `camera-baseline.md` to locate them privately.
4. When the owner is ready, pair a selected accessory and verify actual input
   or data, reconnect and persistence. Bluetooth audio additionally depends on
   the audio stack; discovery alone is not enough. Modem testing will later
   require the owner's SIM participation.
5. Track remaining factory Wi-Fi/Bluetooth address retrieval, longer coexistence
   tests, 2.4/6 GHz association, regulatory handshakes, native panel control and
   suspend. CPU retention is the current limit; APSS deep idle is not enabled.

## Repository and privacy

At handoff, root `README.md` and `plans/assets/data/devices.js` are modified;
the entire `devices/oneplus13/` tree and
`plans/assets/data/device-oneplus13.js` are untracked accumulated port work.
**Do not reset or clean these files.** No commit or push was made for this
handoff. Preserve source, private backups and untracked build artifacts.

`out/` is ignored and contains serials, firmware, stock captures, logs, sample
photos and other device-specific data. Keep those out of version control.
Do not publish credentials or private address/identity files. This handoff
contains references rather than their contents.
