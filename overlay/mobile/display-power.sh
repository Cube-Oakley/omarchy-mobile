#!/usr/bin/env bash
# Display blanking. The CRT starts in the shell as soon as this asks; DPMS follows.
# System suspend is the sleep policy's (omarchy-mobile-sleep), once the screen is dark.
# ambient shows the always-on display instead, after the CRT close (the shell
# draws it, the panel stays lit and dim); on leaves it, and the CRT opens.
# ambient-sleep and ambient-wake switch the panel off and on under it (face
# down, a pocket: omarchy-mobile-ambient). idle is the screen timeout: off, or
# ambient when the always-on display is chosen, only from a lit screen.
set -euo pipefail
action=${1:-toggle}
case $action in
    on|off|toggle|preview|ambient|ambient-sleep|ambient-wake|wake|idle) ;;
    *) echo 'Usage: omarchy-mobile-display on|off|toggle|ambient|ambient-sleep|ambient-wake|wake|idle [ambient]' >&2; exit 2 ;;
esac
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
if [[ -z ${HYPRLAND_INSTANCE_SIGNATURE:-} ]]; then
    for socket in "$XDG_RUNTIME_DIR"/hypr/*/.socket.sock; do
        [[ -S $socket ]] || continue
        instance=${socket%/.socket.sock}
        export HYPRLAND_INSTANCE_SIGNATURE=${instance##*/}
        break
    done
    [[ -n ${HYPRLAND_INSTANCE_SIGNATURE:-} ]] || exit 1
fi
# quickshell ipc only finds a shell on the same Wayland display. Without it
# the CRT request fails, and the fallback lights the panel under the parked
# black cover: a dark screen that swallows touch.
if [[ -z ${WAYLAND_DISPLAY:-} ]]; then
    WAYLAND_DISPLAY=$(hyprctl instances -j | python3 -c 'import json, sys
for instance in json.load(sys.stdin):
    if instance.get("instance") == sys.argv[1]:
        print(instance.get("wl_socket", ""))' "$HYPRLAND_INSTANCE_SIGNATURE")
    [[ -n $WAYLAND_DISPLAY ]] || exit 1
    export WAYLAND_DISPLAY
fi
shell=${XDG_CONFIG_HOME:-$HOME/.config}/quickshell/omarchy-mobile/shell.qml
state=$XDG_RUNTIME_DIR/omarchy-mobile-crt.state
ambient_flag=$XDG_RUNTIME_DIR/omarchy-mobile-ambient
asleep_flag=$XDG_RUNTIME_DIR/omarchy-mobile-ambient-asleep

dpms() {
    hyprctl eval "hl.dispatch(hl.dsp.dpms({ action = \"$1\" }))"
}
dpms_on() {
    hyprctl -j monitors | python3 -c 'import json,sys; monitors=json.load(sys.stdin); raise SystemExit(0 if any(m.get("dpmsStatus") for m in monitors) else 1)'
}
wait_state() {
    local want=$1 i line
    for i in $(seq 1 110); do
        line=$(cat "$state" 2>/dev/null || true)
        [[ $line == "$want" ]] && return 0
        sleep 0.03
    done
    return 1
}
crt() {
    local mode=$1 token
    [[ -f $shell ]] || return 1
    token=$(date +%s%N)
    printf 'pending %s\n' "$token" > "$state"
    quickshell ipc -n -p "$shell" call mobile crt "$mode" "$token" >/dev/null || return 1
    wait_state "settled $token"
}

ambient() {
    [[ -f $shell ]] && quickshell ipc -n -p "$shell" call mobile ambient "$1" >/dev/null
}

case $action in
    toggle) dpms_on && action=off || action=on ;;
    # An incoming call: light a dark or always-on screen, leave a lit one alone.
    wake) [[ -f $ambient_flag ]] || ! dpms_on && action=on || exit 0 ;;
    idle)
        [[ ! -f $ambient_flag ]] && dpms_on || exit 0
        [[ ${2:-} == ambient ]] && action=ambient || action=off ;;
esac
# Leaving the always-on display: the clock fades out onto the CRT's parked
# black, and the CRT opens the screen as on any wake.
if [[ $action == on && -f $ambient_flag ]]; then
    rm -f "$ambient_flag" "$asleep_flag"
    dpms_on || dpms enable
    ambient false || true
    sleep 0.3
    crt on || dpms enable
    exit 0
fi
# A real off from the always-on display leaves it first.
if [[ $action == off && -f $ambient_flag ]]; then
    rm -f "$ambient_flag" "$asleep_flag"
    ambient false || true
fi
case $action in
    preview)
        crt off
        crt on
        ;;
    off)
        crt off || true
        dpms disable
        ;;
    ambient)
        crt off || true
        dpms_on || dpms enable
        ambient true
        touch "$ambient_flag"
        ;;
    ambient-sleep)
        [[ -f $ambient_flag ]] || exit 0
        touch "$asleep_flag"
        dpms disable
        ;;
    ambient-wake)
        [[ -f $ambient_flag && -f $asleep_flag ]] || exit 0
        rm -f "$asleep_flag"
        dpms enable
        ;;
    on)
        # Suspend cleanup and the power key both ask for wake. The open
        # animation enables the panel itself, so a second on in the next
        # few seconds would play it again. Sleep is unchanged.
        now=$(date +%s)
        stamp=$XDG_RUNTIME_DIR/omarchy-mobile-crt.wake
        if [[ -f $stamp ]]; then
            prev=$(tr -cd '0-9' < "$stamp" || true)
            if [[ -n ${prev} ]] && (( now - prev < 3 )) && dpms_on; then
                exit 0
            fi
        fi
        printf '%s\n' "$now" > "$stamp"
        crt on || dpms enable
        ;;
esac
