#!/bin/sh
# Run a test with the clocks held high, then put the floors back (test only).
# The GPU floor changes only while the GPU is awake (a reclock while it is
# power-gated reset the phone, 09-28): writes wait for runtime "active", and a
# synthetic fling keeps it awake for the restore.
#   pinned-clocks-run.sh COMMAND...
F=/sys/class/devfreq/28000000.gpu/min_freq
R=/sys/devices/platform/28000000.gpu/power/runtime_status
C=/sys/devices/system/cpu/cpufreq
gpu_floor() {
    i=0
    while [ $i -lt 8000 ]; do
        if [ "$(cat $R)" = active ] && echo "$1" > $F 2>/dev/null; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.001
    done
    return 1
}
old0=$(cat $C/policy0/scaling_min_freq) old4=$(cat $C/policy4/scaling_min_freq) old6=$(cat $C/policy6/scaling_min_freq)
oldg=$(cat $F)
echo 1704000 > $C/policy0/scaling_min_freq
echo 1999000 > $C/policy4/scaling_min_freq
echo 2048000 > $C/policy6/scaling_min_freq
"$@" &
test=$!
gpu_floor 885000000 && echo "GPU floor $(cat $F)" || echo "GPU floor not set"
wait $test
python3 /root/tools/touch-seq.py swipe:720,2500,900,110 wait:1200 >/dev/null 2>&1 &
gpu_floor "$oldg" && echo "GPU floor back to $(cat $F)" || echo "GPU FLOOR NOT RESTORED: $(cat $F)"
wait
echo "$old0" > $C/policy0/scaling_min_freq
echo "$old4" > $C/policy4/scaling_min_freq
echo "$old6" > $C/policy6/scaling_min_freq
echo "CPU floors back: $(cat $C/policy0/scaling_min_freq) $(cat $C/policy4/scaling_min_freq) $(cat $C/policy6/scaling_min_freq)"
