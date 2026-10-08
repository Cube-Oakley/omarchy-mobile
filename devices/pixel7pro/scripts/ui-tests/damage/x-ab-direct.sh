#!/bin/bash
# x-ab-direct.sh VARIANT : X at rest with videos stopped (test only; restarts
# the X web app on the user profile, measures with x-measure.sh in /root/tools).
#   installed  the installed browser and library
#   qt         the new library (0007), Qt draws (OMARCHY_BROWSER_DIRECT=0)
#   direct     the new library, frames to Hyprland directly
v=$1
for p in $(pgrep -f "omarchy-mobile-browser --app https://x.com/$"); do kill -TERM $p; done
sleep 5
case $v in
    installed) cmd=/home/cube/.local/bin/omarchy-mobile-browser env="" ;;
    qt) cmd=/home/cube/.cache/omarchy-mobile/direct-test/build/omarchy-mobile-browser env="OMARCHY_BROWSER_DIRECT=0 LD_LIBRARY_PATH=/opt/webengine-ab/direct:/opt/pixel-mesa/lib" ;;
    direct) cmd=/home/cube/.cache/omarchy-mobile/direct-test/build/omarchy-mobile-browser env="LD_LIBRARY_PATH=/opt/webengine-ab/direct:/opt/pixel-mesa/lib" ;;
esac
/root/tools/as-cube.sh sh -c "$env QTWEBENGINE_REMOTE_DEBUGGING=127.0.0.1:9335 setsid -f $cmd --app https://x.com/ >/tmp/x-$v.log 2>&1"
sleep 20
python3 /root/tools/cdp.py 9335 page Runtime.evaluate '{"expression": "HTMLMediaElement.prototype.play = function () { return Promise.resolve(); }; document.querySelectorAll(\"video\").forEach(v => v.pause()); scrollTo(0, 0); document.querySelectorAll(\"video\").length", "returnByValue": true}' | head -c 200; echo
sleep 8
bash /root/tools/x-measure.sh 30 "$v"
