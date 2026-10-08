#!/bin/sh
# Bluetooth through deep sleep (README.md, "Sleep"), put on the phone by the
# user from the host:
#
#   sh devices/pixel7pro/kernel/bluetooth/install-lpm.sh [WIFI-ADDRESS]
#
# With the phone's Wi-Fi address it goes over Wi-Fi (the same host key as the
# USB link).
#
# - pixel-bt.ko and hci_uart.ko go to /usr/local/lib/omarchy-mobile/bluetooth,
#   which the boot script and pixel-bt-restart use before the image's;
# - the modem bundle's pixel-gpio.ko (the GPIO provider while the modem is
#   on) is replaced by the build that also carries Bluetooth's wake lines, and
#   the bundle's manifest gets its checksum;
# - the boot script (which now starts Bluetooth after the GPIO provider),
#   pixel-bt-restart and pixel-suspend are installed.
#
# Nothing is reloaded: pixel-gpio cannot be replaced while the amplifiers and
# the modem hold its lines, so it all takes effect at the next boot. The
# previous files are kept in /root/*.before-lpm. Build the modules first
# (README.md, "Building").
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
for f in kernel/bluetooth/pixel-bt.ko kernel/bluetooth/hci_uart.ko kernel/gpio/pixel-gpio.ko; do
    [ -f "$f" ] || { echo "missing $f (build it first)" >&2; exit 1; }
done
release=$(modinfo -F vermagic kernel/bluetooth/pixel-bt.ko | cut -d' ' -f1)

"$ssh" 'cat > /root/pixel-bt.ko.lpm' < kernel/bluetooth/pixel-bt.ko
"$ssh" 'cat > /root/hci_uart.ko.lpm' < kernel/bluetooth/hci_uart.ko
"$ssh" 'cat > /root/pixel-gpio.ko.lpm' < kernel/gpio/pixel-gpio.ko
"$ssh" 'cat > /root/pixel-boot.sh.lpm' < scripts/pixel-persistent-session.sh
"$ssh" 'cat > /root/pixel-bt-restart.lpm' < scripts/pixel-bt-restart.sh
"$ssh" 'cat > /root/pixel-suspend.lpm' < scripts/pixel-suspend.py
"$ssh" "set -e
[ \"\$(uname -r)\" = '$release' ] || { echo 'modules are for $release, the phone runs '\$(uname -r) >&2; exit 1; }
bash -n /root/pixel-boot.sh.lpm && bash -n /root/pixel-bt-restart.lpm && python3 -m py_compile /root/pixel-suspend.lpm
D=/usr/local/lib/omarchy-mobile
B=/var/lib/omarchy-mobile/modem
keep() { [ ! -e \"\$1\" ] || [ -e \"\$2\" ] || cp -a \"\$1\" \"\$2\"; }
keep \$D/pixel-boot.sh /root/pixel-boot.sh.before-lpm
keep /usr/local/sbin/pixel-bt-restart /root/pixel-bt-restart.before-lpm
keep /usr/local/sbin/pixel-suspend /root/pixel-suspend.before-lpm
mkdir -p \$D/bluetooth
for m in pixel-bt hci_uart; do
    install -o root -g root -m 0644 /root/\$m.ko.lpm \$D/bluetooth/\$m.ko.new
    mv \$D/bluetooth/\$m.ko.new \$D/bluetooth/\$m.ko
done
if [ -f \$B/manifest.json ]; then
    keep \$B/pixel-gpio.ko /root/pixel-gpio.ko.before-lpm
    keep \$B/manifest.json /root/modem-manifest.json.before-lpm
    cp /root/pixel-gpio.ko.lpm \$B/pixel-gpio.ko.new
    chown root:root \$B/pixel-gpio.ko.new
    chmod --reference=\$B/pixel-gpio.ko \$B/pixel-gpio.ko.new
    mv \$B/pixel-gpio.ko.new \$B/pixel-gpio.ko
    python3 - \$B <<'PY'
import hashlib, json, os, sys
from pathlib import Path
bundle = Path(sys.argv[1])
manifest = json.loads((bundle / 'manifest.json').read_text())
manifest['sha256']['pixel-gpio.ko'] = hashlib.sha256((bundle / 'pixel-gpio.ko').read_bytes()).hexdigest()
new = bundle / 'manifest.json.new'
new.write_text(json.dumps(manifest, indent=2) + '\n')
os.chmod(new, (bundle / 'manifest.json').stat().st_mode & 0o777)
os.replace(new, bundle / 'manifest.json')
PY
    echo 'modem bundle: pixel-gpio.ko replaced, manifest updated'
else
    echo 'no modem bundle: the image pixel-gpio stays (no wake lines; Bluetooth stays awake)'
fi
install -m 0755 /root/pixel-bt-restart.lpm /usr/local/sbin/pixel-bt-restart.new
mv /usr/local/sbin/pixel-bt-restart.new /usr/local/sbin/pixel-bt-restart
install -m 0755 /root/pixel-suspend.lpm /usr/local/sbin/pixel-suspend.new
mv /usr/local/sbin/pixel-suspend.new /usr/local/sbin/pixel-suspend
install -m 0700 /root/pixel-boot.sh.lpm \$D/pixel-boot.sh.new
mv \$D/pixel-boot.sh.new \$D/pixel-boot.sh
sync
echo 'installed; reboot to load them'"
echo "To undo: remove /usr/local/lib/omarchy-mobile/bluetooth, put /root/*.before-lpm back"
echo "(pixel-gpio.ko and manifest.json in /var/lib/omarchy-mobile/modem) and reboot."
