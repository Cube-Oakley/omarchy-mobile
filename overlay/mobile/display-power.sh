#!/usr/bin/env bash
# Display blanking. The CRT starts in the shell as soon as this asks; DPMS follows.
# System suspend is the sleep policy's (omarchy-mobile-sleep), once the screen is dark.
# ambient shows the always-on display instead, after the CRT close (the shell
# draws it, the panel stays lit and dim); on leaves it, and the CRT opens.
# ambient-sleep and ambient-wake switch the panel off and on under it (face
# down, a pocket: omarchy-mobile-ambient). idle is the screen timeout: off, or
# ambient when the always-on display is chosen, only from a lit screen.
# unlock leaves the always-on display at once for the fingerprint sensor.
# resync is the shell's, as it starts: it draws a lit screen, so the display
# leaves the always-on display or a dark panel to match.
set -euo pipefail
action=${1:-toggle}
case $action in
    on|off|toggle|preview|ambient|ambient-sleep|ambient-wake|wake|idle|unlock|resync) ;;
    *) echo 'Usage: omarchy-mobile-display on|off|toggle|ambient|ambient-sleep|ambient-wake|wake|idle [ambient]|unlock|resync' >&2; exit 2 ;;
esac
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
# A line per request in the runtime log (this boot only): what was asked, how
# it ended and the last command run, so a change that stops part way says
# where (set -e ends the script at a failing command).
log=$XDG_RUNTIME_DIR/omarchy-mobile-display.log
asked=$action started=$(date +%s%3N)
[[ ! -f $log ]] || (( $(stat -c %s "$log") < 200000 )) || : >"$log"
trap 'status=$? last=$BASH_COMMAND; printf "%s %s->%s exit %s in %s ms, last: %s\n" "$(date +%T.%3N)" \
    "$asked" "$action" "$status" "$(( $(date +%s%3N) - started ))" "$last" >>"$log"' EXIT
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

# The device's optional screen hook: display-hook on|off tells hardware
# outside the display whether the screen is lit (the Pixel's modem then
# stops or resumes its optional reports). It runs in the background.
# `ambient` follows once the always-on clock shows (as off, plus whatever
# low-power display mode the device has), and `ambient-exit` runs, waited
# for, before the clock fades out; a hook without them may ignore both.
hook=${XDG_CONFIG_HOME:-$HOME/.config}/omarchy-mobile/display-hook
screen_hook() {
    # The screen's state for apps (on, off or ambient): the browser hides its
    # pages unless it is on, since Hyprland keeps a window exposed with the
    # display off and X went on decoding its feed's video with the screen dark.
    mkdir -p "$XDG_RUNTIME_DIR/omarchy-mobile"
    printf '%s\n' "$1" > "$XDG_RUNTIME_DIR/omarchy-mobile/screen.new" &&
        mv "$XDG_RUNTIME_DIR/omarchy-mobile/screen.new" "$XDG_RUNTIME_DIR/omarchy-mobile/screen"
    [[ -x $hook ]] || return 0
    "$hook" "$1" >/dev/null 2>&1 </dev/null &
}

dpms() {
    hyprctl eval "hl.dispatch(hl.dsp.dpms({ action = \"$1\" }))"
}
dpms_on() {
    # Not grep -q: quitting at the first match could kill hyprctl with SIGPIPE,
    # which pipefail would report as "off".
    hyprctl monitors | grep 'dpmsStatus: 1' >/dev/null
}
# A request to the shell: through its FIFO, else quickshell ipc, which starts
# a process of its own (about 0.4 s while only the little cores run).
fifo=$XDG_RUNTIME_DIR/omarchy-mobile-shell.fifo
shell_request() {
    [[ -f $shell ]] || return 1
    if [[ -p $fifo ]] && timeout 1 sh -c 'printf "%s\n" "$2" > "$1"' request "$fifo" "$*"; then
        return 0
    fi
    quickshell ipc -n -p "$shell" call mobile "$@" >/dev/null
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
    shell_request crt "$mode" "$token" || return 1
    wait_state "settled $token"
}

ambient() {
    shell_request ambient "$1"
}

case $action in
    toggle) dpms_on && action=off || action=on ;;
    # An incoming call: light a dark or always-on screen, leave a lit one alone.
    wake) [[ -f $ambient_flag ]] || ! dpms_on && action=on || exit 0 ;;
    idle)
        [[ ! -f $ambient_flag ]] && dpms_on || exit 0
        [[ ${2:-} == ambient ]] && action=ambient || action=off ;;
esac
# A finger on the fingerprint sensor in the always-on display: the panel
# leaves its low-power mode first, so it can light the sensor's circle, and the
# screen opens at once, without the fade or the CRT open.
if [[ $action == unlock ]]; then
    [[ -f $ambient_flag ]] || exit 0
    rm -f "$ambient_flag" "$asleep_flag"
    [[ -x $hook ]] && "$hook" ambient-exit >/dev/null 2>&1 </dev/null
    dpms_on || dpms enable
    ambient false || true
    shell_request crt open "$(date +%s%N)"
    screen_hook on
    exit 0
fi
# A shell that has just started (a restart, a crash relaunch, a reload)
# draws a lit, open screen and knows nothing of the display before it. Under
# the always-on display or a dark panel, the display follows the shell:
# otherwise the panel stayed in its low-power mode, with CPUs 4-7 offline and
# touch in gesture mode, under a phone in use (October 2). A lit screen is
# left alone.
if [[ $action == resync ]]; then
    if [[ -f $ambient_flag ]]; then
        rm -f "$ambient_flag" "$asleep_flag"
        [[ -x $hook ]] && "$hook" ambient-exit >/dev/null 2>&1 </dev/null
    elif dpms_on; then
        exit 0
    fi
    dpms_on || dpms enable
    screen_hook on
    exit 0
fi
# Leaving the always-on display: the clock fades out onto the CRT's parked
# black, and the CRT opens the screen as on any wake.
if [[ $action == on && -f $ambient_flag ]]; then
    rm -f "$ambient_flag" "$asleep_flag"
    [[ -x $hook ]] && "$hook" ambient-exit >/dev/null 2>&1 </dev/null
    dpms_on || dpms enable
    ambient false || true
    sleep 0.15
    crt on || dpms enable
    screen_hook on
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
        screen_hook off
        ;;
    ambient)
        crt off || true
        dpms_on || dpms enable
        ambient true
        touch "$ambient_flag"
        screen_hook ambient
        ;;
    ambient-sleep)
        [[ -f $ambient_flag ]] || exit 0
        touch "$asleep_flag"
        dpms disable
        screen_hook off
        ;;
    ambient-wake)
        [[ -f $ambient_flag && -f $asleep_flag ]] || exit 0
        rm -f "$asleep_flag"
        dpms enable
        screen_hook ambient
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
        # The hook first: the cores and touch come back while the screen opens.
        screen_hook on
        crt on || dpms enable
        ;;
esac
