#!/bin/sh
# NetworkManager also invokes this after DHCP renewal and reconnection.
case "$2" in
    up|down|dhcp4-change) /usr/local/sbin/pixel-wifi-ssh refresh ;;
esac
