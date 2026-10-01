#!/bin/bash
# Bring the UW up to a streaming CSIS link 2 (bring-up helper, phone side).
cd /root/tools || exit 1
T=/root/camera-tables
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
lsmod | grep -q '^i2c_dev ' || insmod i2c-dev.ko
lsmod | grep -q '^pixel_hsi2c_cam ' || insmod pixel-hsi2c-cam.ko buses=3
lsmod | grep -q '^pixel_camera_power ' || insmod pixel-camera-power.ko || exit 1
echo 1 > /sys/devices/platform/pixel-camera-power/uw/power || exit 1
lsmod | grep -q '^pixel_csis_iso ' || insmod pixel-csis-iso.ko || exit 1
python3 cam-stream.py uw $T/sandworm_imx386_init_PD_212.txt $T/sandworm_imx386_mode_0x919860_2016x1508_73.txt \
    --set 0x0340=0x0e 0x0341=0x0e 0x0202=0x0d 0x0203=0xfa 0x0204=0x03 0x0205=0x80 --watch 0.3 || exit 1
python3 csis-probe.py setup 2 2 --rate 1481 | tail -2
python3 csis-probe.py status 2 --count 2
