#!/bin/bash
set -euo pipefail
case ${1:-} in
    --check)
        [[ -x /usr/local/sbin/pixel-reboot && -w /sys/module/pixel_reboot/parameters/bootloader ]]
        echo '{"restart":true,"bootloader":true}'
        ;;
    restart) exec /usr/local/sbin/pixel-reboot linux ;;
    bootloader) exec /usr/local/sbin/pixel-reboot bootloader ;;
    *) echo 'Unsupported restart action' >&2; exit 2 ;;
esac
