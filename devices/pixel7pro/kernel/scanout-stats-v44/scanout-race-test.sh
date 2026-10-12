#!/bin/bash
# Read pixel_scanout's stats as fast as possible while dpu-cycle-test.sh
# powers the display pipeline off and on through the AOD hibernation N times.
# Before v44 a read inside pixel_power_down() was a synchronous external
# abort (panic); stop the modem and sync before running this on such a kernel.
n=${1:-300}
grep -q ' /sys/kernel/debug ' /proc/mounts || mount -t debugfs none /sys/kernel/debug
stats=$(ls /sys/kernel/debug/dri/*/pixel_scanout | head -1)
sync
python3 -c 'import sys, time
f = open(sys.argv[1])
n, start = 0, time.monotonic()
while True:
    f.seek(0); f.read(); n += 1
    if n % 20000 == 0:
        print("scanout-race: %d reads, %.0f/s" % (n, n / (time.monotonic() - start)), flush=True)' "$stats" &
reader=$!
trap 'kill $reader' EXIT
echo "scanout-race: start $n" > /dev/kmsg
/root/tools/dpu-cycle-test.sh "$n"
echo "scanout-race: done" > /dev/kmsg
