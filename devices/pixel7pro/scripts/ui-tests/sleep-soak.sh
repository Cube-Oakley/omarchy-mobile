#!/bin/bash
# sleep-soak.sh [MINUTES-PER-PHASE] [PHASES]  (test only; run on the phone as
# root, with setsid: SSH drops while the phone sleeps)
#
# The sleep policy as on the battery, unattended, on the cable: the battery
# supplies everything (pixel_battery.input_off) and the suspend adapter
# sleeps on the cable (/run/pixel-suspend-allow-charging). Each phase puts
# the screen in a state and waits MINUTES by the wall clock (the script only
# runs while the phone is awake), then logs the gauge's average current.
# PHASES: ambient (the always-on clock) and/or off; default "ambient off".
# The battery history's "awake" reasons and the sleep log say the rest.
# Results: /root/tools/soak/sleep-soak.log. Everything is put back at exit.
set -u
minutes=${1:-20} phases=${2:-ambient off}
dir=/root/tools/soak out=/root/tools/soak/sleep-soak.log
mkdir -p $dir
input_off=/sys/module/pixel_battery/parameters/input_off
coulomb=/sys/module/pixel_battery/parameters/coulomb
flag=/run/pixel-suspend-allow-charging
display() { /root/tools/as-cube.sh omarchy-mobile-display "$@" >/dev/null 2>&1; }
restore() {
    echo 0 > $input_off
    rm -f $flag
    echo "$(date +%T) restored charging" >> $out
}
trap restore EXIT
nah() { awk '{print $2}' $coulomb; }
echo "=== $(date '+%F %T') start: $minutes min each: $phases" >> $out
echo 1 > $input_off
touch $flag
for phase in $phases; do
    display "$phase"
    sleep 5
    q0=$(nah) t0=$(date +%s)
    end=$((t0 + minutes * 60))
    while (( $(date +%s) < end )); do sleep 5; done
    q1=$(nah) t1=$(date +%s)
    python3 -c "import sys; q0, q1, t0, t1 = map(int, sys.argv[1:5]); print(f'{sys.argv[5]}: {(t1 - t0) / 60:.1f} min, {(q0 - q1) / 1e6:.1f} mAh, {(q0 - q1) / 1e6 / ((t1 - t0) / 3600):.1f} mA average')" \
        "$q0" "$q1" "$t0" "$t1" "$phase" >> $out
done
display ambient
