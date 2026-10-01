#!/bin/bash
# Restart the Pixel's Wi-Fi stack: brcmfmac and the PCIe link (kernel/pcie),
# loaded as the boot script's start_wifi does. Used when the BCM4389
# firmware stops answering (its control ring fills after an s2idle resume, and
# every later suspend is then refused by 0000:01:00.0). NetworkManager
# reconnects on its own. With --start it only loads Wi-Fi that is not
# loaded: pixel-suspend and the display hook both do that after a deep sleep,
# so runs take turns. Installed as /usr/local/sbin/pixel-wifi-restart.
set -euo pipefail
exec 9>/run/pixel-wifi-restart.lock
flock 9
if [[ ${1-} == --start && -d /sys/module/brcmfmac ]]; then
    exit 0
fi
modules=/proc/1/root/lib/modules/pixel
wifi_driver=$modules/brcmfmac.ko
if [[ -f /var/lib/omarchy-mobile/modem/enabled ]]; then
    python3 /usr/local/lib/omarchy-mobile/modem/manager.py verify
    wifi_driver=/var/lib/omarchy-mobile/modem/brcmfmac.ko
fi
for module in brcmfmac_wcc brcmfmac pixel_pcie; do
    [[ -d /sys/module/$module ]] && rmmod "$module"
done
echo /lib/modules/pixel/request-module >/proc/sys/kernel/modprobe
insmod "$modules/pixel-pcie.ko" l1ss=1
for module in rfkill cfg80211 brcmutil; do
    [[ -d /sys/module/$module ]] || insmod "$modules/$module.ko"
done
insmod "$wifi_driver"
insmod "$modules/brcmfmac-wcc.ko" 2>/dev/null || [[ -d /sys/module/brcmfmac_wcc ]]
echo 'Wi-Fi restarted' >&2
