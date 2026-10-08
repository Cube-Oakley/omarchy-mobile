#!/bin/bash
# Load the Pixel's Bluetooth again after pixel-suspend unloaded it for deep
# sleep: pixel-bt powers the chip and adds its serdev node, hci_uart loads the
# patch firmware (kernel/bluetooth), and bluetoothd powers hci0 as before.
# Does nothing unless pixel-suspend left its marker. Run by the display hook
# when the screen lights. Installed as /usr/local/sbin/pixel-bt-restart.
set -euo pipefail
marker=/run/pixel-bt-sleeping
modules=/proc/1/root/lib/modules/pixel
newer=/usr/local/lib/omarchy-mobile/bluetooth
exec 9>/run/pixel-bt-restart.lock
flock 9
[[ -e $marker ]] || exit 0
# The boot's choice: a build installed for this kernel, else the image's.
load() {
    { [[ -f $newer/$1.ko ]] && insmod "$newer/$1.ko"; } || insmod "$modules/$1.ko"
}
[[ -d /sys/module/pixel_bt ]] || load pixel-bt
[[ -d /sys/module/hci_uart ]] || load hci_uart
rm -f "$marker"
echo 'Bluetooth restarted' >&2
