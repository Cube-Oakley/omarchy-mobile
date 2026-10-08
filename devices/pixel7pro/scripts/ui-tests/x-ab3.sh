#!/bin/sh
# x-ab3.sh VARIANT [RUNS] : X web app scroll frames for one browser setup
# (test only; restarts the X web app on the user's profile).
#   base   installed library and browser
#   lib    /opt/webengine-ab/new library, test browser, Qt's swap interval 1
#   swap   /opt/webengine-ab/new library, test browser, swap interval 0
#   mesa   as lib, with the Mesa in /opt/pixel-mesa-ab
#   pf     /opt/webengine-ab/pf library (perfetto fix 0005), test browser,
#          the Mesa in /opt/pixel-mesa-ab
# The test browser is ~cube/.cache/omarchy-mobile/swap-test/build: the browser
# built with a swap interval of 0 unless OMARCHY_BROWSER_SWAP_INTERVAL says
# otherwise (October 6: 0 was no better than Qt's 1 on X, and is not kept).
v=$1 runs=${2:-2}
installed=/home/cube/.local/bin/omarchy-mobile-browser
test=/home/cube/.cache/omarchy-mobile/swap-test/build/omarchy-mobile-browser
for p in $(ps -eo pid,args | grep "omarchy-mobile-browser --app https://x.com/$" | grep -v grep | awk '{print $1}'); do
    kill -TERM "$p"
done
sleep 3
case $v in
    base) cmd="$installed" env="" ;;
    lib) cmd="$test" env="LD_LIBRARY_PATH=/opt/webengine-ab/new:/opt/pixel-mesa/lib OMARCHY_BROWSER_SWAP_INTERVAL=1" ;;
    swap) cmd="$test" env="LD_LIBRARY_PATH=/opt/webengine-ab/new:/opt/pixel-mesa/lib" ;;
    mesa) cmd="$test" env="LD_LIBRARY_PATH=/opt/webengine-ab/new:/opt/pixel-mesa-ab/lib OMARCHY_BROWSER_SWAP_INTERVAL=1" ;;
    pf) cmd="$test" env="LD_LIBRARY_PATH=/opt/webengine-ab/pf:/opt/pixel-mesa-ab/lib OMARCHY_BROWSER_SWAP_INTERVAL=1" ;;
    *) echo "unknown variant $v" >&2; exit 2 ;;
esac
/root/tools/as-cube.sh omarchy-mobile-display on >/dev/null 2>&1
/root/tools/as-cube.sh sh -c "$env QTWEBENGINE_REMOTE_DEBUGGING=127.0.0.1:9335 QTWEBENGINE_FRAME_STATS=1 setsid -f $cmd --app https://x.com/ >/tmp/x-$v.log 2>&1"
sleep 14
/root/tools/as-cube.sh omarchy-mobile-display on >/dev/null 2>&1
echo "$v at $(($(cat /sys/class/thermal/thermal_zone0/temp) / 1000)) C, big max $(($(cat /sys/devices/system/cpu/cpufreq/policy6/scaling_max_freq) / 1000)) MHz"
python3 /root/tools/scroll-frames-test.py both "$runs"
grep "webengine frames" /tmp/x-$v.log | tail -1
