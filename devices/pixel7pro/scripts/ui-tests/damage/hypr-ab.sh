#!/bin/bash
# hypr-ab.sh SECS : Hyprland GPU cycles/s, panel frames/s and cycles per frame
# over SECS (test only; turns Panthor profiling on, which resets the counters).
secs=${1:-8}
[ "$(cat /sys/devices/platform/28000000.gpu/profiling)" = 1 ] || { echo 1 > /sys/devices/platform/28000000.gpu/profiling; sleep 1; }
h=$(pgrep -x Hyprland)
cyc() { for f in /proc/$h/fdinfo/*; do awk '/drm-client-id/{c=$2} /drm-cycles-panthor/{e=$2} END{if (e!="") print c, e}' $f 2>/dev/null; done | awk '{if ($2 > m[$1]) m[$1] = $2} END {for (c in m) s += m[c]; print s}'; }
dec() { awk '/pixel-decon-start/{s=0; for(i=2;i<=NF;i++) if($i ~ /^[0-9]+$/) s+=$i; else break; print s}' /proc/interrupts; }
c0=$(cyc); d0=$(dec); sleep $secs; c1=$(cyc); d1=$(dec)
fps=$(( (d1 - d0) / secs ))
echo "$(( (c1 - c0) / secs / 1000000 )) Mcycles/s, $fps frames/s, $(( fps > 0 ? (c1 - c0) / secs / fps / 1000 : 0 )) kcycles/frame"
