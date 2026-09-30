#!/bin/bash
# Orderly restart of the native Pixel, optionally into the bootloader.
#
#   pixel-reboot             restart into boot_a (Linux)
#   pixel-reboot bootloader  restart into fastboot, no buttons needed
#
# ABL reads the reboot mode from PMU SYSIP_DAT0 (0x18060810); pixel-reboot.ko
# writes 0xfc there from its restart notifier when its bootloader parameter is
# set. A cold PSCI SYSTEM_RESET loses that value and ABL boots normally, so a
# bootloader restart uses the warm SYSTEM_RESET2. PID 1 then stops writers,
# syncs and remounts the persistent root read-only before restarting.
set -euo pipefail
modem_service=/usr/local/lib/omarchy-mobile/modem/manager.py
if [[ -d /run/pixel-modem && -f $modem_service ]]; then
    python3 "$modem_service" stop
fi
param=/sys/module/pixel_reboot/parameters/bootloader
case ${1:-linux} in
    linux)
        [[ -w $param ]] && echo 0 >"$param"
        echo cold >/sys/kernel/reboot/mode
        ;;
    bootloader)
        if [[ ! -w $param ]]; then
            echo 'pixel-reboot: pixel_reboot module is not loaded; not restarting' >&2
            exit 1
        fi
        echo 1 >"$param"
        echo warm >/sys/kernel/reboot/mode
        ;;
    *)
        echo 'usage: pixel-reboot [linux|bootloader]' >&2
        exit 2
        ;;
esac
sync
echo "pixel-reboot: restarting into ${1:-linux}"
kill -TERM 1
