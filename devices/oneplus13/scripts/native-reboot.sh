#!/bin/sh
# Execute from the recovery initramfs, outside the installed Arch chroot.
set -eu
export PATH=/bin
case "${1:-}" in
    --bootloader|--restart|--poweroff) action=$1 ;;
    *) echo 'Usage: native-reboot.sh --bootloader|--restart|--poweroff' >&2; exit 2 ;;
esac
[ "$(stat -f -c %T /)" = tmpfs ]
[ "$(tr -d '\000' </sys/firmware/devicetree/base/model)" = 'OnePlus 13 CPH2655 Omarchy USB probe' ]
[ "$(cat /run/arch/etc/omarchy-mobile-oneplus13-root)" = oneplus13-cph2655-v1 ]
# Stop only processes rooted in this device's installed filesystem.
for signal in TERM KILL; do
    for link in /proc/[0-9]*/root; do
        case "$(readlink "$link" 2>/dev/null || true)" in
            /run/arch|/run/arch/*)
                pid=${link#/proc/}; pid=${pid%/root}
                kill -"$signal" "$pid" 2>/dev/null || true ;;
        esac
    done
    sleep 1
done
sync
awk '$2 == "/run/arch" || index($2, "/run/arch/") == 1 { print $2 }' /proc/mounts |
    sort -r | while read -r path; do umount "$path"; done
! grep -q ' /run/arch ' /proc/mounts
sync
case "$action" in
    --bootloader) exec /bin/reboot-bootloader ;;
    --restart) exec /bin/reboot -f ;;
    --poweroff) exec /bin/poweroff -f ;;
esac
