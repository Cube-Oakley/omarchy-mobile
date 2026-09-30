#!/usr/bin/env bash
# One instrumented cellular trial from the host: deploy the phone scripts
# (hash-checked), load qcom_stats, run phone-online-trial.py, then pull the
# slice of the boot-long DIAG capture it covered and decode everything into
# out/cellular/trials/NAME/. MSS_LEVEL=N first sets the AP's mss.lvl vote
# with kernel/radio/mss_vote.c (0 = released, as stock).
#
#   scripts/run-cellular-trial.sh NAME "TRIAL ARGS"
#   e.g. MSS_LEVEL=0 scripts/run-cellular-trial.sh cycle1 "240 --cycle 20,40"
#
# Needs diag-router and its capture running from boot (touch
# /root/radio-bringup/cellular/diag-at-boot, then reboot).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR=/root/radio-bringup/cellular
NAME=$1 TRIAL_ARGS=${2:-360}
CAPTURE=$DIR/diag-boot.bin
OUT="$ROOT/out/cellular/trials/$NAME"
ssh_() { timeout "${T:-60}" bash "$ROOT/scripts/phone-ssh.sh" "$@"; }
mkdir -p "$OUT"

for f in phone-qmi.py phone-online-trial.py phone-diag-capture.py; do
    got=$(ssh_ "cat > $DIR/$f && sha256sum $DIR/$f" < "$ROOT/scripts/$f" | cut -d' ' -f1)
    [[ $got == $(sha256sum "$ROOT/scripts/$f" | cut -d' ' -f1) ]] ||
        { echo "upload of $f failed" >&2; exit 1; }
done
ssh_ "pgrep -f src/diag/diag-router >/dev/null || { echo 'diag-router is not running' >&2; exit 1; }
      pgrep -f 'phone-diag-capture.py $CAPTURE' >/dev/null || { echo 'no boot capture' >&2; exit 1; }
      lsmod | grep -q qcom_stats || insmod /root/radio-bringup/stats/qcom_stats.ko"
if [[ -n ${MSS_LEVEL:-} ]]; then
    ssh_ "lsmod | grep -q mss_vote || insmod /root/radio-bringup/stats/mss_vote.ko
          echo $MSS_LEVEL > /sys/module/mss_vote/parameters/level && dmesg | tail -1"
fi
start=$(ssh_ "stat -c %s $CAPTURE")

T=900 bash "$ROOT/scripts/phone-ssh.sh" 'dmesg -w --time-format iso' > "$OUT/dmesg.log" 2>&1 &
dmesg_pid=$!
T=3700 ssh_ "python3 $DIR/phone-online-trial.py $TRIAL_ARGS" > "$OUT/trial.jsonl" 2>&1 || true
sleep 2
kill "$dmesg_pid" 2>/dev/null || true

sleep 3
end=$(ssh_ "stat -c %s $CAPTURE")
T=120 ssh_ "tail -c +$((start + 1)) $CAPTURE | head -c $((end - start))" > "$OUT/capture.bin"
python3 "$ROOT/scripts/decode_diag.py" "$OUT/capture.bin" > "$OUT/capture.txt"
rm -rf "$ROOT/scripts/__pycache__"
grep -vE '"event": "(sleep|nas)"' "$OUT/trial.jsonl" | grep '^{' || true
grep -E 'watchdog received|crash detected' "$OUT/dmesg.log" || echo 'no modem crash in the kernel log'

# A crashed modem also takes Wi-Fi down, and its restart path soon stalls
# SSH; reboot at once rather than leave the phone in that state.
if grep -q 'crash detected in modem' "$OUT/dmesg.log"; then
    bash "$ROOT/scripts/phone-crash-reboot.sh"
fi
