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
# The S2MPG13 keeps the camera PMIC's enable lines through a restart, which
# left the SLG51002 on in the next boot (about 13 mA). Unloading the camera
# driver, then its power module, powers it down first.
for module in pixel_camera pixel_camera_power; do
    [[ ! -d /sys/module/$module ]] || rmmod "$module" ||
        echo "pixel-reboot: $module stays loaded" >&2
done
# The PMIC also keeps the sleep opmodes pixel-pmic-opmode set (rails that
# follow PWREN or PWREN_MIF). After a day of deep sleep, the first boot from
# a restart with them in place hit an APM watchdog reset, twice; the second
# boot was clean. Put back the fields the module found at load.
opmode=/sys/module/pixel_pmic_opmode/parameters/restore
if [[ -w $opmode ]]; then
    echo 1 >"$opmode" && rmmod pixel_pmic_opmode ||
        echo 'pixel-reboot: PMIC opmodes stay as set for sleep' >&2
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
