#!/usr/bin/env bash
# Launch the Settings app. Optional argument: home|appearance|clipboard
set -euo pipefail
export PATH="$HOME/.local/bin:/usr/local/bin:$PATH"
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export QT_IM_MODULE=none
export QML_IMPORT_PATH="${XDG_DATA_HOME:-$HOME/.local/share}/omarchy-mobile/qml${QML_IMPORT_PATH:+:$QML_IMPORT_PATH}"
export QML2_IMPORT_PATH="$QML_IMPORT_PATH${QML2_IMPORT_PATH:+:$QML2_IMPORT_PATH}"
config=${XDG_CONFIG_HOME:-$HOME/.config}
shell_config="$config/quickshell/omarchy-mobile-settings/shell.qml"
panel=${1:-home}
case $panel in home|appearance|clipboard|network|wifi|sound|battery|about|bluetooth|display|storage|apps|weather) ;; *) panel=home ;; esac
: "${WAYLAND_DISPLAY:?Launch from a Wayland session}"
if [[ -z ${HYPRLAND_INSTANCE_SIGNATURE:-} ]]; then
    for socket in "$XDG_RUNTIME_DIR"/hypr/*/.socket.sock; do
        [[ -S $socket ]] || continue
        instance=${socket%/.socket.sock}
        export HYPRLAND_INSTANCE_SIGNATURE=${instance##*/}
        break
    done
    [[ -n ${HYPRLAND_INSTANCE_SIGNATURE:-} ]] || exit 1
fi
if [[ -z ${DBUS_SESSION_BUS_ADDRESS:-} && -S $XDG_RUNTIME_DIR/bus ]]; then
    export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
fi

# Closing the window often leaves a headless Quickshell that no longer maps a
# toplevel. IPC to that process hangs, so the launcher must not wait on it.
stop_settings() {
    local pid cmd
    for pid in $(pgrep -u "$(id -u)" -x quickshell || true); do
        cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)
        if [[ $cmd == *"$shell_config"* ]]; then
            kill "$pid" 2>/dev/null || true
        fi
    done
}
stop_settings
for _ in 1 2 3 4 5 6 7 8; do
    still=0
    for pid in $(pgrep -u "$(id -u)" -x quickshell || true); do
        cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)
        if [[ $cmd == *"$shell_config"* ]]; then
            still=1
            break
        fi
    done
    [[ $still -eq 0 ]] && break
    sleep 0.1
done
stop_settings
for pid in $(pgrep -u "$(id -u)" -x quickshell || true); do
    cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)
    if [[ $cmd == *"$shell_config"* ]]; then
        kill -KILL "$pid" 2>/dev/null || true
    fi
done

export OMARCHY_MOBILE_SETTINGS_PANEL=$panel
exec quickshell -n -d -p "$shell_config"
