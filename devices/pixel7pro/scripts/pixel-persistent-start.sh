#!/bin/sh
# Initramfs bootstrap. Never formats storage; a missing root leaves USB recovery.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
log() { printf '<5>PIXEL ROOT: %s\n' "$*" >/dev/kmsg; echo "$*"; }
trap 'log "startup stopped; inspect /run/pixel-persistent.log over USB serial"' EXIT
insmod /lib/modules/pixel/pixel-reboot.ko
# HS gear 4 rate B on both lanes with Google's GS201 calibration
# (kernel/storage/README.md); a failed change retries in PWM gear 1.
# dev_reset starts the UFS device from reset, as the vendor driver does. If
# that load fails, load once more without it, the previously proven setting.
if ! insmod /lib/modules/pixel/pixel-ufs.ko vendor_cal=1 hs_gear=4 dev_reset=1; then
    log "UFS load with device reset failed; retrying without it"
    insmod /lib/modules/pixel/pixel-ufs.ko vendor_cal=1 hs_gear=4
fi
for n in $(seq 1 60); do
    [ -b /dev/sda31 ] && break
    sleep 1
done
# This bootstrap is specific to the inspected 256 GB cheetah layout.
grep -qx 'PARTNAME=userdata' /sys/class/block/sda31/uevent
[ "$(cat /sys/class/block/sda31/size)" = 480424104 ]
mkdir -p /run/arch
mount -t ext4 -o ro /dev/sda31 /run/arch
[ "$(cat /run/arch/etc/omarchy-mobile-pixel-root)" = v1 ]
[ -x /run/arch/usr/bin/bash ]
mount -o remount,rw /run/arch
for d in dev proc sys run; do mkdir -p "/run/arch/$d"; done
mkdir -p /dev/shm
mount -t tmpfs -o mode=1777 tmpfs /dev/shm
mount --rbind /dev /run/arch/dev
mount -t proc proc /run/arch/proc
mount -t sysfs sysfs /run/arch/sys
mount -t tmpfs -o mode=0755 tmpfs /run/arch/run
mkdir -p /sys/kernel/debug
mount -t debugfs debugfs /sys/kernel/debug
for pair in fd:/proc/self/fd stdin:/proc/self/fd/0 stdout:/proc/self/fd/1 stderr:/proc/self/fd/2; do
    name=${pair%%:*}; target=${pair#*:}
    [ -L "/dev/$name" ] || ln -s "$target" "/dev/$name"
done
log 'persistent Arch root mounted; starting USB SSH and desktop'
chroot /run/arch /usr/bin/bash /usr/local/lib/omarchy-mobile/pixel-boot.sh
trap - EXIT
log 'persistent desktop startup complete'
