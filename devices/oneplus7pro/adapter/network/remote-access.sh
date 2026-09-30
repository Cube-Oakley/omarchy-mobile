#!/usr/bin/env bash
# Remote access that does not depend on USB. Runs once per boot from
# session-prepare (installed as /usr/local/sbin/guacamole-remote-access).
#
# - SSH: the initramfs starts sshd with /etc/ssh/sshd_config.usb, which now
#   listens on every interface (key-only, private addresses only), so Wi-Fi
#   reaches the phone when USB does not come up.
# - The initramfs recovery shell on port 23 has no authentication. It is
#   moved to the USB address, where the reboot helpers use it, and off Wi-Fi.
# - After a hard reset USB sometimes never attaches although the gadget is
#   bound (UDC state "not attached"). That state is saved for diagnosis, then
#   one soft reconnect is tried and its result saved too.
set -uo pipefail
exec 9>/run/guacamole-remote-access.lock
flock -n 9 || exit 0
boot=$(cat /proc/sys/kernel/random/boot_id)
done_flag=/run/guacamole-remote-access.$boot
[[ -f $done_flag ]] && exit 0
touch "$done_flag"
logs=/root/bringup-logs/$boot
mkdir -p "$logs"
log() { printf '%s %s\n' "$(cut -d' ' -f1 /proc/uptime)" "$*" >> "$logs/remote-access.log"; }
initramfs=/proc/1/root

# Port 23: only the USB address.
recovery_shell() {
    local pids
    pids=$(ss -Hltnp 'sport = :23' 2>/dev/null | grep -o 'pid=[0-9]*' | cut -d= -f2 | sort -u)
    if ss -Hltn 'sport = :23' | grep -q '172.16.42.1:23'; then
        log 'recovery shell already USB-only'
        return
    fi
    if ! ip -4 addr show usb0 2>/dev/null | grep -q '172.16.42.1/'; then
        log 'usb0 has no address; recovery shell left as it is'
        return
    fi
    for pid in $pids; do kill "$pid" 2>/dev/null; done
    sleep 0.5
    setsid chroot "$initramfs" /bin/busybox nc -lk -s 172.16.42.1 -p 23 -e /bin/sh \
        < /dev/null > /dev/null 2>&1 &
    sleep 0.5
    if ss -Hltn 'sport = :23' | grep -q '172.16.42.1:23'; then
        log 'recovery shell moved to 172.16.42.1:23'
    else
        log 'recovery shell restart failed'
    fi
}

usb_state() {
    echo "== $1 at $(cut -d' ' -f1 /proc/uptime)"
    for udc in /sys/class/udc/*; do
        echo "$udc state=$(cat "$udc/state" 2>/dev/null) speed=$(cat "$udc/current_speed" 2>/dev/null)"
    done
    for f in link_state mode; do
        echo "debugfs $f: $(cat /sys/kernel/debug/usb/a600000.usb/$f 2>/dev/null)"
    done
    cat /sys/kernel/debug/usb/a600000.usb/regdump 2>/dev/null | grep -E 'DCTL|DSTS|DEVTEN|GSTS|GCTL|GUSB2PHYCFG|GDBGLTSSM|GEVNTCOUNT' | head -20
    for ps in /sys/class/power_supply/*; do
        echo "-- $ps"
        cat "$ps/uevent" 2>/dev/null | grep -vE 'SERIAL|MANUFACTURER'
    done
    ls /sys/kernel/debug/regmap 2>/dev/null | grep -iE 'spmi|pmic' | head
    ip -br addr show usb0 2>/dev/null
}

usb_watch() {
    local state
    sleep 5
    while [[ $(cut -d. -f1 /proc/uptime) -lt 90 ]]; do sleep 5; done
    state=$(cat /sys/class/udc/*/state 2>/dev/null | head -n 1)
    [[ $state == configured ]] && { log "usb configured"; return; }
    log "usb $state at 90 s: saving state, then one soft reconnect"
    usb_state "before reconnect" >> "$logs/usb-arch.log" 2>&1
    dmesg > "$logs/dmesg-at-usb-failure.log"
    for udc in /sys/class/udc/*; do
        echo disconnect > "$udc/soft_connect" 2>/dev/null
        sleep 1
        echo connect > "$udc/soft_connect" 2>/dev/null
    done
    sleep 15
    usb_state "15 s after reconnect" >> "$logs/usb-arch.log" 2>&1
    log "usb after reconnect: $(cat /sys/class/udc/*/state 2>/dev/null | head -n 1)"
    sync
}

recovery_shell
usb_watch
