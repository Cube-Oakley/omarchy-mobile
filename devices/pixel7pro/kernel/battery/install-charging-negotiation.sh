#!/bin/sh
# Put the charger negotiation on the phone, run by the user from the host:
#
#   sh devices/pixel7pro/kernel/battery/install-charging-negotiation.sh [WIFI-ADDRESS]
#
# With the phone's Wi-Fi address it goes over Wi-Fi (the same host key as the
# USB link), so it also works with the phone on a charger.
#
# The USB input limit then follows what the port offers (the Type-C current
# on CC, or the BC1.2 port type), from 500 mA up in 500 mA steps every 10 s,
# backing off if the source sags (AICL or VBUS under 4.6 V), and above 1.5 A
# only while the port's thermistors read cool (pixel-port-temp). The charge
# current may reach 3 A with the battery at 15-40 C. It reloads pixel-battery,
# pixel-usb-switch and pixel-port-temp, and installs the boot script that
# loads the last: the USB connection stays up, the charge limit is put back,
# and the first previous modules are kept in /root/*.before-negotiate. After
# a successful reload all three become the copies the boot script loads.
set -eu
cd "$(dirname "$0")/../.."
keys=out/checkpoints/20260925-persistence-power/arch-session-f
ssh=$keys/ssh
[ -x "$ssh" ] || { echo "missing $ssh" >&2; exit 1; }
if [ $# -ge 1 ]; then
    wifi=$1
    ssh() { /usr/bin/ssh -F /dev/null -i "$keys/id_ed25519" -o IdentitiesOnly=yes \
        -o UserKnownHostsFile="$keys/known_hosts" -o HostKeyAlias=10.77.7.1 \
        -o StrictHostKeyChecking=yes -o BatchMode=yes -o ConnectTimeout=15 "root@$wifi" "$@"; }
    ssh=ssh
fi
for f in kernel/battery/pixel-battery.ko kernel/usb/pixel-usb-switch.ko kernel/port-temp/pixel-port-temp.ko; do
    [ -f "$f" ] || { echo "missing $f (build it first)" >&2; exit 1; }
done

"$ssh" 'cat > /root/pixel-battery.ko.negotiate' < kernel/battery/pixel-battery.ko
"$ssh" 'cat > /root/pixel-usb-switch.ko.negotiate' < kernel/usb/pixel-usb-switch.ko
"$ssh" 'cat > /root/pixel-port-temp.ko.negotiate' < kernel/port-temp/pixel-port-temp.ko
"$ssh" 'cat > /root/pixel-boot.sh.negotiate' < scripts/pixel-persistent-session.sh
"$ssh" 'set -e
D=/usr/local/lib/omarchy-mobile
[ -e /root/pixel-battery.ko.before-negotiate ] || cp -a $D/pixel-battery.ko /root/pixel-battery.ko.before-negotiate
[ -e /root/pixel-usb-switch.ko.before-negotiate ] || cp -a $D/pixel-usb-switch.ko /root/pixel-usb-switch.ko.before-negotiate
[ -e /root/pixel-boot.sh.before-negotiate ] || cp -a $D/pixel-boot.sh /root/pixel-boot.sh.before-negotiate
rmmod pixel_usb_switch
rmmod pixel_battery
[ -d /sys/module/pixel_port_temp ] && rmmod pixel_port_temp
insmod /root/pixel-port-temp.ko.negotiate
insmod /root/pixel-battery.ko.negotiate
XDG_STATE_HOME=/root/.local/state /root/.local/bin/omarchy-mobile-battery restore >/dev/null ||
    echo "charge limit not restored"
insmod /root/pixel-usb-switch.ko.negotiate
cp /root/pixel-battery.ko.negotiate $D/pixel-battery.ko.new && mv $D/pixel-battery.ko.new $D/pixel-battery.ko
cp /root/pixel-usb-switch.ko.negotiate $D/pixel-usb-switch.ko.new && mv $D/pixel-usb-switch.ko.new $D/pixel-usb-switch.ko
cp /root/pixel-port-temp.ko.negotiate $D/pixel-port-temp.ko.new && mv $D/pixel-port-temp.ko.new $D/pixel-port-temp.ko
cp /root/pixel-boot.sh.negotiate $D/pixel-boot.sh.new && chmod 755 $D/pixel-boot.sh.new && mv $D/pixel-boot.sh.new $D/pixel-boot.sh
sync
echo "installed; watching for 70 s"
for i in 1 2 3 4 5 6 7; do
    sleep 10
    echo "$(cat /sys/bus/i2c/devices/*-0025/typec_status | head -1) -> limit $(($(cat /sys/class/power_supply/usb/current_max) / 1000)) mA, battery $(($(cat /sys/class/power_supply/battery/current_now) / 1000)) mA at $(cat /sys/class/power_supply/battery/temp)/10 C"
done
grep -E "usb|neutral" /sys/module/pixel_port_temp/parameters/temps | tr "\n" " "; echo
dmesg | grep -E "pixel-battery.*(USB input|USB port|sags|step charging)|NTC channels" | tail -12'
echo "To undo: put /root/pixel-battery.ko.before-negotiate, /root/pixel-usb-switch.ko.before-negotiate"
echo "and /root/pixel-boot.sh.before-negotiate back in /usr/local/lib/omarchy-mobile/ and reboot."
