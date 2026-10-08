#!/usr/bin/env bash
# Start the mobile shell again if it exits. Without it nothing draws the
# screen or answers the power button's animation, so the phone looks dead
# (September 24: Hyprland 0.56.2 disconnected the shell when a window closed
# during a preview capture). One per session: the session's prepare step starts
# it every time the shell starts, and the lock keeps the first. A deliberate
# restart takes under a second, so the shell must be missing twice, 3 s apart;
# quickshell -n refuses a second copy anyway. The screen is left as it was.
# A shell that dies leaves its helpers running, and the new one starts its
# own: they are stopped first, found in the dead shell's session (the shell
# leads one) by name, so the board start scripts the session also started are
# left alone.
set -uo pipefail
runtime=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
exec 9>"$runtime/omarchy-mobile-shell-watchdog.lock"
flock -n 9 || exit 0
# Nothing this starts may keep the lock (9>&- below): a shell started here
# would hold it for its whole life, and a leftover sleep for a moment.
config=${XDG_CONFIG_HOME:-$HOME/.config}/quickshell/omarchy-mobile/shell.qml
helpers='^(/bin/|/usr/bin/)?(dbus-monitor|nmcli monitor|udevadm monitor|pactl subscribe|wl-paste --watch|monitor-sensor)|/\.local/bin/omarchy-mobile-'
stop_helpers() {
    local sid=$1 pid s args
    while read -r pid s args; do
        [[ $s == "$sid" && $args =~ $helpers ]] && kill "$pid" 2>/dev/null && echo "  stopped $pid: $args"
    done < <(ps -eo pid=,sid=,args= 9>&-)
}
missing=0
shell=
while sleep 3 9>&-; do
    pgrep -x Hyprland >/dev/null 9>&- || exit 0
    if pid=$(pgrep -f "^quickshell -n -d -p $config\$" 9>&-); then
        shell=$pid
        missing=0
        continue
    fi
    missing=$((missing + 1))
    (( missing < 2 )) && continue
    echo "$(date -Is) the shell is not running; starting it"
    if [[ -n $shell ]]; then
        # Quickshell relaunches a crashed shell in place: the same PID, with
        # only the binary for a command line, which the match above misses.
        # Left running, two shells draw (October 2), so it goes too.
        [[ $(cat "/proc/$shell/comm" 2>/dev/null) == quickshell ]] && kill "$shell" 2>/dev/null &&
            echo "  stopped $shell: the shell Quickshell relaunched after a crash"
        stop_helpers "$shell"
    fi
    "$HOME/.local/bin/omarchy-mobile-session" launch </dev/null >/dev/null 2>&1 9>&- ||
        echo "$(date -Is) starting the shell failed"
    missing=0
done
