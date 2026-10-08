#!/bin/bash
# Cycle the DPU/DISP domains through the AOD hibernate path N times
# (ambient on: pipeline off after 100 ms; ambient off: pipeline on).
# Optional second argument "tmu": hammer ACPM TMU reads at the same time.
n=${1:-300}; mode=${2:-plain}
user=cube; uid=$(id -u $user); home=/home/$user; runtime=/run/user/$uid
display() { setpriv --reuid=$uid --regid=$(id -g $user) --init-groups env -i HOME=$home USER=$user PATH=/usr/local/sbin:/usr/local/bin:/usr/bin XDG_RUNTIME_DIR=$runtime WAYLAND_DISPLAY=wayland-1 HYPRLAND_INSTANCE_SIGNATURE=$(ls $runtime/hypr | head -1) $home/.local/bin/omarchy-mobile-display "$@" >/dev/null 2>&1; }
grep -q ' /sys/kernel/debug ' /proc/mounts || mount -t debugfs none /sys/kernel/debug
stats=$(ls /sys/kernel/debug/dri/*/pixel_scanout | head -1)
p=/sys/module/pixel_scanout/parameters/ambient
display on; sleep 2; sync
if [[ $mode == tmu ]]; then
    # About 1500 ACPM TMU reads a second through the thermal zones.
    python3 -c 'import time
fs = [open("/sys/class/thermal/thermal_zone%d/temp" % i) for i in range(3)]
while True:
    for f in fs:
        f.seek(0); f.read(); time.sleep(0.0005)' &
    hammer=$!
fi
trap '[[ -n $hammer ]] && kill $hammer; echo 0 > $p; display on' EXIT
echo "dpu-cycle: start $n $mode" > /dev/kmsg
grep -E 'hibernations|power_cycles|failures' $stats | tr '\n' ' '; echo
for ((i = 1; i <= n; i++)); do
    echo 1 > $p || { echo "ambient on failed at $i"; break; }
    sleep 0.17
    echo 0 > $p || { echo "ambient off failed at $i"; break; }
    sleep 0.03
    (( i % 100 == 0 )) && echo "dpu-cycle: $i" > /dev/kmsg
done
grep -E 'hibernations|power_cycles|failures' $stats | tr '\n' ' '; echo
echo "dpu-cycle: done" > /dev/kmsg
