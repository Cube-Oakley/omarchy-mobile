# Omarchy Mobile

The Omarchy desktop experience on Linux phones: Arch Linux ARM, Hyprland, GPU
rendering and a touch-oriented Quickshell interface. One mobile shell and one OS,
brought up handset by handset, with the kernel, firmware and power work for each
phone kept in its own device directory.

**Status: experimental, October 11, 2026.** Two phones run the shared shell;
OnePlus 13 native Linux bring-up is underway. These are development
devices, not general-purpose installation images.

**Project plan and status tracker:** [`plans/`](plans/index.html), with a page per
device, a side-by-side comparison, and the shared software and desktop integration.

## Devices

| Device | SoC | Where it is | Details |
|---|---|---|---|
| OnePlus 7 Pro (`guacamole`) | Snapdragon 855 · Adreno 640 | Boots from internal storage into the full touch shell on the GPU; Wi-Fi, audio, Bluetooth, sensors and the rear cameras work; cellular waits on a SIM | [README](devices/oneplus7pro/README.md) · [status](devices/oneplus7pro/docs/status.md) |
| Pixel 7 Pro (`cheetah`) | Google Tensor G2 · Mali-G710 | Mainline Linux boots from internal storage into the shared shell at 120 Hz on the GPU; Wi-Fi, Bluetooth, audio, sensors, cellular data, texts and calls, all four cameras, fingerprint unlock, deep sleep and Android apps (Waydroid) work; GPS and power tuning continue | [README](devices/pixel7pro/README.md) · [status](devices/pixel7pro/docs/status.md) |
| OnePlus 13 (`dodge`) | Snapdragon 8 Elite · Adreno 830 | Native persistent boot, station Wi-Fi and Bluetooth discovery verified; GPU desktop, touch, battery and motion sensors partly working. Bluetooth pairing/audio and hardware parity pending | [README](devices/oneplus13/README.md) · [status](devices/oneplus13/docs/status.md) |

## The shared shell

Real captures from the Pixel 7 Pro on October 8, 2026, from the development
build (the public tree is synced from it periodically). The OnePlus 7 Pro runs
the same shell source.

<table>
<tr><th>Lock screen</th><th>Home screen</th><th>App switcher</th><th>Quick settings</th></tr>
<tr>
<td><a href="devices/pixel7pro/docs/images/lock.jpg"><img src="devices/pixel7pro/docs/images/lock.jpg" width="180" alt="Lock screen with the clock, the date and the fingerprint ring"></a></td>
<td><a href="devices/pixel7pro/docs/images/home.jpg"><img src="devices/pixel7pro/docs/images/home.jpg" width="180" alt="Home screen with the clock and weather widget and app icons"></a></td>
<td><a href="devices/pixel7pro/docs/images/switcher.png"><img src="devices/pixel7pro/docs/images/switcher.png" width="180" alt="App switcher with live app cards"></a></td>
<td><a href="devices/pixel7pro/docs/images/shade.png"><img src="devices/pixel7pro/docs/images/shade.png" width="180" alt="Notification shade with quick toggles, brightness and status tiles"></a></td>
</tr>
<tr><th>Wisp, the on-device assistant</th><th>Keyboard</th><th>Android apps (Waydroid)</th><th>Themes</th></tr>
<tr>
<td><a href="devices/pixel7pro/docs/images/wisp.png"><img src="devices/pixel7pro/docs/images/wisp.png" width="180" alt="Wisp answering a question with a language model running on the phone"></a></td>
<td><a href="devices/pixel7pro/docs/images/keyboard.png"><img src="devices/pixel7pro/docs/images/keyboard.png" width="180" alt="The shell's own touch keyboard"></a></td>
<td><a href="devices/pixel7pro/docs/images/android.png"><img src="devices/pixel7pro/docs/images/android.png" width="180" alt="F-Droid running as its own window through Waydroid"></a></td>
<td><a href="devices/pixel7pro/docs/images/themes.png"><img src="devices/pixel7pro/docs/images/themes.png" width="180" alt="Omarchy theme picker in Settings"></a></td>
</tr>
</table>

Screenshots hide personal details: the network name and notification contents
are hidden, and the home screen layout and chat are examples.

Touch controls and the rest of the shell are described in the
[OnePlus 7 Pro README](devices/oneplus7pro/README.md#touch-controls) and
[`overlay/mobile/`](overlay/mobile/README.md).

## Layout

```text
overlay/mobile/          shared Quickshell UI, gestures, themes, settings and helpers
tests/                   shared shell tests (Python backends, QML touch components)
docs/                    project-wide architecture, roadmap, shared-OS plan, publishing
plans/                   the status tracker (static HTML, opens from disk)
devices/<device>/        one bring-up workspace per phone:
  README.md                what works on that phone and how to connect
  adapter/                 files installed into that phone's root: session setup,
                           scale, power, audio and sensor glue
  kernel/                  kernel patches, device-tree overlays, configurations
  scripts/                 host-side build, transfer, guarded boot/flash, diagnostics
  tests/                   hardware-policy checks
  docs/                    status, dated bring-up notes, recovery instructions
  out/  .work/             ignored local build output, firmware and kernel trees
```

Shared shell, input and service policy belongs in `overlay/mobile/`; anything tied
to one phone's hardware belongs in its device directory. See the
[mobile architecture](docs/mobile-architecture.md) and the
[shared OS plan](docs/shared-os-20260925.md), whose `os/`, `packages/` and
`sources.lock` parts are not built yet.

## Working on a device

Run a device's tools from its directory, or by path; each script finds its own
device workspace, and `out/` and `.work/` stay per device:

```sh
cd devices/pixel7pro
python scripts/boot-pixel-shell.py
```

Handset serials, firmware, backups and raw logs are local configuration, never
checked in. Set `PHONE_SERIAL`, or put the serial on one line in the ignored
`devices/<device>/out/device.serial`. Flash helpers keep their image, hash and
slot checks. Device docs describe historical states as well as the current one;
they are not installation instructions for an arbitrary phone.

Shared shell tests need no phone:

```sh
for t in tests/test_*.py; do python3 "$t"; done
tests/run_touch_qml.sh
```

## Foundations and references

- [Omarchy](https://omarchy.org/) — desktop conventions, themes and applications.
- [Arch Linux ARM](https://archlinuxarm.org/) — AArch64 userspace and packages.
- [Omarchy CM5](https://github.com/TensorFleet/omarchy-cm5) — related handheld work.
- Per-device kernel and bring-up references are listed in each device README.

Upstream licenses and attribution remain with the corresponding source files.
Proprietary firmware and device-specific calibration/backups are not included.
Publication rules are in [docs/publishing.md](docs/publishing.md).
