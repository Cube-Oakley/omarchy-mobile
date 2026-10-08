#!/bin/bash
# Wi-Fi rail power per firmware setting: VSYS_WLAN_BT from the ODPM meter over
# WINDOW seconds per setting, ROUNDS rounds. A setting is NAME=iovar:value,...
# (iovar "PM" is the PM ioctl). Every iovar named anywhere is restored to its
# value at start, and pixel-port-temp is reloaded, at exit.
# Usage: pixel-wifi-ab.sh WINDOW ROUNDS SETTING...
# e.g. pixel-wifi-ab.sh 60 2 base=pm2_sleep_ret:2000 short=pm2_sleep_ret:200
set -u
WINDOW=$1 ROUNDS=$2; shift 2
W=/usr/local/sbin/pixel-wl
P=/sys/module/pixel_odpm/parameters/interval_power
H=/run/user/0/omarchy-mobile-sleep-inhibit
get() { if [[ $1 == PM ]]; then $W get-int 85; else $W get "$1"; fi; }
put() { if [[ $1 == PM ]]; then $W set-int 86 "$2"; else $W set "$1" "$2"; fi; }
declare -A orig
for s in "$@"; do
    IFS=, read -ra kv <<< "${s#*=}"
    for x in "${kv[@]}"; do [[ -n $x ]] && orig[${x%%:*}]=$(get "${x%%:*}"); done
done
restore() {
    for k in "${!orig[@]}"; do put "$k" "${orig[$k]}"; done
    rm -f $H/wifi-ab
    rmmod pixel_odpm 2>/dev/null
    [[ -d /sys/module/pixel_port_temp ]] || insmod /usr/local/lib/omarchy-mobile/pixel-port-temp.ko
    for k in "${!orig[@]}"; do printf "%s=%s " "$k" "$(get "$k")"; done; echo "(restored)"
}
trap restore EXIT
mkdir -p $H && touch $H/wifi-ab	# the shell's sleep policy waits
rmmod pixel_port_temp 2>/dev/null
[[ -d /sys/module/pixel_odpm ]] || insmod /proc/1/root/lib/modules/pixel/pixel-odpm.ko || exit 1
for r in $(seq "$ROUNDS"); do
    for s in "$@"; do
        for k in "${!orig[@]}"; do put "$k" "${orig[$k]}"; done
        IFS=, read -ra kv <<< "${s#*=}"
        for x in "${kv[@]}"; do [[ -n $x ]] && { put "${x%%:*}" "${x#*:}" || echo "  $x refused"; }; done
        sleep 5
        cat $P > /dev/null
        sleep "$WINDOW"
        out=$(cat $P)
        now=; for x in "${kv[@]}"; do [[ -n $x ]] && now+="${x%%:*}=$(get "${x%%:*}") "; done
        printf "round %s %-8s wlan %6s mW  total %s  %s\n" "$r" "${s%%=*}" \
            "$(echo "$out" | awk '/VSYS_WLAN_BT/ {print $2}')" "$(echo "$out" | awk '/^total/ {print $2}')" "$now"
    done
done
