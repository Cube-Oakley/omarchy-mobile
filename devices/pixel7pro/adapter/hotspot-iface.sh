#!/usr/bin/env bash
# The hotspot's interface (overlay/mobile/hotspot.py): pixel-hotspot-iface
# makes the BCM4389's virtual access point, ap0, as root.
(( EUID == 0 )) && exec /usr/local/sbin/pixel-hotspot-iface "$@"
exec sudo -n /usr/local/sbin/pixel-hotspot-iface "$@"
