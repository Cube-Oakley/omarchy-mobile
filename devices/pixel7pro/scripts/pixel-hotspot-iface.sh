#!/bin/sh
# The hotspot's interface on the BCM4389: a second, virtual interface, ap0.
# brcmfmac cannot turn the main interface into an access point (the firmware
# refuses its beacon interval while that interface is up: "Beacon Interval
# Set Error, -52"), but a virtual one starts as an access point, and the
# chip keeps the Wi-Fi connection on the main interface beside it (both on
# the same channel). Installed as /usr/local/sbin/pixel-hotspot-iface; the
# desktop's account runs it through sudo (session/sudoers).
#   pixel-hotspot-iface create   print the interface to use, made if needed
#   pixel-hotspot-iface remove   drop it once the hotspot is off
set -eu
ap=ap0
main=
for path in /sys/class/net/*/wireless; do
    name=$(basename "$(dirname "$path")")
    [ "$name" = "$ap" ] || { main=$name; break; }
done
case "${1:-}" in
    create)
        [ -n "$main" ] || { echo 'no Wi-Fi interface' >&2; exit 1; }
        if [ ! -e "/sys/class/net/$ap" ]; then
            iw dev "$main" interface add "$ap" type __ap
        fi
        # NetworkManager manages it for the hotspot only: never a network
        # of its own (wait for it to appear first).
        n=0
        while [ $n -lt 50 ] && ! nmcli -t -f DEVICE device | grep -qx "$ap"; do
            sleep 0.1
            n=$((n + 1))
        done
        nmcli device set "$ap" managed yes autoconnect no >/dev/null 2>&1 || true
        # Ready once NetworkManager has it disconnected (a new interface is
        # "unavailable" for a moment, and activation fails then).
        n=0
        while [ $n -lt 50 ] && [ "$(nmcli -t -g GENERAL.STATE device show "$ap" 2>/dev/null | cut -d' ' -f1)" -lt 30 ]; do
            sleep 0.1
            n=$((n + 1))
        done
        echo "$ap"
        ;;
    remove)
        [ -e "/sys/class/net/$ap" ] && iw dev "$ap" del
        exit 0
        ;;
    *)
        echo 'usage: pixel-hotspot-iface create|remove' >&2
        exit 2
        ;;
esac
