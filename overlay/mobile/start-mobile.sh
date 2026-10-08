#!/usr/bin/env bash
set -euo pipefail
# Desktop Exec entries use user-installed commands, including webapp helpers.
# The bring-up initramfs supplies only system directories in PATH.
export PATH="$HOME/.local/bin:/usr/local/bin:$PATH"
export QML_IMPORT_PATH="${XDG_DATA_HOME:-$HOME/.local/share}/omarchy-mobile/qml${QML_IMPORT_PATH:+:$QML_IMPORT_PATH}"
export QML2_IMPORT_PATH="$QML_IMPORT_PATH${QML2_IMPORT_PATH:+:$QML2_IMPORT_PATH}"
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
config=${XDG_CONFIG_HOME:-$HOME/.config}
shell_config="$config/quickshell/omarchy-mobile/shell.qml"
# Desktop launchers should inherit WAYLAND_DISPLAY from the session. The
# OnePlus bootstrap wrapper supplies it for its initramfs/chroot environment.
: "${WAYLAND_DISPLAY:?Launch from a Wayland session}"
if [[ -z ${HYPRLAND_INSTANCE_SIGNATURE:-} ]]; then
    # The instance that answers: a Hyprland stopped with a signal leaves its
    # directory and socket behind (October 7: a restarted session's shell
    # took the dead one and lost workspaces and windows).
    for socket in "$XDG_RUNTIME_DIR"/hypr/*/.socket.sock; do
        [[ -S $socket ]] || continue
        instance=${socket%/.socket.sock}
        HYPRLAND_INSTANCE_SIGNATURE=${instance##*/} hyprctl version >/dev/null 2>&1 || continue
        export HYPRLAND_INSTANCE_SIGNATURE=${instance##*/}
        break
    done
    [[ -n ${HYPRLAND_INSTANCE_SIGNATURE:-} ]] || exit 1
fi
# Chroot/embedded sessions may lack a user bus. Reuse normal desktop buses;
# create one at the standard runtime path only for a standalone mobile launch.
if [[ ${1:-prepare} == launch && -z ${DBUS_SESSION_BUS_ADDRESS:-} ]]; then
    if [[ ! -S "$XDG_RUNTIME_DIR/bus" ]]; then
        dbus-daemon --session --address="unix:path=$XDG_RUNTIME_DIR/bus" --fork --nopidfile
    fi
    export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
fi
if [[ ${1:-prepare} == launch ]]; then
    # Opts in only when the separate, version-matched Kitty touch backend is installed.
    export KITTY_MOBILE_TOUCH=1
    exec quickshell -n -d -p "$shell_config"
fi
"$HOME/.local/bin/omarchy-mobile-theme" sync >/dev/null
# Start the shell again if it ever exits (one watchdog per session).
state=${XDG_STATE_HOME:-$HOME/.local/state}/omarchy-mobile
mkdir -p "$state"
setsid "$HOME/.local/bin/omarchy-mobile-shell-watchdog" </dev/null >>"$state/shell-watchdog.log" 2>&1 &
if [[ -x "$config/omarchy-mobile/session-prepare" ]]; then
    "$config/omarchy-mobile/session-prepare"
else
    # A standard Hyprland session sources ~/.config/hypr/mobile.lua itself.
    hyprctl reload
fi
errors=$(hyprctl configerrors)
[[ -z ${errors//[[:space:]]/} ]] || { printf '%s\n' "$errors" >&2; exit 1; }
