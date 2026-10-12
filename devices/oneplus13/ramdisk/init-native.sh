#!/bin/sh
# Persistent native bootstrap, with an independent USB recovery console.
/bin/busybox --install -s /bin
export PATH=/bin
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mkdir -p /dev/pts /run /tmp
mount -t devpts devpts /dev/pts
mount -t tmpfs tmpfs /run
mount -t tmpfs tmpfs /tmp
hostname oneplus13-linux
(
    for n in $(seq 1 30); do [ -e /sys/class/net/usb0 ] && break; sleep 1; done
    ip link set lo up
    ip addr add 10.77.13.1/30 dev usb0
    ip link set usb0 up
) >/run/network.log 2>&1 &
(
    export PS1='oneplus13-linux# '
    while [ ! -c /dev/ttyGS0 ]; do sleep 1; done
    while :; do setsid -c sh -i </dev/ttyGS0 >/dev/ttyGS0 2>&1; sleep 1; done
) &
# A userspace startup failure must not become an unattended Android-style loop.
save_startup_logs() {
    # The startup helper publishes this path only after verifying and mounting
    # the native root read/write. Never write to an unverified filesystem.
    [ -f /run/oneplus13-boot-log-dir ] || return 0
    log_dir=$(cat /run/oneplus13-boot-log-dir)
    cp /run/persistent-start.log "$log_dir/startup.log"
    dmesg >"$log_dir/kernel.log"
    sync -f "$log_dir"
}
(
    for n in $(seq 1 36); do
        sleep 5
        save_startup_logs
        [ ! -e /run/oneplus13-persistent-ready ] || exit 0
    done
    /bin/reboot-bootloader
) &
if ! sh /bin/native-persistent-start >/run/persistent-start.log 2>&1; then
    echo 'OMARCHY: persistent startup failed; returning to fastboot' >/dev/kmsg
    save_startup_logs
    sleep 15
    /bin/reboot-bootloader
fi
save_startup_logs
while :; do sleep 60; done
