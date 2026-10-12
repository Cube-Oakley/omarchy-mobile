#!/bin/bash
# Optional CPH2655 Bluetooth startup. A failed attempt is skipped next boot.
set -euo pipefail
[[ $(cat /etc/omarchy-mobile-oneplus13-root) == oneplus13-cph2655-v1 ]]
[[ $(tr -d '\0' </sys/firmware/devicetree/base/model) == 'OnePlus 13 CPH2655 Omarchy USB probe' ]]
[[ $(tr -d '\0' </sys/firmware/devicetree/base/aliases/serial0) == /soc@0/geniqup@8c0000/serial@898000 ]]
base=/opt/oneplus13-bluetooth
[[ $(cat "$base/KERNELRELEASE") == "$(uname -r)" ]]
(cd "$base" && sha256sum --quiet -c SHA256SUMS)
[[ -x /usr/lib/bluetooth/bluetoothd && ! -d /sys/module/bluetooth ]]
busctl --system list >/dev/null
# Bluetooth shares Wi-Fi's rails. Wait for the verified Wi-Fi initialization
# to finish before taking independent regulator votes or enabling the UART.
for n in {1..90}; do
    [[ -d /sys/class/net/wlp1s0 && ! -e /var/lib/omarchy-mobile/oneplus13/wifi-starting ]] && break
    sleep 1
done
[[ -d /sys/class/net/wlp1s0 && ! -e /var/lib/omarchy-mobile/oneplus13/wifi-starting ]]
state=/var/lib/omarchy-mobile/oneplus13
mkdir -p "$state"
[[ ! -e $state/bluetooth-starting ]]
mkdir /run/oneplus13-bluetooth-started
touch "$state/bluetooth-starting"
sync -f "$state/bluetooth-starting"
link=/proc/1/root/lib/firmware/qca
target=/run/arch/opt/oneplus13-bluetooth/firmware/qca
if [[ -e $link || -L $link ]]; then
    [[ $(readlink "$link") == "$target" ]]
else
    ln -s "$target" "$link"
fi
# Images from native-persistent22 describe Bluetooth in the base tree; older
# ones need the runtime overlay, whose loader checks their exact phandles.
if [[ ! -e /sys/firmware/devicetree/base/soc@0/geniqup@8c0000/serial@898000/bluetooth/compatible ]]; then
    insmod "$base/peach-bluetooth-probe-dt.ko" apply=1
fi
for module in kpp ecc ecdh_generic; do
    [[ -d /sys/module/$module ]] || insmod "$base/$module.ko"
done
# All Bluetooth modules below are built together with LE-enabled headers.
for module in uhid bluetooth btbcm btqca rfcomm bnep hidp; do
    insmod "$base/$module.ko"
done
insmod "$base/hci_uart.ko" peach_version_probe=1 peach_firmware_probe=1 peach_radio_probe=1 peach_highspeed_probe=1
python3 /usr/local/lib/omarchy-mobile/oneplus13/configure-bluetooth.py
nohup /usr/lib/bluetooth/bluetoothd --nodetach >/run/oneplus13-bluetoothd.log 2>&1 </dev/null &
echo $! >/run/oneplus13-bluetoothd.pid
for n in {1..30}; do
    if busctl get-property org.bluez /org/bluez/hci0 org.bluez.Adapter1 Powered 2>/dev/null | grep -qx 'b true'; then
        busctl set-property org.bluez /org/bluez/hci0 org.bluez.Adapter1 Alias s 'Omarchy OnePlus 13'
        rm "$state/bluetooth-starting"
        sync -f "$state"
        echo 'Native Bluetooth ready; factory address retrieval and accessory validation remain pending.'
        exit 0
    fi
    sleep 1
done
echo 'Bluetooth startup incomplete; retaining failure marker for recovery.' >&2
exit 1
