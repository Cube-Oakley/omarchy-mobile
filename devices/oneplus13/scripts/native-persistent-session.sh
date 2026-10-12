#!/bin/bash
# Runs inside the installed Arch root, without inherited device/systemd services.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/bin HOME=/root USER=root LOGNAME=root
[[ $(cat /etc/omarchy-mobile-oneplus13-root) == oneplus13-cph2655-v1 ]]
[[ $(stat -f -c %T /) == ext2/ext3 ]]
[[ $(tr -d '\0' </sys/firmware/devicetree/base/model) == 'OnePlus 13 CPH2655 Omarchy USB probe' ]]
base=/usr/local/lib/omarchy-mobile/oneplus13
mkdir -p /run/sshd /var/log/oneplus13
if [[ -f /var/lib/omarchy-mobile/rtc-anchor.json && -x $base/rtc-clock.py ]]; then
    python3 "$base/rtc-clock.py" --restore >/run/rtc-clock.log 2>&1 ||
        echo 'RTC time restoration unavailable; retaining software-clock fallback'
fi
floor=$(stat -c %Y /etc/omarchy-mobile-oneplus13-root)
(( $(date +%s) >= floor )) || date -u -s "@$floor"
sysctl -q fs.protected_hardlinks=1 fs.protected_symlinks=1 fs.protected_regular=1 fs.protected_fifos=1
# USB is a debugging transport, not a prerequisite for the local desktop.
(
    for n in {1..30}; do
        if ip -4 addr show dev usb0 2>/dev/null | grep -q '10.77.13.1/30'; then
            /usr/bin/sshd -t -f /etc/ssh/sshd_config.oneplus13
            exec /usr/bin/sshd -f /etc/ssh/sshd_config.oneplus13 -E /run/sshd-oneplus13.log
        fi
        sleep 1
    done
    echo 'USB SSH unavailable; local desktop startup continues'
) >/run/sshd-start.log 2>&1 &
"$base/start-ram-desktop.sh" --gpu
"$base/start-ram-mobile.sh" --gpu
# NetworkManager and sensor-proxy share this bus. Start it before either
# optional helper, rather than depending on asynchronous sensor discovery.
mkdir -p /run/dbus
if ! busctl --system list >/dev/null 2>&1; then
    dbus-daemon --system --fork --nopidfile || echo 'System bus unavailable' >&2
fi
if [[ -x $base/start-native-haptics.sh && -f /opt/oneplus13-haptics/SHA256SUMS ]]; then
    "$base/start-native-haptics.sh" >/run/oneplus13-haptics.log 2>&1 </dev/null ||
        echo 'Vibration motor unavailable; see /run/oneplus13-haptics.log'
fi
# Audio: the sound card first (it may fail and leave audio off), then the
# session's audio server, which also serves Bluetooth audio through BlueZ.
# There are no systemd user services here; the desktop runs as root.
(
    if [[ -x $base/start-native-audio.sh && -f /opt/oneplus13-audio/SHA256SUMS ]]; then
        "$base/start-native-audio.sh" || echo 'Sound card unavailable'
    fi
    # The shell's start-mobile.sh creates the session bus.
    for n in {1..60}; do [[ -S /run/user/0/bus ]] && break; sleep 1; done
    if [[ -S /run/user/0/bus ]]; then
        for server in pipewire wireplumber pipewire-pulse; do
            setsid -f env HOME=/root XDG_RUNTIME_DIR=/run/user/0 \
                DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/0/bus \
                sh -c "exec $server >>/run/user/0/$server.log 2>&1 </dev/null"
            sleep 0.5
        done
    else
        echo 'No session bus; audio server not started'
    fi
) >/run/oneplus13-audio.log 2>&1 </dev/null &
if [[ -x $base/start-native-sensors.sh && -f /opt/oneplus13-sensors/modules.sha256 ]]; then
    nohup "$base/start-native-sensors.sh" >/run/oneplus13-sensors.log 2>&1 </dev/null &
fi
if [[ -x $base/start-native-wifi.sh && -f /opt/oneplus13-wifi/SHA256SUMS ]]; then
    nohup "$base/start-native-wifi.sh" >/run/oneplus13-wifi.log 2>&1 </dev/null &
fi
if [[ -x $base/start-native-bluetooth.sh && -f /opt/oneplus13-bluetooth/SHA256SUMS ]]; then
    nohup "$base/start-native-bluetooth.sh" >/run/oneplus13-bluetooth.log 2>&1 </dev/null &
fi
sleep 3
pgrep -x Hyprland >/dev/null
boot=$(cat /proc/sys/kernel/random/boot_id)
dmesg >"/var/log/oneplus13/$boot.kernel.log"
printf '%s native desktop started\n' "$(date -u +%FT%TZ)" >>/var/log/oneplus13/boots.log
sync -f /var/log/oneplus13
