#!/bin/bash
set -euo pipefail
# Root's program (session/sudoers lets the desktop's account run it).
reboot=(/usr/local/sbin/pixel-reboot)
(( EUID == 0 )) || reboot=(sudo -n /usr/local/sbin/pixel-reboot)
case ${1:-} in
    --check)
        [[ -x /usr/local/sbin/pixel-reboot && -e /sys/module/pixel_reboot/parameters/bootloader ]]
        (( EUID == 0 )) || sudo -n -l /usr/local/sbin/pixel-reboot >/dev/null
        echo '{"restart":true,"bootloader":true}'
        ;;
    restart) exec "${reboot[@]}" linux ;;
    bootloader) exec "${reboot[@]}" bootloader ;;
    *) echo 'Unsupported restart action' >&2; exit 2 ;;
esac
