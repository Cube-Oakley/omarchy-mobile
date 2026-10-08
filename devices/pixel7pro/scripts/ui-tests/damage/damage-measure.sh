#!/bin/bash
# damage-measure.sh PORT SECS PHASE... (test only): for each phase, sets the
# damage test page's body class to PHASE over DevTools ("" still, "small" a
# 57 px square spinning, "full" the whole page pulsing), waits 3 s, then
# measures SECS: panel frames, thread CPU, GPU cycles per DRM client
# (Panthor fdinfo with profiling on) and the ODPM rails.
# The page: python3 -m http.server 8765 in this directory (as the session
# user), then omarchy-mobile-browser --app http://127.0.0.1:8765/damage-test.html
# with QTWEBENGINE_REMOTE_DEBUGGING=127.0.0.1:PORT.
port=$1 secs=$2; shift 2
echo 1 > /sys/devices/platform/28000000.gpu/profiling
browser=$(pgrep -f "omarchy-mobile-browser --app http://127.0.0.1:8765/" | head -1)
pids="$browser $(pgrep -x Hyprland) $(pgrep -f '^/usr/lib/qt6/QtWebEngineProcess --type=renderer' | tr '\n' ' ')"
gpu() { for p in $browser $(pgrep -x Hyprland); do for f in /proc/$p/fdinfo/*; do awk -v p=$p '/drm-client-id/{c=$2} /drm-cycles-panthor/{e=$2} END{if (e!="") print p"/"c, e}' $f 2>/dev/null; done; done | sort -u; }
thr() { for p in $pids; do for t in /proc/$p/task/*; do read -r l < $t/stat 2>/dev/null || continue; c=${l#*) }; set -- $c; echo "$p/${t##*/} $(tr ' ' _ < $t/comm) $(( ${12} + ${13} ))"; done; done; }
# The meter gives each rail's energy (uJ) since the last read, over the
# seconds its chip's line says.
rails() { cat /sys/module/pixel_odpm/parameters/energy; }
dec() { awk '/pixel-decon-start/{s=0; for(i=2;i<=NF;i++) if($i ~ /^[0-9]+$/) s+=$i; else break; print s}' /proc/interrupts; }
for phase in "$@"; do
    python3 /root/tools/cdp.py $port page Runtime.evaluate "{\"expression\": \"document.body.className='$phase'\"}" >/dev/null
    sleep 3
    d0=$(dec); thr > /tmp/dm-t0; gpu > /tmp/dm-g0; rails > /tmp/dm-r0
    sleep $secs
    d1=$(dec); thr > /tmp/dm-t1; gpu > /tmp/dm-g1; rails > /tmp/dm-r1
    echo "== ${phase:-still}: $(( (d1 - d0) / secs )) panel frames/s"
    awk -v s=$secs 'NR==FNR{a[$1]=$3; next} ($1 in a) {d=($3-a[$1])/s; if (d>=2) printf "  cpu %5.1f%% %s\n", d, $2}' /tmp/dm-t0 /tmp/dm-t1 | sort -k2 -rn | head -8
    awk -v s=$secs 'NR==FNR{a[$1]=$2; next} ($1 in a) && !seen[$1]++ {v=($2-a[$1])/s/1e6; if (v>0.05) printf "  gpu %6.1f Mcycles/s client %s\n", v, $1}' /tmp/dm-g0 /tmp/dm-g1
    awk '$1 == "seconds" {sec = $3; if (sec < '"$secs"' - 1) bad = 1; next}
         $1 ~ /G3D|MIF|MEM|CPUCL|INT|DISPLAY|SLC/ {v = $2 / sec / 1000; t += v; printf "%s %.0f  ", $1, v}
         END {printf "\n  total %.0f mW%s\n", t, bad ? " (meter read by another reader meanwhile: partial)" : ""}' /tmp/dm-r1
done
python3 /root/tools/cdp.py $port page Runtime.evaluate '{"expression": "document.body.className=\"\""}' >/dev/null
echo 0 > /sys/devices/platform/28000000.gpu/profiling
