#!/usr/bin/env bash
# Hand the PM8150B charger from the built-in bring-up policy (500 mA, 4.20 V)
# to guacamole_charger.ko (the stock OnePlus 7 Pro profile: 4.39 V, input by
# charger type, temperature bands), then apply the saved charge limit.
# Installed as /usr/local/sbin/guacamole-charging-start, run once per boot
# from session-prepare when /root/power-bringup/charging-enabled exists.
# On any failure the built-in driver gets the charger back.
set -uo pipefail
BASE=/root/power-bringup/charger
MODULE=$BASE/guacamole_charger.ko
DEV=c440000.spmi:pmic@2:charger@1000
BUILTIN=/sys/bus/platform/drivers/qcom-smbx-charger
LOG=$BASE/start.log
exec 9>/run/guacamole-charging-start.lock
flock -n 9 || exit 0
exec >> "$LOG" 2>&1
echo "== $(date '+%F %T') boot $(cat /proc/sys/kernel/random/boot_id)"

bound_to() { basename "$(readlink "/sys/bus/platform/devices/$DEV/driver" 2>/dev/null)" 2>/dev/null; }
give_back() {
    echo "giving the charger back to the built-in driver: $1"
    [[ $(bound_to) == guacamole-charger ]] && echo "$DEV" > /sys/bus/platform/drivers/guacamole-charger/unbind
    echo > "/sys/bus/platform/devices/$DEV/driver_override" 2>/dev/null
    [[ -n $(bound_to) ]] || echo "$DEV" > "$BUILTIN/bind"
    echo "now bound to $(bound_to)"
    exit 1
}

# The charger node is enabled by the power overlay about 140 s into the boot.
for i in $(seq 1 120); do
    [[ -n $(bound_to) ]] && break
    sleep 2
done
case $(bound_to) in
    guacamole-charger) echo "already running"; ;;
    qcom-smbx-charger) ;;
    *) echo "charger not present"; exit 1 ;;
esac
if [[ $(bound_to) == qcom-smbx-charger ]]; then
    (cd "$BASE" && sha256sum -c SHA256SUMS --quiet) || { echo "module checksum mismatch"; exit 1; }
    [[ $(modinfo -F vermagic "$MODULE" | cut -d' ' -f1) == "$(uname -r)" ]] || { echo "module is for another kernel"; exit 1; }
    [[ -d /sys/module/guacamole_charger ]] || insmod "$MODULE" || { echo "insmod failed"; exit 1; }
    echo "$DEV" > "$BUILTIN/unbind"
    echo guacamole-charger > "/sys/bus/platform/devices/$DEV/driver_override"
    echo "$DEV" > /sys/bus/platform/drivers_probe
    sleep 1
    [[ $(bound_to) == guacamole-charger ]] || give_back "probe failed"
    [[ -e /sys/class/power_supply/pm8150b-charger/charge_control_end_threshold ]] || give_back "no charger supply"
fi
HOME=/root XDG_STATE_HOME=/root/.local/state /root/.local/bin/omarchy-mobile-battery restore
echo "charging by $(bound_to): $(cat /sys/class/power_supply/pm8150b-charger/constant_charge_voltage) uV float, limit $(cat /sys/class/power_supply/pm8150b-charger/charge_control_end_threshold) %"
