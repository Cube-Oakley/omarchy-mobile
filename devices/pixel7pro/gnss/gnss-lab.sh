#!/bin/bash
# Test runner: Broadcom's stock lhd and gpsd in a RAM-only chroot with the
# pixel-gnss driver's device nodes. Usage: gnss-lab.sh RUNTIME.tar [seconds]
set -euo pipefail
tarball=$1 seconds=${2:-120}
R=/run/gnss-lab
[[ -d /sys/devices/platform/pixel-gnss ]] || { echo 'pixel-gnss is not loaded' >&2; exit 1; }
if ! mountpoint -q "$R"; then
    mkdir -p "$R"
    mount -t tmpfs -o size=96m,mode=0700 gnss-lab "$R"
    tar -xf "$tarball" -C "$R"
fi
exec unshare --mount --pid --fork --kill-child=KILL --mount-proc="$R/proc" bash -c '
    set -e
    R=$1 seconds=$2
    for d in ttyBCM bbd_control bbd_patch bbd_sensor bbd_pwrstat null zero urandom; do
        touch "$R/dev/$d"; mount --bind "/dev/$d" "$R/dev/$d"
    done
    mount --bind /sys/devices/platform/pixel-gnss "$R/sys/devices/platform/pixel-gnss"
    "$R/enter-gnss" "$R" /vendor/bin/hw/lhd /vendor/etc/gnss/lhd.conf >"$R/tmp/lhd.log" 2>&1 &
    sleep 3
    # gpsd exits when its HAL client disconnects; start it again each time.
    ( while :; do
        "$R/enter-gnss" "$R" /vendor/bin/hw/gpsd -c /vendor/etc/gnss/gps.xml >>"$R/tmp/gpsd.log" 2>&1
        sleep 1
    done ) &
    sleep "$seconds"
' _ "$R" "$seconds"
