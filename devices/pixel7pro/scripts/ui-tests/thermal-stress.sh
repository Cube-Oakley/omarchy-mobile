#!/bin/sh
# Thermal stress (test only): 8 CPU busy loops and a WebGL shader page for
# SECONDS, logging chip and skin temperatures, clocks and guard steps every 2 s.
#   thermal-stress.sh SECONDS [cpu|gpu|both]
secs=${1:-180} what=${2:-both}
T=/sys/class/thermal
zone() { for z in $T/thermal_zone*; do [ "$(cat $z/type)" = "$1" ] && cat $z/temp; done; }
pids=""
if [ "$what" != gpu ]; then
    for i in 1 2 3 4 5 6 7 8; do sh -c 'while :; do :; done' & pids="$pids $!"; done
fi
if [ "$what" != cpu ]; then
    /root/tools/as-cube.sh sh -c "setsid -f /home/cube/.local/bin/omarchy-mobile-browser file:///tmp/gpu-burn.html >/tmp/gpu-burn.log 2>&1"
fi
start=$(date +%s)
echo "t big mid g3d | big_MHz mid_MHz gpu_MHz | skin guard"
while [ $(( $(date +%s) - start )) -lt "$secs" ]; do
    sk=$(python3 /usr/local/lib/omarchy-mobile/thermal/pixel-skin-guard.py --once 2>/dev/null | sed -n 's/.*skin //p')
    lvl=$(grep -o 'level [0-9] -> [0-9]' /run/pixel-skin-guard.log 2>/dev/null | tail -1)
    echo "$(( $(date +%s) - start )) $(( $(zone pixel-big) / 1000 )) $(( $(zone pixel-mid) / 1000 )) $(( $(zone pixel-g3d) / 1000 )) | $(( $(cat /sys/devices/system/cpu/cpufreq/policy6/scaling_cur_freq) / 1000 )) $(( $(cat /sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq) / 1000 )) $(( $(cat /sys/class/devfreq/28000000.gpu/cur_freq) / 1000000 )) | $sk $lvl"
    sleep 2
done
[ -n "$pids" ] && kill $pids
pkill -TERM -f "^/home/cube/.local/bin/omarchy-mobile-browser file:///tmp/gpu-burn.html"
echo done
