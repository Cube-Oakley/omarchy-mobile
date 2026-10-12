# OnePlus 13

**Native persistent boot verified, October 12, 2026.** This is a
development port, not a release image. The shared Arch/Hyprland desktop runs
from an installed ext4 root with Adreno 830, native DMA scanout, touch, battery
telemetry and core CPU idle. Accelerometer, gyroscope, compass and proximity
streams have been sampled; the shared rotate button and touch alignment are
physically verified, with automatic sensor startup. The installed compositor
now presents at about 120.7 fps; the owner confirms significantly smoother
interaction, with slight flicker during orientation changes. Native station
Wi-Fi reconnects automatically after normal boot; 5 GHz Internet access,
reconnect and bidirectional transfer integrity are verified. See
[wireless bring-up](docs/wireless.md) for its remaining limitations.
Experimental Bluetooth starts automatically; LE discovery, mobile-shell
scanning and power cycling pass. Pairing and audio remain pending; see
[Bluetooth bring-up](docs/bluetooth.md).
Brightness now follows the shell's slider and the panel sleeps while the
screen is off ([panel](docs/panel.md)); the vibration motor works through the
shell's force-feedback path. Speakers and the main microphone work through
PipeWire ([audio](docs/audio.md)). Cold panel initialization, suspend and the
remaining peripherals are unfinished. Pixel 7 Pro parity is the acceptance target.

**Current recovery checkpoint:** the native kernel is installed in `boot_a` and
starts the installed desktop directly. Repeated normal boot and clean fastboot
return passed, as did clean shutdown followed by physical Power-button boot
with Wi-Fi automatically reconnecting.
Bootloader slot A is marked successful and independently
confirmed by fastboot. Stock `vbmeta_a` and its verification policy are unchanged.
Stock boot/recovery images and RAM recovery images remain saved. See
[recovery](docs/recovery.md) before restarting or flashing.

The surveyed handset is **CPH2655 / OP5D55L1**, Qualcomm **SM8750** (Snapdragon
8 Elite / Adreno 830), platform `sun`. Android projects call this model `dodge`.
It arrived on OxygenOS `CPH2655_16.0.10.501(EX01)`, Android 16, with the
2026-08-01 security patch and Linux 6.6.118. Stock uses slot A.

See [status and next steps](docs/status.md) and the
[camera reference plan](docs/camera-baseline.md). The bootloader is now verified
unlocked; [matching stock boot/recovery images](docs/recovery.md) have been
reconstructed and authenticated locally. Stock RAM boot is verified; an
authorized Magisk `init_boot_a` flash booted Android and enables read-only
calibration backups. The native root now occupies Android userdata and starts
the desktop automatically under the installed native kernel. Remaining
hardware support is still in development. Wall time survives reboot using the
PMK8550 RTC counter and a native-root software offset.

## Local stock capture

Put the ADB serial on one line in ignored `out/device.serial`, or set
`PHONE_SERIAL`. Run from any directory:

```sh
python devices/oneplus13/scripts/capture-stock.py --camera-assets
```

The helper checks CPH2655/SM8750 identity, reads diagnostics, copies DCIM and
Pictures without re-encoding, and verifies their SHA256 hashes against the
phone. `--camera-assets` additionally saves the readable stock camera tuning,
libraries and APK. Omit it when refreshing photo samples. Each run creates a
new timestamped directory under ignored `out/` with operation results and local
integrity hashes. Phone-side permission failures are retained as partial probes.
Raw logs, photos, EXIF, proprietary assets and handset identifiers stay local.

This helper never reboots, unlocks, roots, flashes, or writes to the phone.
Its output is **not a full partition backup**. Locked production Android denies
raw boot/firmware/calibration reads; those need a separate recovery procedure.

## Source candidates

- [OnePlus SM8750 manifest](https://github.com/OnePlusOSS/kernel_manifest/blob/oneplus/sm8750/oneplus_13_b.xml):
  Android 16 kernel, modules and device trees. This is a hardware reference;
  its branch name alone does not prove a match to this handset's build.
- [Original OnePlus 13 Linux work](https://github.com/sm8750-mainline/linux/tree/OnePlus-13-WIP),
  inspected at `1f2047e8663eda76c8b172fd3e040e3b3fa17950`: board memory
  layout, inherited framebuffer and early platform work. Its fixed carveouts
  must be checked against this handset before use.
- [Additional SM8750 bring-up work](https://github.com/skb8/linux-sm8750/tree/oneplus-13-sm8750-bringup-v2):
  useful downstream mapping, but its checks include configured or expected
  support, and its surveyed handset is PJZ110. Do not treat those checks as
  runtime evidence for this CPH2655.
- [LineageOS device metadata](https://github.com/LineageOS/lineage_wiki/blob/main/_data/devices/dodge.yml):
  lists CPH2655 and `fastboot flashing unlock`; Android recovery reference,
  not an Omarchy installation procedure.

The tested native boot image is installed in slot A. Device-specific work belongs here;
shared shell changes belong in `overlay/mobile/`.
