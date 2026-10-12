#!/bin/sh
# Conservative, volatile provisioning/desktop policy for the native RAM probe.
set -eu
case "$(uname -r)" in *-omarchy-oneplus13-probe*) ;; *) exit 1;; esac
[ "$(tr -d '\000' </sys/firmware/devicetree/base/model)" = 'OnePlus 13 CPH2655 Omarchy USB probe' ]
[ "$(stat -f -c %T /)" = tmpfs ]
# Earlier probes have no CPU frequency driver; leave their behavior unchanged.
[ -d /sys/devices/system/cpu/cpufreq/policy0 ] || exit 0
for item in 0:1996800 6:1958400; do
    policy=/sys/devices/system/cpu/cpufreq/policy${item%:*}
    cap=${item#*:}
    grep -qw "$cap" "$policy/scaling_available_frequencies"
    grep -qw schedutil "$policy/scaling_available_governors"
done
for item in 0:1996800 6:1958400; do
    policy=/sys/devices/system/cpu/cpufreq/policy${item%:*}
    cap=${item#*:}
    echo "$cap" >"$policy/scaling_max_freq"
    echo schedutil >"$policy/scaling_governor"
    [ "$(cat "$policy/scaling_max_freq")" = "$cap" ]
    [ "$(cat "$policy/scaling_governor")" = schedutil ]
    echo "policy${item%:*}: schedutil, cap $cap kHz"
done
