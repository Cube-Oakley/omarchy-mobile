#!/bin/sh
# The hotspot's way out: masquerade the hotspot's clients (NetworkManager's
# shared network, 10.42.0.0/24 by default) onto mobile data. NetworkManager
# would do it itself, but this kernel has no nf_tables and Arch's iptables
# is the nf_tables one, so its firewall is off (conf.d/30-pixel-hotspot.conf)
# and this adds the rule with legacy iptables while the hotspot is up.
# Installed as /etc/NetworkManager/dispatcher.d/52-pixel-hotspot; the
# hotspot is the connection omarchy-mobile-hotspot makes.
[ "$CONNECTION_ID" = omarchy-hotspot ] || exit 0
subnet=$(printf '%s' "${IP4_ADDRESS_0%% *}" | sed -n 's#^\([0-9.]*\)\.[0-9]*/24$#\1.0/24#p')
[ -n "$subnet" ] || subnet=10.42.0.0/24
rule="POSTROUTING -s $subnet ! -d $subnet -j MASQUERADE"
case "$2" in
    up)
        echo 1 >/proc/sys/net/ipv4/ip_forward
        iptables-legacy -t nat -C $rule 2>/dev/null || iptables-legacy -t nat -A $rule
        ;;
    down|pre-down)
        while iptables-legacy -t nat -C $rule 2>/dev/null; do
            iptables-legacy -t nat -D $rule
        done
        # Android in Waydroid routes through its own bridge: leave forwarding
        # on while that is up.
        [ -d /sys/class/net/waydroid0 ] || echo 0 >/proc/sys/net/ipv4/ip_forward
        ;;
esac
exit 0
