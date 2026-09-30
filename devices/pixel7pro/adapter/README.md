# Pixel 7 Pro development adapter

These files connect the shared Omarchy mobile overlay to the verified Pixel
native Hyprland session. They are source-development artifacts, not host desktop
configuration. Install them only inside the target Arch root.

- `telephony.json`: selects the Pixel SIT backend for Phone and Messages.
  Requires `modem/app_service.py`. The optional private modem bundle and boot
  manager provide automatic startup; this adapter only selects the app backend.
- `reboot.sh`: orderly Restart and Restart to bootloader actions for the
  shared power menu. Hold the power button for two seconds to open it; a tap
  still controls the display. Uses `pixel-reboot`, which stops the modem and
  lets PID 1 stop writers and sync storage before restarting.
- `power-suspend.sh`: delegates to `pixel-suspend` for guarded s2idle. The
  shared shell owns idle policy; the helper refuses while plugged in or in a
  call and requires the validated persistent modem/suspend bundle.
- `mobile.json`: display scale, keyboard dimensions and common shortcut settings.
  The display driver's preferred mode is used; no OnePlus 90 Hz assumption.
- `desktop-prepare.sh`: sources the shared generated mobile config into
  `/root/pixel-hyprland.lua` after checking a validated Pixel kernel and RAM or marked persistent root. No register,
  partition, clock, charging, suspend or radio operations.
- `quickshell-bootstrap.qml`: optional entry point that inherits the caller's
  Wayland environment. The tested host-driven entry is
  `scripts/pixel-mobile-start.sh`, which discovers the live compositor instance.
- `kernel/`: pinned mainline base, complete bring-up patches and configurations.

Shared shell source remains owned by the common mobile layer. Both devices now use `overlay/mobile/` in the merged repository. The current
Pixel test installs that shared source with this device adapter. Do not copy
OnePlus hardware control adapters here.

| Capability | Current state |
|---|---|
| Native CPU/userspace | Verified, Arch Linux ARM on ext4 userdata, RAM recovery PID1 |
| Internal display | Native DMA scanout; clean 60/120 Hz with integrated memory bandwidth floor |
| Hyprland/mobile shell | Mali-G710 rendering; fullscreen animation verified at 119.6–120.2 fps |
| USB serial/Ethernet/SSH | Verified |
| Development keyboard | Verified USB-fed uinput; on-screen keyboard renders |
| Physical touch | Temporary GPIO SPI input; v18 packet readiness fix under validation, hardware SPI pending |
| GPU, full panel/power control | GPU rendering works; full display power/PHY ownership remains unfinished |
| CPU/thermal | Bounded cpufreq, schedutil and seven thermal zones verified |
| Battery/charging | Not brought up |
| Wi-Fi, Bluetooth, cellular, audio, camera | Not brought up |
| Persistent storage/native install | UFS read/write checks pass; installed Arch desktop validated; saved files survive reset/recovery RAM boot; H RAM-tested and boot_a verified, H normal reboot untested; cold shell layers transparent |
| Wired external display | Not a requirement for this device |

See [the working checkpoint](../docs/hyprland-mobile-20260925.md),
[current status](../docs/status.md), and
[physical touch checkpoint](../docs/touch-bringup-20260925.md).
