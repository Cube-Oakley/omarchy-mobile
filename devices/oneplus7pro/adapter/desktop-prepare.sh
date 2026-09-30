#!/usr/bin/env bash
# touch1 restores the hardware baseline at boot; append our persistent override.
set -euo pipefail
export XDG_RUNTIME_DIR=/run/user/0
# Persistent board adapter for the frozen native4 initramfs. The worker waits
# for charging support and never blocks the desktop; its own lock avoids repeats.
if [[ -x /usr/local/sbin/guacamole-radio-start &&
      -f /root/radio-bringup/autostart-enabled ]]; then
    nohup /usr/local/sbin/guacamole-radio-start \
        >> /root/radio-bringup/logs/startup.log 2>&1 < /dev/null &
fi
# SSH over Wi-Fi, the recovery shell kept to USB, and USB failure evidence.
if [[ -x /usr/local/sbin/guacamole-remote-access ]]; then
    nohup /usr/local/sbin/guacamole-remote-access > /dev/null 2>&1 < /dev/null &
fi
# Charging with the stock profile (4.39 V, input by charger type) and the
# saved charge limit, in place of the built-in 500 mA / 4.20 V bring-up policy.
if [[ -x /usr/local/sbin/guacamole-charging-start &&
      -f /root/power-bringup/charging-enabled ]]; then
    nohup /usr/local/sbin/guacamole-charging-start > /dev/null 2>&1 < /dev/null &
fi
if [[ -x /usr/local/sbin/guacamole-audio-start &&
      -f /root/audio-bringup/autostart-enabled ]]; then
    nohup /usr/local/sbin/guacamole-audio-start \
        >> /root/audio-bringup/startup.log 2>&1 < /dev/null &
fi
# CPU frequency scaling (qcom-cpufreq-hw) as a runtime overlay on kernels whose
# boot DTB leaves it off (before #191); without it every core stays at its boot clock.
if [[ -f /root/power-bringup/cpufreq-enabled && ! -d /sys/module/guacamole_cpufreq &&
      ! -d /sys/devices/system/cpu/cpufreq/policy0 ]]; then
    insmod /root/power-bringup/cpufreq/guacamole_cpufreq.ko ||
        echo 'CPU frequency overlay failed to load.' >&2
fi
if [[ -x /usr/local/sbin/guacamole-controls-start &&
      -f /root/controls-bringup/autostart-enabled ]]; then
    nohup /usr/local/sbin/guacamole-controls-start \
        >> /root/controls-bringup/startup.log 2>&1 < /dev/null &
fi
# The sensor DSP and its file service, once per boot after the radio (pd-mapper).
if [[ -x /usr/local/sbin/guacamole-sensors-start &&
      -f /root/sensors-bringup/autostart-enabled ]]; then
    nohup /usr/local/sbin/guacamole-sensors-start \
        >> /root/sensors-bringup/startup.log 2>&1 < /dev/null &
fi
if [[ -x /usr/local/sbin/guacamole-bluetooth-start &&
      -f /root/bluetooth-bringup/autostart-enabled ]]; then
    nohup /usr/local/sbin/guacamole-bluetooth-start \
        >> /root/bluetooth-bringup/startup.log 2>&1 < /dev/null &
fi
# ABL spends one retry per boot until the slot is marked successful, then
# refuses the slot. Mark it once the desktop has stayed up for a minute; a
# kernel that never gets this far still spends its retries.
if [[ -x /usr/local/sbin/guacamole-boot-slot &&
      -f /root/boot-slot/autostart-enabled ]]; then
    nohup bash -c 'sleep 60; pgrep -x Hyprland >/dev/null || exit 1
        exec /usr/local/sbin/guacamole-boot-slot mark-successful' \
        >> /root/boot-slot/mark.log 2>&1 < /dev/null &
fi
line='dofile("/root/.config/hypr/mobile.lua")'
for config in /etc/hypr/hyprland.lua /root/.config/hypr/hyprland.lua; do
    if ! grep -qFx "$line" "$config"; then
        cp -a "$config" "$config.before-mobile"
        printf '\n%s\n' "$line" >> "$config"
    fi
done
hyprctl -i 0 reload
errors=$(hyprctl -i 0 configerrors)
if [[ -n ${errors//[[:space:]]/} ]]; then
    printf '%s\n' "$errors" >&2
    exit 1
fi
# Align the frozen initramfs's early wallpaper with the saved theme.
if [[ -x /root/.config/omarchy-mobile/wallpaper-apply ]]; then
    image=$(python3 - <<'PYBG'
import json
from pathlib import Path
try:
    print(json.loads(Path('/root/.local/state/omarchy-mobile/palette.json').read_text()).get('wallpaper', {}).get('path', ''))
except (OSError, ValueError):
    print('')
PYBG
    )
    /root/.config/omarchy-mobile/wallpaper-apply "$image"
fi
# Retire only this board's old boot wallpaper once the mobile background exists.
# Native2's frozen initramfs still launches this fallback before Quickshell.
if hyprctl -i 0 layers | grep -q 'namespace: omarchy-mobile-wallpaper'; then
    python3 - <<'PY'
import os
from pathlib import Path
import signal
expected = [b'/usr/bin/swaybg', b'-i', b'/usr/share/hypr/wall0.png', b'-m', b'fill']
for proc in Path('/proc').glob('[0-9]*'):
    try:
        if proc.stat().st_uid == os.getuid() and (proc / 'cmdline').read_bytes().rstrip(b'\0').split(b'\0') == expected:
            os.kill(int(proc.name), signal.SIGTERM)
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        pass
PY
fi
