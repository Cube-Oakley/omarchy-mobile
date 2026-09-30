#!/usr/bin/env bash
# OnePlus native4/native5 chroot boot adapter. Factory storage stays read-only.
set -euo pipefail
BASE=/root/radio-bringup
[[ -f "$BASE/autostart-enabled" ]] || exit 0
[[ $(uname -r) == 6.17.0-sm8150-codex-native[45]-* ]] || exit 0
if [[ ${1:-} != --locked ]]; then
    # --close prevents long-lived radio daemons from inheriting the lock.
    exec flock --nonblock --close /run/guacamole-radio.lock "$0" --locked
fi
# Fail once per boot, rather than repeatedly booting a faulting modem.
[[ ! -e /run/guacamole-radio.attempted ]] || exit 0
touch /run/guacamole-radio.attempted
echo "RADIO_BOOT $(cat /proc/sys/kernel/random/boot_id) $(uname -r)"
trap 'echo "Radio startup failed at line $LINENO; inspect this log before retrying." >&2' ERR
for attempt in $(seq 1 240); do
    [[ -e /sys/class/power_supply/pm8150b-charger/online ]] && break
    sleep 1
done
[[ -e /sys/class/power_supply/pm8150b-charger/online ]]
bash "$BASE/phone-radio-test.sh" prepare
# Opt-in modem DIAG logging (docs/cellular-sim-20260926.md). The modem makes
# its DIAG handshake only with a router already present when it boots, so
# diag-router must start before rmtfs brings the modem up.
if [[ -e $BASE/cellular/diag-at-boot && -x $BASE/src/diag/diag-router ]] &&
   ! pgrep -f "$BASE/src/diag/diag-router" >/dev/null; then
    nohup "$BASE/src/diag/diag-router" > "$BASE/logs/diag-router.log" 2>&1 < /dev/null &
    sleep 1
    # One capture for the whole boot: diag-router never drops a disconnected
    # client, whose backlog then stalls the modem's DIAG data.
    mv -f "$BASE/cellular/diag-boot.bin" "$BASE/cellular/diag-boot.prev.bin" 2>/dev/null || true
    nohup python3 "$BASE/cellular/phone-diag-capture.py" "$BASE/cellular/diag-boot.bin" 0 \
        93=0x1e 32=0x1e 3007=0x1f 3010=0x1f 3011=0x1f \
        > "$BASE/logs/diag-capture.log" 2>&1 < /dev/null &
fi
# Opt-in cellular data path (docs/cellular-sim-20260926.md): IPA must be up
# before the modem boots for the modem's data port handshake to complete, and
# without it the modem drops its default bearer about 12 s after attaching.
if [[ -e $BASE/cellular/ipa-at-boot ]]; then
    # ipv6.ko is built from the running tree (CONFIG_IPV6=m); T-Mobile data is
    # IPv6 first.
    for module in ipa rmnet ipv6; do
        [[ -d /sys/module/$module ]] || insmod "$BASE/ipa/$module.ko"
    done
fi
bash "$BASE/phone-radio-test.sh" modem
sleep 1
pgrep -x rmtfs >/dev/null
pgrep -x tqftpserv >/dev/null
modem=
for candidate in /sys/class/remoteproc/*; do
    if grep -q 'sm8150-mpss-pas' "$candidate/device/modalias"; then
        [[ -z $modem ]] || { echo 'Multiple MPSS devices' >&2; exit 1; }
        modem=$candidate
    fi
done
[[ -n $modem ]]
case $(cat "$modem/state") in
    offline) echo start > "$modem/state" ;;
    running) ;;
    *) echo 'MPSS is not in a safe state to start' >&2; exit 1 ;;
esac
for attempt in $(seq 1 20); do
    [[ $(cat "$modem/state") == running ]] && break
    sleep 1
done
[[ $(cat "$modem/state") == running ]]
# Opt-in: drop the AP's pre-sync_state top-corner vote on the modem rail once
# the modem has taken over its own votes. Held, QLink cannot restart after the
# modem sleeps and the modem firmware asserts (docs/cellular-sim-20260926.md).
if [[ -e $BASE/cellular/mss-release-at-boot ]]; then
    [[ -d /sys/module/mss_vote ]] || insmod "$BASE/stats/mss_vote.ko"
    echo 0 > /sys/module/mss_vote/parameters/level
fi
# Opt-in: an incoming call or text wakes the phone from s2idle. qrtr-smd
# (kernel/radio/qrtr-smd-wake.patch) raises a wakeup for indications from the
# listed server ports on the modem's node; the ports change between modem
# boots, so they come from the name service: voice (9) and messaging (5).
if [[ -e $BASE/cellular/wake-on-modem && -e /sys/module/qrtr_smd/parameters/wake_ports ]]; then
    ports=
    for attempt in $(seq 1 60); do
        ports=$("$BASE/bin/qrtr-lookup" | awk '$4 == 0 && ($1 == 9 || $1 == 5) { print $5 }' | sort -u | paste -sd,)
        [[ $ports == *,* ]] && break
        sleep 1
    done
    if [[ -n $ports ]]; then
        echo "$ports" > /sys/module/qrtr_smd/parameters/wake_ports
        echo "wake on modem: ports $ports"
    fi
fi
# Opt-in IMS: the modem's IMS stack asks the AP for its IMS PDN through the
# QMI IMS DCM service, which the AP must publish (scripts/phone-qmi.py ims-dcm).
if [[ -e $BASE/cellular/ims-dcm-at-boot ]] &&
   ! pgrep -f "^python3 $BASE/cellular/phone-qmi.py ims-dcm" >/dev/null; then
    nohup python3 "$BASE/cellular/phone-qmi.py" ims-dcm > "$BASE/logs/ims-dcm.log" 2>&1 < /dev/null &
fi
bash "$BASE/phone-radio-test.sh" wifi
for attempt in $(seq 1 30); do
    [[ -d /sys/class/net/wlan0 ]] && break
    sleep 1
done
[[ -d /sys/class/net/wlan0 ]]
# Native4's frozen early USB helper still installs a metric-zero default.
# Keep USB as fallback, with its direct SSH subnet route unaffected.
if ip -4 route show default | grep -q 'via 172.16.42.2 dev usb0'; then
    ip route replace default via 172.16.42.2 dev usb0 metric 2000
    ip route del default via 172.16.42.2 dev usb0 metric 0 2>/dev/null || true
fi
bash "$BASE/phone-radio-network.sh"
# Opt-in ModemManager (docs/cellular-sim-20260926.md), once the system bus
# is up (phone-radio-network.sh starts it). The firmware opens no
# USIM provisioning session by itself and ModemManager does not either, so
# open it first; D-Bus activation of ModemManager is disabled by
# /usr/local/share/dbus-1/system-services/org.freedesktop.ModemManager1.service.
if [[ -e $BASE/cellular/mm-at-boot ]] && ! pgrep -x ModemManager >/dev/null; then
    for attempt in $(seq 1 10); do
        timeout 12 qmicli -d qrtr://0 --uim-change-provisioning-session=\
"session-type=primary-gw-provisioning,activate=yes,slot=1,aid=A0:00:00:00:87:10:02:FF:FF:FF:FF:89:06:19:00:00" \
            >/dev/null 2>&1 && break
        sleep 2
    done
    nohup /usr/bin/ModemManager > "$BASE/logs/ModemManager.log" 2>&1 < /dev/null &
fi
touch /run/guacamole-radio.ready
echo 'RADIO_READY: NetworkManager can autoconnect saved Wi-Fi profiles.'
