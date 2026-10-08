#!/bin/bash
# Real-use standby on the cable: the session's sleep policy as is, the
# screen timed out as by its timeout (dark, or the always-on display with
# "ambient"), the battery supplying everything (pixel_battery input_off), and
# pixel-suspend allowed to sleep on the cable (/run/pixel-suspend-allow-charging).
# Logs the average current every 5 minutes and at the end. Run as root:
#   pixel-realistic-standby.sh SECONDS LABEL [off|ambient]
set -u
secs=${1:-1800} label=${2:-run} screen=${3:-off}
log=/run/standby-$label.log
flag=/run/pixel-suspend-allow-charging
input_off=/sys/module/pixel_battery/parameters/input_off
user=$(sed -n 's/^user=//p' /etc/omarchy-mobile/session)
uid=$(id -u "$user"); home=$(getent passwd "$user" | cut -d: -f6); runtime=/run/user/$uid
hist=$home/.local/state/omarchy-mobile/battery-history.jsonl
note() { echo "$(date +%T) $*" >> "$log"; sync; }
display() {
    setpriv --reuid="$uid" --regid="$(id -g "$user")" --init-groups env -i HOME="$home" USER="$user" \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/bin XDG_RUNTIME_DIR="$runtime" WAYLAND_DISPLAY=wayland-1 \
        HYPRLAND_INSTANCE_SIGNATURE="$(ls "$runtime/hypr" | head -1)" \
        "$home/.local/bin/omarchy-mobile-display" "$@" >/dev/null 2>&1
}
used() { awk '{print $2}' /sys/module/pixel_battery/parameters/coulomb; }
finish() { rm -f $flag; echo 0 > $input_off; note done; }
trap finish EXIT
touch $flag; echo 1 > $input_off
display on; sleep 3
if [[ $screen == ambient ]]; then display idle ambient; else display idle; fi
sleep 5
lines0=$(wc -l < $hist); entries0=$(dmesg | grep -c 'PM: suspend entry')
exits0=$(cat /sys/module/pixel_sleep/parameters/exit_count)
u0=$(used); t0=$(date +%s.%N)
note "start: battery $(cat /sys/class/power_supply/battery/capacity)%, used $u0 nAh"
# Wall time: sleep(1) counts only time awake (CLOCK_MONOTONIC stops in
# suspend), so poll the clock; each wake moves the loop on.
end=$(( ${t0%.*} + secs )); last=${t0%.*}
while (( $(date +%s) < end )); do
    sleep 1
    if (( $(date +%s) - last >= 300 )); then
        last=$(date +%s)
        note "so far $(python3 -c "print(round(abs($(used)-$u0)/1e6*3600/($(date +%s.%N)-$t0),1))") mA"
    fi
done
u1=$(used); t1=$(date +%s.%N)
note "average $(python3 -c "print(round(abs($u1-$u0)/1e6*3600/($t1-$t0),1))") mA over $(python3 -c "print(round($t1-$t0))") s; suspend entries +$(( $(dmesg | grep -c 'PM: suspend entry') - entries0 )), SYS_SLEEP exits +$(( $(cat /sys/module/pixel_sleep/parameters/exit_count) - exits0 ))"
tail -n +$((lines0 + 1)) $hist > /run/standby-$label.history
