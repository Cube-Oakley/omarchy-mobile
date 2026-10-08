#!/usr/bin/env bash
set -euo pipefail
export PATH="$HOME/.local/bin:/usr/local/bin:$PATH"
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
: "${WAYLAND_DISPLAY:?Launch from a Wayland session}"
state=${XDG_STATE_HOME:-$HOME/.local/state}/omarchy-mobile
mkdir -p "$state"
pidfile="$XDG_RUNTIME_DIR/omarchy-mobile-keyboard.pid"
exec 9>"$XDG_RUNTIME_DIR/omarchy-mobile-keyboard.lock"
flock 9
action=${1:-toggle}
case $action in start|show|hide|toggle) ;; *) echo 'Usage: keyboard.sh start|show|hide|toggle' >&2; exit 2 ;; esac

# The keyboard (keyboard/shell.qml, a Quickshell window in the theme's
# colours, with swipe typing). It watches a command file; each line carries
# the time, so a command left from before it started is not replayed.
# wvkbd below remains for $state/keyboard-wvkbd or a missing keyboard.
qml=${XDG_CONFIG_HOME:-$HOME/.config}/quickshell/omarchy-mobile-keyboard/shell.qml
if [[ -f $qml && ! -e $state/keyboard-wvkbd ]]; then
    if ! pgrep -u "$(id -u)" -f "^quickshell .*-p $qml\$" > /dev/null; then
        pkill -u "$(id -u)" -x wvkbd-mobintl 2>/dev/null || true
        imports=${XDG_DATA_HOME:-$HOME/.local/share}/omarchy-mobile/qml
        QML_IMPORT_PATH=$imports QML2_IMPORT_PATH=$imports \
            nohup quickshell -n -p "$qml" > "$state/keyboard.log" 2>&1 9>&- < /dev/null &
    fi
    if [[ $action != start ]]; then
        printf '%s %s\n' "$action" "$(date +%s%3N)" > "$XDG_RUNTIME_DIR/omarchy-mobile-keyboard.cmd"
    fi
    exit 0
fi
pid=$(cat "$pidfile" 2>/dev/null || true)
if [[ ! $pid =~ ^[0-9]+$ ]] || [[ $(cat "/proc/$pid/comm" 2>/dev/null || true) != wvkbd-mobintl ]]; then
    # Process substitution needs /dev/fd, which minimal boot environments may
    # not provide. Capture first so Python failures also propagate through -e.
    settings_text=$(python3 - "$state/palette.json" <<'PY'
import json,sys
try:
    p=json.load(open(sys.argv[1])); c=p['colors']; d=p.get('device',{})
except (OSError,ValueError,KeyError):
    p={}; c={}; d={}
for key,default in [('keyboardHeight',280),('keyboardLandscapeHeight',200)]:
    value=d.get(key,default)
    print(value if isinstance(value,int) and 100 <= value <= 500 else default)
for key,default in [('background','#1a1b26'),('lighter_background','#24283b'),('muted','#414868'),('foreground','#c0caf5'),('accent','#7aa2f7')]:
    print(c.get(key,default).lstrip('#'))
font=p.get('fontFamily') or 'JetBrainsMono Nerd Font'
print(font if isinstance(font,str) and font.strip() else 'JetBrainsMono Nerd Font')
PY
    )
    mapfile -t settings <<< "$settings_text"
    # No --auto: focusing Kitty must not pop the keyboard. It stays down
    # until a text field or an explicit show asks for it.
    nohup wvkbd-mobintl --hidden -H "${settings[0]}" -L "${settings[1]}" \
        -l simple,special,nav --fn "${settings[7]} 16" \
        --bg "${settings[2]}" --fg "${settings[3]}" --fg-sp "${settings[4]}" \
        --text "${settings[5]}" --text-sp "${settings[5]}" \
        --press "${settings[6]}" --press-sp "${settings[6]}" \
        > "$state/keyboard.log" 2>&1 9>&- < /dev/null &
    pid=$!
    printf '%s\n' "$pid" > "$pidfile"
    sleep 0.3
    kill -0 "$pid"
fi
case ${1:-toggle} in
    start) ;;
    show) kill -USR2 "$pid" ;;
    hide) kill -USR1 "$pid" ;;
    toggle) kill -RTMIN "$pid" ;;
    *) echo 'Usage: keyboard.sh start|show|hide|toggle' >&2; exit 2 ;;
esac
