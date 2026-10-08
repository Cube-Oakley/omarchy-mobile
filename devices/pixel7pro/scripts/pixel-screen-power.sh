#!/bin/bash
# Screen-on power per display setting, as the phone sits (run on the phone, as
# root, with the session up and nobody touching it). The battery supplies
# everything (pixel_battery.input_off) and the gauge's coulomb counter gives
# the average current over each window; the scanout's completed transfers
# give the frames sent to the panel. The screen timeout is held at "never"
# meanwhile; everything is put back at exit.
#   pixel-screen-power.sh [SECONDS-PER-WINDOW] [ROUNDS] [CONDITIONS]
# CONDITIONS: space-separated HZ:IDLE[:MIF[:MIF_IDLE_MS]], the Hyprland mode,
# the panel's idle rate at 120 Hz (pixel_scanout.idle_refresh), the memory bus
# floor at 120 Hz in MHz (mif_floor_mhz) and the delay before the bus idles
# after the last frame (mif_idle_ms, 0: never); default "120:0 60:0".
set -euo pipefail
window=${1:-120} rounds=${2:-2} conditions=${3:-120:0 60:0}
idle_param=/sys/module/pixel_scanout/parameters/idle_refresh
mif_param=/sys/module/pixel_scanout/parameters/mif_floor_mhz
mif_idle_param=/sys/module/pixel_scanout/parameters/mif_idle_ms
export XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-1
export HYPRLAND_INSTANCE_SIGNATURE=$(ls /run/user/0/hypr | head -1)
prefs=/root/.local/bin/omarchy-mobile-prefs
input_off=/sys/module/pixel_battery/parameters/input_off
coulomb=/sys/module/pixel_battery/parameters/coulomb
mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug
scanout=$(find /sys/kernel/debug -maxdepth 3 -name pixel_scanout | head -1)
timeout=$(python3 -c 'import json; print(json.load(open("/root/.config/omarchy-mobile/prefs.json")).get("screenTimeout", 60))')
mode() { hyprctl eval "hl.monitor({ output = \"DSI-1\", mode = \"1440x3120@$1\", position = \"auto\", scale = 3 })" >/dev/null; }
idle0=$(cat $idle_param 2>/dev/null || echo 0)
mif0=$(cat $mif_param 2>/dev/null || echo 0)
mif_idle0=$(cat $mif_idle_param 2>/dev/null || echo)
mif_idle() { [[ -w $mif_idle_param && -n $1 ]] && echo "$1" > $mif_idle_param; }
idle() { [[ -w $idle_param ]] && echo "$1" > $idle_param; }
mif() { [[ -w $mif_param && -n $1 ]] && echo "$1" > $mif_param; }
restore() {
    echo 0 > $input_off
    mode 120
    idle "$idle0" || true
    [[ $mif0 != 0 ]] && mif "$mif0" || true
    mif_idle "$mif_idle0" || true
    echo "{\"screenTimeout\": $timeout}" | $prefs set >/dev/null
}
trap restore EXIT
echo '{"screenTimeout": 0}' | $prefs set >/dev/null
/root/.local/bin/omarchy-mobile-display on || true
sleep 3
echo 1 > $input_off
counter() { awk '{print $2}' $coulomb; }
transfers() { awk '/^completed_transfers/{print $2}' "$scanout"; }
for round in $(seq 1 $rounds); do
    for cond in $conditions; do
        IFS=: read -r hz ihz mhz midle <<< "$cond"
        mode $hz
        idle "$ihz" || true
        mif "${mhz:-}" || true
        mif_idle "${midle:-}" || true
        sleep 5
        q0=$(counter) f0=$(transfers) t0=$(date +%s.%N)
        sleep $window
        q1=$(counter) f1=$(transfers) t1=$(date +%s.%N)
        python3 - "$cond" "$q0" "$q1" "$f0" "$f1" "$t0" "$t1" "$(cat /sys/class/backlight/*/brightness)" <<'PY'
import sys
hz, q0, q1, f0, f1, t0, t1, bl = sys.argv[1:]
dt = float(t1) - float(t0)
ma = (int(q0) - int(q1)) / 1e6 / (dt / 3600)
print(f"{hz:>6}: {ma:6.1f} mA over {dt:.0f} s, {(int(f1) - int(f0)) / dt:5.1f} frames/s, brightness {bl}")
PY
    done
done
