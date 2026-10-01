#!/bin/bash
# Load the Pixel's Bluetooth again after pixel-suspend unloaded it for deep
# sleep: pixel-bt powers the chip and adds its serdev node, hci_uart loads the
# patch firmware (kernel/bluetooth), and bluetoothd powers hci0 as before.
# Does nothing unless pixel-suspend left its marker. Run by the display hook
# when the screen lights. Installed as /usr/local/sbin/pixel-bt-restart.
set -euo pipefail
marker=/run/pixel-bt-sleeping
modules=/proc/1/root/lib/modules/pixel
exec 9>/run/pixel-bt-restart.lock
flock 9
[[ -e $marker ]] || exit 0
[[ -d /sys/module/pixel_bt ]] || insmod "$modules/pixel-bt.ko"
[[ -d /sys/module/hci_uart ]] || insmod "$modules/hci_uart.ko"
rm -f "$marker"
echo 'Bluetooth restarted' >&2
