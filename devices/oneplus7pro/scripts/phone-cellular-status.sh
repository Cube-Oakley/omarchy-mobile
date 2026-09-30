#!/usr/bin/env bash
# Report the cellular stack in one call: boot, the opt-in boot steps, the
# ModemManager modem, NetworkManager's mobile connection with a live HTTPS
# check over it, and IMS registration (IMSA). Addresses are masked.
#   scripts/phone-cellular-status.sh [--wait SECONDS]   (wait for IMS first)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WAIT=0
[[ ${1:-} == --wait ]] && WAIT=${2:-120}
timeout $((WAIT + 90)) bash "$ROOT/scripts/phone-ssh.sh" "WAIT=$WAIT bash -s" <<'EOF' |
cd /root/radio-bringup/cellular
ims() {
    python3 - <<'PY'
import sys
sys.argv = ["x"]
exec(open("phone-qmi.py").read().split("COMMANDS = {")[0])
c = Client(0x21)
try:
    c.request(0x0033, tlv(0x10, struct.pack("<I", 0)))
    r = c.request(0x0020, check=False)
    s = c.request(0x0021, check=False)
    reg = {0: "not-registered", 1: "registering", 2: "registered"}
    svc = {0: "none", 1: "limited", 2: "full"}
    u = lambda d, t: struct.unpack("<I", d[t])[0] if t in d else None
    print(f"IMS {reg.get(u(r, 0x12), u(r, 0x12))} SMS {svc.get(u(s, 0x10), '-')} "
          f"voice {svc.get(u(s, 0x11), '-')}")
finally:
    c.close()
PY
}
end=$((SECONDS + WAIT))
until [[ $SECONDS -ge $end ]] || ims 2>/dev/null | grep -q "IMS registered"; do sleep 3; done
echo "boot $(cat /proc/sys/kernel/random/boot_id) up $(cut -d. -f1 /proc/uptime) s"
echo "mss vote $(cat /sys/module/mss_vote/parameters/level 2>/dev/null || echo held) ipv6 $(test -e /proc/net/if_inet6 && echo on || echo off)"
for p in ModemManager "phone-qmi.py ims-dcm" diag-router NetworkManager; do
    printf '%s %s\n' "$p" "$(pgrep -f "$p" >/dev/null && echo running || echo missing)"
done
mmcli -m 0 2>/dev/null | grep -E " state:|access tech|operator name|signal quality" || echo "no ModemManager modem"
nmcli -t -f DEVICE,TYPE,STATE,CONNECTION d | grep -E "gsm|wifi:" | grep -v p2p
dev=$(nmcli -g GENERAL.IP-IFACE d show qrtr0 2>/dev/null || true)
if [[ -n $dev ]]; then
    curl -6 -s -o /dev/null -w "mobile IPv6 HTTPS %{http_code} in %{time_total}s\n" -m 20 --interface "$dev" https://ipv6.google.com/ || true
    curl -4 -s -o /dev/null -w "mobile IPv4 HTTPS %{http_code} in %{time_total}s\n" -m 20 --interface "$dev" https://archlinux.org/ || true
fi
ims 2>&1 | tail -1
EOF
sed -E 's/[0-9a-f]{1,4}(:[0-9a-f]{0,4}){3,7}/<v6>/g'
