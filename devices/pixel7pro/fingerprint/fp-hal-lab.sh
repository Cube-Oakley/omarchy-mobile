#!/bin/bash
# Fingerprint HAL lab: stock servicemanager and Goodix's HAL on binderfs, in
# a RAM-only chroot like fp-lab.sh's, for SECONDS seconds. Logs go to
# /run/fp-hal-logs/.
#   fp-hal-lab.sh RUNTIME.tar SHIM.so [sm|hal|full|serve] [SECONDS]
#
# fp-hal-exec.sh runs a program (fp-client) inside a running lab, and
# `fp-hal-exec.sh stop` ends it early, with the same clean-up.
#
# - sm: servicemanager alone.
# - hal: servicemanager and the HAL (needs pixel-goodix-fp loaded); without
#   the trusted app the HAL retries its connection.
# - full: as fp-lab.sh --storage first (approved by the user on October 1):
#   storageproxyd for Trusty's secure storage (RPMB through sg.ko, the
#   persist partition's ss directory writable only inside this namespace, an
#   exit trap restoring the block-layer guard) and the apploader for g7.app;
#   storageproxyd keeps running alongside the HAL. Trusty's KeyMint (for
#   the shared secret that signs auth tokens) and gatekeeper HALs run too,
#   for enrollment's auth token; fp-client keeps the lab password's
#   gatekeeper handle in FPLAB.
# - serve: full, then the fingerprint service (fp-client serve) on
#   SOCKDIR/fingerprint.sock for the shell, with its credential in STATE,
#   until fp-hal-exec.sh stop or the service ends (the HAL died). Its
#   "@hold" and "@release" keep CPUs 4 and 6 online while the trusted app
#   works (/run/pixel-cores-hold, which the display hook leaves online). The
#   user approved persist staying writable while it runs (October 2).
#
# sg.ko, pixel-goodix-fp.ko and fp-client are taken from this script's
# directory (sg.ko also from /root/fp).
#
# The runtime is out/fp-work/fp-hal-runtime.tar (proprietary, never
# committed); SHIM.so is fp-shim.c built for Bionic, installed as the
# runtime's pixel-android-shim.so. Every program runs without capabilities
# (enter-gnss). The HAL reads its factory calibration from mfg_data through
# a read-only node (0444, no capabilities; the boot also sets the partition
# read-only in the block layer).
set -euo pipefail
tarball=$1
shim=$2
stage=${3:-sm}
secs=${4:-20}
[[ $stage == serve ]] && secs=0
R=/run/fp-hal
L=/run/fp-hal-logs
SS=/var/lib/omarchy-mobile/trusty-ss
FPLAB=/var/lib/omarchy-mobile/fp-lab
STATE=/var/lib/omarchy-mobile/fingerprint/state
SOCKDIR=/run/omarchy-fingerprint
HERE=$(dirname "$(readlink -f "$0")")
SG=$HERE/sg.ko
[[ -f $SG ]] || SG=/root/fp/sg.ko
[[ -c /dev/trusty-ipc-dev0 ]] || { echo 'Trusty IPC is not up' >&2; exit 1; }
grep -qw binder /proc/filesystems || { echo 'no binderfs in this kernel' >&2; exit 1; }
if [[ $stage != sm ]]; then
    [[ -c /dev/goodix_fp || ! -f $HERE/pixel-goodix-fp.ko ]] || insmod "$HERE/pixel-goodix-fp.ko"
    [[ -c /dev/goodix_fp ]] || { echo 'pixel-goodix-fp is not loaded' >&2; exit 1; }
fi
rpmb= persist= mfg=
if [[ $stage == full || $stage == serve ]]; then
    [[ -d /sys/module/sg ]] || insmod "$SG"
    for d in /sys/class/scsi_generic/sg*; do
        [[ $(basename "$(readlink -f "$d/device")") == 0:0:0:49476 ]] && rpmb=$(cat "$d/dev")
    done
    [[ -n $rpmb ]] || { echo 'no SCSI generic node for the RPMB LUN' >&2; exit 1; }
    persist=$(blkid -t PARTLABEL=persist -o device)
    [[ -b $persist ]] || { echo 'no persist partition' >&2; exit 1; }
    mfg=$(blkid -t PARTLABEL=mfg_data -o device || true)
    [[ -z $mfg || $(blockdev --getro "$mfg") == 1 ]] || { echo 'mfg_data is writable' >&2; exit 1; }
    mkdir -p "$SS/persist" "$FPLAB" "$STATE" "$SOCKDIR"
    chmod 0700 "$FPLAB" "$STATE"
    chown 1000:1000 "$SS" "$SS/persist"
    chmod 0770 "$SS"
fi
mkdir -p "$R" "$L"
if ! mountpoint -q "$R"; then
    mount -t tmpfs -o size=160m,mode=0700 fp-hal "$R"
    tar -xf "$tarball" -C "$R"
fi
install -m 0644 "$shim" "$R/system/lib64/pixel-android-shim.so"
[[ $stage != serve ]] || install -m 0755 "$HERE/fp-client" "$R/system/bin/fp-client"
rm -f "$L"/*.log "$L/stop" "$L/lab.pid"
exec unshare --mount --pid --fork --kill-child=KILL --mount-proc="$R/proc" bash -c '
    set -e
    R=$1 L=$2 stage=$3 secs=$4 rpmb=$5 persist=$6 SS=$7 mfg=$8 FPLAB=$9 STATE=${10}
    SOCKDIR=${11}
    # This shell'"'"'s PID outside the namespace, for fp-hal-exec.sh: /proc is
    # still the host'"'"'s.
    read -r pid _ < /proc/self/stat
    echo "$pid" > "$L/lab.pid"
    # The HAL keeps its state and statistics in these (RAM-only here).
    mkdir -p "$R/dev/binderfs" "$R/dev/dma_heap" "$R/dev/block/by-name" "$R/dev/input" "$R/properties" \
        "$R/data/vendor/goodix/fpdata" "$R/data/vendor/fingerprint" "$R/data/media/0" \
        "$R/data/vendor_de/0/fpdata/sensor0"
    # This binderfs instance is the lab'"'"'s own: storageproxyd runs as system
    # (1000), and libbinder aborts when it cannot open /dev/binder.
    mount -t binder binder "$R/dev/binderfs"
    for b in binder hwbinder vndbinder; do
        chmod 0666 "$R/dev/binderfs/$b"
        ln -sf binderfs/$b "$R/dev/$b"
    done
    node() {	# NAME MODE [HOST-DEVICE]: a node in the chroot, as on the host
        local spec; spec=$(stat -L -c "%t %T" "${3:-/dev/$1}")
        rm -f "$R/dev/$1"
        mknod -m "$2" "$R/dev/$1" ${4:-c} $((0x${spec% *})) $((0x${spec#* }))
    }
    node trusty-ipc-dev0 0666
    [[ $stage != sm ]] && node goodix_fp 0666
    node input/event1 0400	# the touchscreen, for fp-client
    [[ -n $mfg ]] && node block/by-name/mfg_data 0444 "$mfg" b
    for d in dma_heap/system null zero urandom; do
        touch "$R/dev/$d"; mount --bind "/dev/$d" "$R/dev/$d"
    done
    # A small /sys of its own for the HAL, which also writes some of these:
    # the device tree (/chosen/plat, the board) and the CPUs read-only, and
    # stand-ins for the stock display sysfs (the panel name, from the stock
    # bugreport, selects its configuration), a thermal zone and the Trusty
    # module parameters.
    mkdir -p "$R/sys"
    mount -t tmpfs -o size=1m,mode=0755 fp-hal-sys "$R/sys"
    for d in firmware/devicetree devices/system/cpu; do
        mkdir -p "$R/sys/$d"; mount --bind -o ro "/sys/$d" "$R/sys/$d"
    done
    put() { mkdir -p "$(dirname "$R/sys/$1")"; printf "%s\n" "$2" > "$R/sys/$1"; }
    put devices/platform/exynos-drm/primary-panel/panel_name sdc-s6e3hc4
    put devices/platform/exynos-drm/primary-panel/panel_extinfo 0a8006
    put devices/platform/exynos-drm/primary-panel/op_hz 120
    put class/backlight/panel0-backlight/hbm_mode 0
    put class/backlight/panel0-backlight/local_hbm_max_timeout 3000
    put class/backlight/panel0-backlight/state on
    put class/backlight/panel0-backlight/device/serial_number 0
    put class/drm/card0/device/decon0/hibernation 0
    put class/thermal/thermal_zone74/temp 30000
    put module/trusty_core/parameters/use_high_wq N
    put module/trusty_virtio/parameters/use_high_wq N
    mkdir -p "$R/sys/devices/platform/17000000.aoc/control" "$R/sys/devices/platform/19000000.aoc/control"
    # Stock scales the exposure of each capture with ambient light from the
    # Android sensor service (als_feed_forward_supported=true), which is not
    # here: the HAL would wait for it at every touch.
    printf false > "$R/properties/persist.vendor.udfps.als_feed_forward_supported"
    # The fingerprint circle, which fp-client lights (root only).
    put module/pixel_scanout/parameters/lhbm 0
    mount --bind /sys/module/pixel_scanout/parameters/lhbm "$R/sys/module/pixel_scanout/parameters/lhbm"
    # Both context managers first: vendor programs (storageproxyd) wait for
    # vndservicemanager on /dev/vndbinder, the HAL registers with servicemanager.
    "$R/enter-gnss" "$R" /system/bin/servicemanager > "$L/servicemanager.log" 2>&1 &
    sm=$!
    "$R/enter-gnss" "$R" /vendor/bin/vndservicemanager /dev/vndbinder \
        > "$L/vndservicemanager.log" 2>&1 &	# as init.rc starts it; else /dev/binder
    vsm=$!
    sleep 2
    for p in "servicemanager $sm" "vndservicemanager $vsm"; do
        kill -0 ${p#* } 2>/dev/null && echo "${p% *} running" || echo "${p% *} exited"
    done
    proxy=
    if [[ $stage == full || $stage == serve ]]; then
        rm -f "$R/dev/sg1"
        mknod -m 0660 "$R/dev/sg1" c ${rpmb%:*} ${rpmb#*:}
        chown 1000:1000 "$R/dev/sg1"
        mkdir -p "$R/data/vendor/ss" /run/fp-hal-persist
        mount --bind "$SS" "$R/data/vendor/ss"
        # set +e: errexit would stop the trap at a failing umount.
        trap "set +e; umount -q $R/data/vendor/ss/persist /run/fp-hal-persist 2>/dev/null; blockdev --setro $persist" EXIT
        blockdev --setrw "$persist"
        mount -t ext4 -o rw,nosuid,nodev,noexec "$persist" /run/fp-hal-persist
        mount --bind /run/fp-hal-persist/ss "$R/data/vendor/ss/persist"
        ENTER_UID=1000 ENTER_GID=1000 "$R/enter-gnss" "$R" /vendor/bin/storageproxyd \
            -d /dev/trusty-ipc-dev0 -r /dev/sg1 -p /data/vendor/ss -t ufs \
            > "$L/storageproxyd.log" 2>&1 &
        proxy=$!
        sleep 3
        kill -0 $proxy 2>/dev/null || { echo "storageproxyd exited"; wait $proxy || true; }
        rc=0
        timeout 60 "$R/enter-gnss" "$R" /vendor/bin/trusty_apploader /vendor/firmware/g7.app \
            > "$L/apploader.log" 2>&1 || rc=$?
        echo "apploader exit: $rc"
        # Auth tokens, as init starts these: KeyMint (which also serves the
        # shared secret) as nobody/drmrpc, configuring its app with stock'"'"'s
        # versions; gatekeeper as system.
        printf 15 > "$R/properties/ro.build.version.release"
        printf 2025-02-05 > "$R/properties/ro.build.version.security_patch"
        printf 2025-02-05 > "$R/properties/ro.vendor.build.security_patch"
        mkdir -p "$R/data/vendor/fplab" "$R/data/vendor/omarchy"
        mount --bind "$FPLAB" "$R/data/vendor/fplab"
        mount --bind "$STATE" "$R/data/vendor/omarchy"
        ENTER_UID=9999 ENTER_GID=1026 "$R/enter-gnss" "$R" \
            /vendor/bin/hw/android.hardware.security.keymint-service.trusty \
            --dev /dev/trusty-ipc-dev0 > "$L/keymint.log" 2>&1 &
        km=$!
        ENTER_UID=1000 ENTER_GID=1000 "$R/enter-gnss" "$R" \
            /vendor/bin/hw/android.hardware.gatekeeper-service.trusty \
            --dev /dev/trusty-ipc-dev0 > "$L/gatekeeper.log" 2>&1 &
        gk=$!
        sleep 2
        for p in "keymint $km" "gatekeeper $gk"; do
            kill -0 ${p#* } 2>/dev/null && echo "${p% *} running" || echo "${p% *} exited"
        done
    fi
    if [[ $stage != sm ]]; then
        "$R/enter-gnss" "$R" /vendor/bin/hw/android.hardware.biometrics.fingerprint-service.goodix \
            > "$L/fingerprint.log" 2>&1 &
        hal=$!
        sleep 3
        kill -0 $hal 2>/dev/null && echo "fingerprint HAL running" || echo "fingerprint HAL exited: $(wait $hal; echo $?)"
    fi
    if [[ $stage == serve ]]; then
        mkdir -p "$R/run/omarchy-fingerprint"
        mount --bind "$SOCKDIR" "$R/run/omarchy-fingerprint"
        # CPUs 4 and 6 for the trusted app: online while the service holds
        # them, back to the display hook'"'"'s choice when it releases them.
        hold_cpus() {
            echo "4 6" > /run/pixel-cores-hold
            for c in 4 6; do echo 1 > /sys/devices/system/cpu/cpu$c/online || true; done
        }
        release_cpus() {
            rm -f /run/pixel-cores-hold
            [[ ! -e /run/pixel-cores-dark ]] || /usr/local/sbin/pixel-display-hook cores || true
        }
        # A socket left by the last run would be shared below, then replaced.
        rm -f "$SOCKDIR/fingerprint.sock"
        "$R/enter-gnss" "$R" /system/bin/fp-client serve /run/omarchy-fingerprint/fingerprint.sock 2>&1 |
            while IFS= read -r line; do
                case $line in
                    @hold) hold_cpus ;;
                    @release) release_cpus ;;
                    *) printf "%s\n" "$line" ;;
                esac
            done > "$L/service.log" &
        service=$!
        echo "fingerprint service started"
        # The desktop account (named in /etc/omarchy-mobile/session) reaches
        # the socket through its group.
        user=$(sed -n "s/^user=\([a-z_][a-z0-9_-]*\)$/\1/p" /etc/omarchy-mobile/session 2>/dev/null | head -1)
        if [[ -n $user ]] && gid=$(id -g "$user" 2>/dev/null); then
            for n in $(seq 100); do [[ -S $SOCKDIR/fingerprint.sock ]] && break; sleep 0.1; done
            chgrp "$gid" "$SOCKDIR" "$SOCKDIR/fingerprint.sock" && chmod 0750 "$SOCKDIR" &&
                chmod 0660 "$SOCKDIR/fingerprint.sock" || echo "fingerprint socket not shared"
        fi
        while [[ ! -e $L/stop ]] && kill -0 $service 2>/dev/null; do sleep 1; done
        [[ -e $L/stop ]] || echo "fingerprint service exited"
        rm -f /run/pixel-cores-hold
    else
        end=$((SECONDS + secs))
        while (( SECONDS < end )) && [[ ! -e $L/stop ]]; do sleep 1; done
    fi
    if [[ -n $proxy ]]; then
        kill -TERM ${hal:-} $km $gk $sm $vsm 2>/dev/null || true
        sleep 1
        kill -TERM $proxy 2>/dev/null || true
        wait $proxy 2>/dev/null || true
        sync
        umount "$R/data/vendor/ss/persist" || true
        umount /run/fp-hal-persist || true
        umount "$R/data/vendor/ss" || true
        umount "$R/data/vendor/fplab" || true
        umount "$R/data/vendor/omarchy" 2>/dev/null || true
        blockdev --setro "$persist"
    fi
' _ "$R" "$L" "$stage" "$secs" "$rpmb" "$persist" "$SS" "$mfg" "$FPLAB" "$STATE" "$SOCKDIR"
