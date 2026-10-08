#!/bin/bash
# What the deep-sleep floor is made of: A/B sleeps in one boot, with the
# cable in and the charger's input off (the phone runs from its battery, so
# the gauge's coulomb counter measures it). Run as root on the phone:
#   pixel-drain-ab.sh [SECONDS] [LOG]
# Each test sets the screen through the session's display script (off, or
# the always-on display), sleeps once through pixel-suspend --keep-dark as
# the sleep policy does, and logs pixel-suspend's result (deep_ma: the
# average battery current over the deep sleeps). The session's sleep policy
# is paused meanwhile. Bluetooth, the fingerprint service, the ambient touch
# flag, charging and the policy are put back at the end, also on failure.
set -u
seconds=${1:-150}
log=${2:-/run/pixel-drain-ab.log}
user=$(sed -n 's/^user=//p' /etc/omarchy-mobile/session)
uid=$(id -u "$user")
home=$(getent passwd "$user" | cut -d: -f6)
runtime=/run/user/$uid
input_off=/sys/module/pixel_battery/parameters/input_off
fp=/var/lib/omarchy-mobile/fingerprint
touch_flag=$runtime/omarchy-mobile-ambient-touch
policy=$(pgrep -f "^python3 $home/.local/bin/omarchy-mobile-sleep run$" | head -1)
fp_loop=$(pgrep -f "^sh -c while \[ -f $fp/enabled \]" | head -1)
touch_was=$(cat "$touch_flag" 2>/dev/null || echo 1)

as_user() {
    setpriv --reuid="$uid" --regid="$(id -g "$user")" --init-groups env -i HOME="$home" USER="$user" \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/bin XDG_RUNTIME_DIR="$runtime" WAYLAND_DISPLAY=wayland-1 \
        HYPRLAND_INSTANCE_SIGNATURE="$(ls "$runtime/hypr" | head -1)" "$@"
}
display() { as_user "$home/.local/bin/omarchy-mobile-display" "$@" >/dev/null 2>&1; }
note() { echo "$(date +%T) $*" >> "$log"; }

restore() {
    display on
    echo 0 > "$input_off"
    printf '%s\n' "$touch_was" > "$touch_flag"
    if [[ ! -d /sys/module/hci_uart ]]; then
        touch /run/pixel-bt-sleeping
        /usr/local/sbin/pixel-bt-restart >/dev/null 2>&1
    fi
    if [[ -f $fp/enabled ]] && ! pgrep -f "^sh -c while \[ -f $fp/enabled \]" >/dev/null; then
        setsid -f sh -c "while [ -f $fp/enabled ]; do
            $fp/fp-hal-lab.sh $fp/runtime.tar $fp/fp-shim.so serve
            sleep 30
        done >>/run/pixel-fingerprint.log 2>&1 </dev/null"
    fi
    [[ -n $policy ]] && kill -CONT "$policy"
    note "restored: input_off $(cat "$input_off"), bluetooth $([[ -d /sys/module/hci_uart ]] && echo loaded || echo missing)"
}
trap restore EXIT

# One sleep with the screen as $2 (off or ambient).
test_sleep() {
    local label=$1 screen=$2 result
    display "$screen"
    sleep 15
    result=$(OMARCHY_MOBILE_WAKE_PURPOSE=ambient /usr/local/sbin/pixel-suspend --keep-dark --allow-charging \
        --wake-after "$seconds" 2>&1 | tail -1)
    note "$label ($screen): $result"
    display on
    sleep 15
}

note "start: ${seconds} s sleeps, battery $(cat /sys/class/power_supply/battery/capacity)%"
[[ -n $policy ]] && kill -STOP "$policy"
echo 1 > "$input_off"
sleep 5

test_sleep base off
rmmod hci_uart pixel_bt 2>/dev/null
test_sleep no-bt off
[[ -n $fp_loop ]] && kill "$fp_loop"
"$fp/fp-hal-exec.sh" stop >/dev/null 2>&1
for i in $(seq 30); do pgrep -f "fp-client serve" >/dev/null || break; sleep 1; done
test_sleep no-bt-no-fp off
test_sleep no-bt-no-fp ambient
printf '0\n' > "$touch_flag"
test_sleep no-bt-no-fp-no-touch ambient
printf '%s\n' "$touch_was" > "$touch_flag"
restore
trap restore EXIT
[[ -n $policy ]] && kill -STOP "$policy"
echo 1 > "$input_off"
sleep 60    # the fingerprint service and Bluetooth come back
test_sleep base-again off
test_sleep base ambient
note "done"
