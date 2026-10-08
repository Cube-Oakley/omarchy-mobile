#!/bin/sh
# Network time when Wi-Fi comes up, at most every six hours: pixel-time keeps
# the clock and its offset from the read-only RTC. Wi-Fi comes up again after
# each deep sleep, so --due skips all but the first in six hours. In the
# background: the dispatcher waits for its scripts.
# Installed as /etc/NetworkManager/dispatcher.d/54-pixel-time.
[ "$2" = up ] || exit 0
[ -d "/sys/class/net/$1/wireless" ] || exit 0
setsid -f timeout 30 /usr/local/sbin/pixel-time sync --due >>/run/pixel-time.log 2>&1 </dev/null
