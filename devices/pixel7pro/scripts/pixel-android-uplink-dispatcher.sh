#!/bin/sh
# Android's connections follow the phone's uplink. The Waydroid container sits
# behind NAT and never sees Wi-Fi come or go, so its sockets (microG's push
# connection, the apps' own) keep the old route and go dead until the next
# heartbeat. When Wi-Fi comes up or goes down, they are closed once the
# default route has followed, as Android closes a lost network's sockets, and
# the apps open them again on the new route. pixel-suspend moves them onto
# mobile data itself before a deep sleep (and waits for microG); while it
# runs, this leaves them alone.
#
# Every Wi-Fi up or down moves them, without comparing with the record in
# /run/pixel-push-uplink: on October 7 the record said mobile data while an
# Android restart had made new connections over Wi-Fi, and nothing moved.
# Installed as /etc/NetworkManager/dispatcher.d/53-pixel-android-uplink.
case $2 in up|down) ;; *) exit 0 ;; esac
case $1 in wl*) ;; *) exit 0 ;; esac
[ -e /run/pixel-suspend.active ] && exit 0
pid=$(lxc-info -P /var/lib/waydroid/lxc -n waydroid -pH 2>/dev/null)
case $pid in ''|*[!0-9]*) exit 0 ;; esac
route() { ip route get 8.8.8.8 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p'; }
# The default route moves with the event: onto Wi-Fi when it comes up, off
# it when it goes. At most 10 s.
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    dev=$(route)
    if [ "$2" = up ]; then [ "$dev" = "$1" ] && break; else [ "$dev" != "$1" ] && break; fi
    sleep 0.5
done
[ -n "$dev" ] || exit 0
nsenter -t "$pid" -n ss -K -tnH state established \
    '( ! dst 127.0.0.0/8 and ! dst [::1] and ! dst [::ffff:127.0.0.1] )' >/dev/null
echo "$dev" >/run/pixel-push-uplink
