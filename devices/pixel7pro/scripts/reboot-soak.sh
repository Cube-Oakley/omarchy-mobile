#!/bin/bash
# Laptop side: restart the Pixel N times (orderly, the modem stopped first)
# and record for each boot whether mobile data came up and after how long,
# Wi-Fi, the video codec, kernel oopses, new pstore records and new modem
# crash records (/var/log/omarchy-mobile/modem-crashes.log). A modem session
# that ends before its firmware is ONLINE keeps the start marker, which
# skips the modem on later boots: the soak archives it as
# starting.json.soak-<stage>-<boot> and goes on. One that ends after ONLINE
# is left to manager.py, which starts the modem again on the next boot
# (unless it skipped the start: two such ends within 30 minutes).
# Usage: reboot-soak.sh N OUTDIR
set -u
n=$1 out=$2
here=$(cd "$(dirname "$0")" && pwd)
ssh=$here/../out/checkpoints/20260925-persistence-power/arch-session-f/ssh
mkdir -p "$out"
log=$out/soak.log
phone() { timeout "${2:-30}" "$ssh" "$1" 2>/dev/null; }
echo "# $(date '+%F %T') reboot soak, $n boots" >> "$log"
for ((i = 1; i <= n; i++)); do
    phone 'python3 /usr/local/lib/omarchy-mobile/modem/manager.py stop >/dev/null 2>&1; sync' 90
    if ! timeout 420 python3 "$here/pixel-reboot.py" linux > "$out/reboot-$i.txt" 2>&1; then
        timeout 300 python3 "$here/pixel-reboot.py" wait >> "$out/reboot-$i.txt" 2>&1 ||
            { echo "$i reboot failed; stopping" >> "$log"; exit 1; }
    fi
    start=$(date +%s) result=timeout
    while (( $(date +%s) - start < 240 )); do
        state=$(phone 'ip -4 -br addr show rmnet0 2>/dev/null | grep -c "/32"; grep -cE "Traceback|RuntimeError" /run/pixel-modem/manager.log 2>/dev/null')
        [[ $(sed -n 1p <<< "$state") == 1 ]] && { result=data; break; }
        [[ $(sed -n 2p <<< "$state") -ge 1 ]] && { result=failed; break; }
        sleep 4
    done
    facts=$(phone 'printf "boot=%s up=%s wifi=%s codec=%s oops=%s warn=%s pstore=%s crashes=%s stage=%s" \
        "$(cut -c1-8 /proc/sys/kernel/random/boot_id)" "$(cut -d. -f1 /proc/uptime)" \
        "$(ip -4 -br addr show wlp1s0 2>/dev/null | grep -c /24)" "$([ -e /dev/video-dec0 ] && echo 1 || echo 0)" \
        "$(dmesg | grep -cE "Oops|BUG:|Internal error|Kernel panic")" "$(dmesg | grep -c "WARNING:")" \
        "$(ls /var/log/pstore | wc -l)" "$(grep -c "CP left ONLINE" /var/log/omarchy-mobile/modem-crashes.log 2>/dev/null || echo 0)" \
        "$(python3 -c "import json; print(json.load(open(\"/var/lib/omarchy-mobile/modem/starting.json\"))[\"stage\"])" 2>/dev/null || echo none)"')
    echo "$i $(date +%T) modem=$result after=$(( $(date +%s) - start ))s $facts" | tee -a "$log"
    if [[ $result != data ]]; then
        phone 'cat /run/pixel-modem/manager.log /run/pixel-modem/runtime.log; tail -30 /run/pixel-modem/apps.log; tail -120 /var/log/omarchy-mobile/modem-crashes.log 2>/dev/null' 60 > "$out/modem-$i.txt"
        phone 'dmesg' 60 > "$out/dmesg-$i.txt"
        phone 'g=/var/lib/omarchy-mobile/modem/starting.json; s=$(python3 -c "import json; d=json.load(open(\"$g\")); print(d[\"stage\"], d[\"boot\"][:8])" 2>/dev/null) || exit 0
            skipped=$(grep -c "automatic start skipped" /run/pixel-modem/manager.log 2>/dev/null)
            case "$s" in online*|running*) [ "${skipped:-0}" -ge 1 ] || exit 0;; esac
            set -- $s; mv $g $g.soak-$1-$2; sync; echo "archived $g.soak-$1-$2"' >> "$log"
    fi
done
echo "# $(date '+%F %T') done" >> "$log"
