#!/bin/bash
# Five looping 1080p H.264 videos on one page, more than the MFC's 128 MB CMA
# area and 8 MiB context block hold, in the browser with a chosen WebEngine
# library (NAME = a directory in /opt/webengine-ab, or "installed"). Logs the
# page's per-video status, the GPU process's CPU time and the codec's kernel
# errors every 10 s for SECONDS, then closes the browser. The page and videos
# are in /tmp/vt (blank.html, index.html, v1-v5.mp4). The phone must be unlocked and lit.
# Usage (root): mfc-pressure-test.sh NAME [SECONDS]
name=$1 seconds=${2:-70} port=9333
log=/tmp/mfc-$name.log
lib=/opt/pixel-mesa/lib
[[ $name == installed ]] || lib=/opt/webengine-ab/$name:$lib
pgrep -f '^python3 -m http.server 8099' >/dev/null ||
    /root/tools/as-cube.sh sh -c 'cd /tmp/vt && setsid -f python3 -m http.server 8099 --bind 127.0.0.1 >/dev/null 2>&1'
# dmesg by time: the modem's PCIe messages wrap the ring buffer.
since=$(cut -d' ' -f1 /proc/uptime)
new_dmesg() { dmesg | awk -v t=$since -F'[][]' '$2 + 0 > t'; }
# A web app with a scratch profile: the user's browser, its tabs and its
# profile stay untouched (a plain launch restored and saved the user's tabs).
profile=/tmp/mfc-profile
rm -rf $profile
mkdir -p $profile/data/omarchy-mobile/browser
cp /home/cube/.local/share/omarchy-mobile/browser/Browser.qml $profile/data/omarchy-mobile/browser/
chown -R cube:cube $profile
xdg="XDG_DATA_HOME=$profile/data XDG_STATE_HOME=$profile/state XDG_CONFIG_HOME=$profile/config XDG_CACHE_HOME=$profile/cache"
/root/tools/as-cube.sh sh -c "$xdg LD_LIBRARY_PATH=$lib QTWEBENGINE_REMOTE_DEBUGGING=127.0.0.1:$port setsid -f /home/cube/.local/bin/omarchy-mobile-browser --app http://127.0.0.1:8099/blank.html >$log 2>&1"
browser_cpu() {
    # QtWebEngine decodes and draws in the browser process (in-process GPU).
    local pid total=0
    for pid in $(pgrep -f '^/home/cube/.local/bin/omarchy-mobile-browser --app http://127.0.0.1:8099/') $(pgrep -x QtWebEngineProc); do
        total=$((total + $(awk '{print $14 + $15}' /proc/$pid/stat 2>/dev/null || echo 0)))
    done
    echo $total
}
sleep 5
python3 /root/tools/cdp-media.py $port $((seconds - 10)) http://127.0.0.1:8099/index.html >/tmp/mfc-$name-media.txt 2>&1 &
start=$(browser_cpu)
for ((t = 10; t <= seconds; t += 10)); do
    sleep 10
    title=$(timeout 5 python3 /root/tools/cdp.py $port page Runtime.evaluate '{"expression":"document.title"}' 2>/dev/null |
        python3 -c 'import json,sys; print(json.load(sys.stdin)["result"]["value"])' 2>/dev/null)
    new=$(new_dmesg)
    echo "t=$t cpu_ticks=$(( $(browser_cpu) - start )) cma_fail=$(grep -c 'pixel-mfc: alloc failed' <<< "$new") mfc_err=$(grep -c 'Interrupt Error' <<< "$new") dqbuf_err=$(grep -c 'DQBUF after unrecoverable' <<< "$new")"
    echo "   $title"
done
pid=$(pgrep -f '^/home/cube/.local/bin/omarchy-mobile-browser --app http://127.0.0.1:8099/')
[[ -n $pid ]] && kill $pid
sleep 2
wait
echo "--- decoders (DevTools Media)"
cat /tmp/mfc-$name-media.txt
echo "--- kernel"
new_dmesg | grep -E 'mfc|MFC|alloc failed|vidioc|DQBUF|instance' | grep -v 'pixel-pd-off' | sed -E 's/^\[ *[0-9.]+\] //' | sort | uniq -c | sort -rn | head -12 | cut -c1-160
echo "--- browser log"
grep -E 'ERROR|WARNING' $log | sed -E 's/^\[[0-9:\/.]+:/[/' | sort | uniq -c | sort -rn | head -12 | cut -c1-200
