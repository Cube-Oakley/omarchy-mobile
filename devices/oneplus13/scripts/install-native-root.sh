#!/bin/bash
# Run inside the prepared RAM Arch root. Only the verified userdata is replaced.
set -euo pipefail
[[ $# == 1 && $1 == --replace-android-data ]] || {
    echo 'Usage: install-native-root.sh --replace-android-data' >&2; exit 2;
}
[[ $EUID == 0 && $(uname -m) == aarch64 ]]
[[ $(uname -r) == *-omarchy-oneplus13-probe* ]]
[[ $(stat -f -c %T /) == tmpfs ]]
[[ $(tr -d '\0' </sys/firmware/devicetree/base/model) == 'OnePlus 13 CPH2655 Omarchy USB probe' ]]
grep -qx 'PARTNAME=userdata' /sys/class/block/sda15/uevent
grep -qx 'PARTNAME=boot_a' /sys/class/block/sde12/uevent
[[ $(blockdev --getsize64 /dev/sda15) == 493792325632 ]]
[[ $(blockdev --getsize64 /dev/sde12) == 100663296 ]]
[[ -z $(findmnt -rn -S /dev/sda15) ]]
[[ -z $(ls -A /sys/class/block/sda15/holders) ]]
[[ $(blkid -s TYPE -o value /dev/sda15 || true) != ext4 ]]
[[ $(cat /sys/class/power_supply/qcom-battmgr-usb/online) == 1 ]]
(( $(cat /sys/class/power_supply/qcom-battmgr-bat/capacity) >= 50 ))
for script in session.sh start-ram-desktop.sh start-ram-mobile.sh; do
    [[ -x /usr/local/lib/omarchy-mobile/oneplus13/$script ]]
done
[[ -s /root/.ssh/authorized_keys && -s /etc/ssh/ssh_host_ed25519_key.oneplus13 ]]
[[ -s /opt/oneplus13-mesa/lib/libEGL_mesa.so.0 ]]
[[ -s /opt/oneplus13-adsp/SHA256SUMS ]]
[[ $(wc -l </opt/oneplus13-adsp/SHA256SUMS) == 58 ]]
sed 's|  /lib/firmware/qcom/oneplus13/|  /opt/oneplus13-adsp/|' \
    /opt/oneplus13-adsp/SHA256SUMS | sha256sum --quiet -c -
[[ -s /run/oneplus13-install/boot-baseline.sha256 ]]
[[ $(wc -l </run/oneplus13-install/boot-baseline.sha256) == 8 ]]
sha256sum -c /run/oneplus13-install/boot-baseline.sha256
# Verify the installed programs, libraries and device configuration after remount.
# Runtime logs and caches are intentionally excluded from this stable inventory.
cd /
find usr/bin usr/lib usr/local opt etc root/.config -xdev -type f -print0 |
    sort -z | xargs -0 sha256sum > /run/oneplus13-install/root-files.sha256
target=/mnt/oneplus13-root
mkdir -p "$target"
! mountpoint -q "$target"
# The parent disk must permit writes. Keep all sibling partitions protected.
blockdev --setrw /dev/sda
for part in /sys/class/block/sda[0-9]*; do
    [[ ${part##*/} == sda15 ]] && continue
    blockdev --setro "/dev/${part##*/}"
    [[ $(blockdev --getro "/dev/${part##*/}") == 1 ]]
done
blockdev --setrw /dev/sda15
echo 'Replacing this project phone’s Android userdata with the native Linux root.'
mkfs.ext4 -F -L omarchy-op13 -m 1 -N 1048576 \
    -E nodiscard,lazy_itable_init=1,lazy_journal_init=1 /dev/sda15
mount -t ext4 /dev/sda15 "$target"
mkdir -p /tmp
chmod 1777 /tmp
dd if=/dev/urandom of=/tmp/oneplus13-storage-test bs=1M count=16 status=none
expected=$(sha256sum /tmp/oneplus13-storage-test | cut -d' ' -f1)
cp /tmp/oneplus13-storage-test "$target/.storage-test"
sync -f "$target"
umount "$target"
mount -t ext4 /dev/sda15 "$target"
[[ $(sha256sum "$target/.storage-test" | cut -d' ' -f1) == "$expected" ]]
rm /tmp/oneplus13-storage-test "$target/.storage-test"
tar --one-file-system --numeric-owner --acls --xattrs \
    --exclude=./mnt --exclude=./run --exclude=./dev --exclude=./proc \
    --exclude=./sys --exclude=./tmp -C / -cpf - . |
    tar --numeric-owner --acls --xattrs -C "$target" -xpf -
mkdir -p "$target"/{mnt,run,dev,proc,sys,tmp}
chmod 1777 "$target/tmp"
# PID1 is the device-specific recovery initramfs; never start inherited services.
printf 'oneplus13-linux\n' >"$target/etc/hostname"
printf '# Managed by the OnePlus recovery initramfs.\n' >"$target/etc/fstab"
# Account for the two deliberate installed configuration changes.
sed -i '\|  etc/hostname$|d; \|  etc/fstab$|d' /run/oneplus13-install/root-files.sha256
sync -f "$target"
umount "$target"
e2fsck -fn /dev/sda15
mount -t ext4 -o ro /dev/sda15 "$target"
(cd "$target" && sha256sum --quiet -c /run/oneplus13-install/root-files.sha256)
sha256sum -c /run/oneplus13-install/boot-baseline.sha256
# Mark bootable only after filesystem and readback verification have passed.
umount "$target"
mount -t ext4 /dev/sda15 "$target"
printf 'oneplus13-cph2655-v1\n' >"$target/etc/omarchy-mobile-oneplus13-root"
sync -f "$target"
umount "$target"
mount -t ext4 -o ro /dev/sda15 "$target"
[[ $(cat "$target/etc/omarchy-mobile-oneplus13-root") == oneplus13-cph2655-v1 ]]
echo 'Native root installed and remounted read-only. Boot partitions are unchanged.'
blkid /dev/sda15
