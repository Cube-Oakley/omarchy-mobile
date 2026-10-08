#!/bin/bash
# omarchy-mobile-report: a problem report, from the power menu's "Report a
# problem". Everything that tells what the phone was doing just now goes into
# ~/.local/state/omarchy-mobile/reports/<time>/ (the last 20 are kept), and a
# notification says it is saved. With the device's root half
# (pixel-report-root through sudo, when there is one): the kernel's log,
# Android's log, the modem. Nothing leaves the phone.
set -u
state=${XDG_STATE_HOME:-$HOME/.local/state}/omarchy-mobile
runtime=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
stamp=$(date +%Y%m%d-%H%M%S)
dir=$state/reports/$stamp
mkdir -p "$dir" || exit 1
run() { local name=$1; shift; timeout 20 "$@" > "$dir/$name" 2>&1; }
{
    echo "report $stamp, $(uptime)"
    cat "$runtime/omarchy-mobile/screen" 2>/dev/null && echo
    cat /sys/class/power_supply/battery/{capacity,status,current_now} 2>/dev/null
} > "$dir/summary.txt"
# The shell's and the keyboard's logs (they live in /run: gone after a restart).
run shell.log env NO_COLOR=1 quickshell log --log-times -t 4000 -p "$HOME/.config/quickshell/omarchy-mobile/shell.qml"
run keyboard.log env NO_COLOR=1 quickshell log --log-times -t 1000 -p "$HOME/.config/quickshell/omarchy-mobile-keyboard/shell.qml"
if [[ -n ${HYPRLAND_INSTANCE_SIGNATURE:-} ]]; then
    tail -n 3000 "$runtime/hypr/$HYPRLAND_INSTANCE_SIGNATURE/hyprland.log" > "$dir/hyprland.log" 2>/dev/null
fi
run windows.json hyprctl clients -j
run monitors.json hyprctl monitors -j
run layers.txt hyprctl layers
for name in sleep.log browser-events.log shell-watchdog.log; do
    tail -n 300 "$state/$name" > "$dir/$name" 2>/dev/null
done
tail -n 60 "$state/battery-history.jsonl" > "$dir/battery-history.jsonl" 2>/dev/null
run processes.txt ps -eo pid,ppid,etime,stat,pcpu,rss,args --sort=-pcpu
run memory.txt cat /proc/meminfo
run network.txt sh -c 'nmcli -t device status; ip route; ip -br addr'
run audio.txt pactl list sink-inputs short
run waydroid.txt waydroid status
if sudo -n -l /usr/local/sbin/pixel-report-root >/dev/null 2>&1; then
    mkdir -p "$dir/root"
    sudo -n /usr/local/sbin/pixel-report-root | tar -C "$dir/root" -xf - 2>/dev/null
fi
# The last 20 reports.
ls -1d "$state"/reports/*/ 2>/dev/null | head -n -20 | xargs -r rm -rf
gdbus call --session --dest org.freedesktop.Notifications --object-path /org/freedesktop/Notifications \
    --method org.freedesktop.Notifications.Notify -- 'Problem report' 0 dialog-information \
    'Problem report saved' "Saved at $(date +%H:%M:%S). Tell what happened, and when." '[]' \
    "{'category': <'x-omarchy.report'>}" -1 >/dev/null 2>&1
echo "$dir"
