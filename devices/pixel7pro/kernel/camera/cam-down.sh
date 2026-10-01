#!/bin/bash
# Undo cam-up.sh: stop the link and the sensor, power the UW down.
cd /root/tools || exit 1
rmmod pixel_csis_capture 2>/dev/null
python3 csis-probe.py stop 2 2 | tail -1
python3 cam-stream.py uw --stop
echo 0 > /sys/devices/platform/pixel-camera-power/uw/power
