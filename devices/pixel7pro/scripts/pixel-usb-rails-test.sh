#!/bin/bash
# The USB PHY supplies' share of the deep-sleep floor, unplugged (the only
# safe way: USB is never switched off while it carries the session). Run as
# root on the phone, detached:
#   pixel-usb-rails-test.sh TEST-MODULE [SECONDS] [LOG]
# Waits up to 10 minutes for the cable to come out, then with the screen off
# and the session's sleep policy paused:
#   A: two deep sleeps as booted (the PHY supplies LDO8M-LDO10M always on);
#   B: two with them following PWREN (off inside SYS_SLEEP only), through
#      TEST-MODULE, a pixel-pmic-opmode build that knows the USB test rails.
# Then it puts the fields back (restore=1 at unload), reloads the boot's
# pixel-pmic-opmode, resumes the policy and lights the screen: the signal to
# plug the cable back in. If USB does not enumerate afterwards, restart.
set -u
test_module=$1
seconds=${2:-150}
log=${3:-/run/pixel-usb-rails.log}
boot_module=/usr/local/lib/omarchy-mobile/suspend/pixel-pmic-opmode.ko
user=$(sed -n 's/^user=//p' /etc/omarchy-mobile/session)
uid=$(id -u "$user")
home=$(getent passwd "$user" | cut -d: -f6)
runtime=/run/user/$uid
policy=$(pgrep -f "^python3 $home/.local/bin/omarchy-mobile-sleep run$" | head -1)
note() { echo "$(date +%T) $*" >> "$log"; sync; }
display() {
    setpriv --reuid="$uid" --regid="$(id -g "$user")" --init-groups env -i HOME="$home" USER="$user" \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/bin XDG_RUNTIME_DIR="$runtime" WAYLAND_DISPLAY=wayland-1 \
        HYPRLAND_INSTANCE_SIGNATURE="$(ls "$runtime/hypr" | head -1)" \
        "$home/.local/bin/omarchy-mobile-display" "$@" >/dev/null 2>&1
}
boot_opmode() {
    if [[ -d /sys/module/pixel_pmic_opmode ]]; then
        echo 1 > /sys/module/pixel_pmic_opmode/parameters/restore
        rmmod pixel_pmic_opmode
    fi
    insmod "$boot_module" retention=1 suspend=all || note "boot pixel-pmic-opmode did not load"
}
finish() {
    [[ $phase == B ]] && boot_opmode
    [[ -n $policy ]] && kill -CONT "$policy"
    display on
    note "done (opmode $(cat /sys/module/pixel_pmic_opmode/parameters/suspend 2>/dev/null))"
}
sleep_once() {
    local result
    result=$(OMARCHY_MOBILE_WAKE_PURPOSE=ambient /usr/local/sbin/pixel-suspend --keep-dark --wake-after "$seconds" 2>&1 | tail -1)
    note "$1: $result"
    sleep 10
}

phase=wait
note "waiting for the cable to come out"
for i in $(seq 600); do
    [[ $(cat /sys/class/power_supply/usb/online 2>/dev/null) == 0 ]] && break
    sleep 1
done
if [[ $(cat /sys/class/power_supply/usb/online 2>/dev/null) != 0 ]]; then
    note "still plugged in after 10 minutes; nothing done"
    exit 1
fi
trap finish EXIT
note "unplugged; battery $(cat /sys/class/power_supply/battery/capacity)%"
sleep 20
[[ -n $policy ]] && kill -STOP "$policy"
display off
sleep 10
phase=A
sleep_once "A1 PHY supplies on"
sleep_once "A2 PHY supplies on"
rmmod pixel_pmic_opmode
phase=B
if ! insmod "$test_module" retention=1 suspend=all,LDO8M,LDO9M,LDO10M; then
    note "test module did not load"
    phase=B    # finish reloads the boot module
    exit 1
fi
note "$(dmesg | grep -E 'pixel-pmic-opmode: LDO(8|9|10)M' | tail -3 | tr '\n' ' ')"
sleep_once "B1 PHY supplies off in sleep"
sleep_once "B2 PHY supplies off in sleep"
