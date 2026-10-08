#!/bin/bash
# Each power rail's average over one deep sleep, while connected: the
# diagnostic for idle drain (kernel/odpm). Run as root on the phone:
#   pixel-sleep-rails.sh [SECONDS] [OUT]
# Pauses the session's sleep policy (the meters' other reader), reads the
# meters' energy right before and after `pixel-suspend --allow-charging`,
# then lets the policy go on. OUT (default /run/pixel-sleep-rails.txt) gets
# the suspend result and each rail's energy over the interval, as mW.
set -u
seconds=${1:-180}
out=${2:-/run/pixel-sleep-rails.txt}
energy=/sys/module/pixel_odpm/parameters/energy
[[ -r $energy ]] || { echo "pixel-odpm is not loaded" > "$out"; exit 1; }
policy=$(pgrep -f '^python3 /home/[^/]+/.local/bin/omarchy-mobile-sleep run$' | head -1)
[[ -n $policy ]] && kill -STOP "$policy"
trap '[[ -n $policy ]] && kill -CONT "$policy"' EXIT
{
    echo "sleep $seconds s, $(date '+%F %T')"
    cat "$energy" > /dev/null
    result=$(/usr/local/sbin/pixel-suspend --allow-charging --wake-after "$seconds" 2>&1)
    after=$(cat "$energy")
    echo "suspend: $result"
    # "seconds PMIC S" heads each PMIC's rails ("RAIL uJ"): the average
    # power per rail over that PMIC's interval, the largest first.
    awk '$1 == "seconds" { span = $3; print; next }
         NF == 2 && span > 0 { total += $2 / 1000 / span; printf "%-20s %8.1f mW\n", $1, $2 / 1000 / span }
         END { printf "%-20s %8.1f mW\n", "metered total", total }' <<< "$after" | sort -k2 -g -r
} > "$out" 2>&1
