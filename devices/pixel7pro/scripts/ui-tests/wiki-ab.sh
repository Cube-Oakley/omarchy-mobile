#!/bin/sh
# wiki-ab.sh old|new : Wikipedia scroll frames with one WebEngine library (test only)
pkill -TERM -f "^/home/cube/.local/bin/omarchy-mobile-browser https://en.wikipedia.org"; sleep 2
/root/tools/as-cube.sh sh -c "LD_LIBRARY_PATH=/opt/webengine-ab/$1:/opt/pixel-mesa/lib QTWEBENGINE_FRAME_STATS=1 QTWEBENGINE_REMOTE_DEBUGGING=127.0.0.1:9333 setsid -f /home/cube/.local/bin/omarchy-mobile-browser https://en.wikipedia.org/wiki/Linux >/tmp/wiki-$1.log 2>&1"
for i in $(seq 1 40); do curl -s -m 2 http://127.0.0.1:9333/json >/dev/null && break; sleep 0.5; done; sleep 8
grep -m1 -o "/opt/webengine-ab/[a-z]*/libQt6WebEngineCore.so.6" /proc/$(pgrep -n -f "^/home/cube/.local/bin/omarchy-mobile-browser https://en.wikipedia.org")/maps
echo "$1 at $(cat /sys/class/thermal/thermal_zone0/temp)"
python3 /root/tools/scroll-frames-test.py drag 2
python3 /root/tools/cdp-eval.py 9333 "scrollTo(0,0); 1" >/dev/null; sleep 1
python3 /root/tools/scroll-frames-test.py fling 1
grep "webengine frames" /tmp/wiki-$1.log | tail -2
pkill -TERM -f "^/home/cube/.local/bin/omarchy-mobile-browser https://en.wikipedia.org"
