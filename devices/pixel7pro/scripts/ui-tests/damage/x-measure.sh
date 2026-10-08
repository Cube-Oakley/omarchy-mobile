#!/bin/bash
# x-measure.sh SECS LABEL : the X web app at rest (test only): panel frames,
# touches, thread CPU, GPU cycles per client, ODPM rails, over SECS.
secs=$1 label=$2
echo 1 > /sys/devices/platform/28000000.gpu/profiling
browser=$(pgrep -f "omarchy-mobile-browser --app https://x.com/$" | head -1)
pids="$browser $(pgrep -x Hyprland) $(pgrep -f '^/usr/lib/qt6/QtWebEngineProcess --type=renderer' | tr '\n' ' ')"
gpu() { for p in $browser $(pgrep -x Hyprland); do for f in /proc/$p/fdinfo/*; do awk -v p=$p '/drm-client-id/{c=$2} /drm-cycles-panthor/{e=$2} END{if (e!="") print p"/"c, e}' $f 2>/dev/null; done; done | sort -u; }
thr() { for p in $pids; do for t in /proc/$p/task/*; do read -r l < $t/stat 2>/dev/null || continue; c=${l#*) }; set -- $c; echo "$p/${t##*/} $(tr ' ' _ < $t/comm) $(( ${12} + ${13} ))"; done; done; }
# The meter gives each rail's energy (uJ) since the last read, over the
# seconds its chip's line says.
rails() { cat /sys/module/pixel_odpm/parameters/energy; }
irq() { awk -v n="$1" '$0 ~ n {s=0; for(i=2;i<=NF;i++) if($i ~ /^[0-9]+$/) s+=$i; else break; print s}' /proc/interrupts; }
d0=$(irq pixel-decon-start); t0=$(irq pixel-touch-attn); thr > /tmp/xm-t0; gpu > /tmp/xm-g0; rails > /tmp/xm-r0
sleep $secs
d1=$(irq pixel-decon-start); t1=$(irq pixel-touch-attn); thr > /tmp/xm-t1; gpu > /tmp/xm-g1; rails > /tmp/xm-r1
echo "== $label: $(( (d1 - d0) / secs )) panel frames/s, $((t1 - t0)) touch interrupts"
awk -v s=$secs 'NR==FNR{a[$1]=$3; next} ($1 in a) {d=($3-a[$1])/s; if (d>=2) printf "  cpu %5.1f%% %s\n", d, $2}' /tmp/xm-t0 /tmp/xm-t1 | sort -k2 -rn | head -8
awk -v s=$secs 'NR==FNR{a[$1]=$2; next} ($1 in a) && !seen[$1]++ {v=($2-a[$1])/s/1e6; if (v>0.05) printf "  gpu %6.1f Mcycles/s %s\n", v, $1}' /tmp/xm-g0 /tmp/xm-g1
awk '$1 == "seconds" {sec = $3; if (sec < '"$secs"' - 1) bad = 1; next}
     $1 ~ /G3D|MIF|MEM|CPUCL|INT|DISPLAY|SLC/ {v = $2 / sec / 1000; t += v; printf "%s %.0f  ", $1, v}
     END {printf "\n  total %.0f mW%s\n", t, bad ? " (meter read by another reader meanwhile: partial)" : ""}' /tmp/xm-r1
echo 0 > /sys/devices/platform/28000000.gpu/profiling
