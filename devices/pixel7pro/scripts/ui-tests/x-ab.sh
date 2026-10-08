#!/bin/sh
# x-ab.sh old|new [PHASE_US] : X web app scroll frames with one WebEngine
# library (test only). Restarts the X web app; the profile is the user's.
v=$1 phase=${2:-0}
pkill -TERM -f "^/home/cube/.local/bin/omarchy-mobile-browser --app https://x.com/"; sleep 3
lib=/opt/webengine-ab/$v:/opt/pixel-mesa/lib
/root/tools/as-cube.sh sh -c "LD_LIBRARY_PATH=$lib QTWEBENGINE_FRAME_PHASE_US=$phase QTWEBENGINE_FRAME_STATS=1 setsid -f /home/cube/.local/bin/omarchy-mobile-browser --app https://x.com/ >/tmp/x-$v.log 2>&1"
sleep 14
grep -q "dri2 screen" /tmp/x-$v.log && echo "NO GPU"
echo "$v phase $phase at $(cat /sys/class/thermal/thermal_zone0/temp)"
python3 /root/tools/scroll-frames-test.py both 2
grep "webengine frames" /tmp/x-$v.log | tail -1
