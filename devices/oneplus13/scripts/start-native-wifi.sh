#!/bin/bash
# Optional CPH2655 station startup, after the desktop and system bus are ready.
set -euo pipefail
[[ $(cat /etc/omarchy-mobile-oneplus13-root) == oneplus13-cph2655-v1 ]]
[[ $(tr -d '\0' </sys/firmware/devicetree/base/model) == 'OnePlus 13 CPH2655 Omarchy USB probe' ]]
[[ $(tr -d '\0' </sys/firmware/devicetree/base/peach-pmu/compatible) == oneplus,cph2655-peach-pmu ]]
pcie=/sys/firmware/devicetree/base/soc@0/pcie@1c00000
[[ -d $pcie && ! -e $pcie/msi-map && -e $pcie/iommu-map ]]
base=/opt/oneplus13-wifi
[[ $(cat "$base/KERNELRELEASE") == "$(uname -r)" ]]
(cd "$base" && sha256sum --quiet -c SHA256SUMS)
[[ -x /usr/bin/NetworkManager && -x /usr/bin/iw ]]
busctl --system list >/dev/null
state=/var/lib/omarchy-mobile/oneplus13
mkdir -p "$state"
# An interrupted initialization is skipped on the next boot, preserving the
# desktop and USB recovery. Clear the marker explicitly after investigating.
[[ ! -e $state/wifi-starting ]]
mkdir /run/oneplus13-wifi-started
touch "$state/wifi-starting"
sync -f "$state/wifi-starting"

# Firmware requests run in the outer initramfs, outside this Arch chroot.
outer=/proc/1/root
for name in ath12k regulatory.db regulatory.db.p7s; do
    target=/run/arch/opt/oneplus13-wifi/firmware/$name
    link=$outer/lib/firmware/$name
    if [[ -e $link || -L $link ]]; then
        [[ $(readlink "$link") == "$target" ]]
    else
        ln -s "$target" "$link"
    fi
done
for module in pwrseq-qcom-wcn pci-pwrctrl-pwrseq phy-qcom-qmp-pcie; do
    [[ -d /sys/module/${module//-/_} ]] || insmod "$base/$module.ko"
done
for n in {1..50}; do
    [[ -e /sys/bus/pci/devices/0000:01:00.0 ]] && break
    sleep .1
done
[[ $(cat /sys/bus/pci/devices/0000:01:00.0/vendor) == 0x17cb ]]
[[ $(cat /sys/bus/pci/devices/0000:01:00.0/device) == 0x110e ]]
for module in rfkill libarc4 cfg80211 mac80211 mhi qrtr-mhi; do
    [[ -d /sys/module/${module//-/_} ]] || insmod "$base/$module.ko"
done
# Calibration and firmware restart are bounded in the driver. No automatic
# firmware crash recovery, concurrent AP, monitor mode or suspend is enabled.
insmod "$base/ath12k.ko" peach_calibration_probe=1 peach_mission_probe=1
insmod "$base/ath12k_wifi7.ko"
for n in {1..60}; do
    [[ -d /sys/class/net/wlp1s0 ]] && break
    sleep 1
done
[[ -d /sys/class/net/wlp1s0 ]]
# Stock returns a placeholder MAC until modem DMS is available. Give scanning
# and profiles using 'preserve' a persistent local unicast identity first.
python3 - "$state/wifi-mac" <<'PY'
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
p = Path(sys.argv[1])
if not p.exists():
    octets = bytearray(secrets.token_bytes(6))
    octets[0] = (octets[0] & 0xfc) | 2
    with os.fdopen(os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as f:
        f.write(':'.join(f'{b:02x}' for b in octets) + '\n')
address = p.read_text().strip()
if not re.fullmatch(r'(?:[0-9a-f]{2}:){5}[0-9a-f]{2}', address) or int(address[:2], 16) & 3 != 2:
    raise SystemExit('Invalid local Wi-Fi identity')
subprocess.run(['ip', 'link', 'set', 'wlp1s0', 'address', address], check=True)
PY
mkdir -p /run/systemd/resolve
NetworkManager
for n in {1..20}; do
    nmcli -g GENERAL.TYPE device show wlp1s0 2>/dev/null | grep -qx wifi && break
    sleep .5
done
nmcli -g GENERAL.TYPE device show wlp1s0 | grep -qx wifi
rm "$state/wifi-starting"
sync -f "$state"
echo 'Native station Wi-Fi ready; NetworkManager owns saved connections.'
