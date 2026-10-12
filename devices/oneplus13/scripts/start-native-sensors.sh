#!/bin/bash
# Optional native SSC startup; run in the Arch root after the desktop starts.
set -euo pipefail
[[ $(cat /etc/omarchy-mobile-oneplus13-root) == oneplus13-cph2655-v1 ]]
[[ $(tr -d '\0' </sys/firmware/devicetree/base/model) == 'OnePlus 13 CPH2655 Omarchy USB probe' ]]
base=/opt/oneplus13-sensors
[[ $(cat "$base/KERNELRELEASE") == "$(uname -r)" ]]
(cd "$base" && sha256sum --quiet -c modules.sha256)
[[ -x /usr/lib/iio-sensor-proxy && -x $base/ssccli && -x $base/hexagonrpcd ]]
[[ -s /etc/udev/rules.d/81-oneplus13-sensors.rules ]]
[[ $(cat /sys/class/remoteproc/remoteproc0/state) == running ]]
# Restarting a partially initialized firmware file service can stall in RPC.
# One attempt per boot; failed startup remains optional and needs a clean reboot.
mkdir /run/oneplus13-sensors-started || {
    echo 'Sensor startup already attempted this boot; inspect the log.' >&2
    exit 1
}
[[ -d /sys/module/fastrpc ]] || insmod "$base/fastrpc.ko"
[[ -c /dev/fastrpc-adsp-secure ]]
SYSTEMD_IN_CHROOT=0 udevadm control --reload
SYSTEMD_IN_CHROOT=0 udevadm trigger --subsystem-match=misc --sysname-match=fastrpc-adsp-secure --action=change
SYSTEMD_IN_CHROOT=0 udevadm settle --timeout=10
timeout 300 "$base/hexagonrpcd" -f /dev/fastrpc-adsp-secure -d adsp -s \
    -R "$base/hexagon-files" >/run/hexagonrpcd.log 2>&1 &
service=$!
# A successful file-service exit does not imply the SSC SUID is published yet.
# Require actual samples before starting sensor-proxy and its shell clients.
ready=0
deadline=$((SECONDS + 90))
while (( SECONDS < deadline )); do
    if timeout 10 "$base/ssccli" --sensor=accelerometer --timeout=1 \
            >/run/oneplus13-accel-ready.log 2>&1 &&
            grep -q '^Accelerometer sensor measurement:' /run/oneplus13-accel-ready.log; then
        ready=1
        break
    fi
    sleep 2
done
if (( ! ready )); then
    kill "$service" 2>/dev/null || true
    echo 'SSC accelerometer did not become ready; desktop continues without rotation.' >&2
    exit 1
fi
mkdir -p /run/dbus
if ! busctl --system list >/dev/null 2>&1; then
    dbus-daemon --system --fork --nopidfile
fi
busctl --system call org.freedesktop.DBus / org.freedesktop.DBus ReloadConfig
echo 'SSC accelerometer ready; starting sensor-proxy.'
exec /usr/lib/iio-sensor-proxy
