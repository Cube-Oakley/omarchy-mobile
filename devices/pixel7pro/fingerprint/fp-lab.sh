#!/bin/bash
# Load Goodix's trusted app (g7.app) into Trusty with the stock
# trusty_apploader, in a RAM-only chroot that sees only the Trusty IPC node,
# the system DMA heap and the null/zero/urandom nodes. A reboot unloads the
# app. Usage: fp-lab.sh RUNTIME.tar [--storage]
#
# Trusty's apploader answers only once its secure storage is up, which stock
# serves with storageproxyd (vendor init.gs201.rc). --storage runs it first,
# as stock does, as system (1000) and without capabilities:
# - RPMB through SCSI generic (sg.ko, CONFIG_CHR_DEV_SG is off in our kernel)
#   on the UFS RPMB well-known LUN, 0:0:0:49476, as /dev/sg1;
# - /data/vendor/ss on /var/lib/omarchy-mobile/trusty-ss (stock's lived on the
#   old userdata, which is our root now);
# - /data/vendor/ss/persist on the persist partition's ss directory, Trusty's
#   files from stock, mounted read-write only inside the lab's mount namespace.
#   The boot script sets persist read-only in the block layer; the lab lifts
#   that only while it runs, and an exit trap restores it.
# The user approved Trusty writing its secure storage (October 1).
set -euo pipefail
tarball=$1
storage=${2:-}
R=/run/fp-lab
SS=/var/lib/omarchy-mobile/trusty-ss
[[ -c /dev/trusty-ipc-dev0 ]] || { echo 'Trusty IPC is not up' >&2; exit 1; }
if ! mountpoint -q "$R"; then
    mkdir -p "$R"
    mount -t tmpfs -o size=128m,mode=0700 fp-lab "$R"
    tar -xf "$tarball" -C "$R"
fi
rpmb= persist=
if [[ $storage == --storage ]]; then
    [[ -d /sys/module/sg ]] || insmod "$(dirname "$0")/sg.ko"
    for d in /sys/class/scsi_generic/sg*; do
        [[ $(basename "$(readlink -f "$d/device")") == 0:0:0:49476 ]] && rpmb=$(cat "$d/dev")
    done
    [[ -n $rpmb ]] || { echo 'no SCSI generic node for the RPMB LUN' >&2; exit 1; }
    persist=$(blkid -t PARTLABEL=persist -o device)
    [[ -b $persist ]] || { echo 'no persist partition' >&2; exit 1; }
    mkdir -p "$SS/persist"
    chown 1000:1000 "$SS" "$SS/persist"
    chmod 0770 "$SS"
    echo "RPMB LUN at $rpmb, persist $persist, storage in $SS"
fi
exec unshare --mount --pid --fork --kill-child=KILL --mount-proc="$R/proc" bash -c '
    set -e
    R=$1 rpmb=$2 persist=$3 SS=$4
    ipc=$(stat -c "%t %T" /dev/trusty-ipc-dev0)
    mkdir -p "$R/dev/dma_heap"
    rm -f "$R/dev/trusty-ipc-dev0"
    mknod -m 0666 "$R/dev/trusty-ipc-dev0" c $((0x${ipc% *})) $((0x${ipc#* }))
    for d in dma_heap/system null zero urandom; do
        touch "$R/dev/$d"; mount --bind "/dev/$d" "$R/dev/$d"
    done
    proxy=
    if [[ -n $rpmb ]]; then
        rm -f "$R/dev/sg1"
        mknod -m 0660 "$R/dev/sg1" c ${rpmb%:*} ${rpmb#*:}
        chown 1000:1000 "$R/dev/sg1"
        mkdir -p "$R/data/vendor/ss" /run/fp-lab-persist
        mount --bind "$SS" "$R/data/vendor/ss"
        # set +e: errexit would stop the trap at a failing umount, before
        # the guard is back (it did once).
        trap "set +e; umount -q $R/data/vendor/ss/persist /run/fp-lab-persist; blockdev --setro $persist" EXIT
        blockdev --setrw "$persist"
        mount -t ext4 -o rw,nosuid,nodev,noexec "$persist" /run/fp-lab-persist
        mount --bind /run/fp-lab-persist/ss "$R/data/vendor/ss/persist"
        ENTER_UID=1000 ENTER_GID=1000 "$R/enter-gnss" "$R" /vendor/bin/storageproxyd \
            -d /dev/trusty-ipc-dev0 -r /dev/sg1 -p /data/vendor/ss -t ufs &
        proxy=$!
        sleep 3
        kill -0 $proxy 2>/dev/null || { echo "storageproxyd exited"; wait $proxy || true; }
    fi
    rc=0
    timeout 60 "$R/enter-gnss" "$R" /vendor/bin/trusty_apploader /vendor/firmware/g7.app || rc=$?
    echo "apploader exit: $rc"
    if [[ -n $proxy ]]; then
        kill -TERM $proxy 2>/dev/null || true
        wait $proxy 2>/dev/null || true
        sync
        umount "$R/data/vendor/ss/persist" || true
        umount /run/fp-lab-persist || true
        umount "$R/data/vendor/ss" || true
        blockdev --setro "$persist"
    fi
    exit $rc
' _ "$R" "$rpmb" "$persist" "$SS"
