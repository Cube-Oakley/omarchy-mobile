#!/bin/sh
# Wi-Fi firmware power settings, applied whenever a Wi-Fi link comes up: the
# firmware forgets them each time brcmfmac reloads (boot, pixel-wifi-restart,
# deep sleep). Installed as /etc/NetworkManager/dispatcher.d/51-pixel-wifi-power.
#
# pm2_sleep_ret: how long the BCM4389 stays awake after a packet before it
# dozes again in power save (PM_FAST). Google's firmware starts at 2000 ms;
# 200 ms measured about 12 mW less on VSYS_WLAN_BT with a static screen
# (116 against 104 mW, October 2). PM_MAX, 50 ms, single-chain beacons,
# SMPS, one chain and DTIM 3 saved at most 4 mW more and were not kept.
[ "$2" = up ] || exit 0
[ -d "/sys/class/net/$1/wireless" ] || exit 0
/usr/local/sbin/pixel-wl -i "$1" set pm2_sleep_ret 200
