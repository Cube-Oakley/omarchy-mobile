#!/bin/sh
# Fingerprint probe 3, run by the user from the host: copy the RAM-only lab
# runtime to the phone, load Goodix's trusted app (g7.app) into Trusty with
# the stock trusty_apploader, and show the end of Trusty's log.
#
#   sh devices/pixel7pro/fingerprint/run-fp-lab.sh [WIFI-ADDRESS]
#
# With the phone's Wi-Fi address it goes over Wi-Fi (the same host key as the
# USB link). While the loader runs, fp-sample.py shows where it waits: its
# kernel stack, wait channel and system call, every 2 s.
#
# The app loads into Trusty's memory only, and a reboot unloads it. With
# storageproxyd (fp-lab.sh --storage, approved by the user on October 1)
# Trusty also reads and writes its secure storage: RPMB, its files from stock
# on the persist partition, and /var/lib/omarchy-mobile/trusty-ss. The runtime tarball (out/fp-work/fp-runtime.tar) holds proprietary
# vendor files and is never committed.
set -eu
cd "$(dirname "$0")/.."
keys=out/checkpoints/20260925-persistence-power/arch-session-f
ssh=$keys/ssh
tarball=out/fp-work/fp-runtime.tar
[ -x "$ssh" ] || { echo "missing $ssh" >&2; exit 1; }
if [ $# -ge 1 ]; then
    wifi=$1
    ssh() { /usr/bin/ssh -F /dev/null -i "$keys/id_ed25519" -o IdentitiesOnly=yes \
        -o UserKnownHostsFile="$keys/known_hosts" -o HostKeyAlias=10.77.7.1 \
        -o StrictHostKeyChecking=yes -o BatchMode=yes -o ConnectTimeout=15 "root@$wifi" "$@"; }
    ssh=ssh
fi
[ -f "$tarball" ] || { echo "missing $tarball" >&2; exit 1; }

echo "Copying the runtime ($(du -h "$tarball" | cut -f1)) to the phone..."
"$ssh" 'mkdir -p /root/fp && cat > /root/fp/fp-runtime.tar' < "$tarball"
"$ssh" 'cat > /root/fp/fp-lab.sh && chmod +x /root/fp/fp-lab.sh' < fingerprint/fp-lab.sh
"$ssh" 'cat > /root/fp/fp-sample.py' < fingerprint/fp-sample.py
# SCSI generic, for storageproxyd's RPMB node (fp-lab.sh --storage).
"$ssh" 'cat > /root/fp/sg.ko' < kernel/scsi-sg/sg.ko

echo "Loading g7.app into Trusty..."
"$ssh" 'dmesg -C 2>/dev/null; python3 /root/fp/fp-sample.py & /root/fp/fp-lab.sh /root/fp/fp-runtime.tar --storage; echo "lab exit: $?"; wait' || true
sleep 2
echo "--- Trusty log (last 30 lines) ---"
"$ssh" 'cat /dev/trusty-log0 | tail -30'
echo "--- kernel log ---"
"$ssh" 'dmesg | grep -i -E "trusty|tipc|sg |scsi|rpmb|EXT4-fs \(sda1\)" | tail -20'
echo "Done. Paste this output to Claude."
