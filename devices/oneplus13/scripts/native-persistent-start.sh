#!/bin/sh
# Initramfs startup: never format storage; failure is handled by PID1 recovery.
set -eu
export PATH=/bin
[ "$(stat -f -c %T /)" = tmpfs ]
[ "$(tr -d '\000' </sys/firmware/devicetree/base/model)" = 'OnePlus 13 CPH2655 Omarchy USB probe' ]
# UFS enumeration is asynchronous. Wait for every stock LUN's final partition
# before protecting the disks; an early glob can silently match no devices.
for last in sda15 sdb6 sdc6 sdd8 sde87 sdf17; do
    for n in $(seq 1 30); do [ -b "/dev/$last" ] && break; sleep 1; done
    [ -b "/dev/$last" ]
done
for node in /sys/class/block/sd*; do
    blockdev --setro "/dev/${node##*/}"
    [ "$(blockdev --getro "/dev/${node##*/}")" = 1 ]
done
echo 'All six UFS LUNs and their partitions protected read-only'
grep -qx 'PARTNAME=userdata' /sys/class/block/sda15/uevent
[ "$(blockdev --getsize64 /dev/sda15)" = 493792325632 ]
mkdir -p /run/arch
mount -t ext4 -o ro,noload /dev/sda15 /run/arch
[ "$(cat /run/arch/etc/omarchy-mobile-oneplus13-root)" = oneplus13-cph2655-v1 ]
[ -x /run/arch/usr/local/lib/omarchy-mobile/oneplus13/session.sh ]
umount /run/arch
blockdev --setrw /dev/sda
for part in /sys/class/block/sda[0-9]*; do
    [ "${part##*/}" = sda15 ] && continue
    blockdev --setro "/dev/${part##*/}"
done
blockdev --setrw /dev/sda15
mount -t ext4 /dev/sda15 /run/arch
boot=$(cat /proc/sys/kernel/random/boot_id)
log_dir="/run/arch/var/log/oneplus13/early-$boot"
mkdir -p "$log_dir"
printf '%s\n' "$log_dir" >/run/oneplus13-boot-log-dir
echo 'Verified native root mounted; beginning hardware startup'
sh /bin/set-probe-cpu-policy
insmod /oneplus13-tcm.ko input_enabled=1
insmod /dispcc-sm8750.ko
insmod /oneplus13-display-probe.ko
insmod /drm_dma_helper.ko
insmod /backlight.ko
# panel=1: DSI brightness (backlight class) and panel sleep while the screen is off.
insmod /oneplus13-scanout.ko enable=1 early_flip=1 panel=1
mkdir -p /dev/shm /sys/kernel/debug
mount -t tmpfs -o mode=1777 tmpfs /dev/shm
mount -t debugfs debugfs /sys/kernel/debug
for d in dev proc sys run; do mkdir -p "/run/arch/$d"; done
mount --rbind /dev /run/arch/dev
mount -t proc proc /run/arch/proc
mount -t sysfs sysfs /run/arch/sys
mount -t tmpfs -o mode=0755 tmpfs /run/arch/run
for pair in fd:/proc/self/fd stdin:/proc/self/fd/0 stdout:/proc/self/fd/1 stderr:/proc/self/fd/2; do
    name=${pair%%:*}; target=${pair#*:}
    [ -L "/dev/$name" ] || ln -s "$target" "/dev/$name"
done
# Firmware lives on the verified native root to keep the boot payload within
# the stock AVB image boundary. The helper verifies every DSP file before use.
sh /bin/load-adsp-probe
chroot /run/arch /usr/bin/bash /usr/local/lib/omarchy-mobile/oneplus13/session.sh
touch /run/oneplus13-persistent-ready
