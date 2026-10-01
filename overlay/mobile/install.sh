#!/usr/bin/env bash
# Run inside the target user's Wayland system. Optional argument: device adapter.
set -euo pipefail
source_dir=$(cd "$(dirname "$0")" && pwd)
config=${XDG_CONFIG_HOME:-$HOME/.config}
data=${XDG_DATA_HOME:-$HOME/.local/share}
state=${XDG_STATE_HOME:-$HOME/.local/state}
device=${1:-}
backup="$state/omarchy-mobile/backups/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$backup" "$config/quickshell/omarchy-mobile" "$config/omarchy-mobile" \
    "$config/kitty" "$config/hypr" "$config/fastfetch" "$HOME/.local/bin" "$data/omarchy-mobile"
for name in quickshell kitty hypr fastfetch omarchy-mobile; do
    [[ -d "$config/$name" ]] && cp -a "$config/$name" "$backup/$name"
done
install -m644 "$source_dir"/*.qml "$source_dir/qmldir" "$config/quickshell/omarchy-mobile/"
install -m755 "$source_dir/start-mobile.sh" "$HOME/.local/bin/omarchy-mobile-session"
install -m755 "$source_dir/shell-watchdog.sh" "$HOME/.local/bin/omarchy-mobile-shell-watchdog"
install -m755 "$source_dir/keyboard.sh" "$HOME/.local/bin/omarchy-mobile-keyboard"
install -m755 "$source_dir/display-power.sh" "$HOME/.local/bin/omarchy-mobile-display"
install -m755 "$source_dir/power-button.py" "$HOME/.local/bin/omarchy-mobile-power"
install -m755 "$source_dir/power-menu.py" "$HOME/.local/bin/omarchy-mobile-power-menu"
install -m755 "$source_dir/battery.py" "$HOME/.local/bin/omarchy-mobile-battery"
install -m755 "$source_dir/about.py" "$HOME/.local/bin/omarchy-mobile-about"
install -m755 "$source_dir/weather.py" "$HOME/.local/bin/omarchy-mobile-weather"
install -m755 "$source_dir/wifi.py" "$HOME/.local/bin/omarchy-mobile-wifi"
install -m755 "$source_dir/speedtest.py" "$HOME/.local/bin/omarchy-mobile-speedtest"
install -m755 "$source_dir/volume.py" "$HOME/.local/bin/omarchy-mobile-volume"
mkdir -p "$config/wireplumber/wireplumber.conf.d"
install -m644 "$source_dir/wireplumber/"*.conf "$config/wireplumber/wireplumber.conf.d/"
install -m755 "$source_dir/stats.py" "$HOME/.local/bin/omarchy-mobile-stats"
install -m755 "$source_dir/prefs.py" "$HOME/.local/bin/omarchy-mobile-prefs"
install -m755 "$source_dir/sleep.py" "$HOME/.local/bin/omarchy-mobile-sleep"
install -m755 "$source_dir/clipboard.py" "$HOME/.local/bin/omarchy-mobile-clipboard"
install -m755 "$source_dir/settings.sh" "$HOME/.local/bin/omarchy-mobile-settings"
install -m755 "$source_dir/app.py" "$HOME/.local/bin/omarchy-mobile-app"
install -m755 "$source_dir/files.py" "$HOME/.local/bin/omarchy-mobile-files"
install -m755 "$source_dir/phone.py" "$HOME/.local/bin/omarchy-mobile-phone"
install -m755 "$source_dir/telephony.py" "$HOME/.local/bin/omarchy-mobile-telephony"
install -m755 "$source_dir/messages.py" "$HOME/.local/bin/omarchy-mobile-messages"
install -m755 "$source_dir/contacts.py" "$HOME/.local/bin/omarchy-mobile-contacts"
install -m755 "$source_dir/bluetooth.py" "$HOME/.local/bin/omarchy-mobile-bluetooth"
install -m755 "$source_dir/storage.py" "$HOME/.local/bin/omarchy-mobile-storage"
install -m755 "$source_dir/display.py" "$HOME/.local/bin/omarchy-mobile-displayinfo"
install -m755 "$source_dir/controls.py" "$HOME/.local/bin/omarchy-mobile-controls"
install -m755 "$source_dir/rotation.py" "$HOME/.local/bin/omarchy-mobile-rotation"
install -m755 "$source_dir/ambient.py" "$HOME/.local/bin/omarchy-mobile-ambient"
kit="$data/omarchy-mobile/qml/OmarchyMobile"
mkdir -p "$kit" "$config/quickshell/omarchy-mobile-settings" "$data/applications"
install -m644 "$source_dir/MobileTheme.qml" "$source_dir/TouchButton.qml" \
    "$source_dir/TouchTextField.qml" "$source_dir/DetailRow.qml" "$source_dir/Avatar.qml" "$kit/"
install -m644 "$source_dir/kit/"*.qml "$source_dir/kit/qmldir" "$kit/"
install -m644 "$source_dir/settings/shell.qml" "$config/quickshell/omarchy-mobile-settings/"
install -m644 "$source_dir/settings/omarchy-mobile-settings.desktop" "$data/applications/"
if [[ -d $source_dir/apps ]]; then
    for app in "$source_dir/apps"/*; do
        [[ -d $app && -f $app/manifest.json && -f $app/shell.qml ]] || continue
        id=$(basename "$app")
        mkdir -p "$data/omarchy-mobile/apps/$id" "$config/quickshell/omarchy-mobile-apps/$id"
        install -m644 "$app/manifest.json" "$data/omarchy-mobile/apps/$id/manifest.json"
        install -m644 "$app/shell.qml" "$config/quickshell/omarchy-mobile-apps/$id/shell.qml"
        for desktop in "$app"/*.desktop; do
            [[ -f $desktop ]] || continue
            install -m644 "$desktop" "$data/applications/"
        done
    done
fi
# The camera engine is a native QML module on libcamera; build it here, where
# its Qt and libcamera match the ones it will run against.
if command -v cmake >/dev/null && command -v ninja >/dev/null && pkg-config --exists libcamera; then
    camera_build="${XDG_CACHE_HOME:-$HOME/.cache}/omarchy-mobile/camera-build"
    camera_module="$data/omarchy-mobile/qml/OmarchyCamera"
    nice -n 10 cmake -S "$source_dir/camera" -B "$camera_build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    nice -n 10 cmake --build "$camera_build" -j2 >/dev/null
    mkdir -p "$camera_module"
    install -m644 "$camera_build/qmldir" "$camera_build/omarchycamera.qmltypes" "$camera_module/"
    install -m755 "$camera_build/libomarchycamera.so" "$camera_module/"
else
    echo "Skipping the camera engine: it needs cmake, ninja and libcamera." >&2
fi
# Hyprland 0.56 sends every finger's moves and lift to wherever the last finger
# landed; touch-fingers sends them to the finger's own surface (a shell left
# without a lift stops taking taps). A plugin loads only into the Hyprland it
# was built against, so it is built here, and again after a Hyprland update.
if command -v g++ >/dev/null && command -v make >/dev/null && pkg-config --exists hyprland; then
    touch_build="${XDG_CACHE_HOME:-$HOME/.cache}/omarchy-mobile/touch-fingers"
    mkdir -p "$touch_build" "$HOME/.local/lib/omarchy-mobile"
    nice -n 10 make -s -f "$source_dir/touch-fingers/Makefile" \
        SRC="$source_dir/touch-fingers/main.cpp" OUT="$touch_build/touch-fingers.so"
    # Replaced by rename: the running Hyprland keeps the old file mapped.
    install -m755 "$touch_build/touch-fingers.so" "$HOME/.local/lib/omarchy-mobile/touch-fingers.so.new"
    mv -f "$HOME/.local/lib/omarchy-mobile/touch-fingers.so.new" "$HOME/.local/lib/omarchy-mobile/touch-fingers.so"
else
    echo "Skipping touch-fingers: it needs g++, make and Hyprland's headers." >&2
fi
install -m755 "$source_dir/theme.py" "$HOME/.local/bin/omarchy-mobile-theme"
install -m755 "$source_dir/theme_install.py" "$HOME/.local/bin/omarchy-mobile-theme-install"
install -m644 "$source_dir/hypr-mobile.lua" "$data/omarchy-mobile/hypr-mobile.lua"
cp -a "$source_dir/themes" "$data/omarchy-mobile/"
install -m644 "$source_dir/fastfetch/omarchy.png" "$source_dir/fastfetch/omarchy.txt" \
    "$source_dir/fastfetch/omarchy-wordmark.txt" "$data/omarchy-mobile/"
install -m644 "$source_dir/fastfetch/config.jsonc" "$config/fastfetch/config.jsonc"
mkdir -p "$data/applications"
install -m644 "$source_dir/fastfetch/omarchy-mobile-fastfetch.desktop" "$data/applications/"
if [[ ! -f "$config/kitty/kitty.conf" ]]; then
    install -m644 "$source_dir/kitty.conf" "$config/kitty/kitty.conf"
elif ! grep -qxF 'include mobile-theme.conf' "$config/kitty/kitty.conf"; then
    printf '\ninclude mobile-theme.conf\n' >> "$config/kitty/kitty.conf"
fi
if ! grep -qE '^copy_on_select ' "$config/kitty/kitty.conf" 2>/dev/null; then
    printf '\ncopy_on_select clipboard\n' >> "$config/kitty/kitty.conf"
fi
install -m644 "$source_dir/kitty-font.conf" "$config/kitty/mobile-font.conf"
if ! grep -qxF "include mobile-font.conf" "$config/kitty/kitty.conf" 2>/dev/null; then
    printf "\ninclude mobile-font.conf\n" >> "$config/kitty/kitty.conf"
fi
python3 - "$config/kitty/kitty.conf" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
lines = path.read_text().splitlines() if path.exists() else []
found = False
out = []
for line in lines:
    if line.strip().startswith("confirm_os_window_close"):
        out.append("confirm_os_window_close 0")
        found = True
    else:
        out.append(line)
if not found:
    if out and out[-1] != "":
        out.append("")
    out.append("confirm_os_window_close 0")
path.parent.mkdir(parents=True, exist_ok=True)
path.write_text("\n".join(out) + "\n")
PY
if [[ -n $device ]]; then
    install -m644 "$device/mobile.json" "$config/omarchy-mobile/device.json"
    install -m755 "$device/desktop-prepare.sh" "$config/omarchy-mobile/session-prepare"
    if [[ -f "$device/reboot.sh" ]]; then
        install -m755 "$device/reboot.sh" "$config/omarchy-mobile/reboot"
    fi
    if [[ -f "$device/telephony.json" ]]; then
        install -m600 "$device/telephony.json" "$config/omarchy-mobile/telephony.json"
    fi
    if [[ -f "$device/power-suspend.sh" ]]; then
        install -m755 "$device/power-suspend.sh" "$config/omarchy-mobile/suspend"
    fi
    if [[ -f "$device/display-hook.sh" ]]; then
        install -m755 "$device/display-hook.sh" "$config/omarchy-mobile/display-hook"
    fi
    if [[ -f "$device/wallpaper-apply.sh" ]]; then
        install -m755 "$device/wallpaper-apply.sh" "$config/omarchy-mobile/wallpaper-apply"
    fi
    if [[ -f "$device/call-audio.sh" ]]; then
        install -m755 "$device/call-audio.sh" "$config/omarchy-mobile/call-audio"
        install -m755 "$device/call-audio-hold.py" "$config/omarchy-mobile/call-audio-hold"
    fi
else
    # Standard Hyprland integration; append one user override without resetting it.
    line=$(python3 - "$config/hypr/mobile.lua" <<'PYLUA'
import json,sys
print('dofile(' + json.dumps(sys.argv[1], ensure_ascii=False) + ')')
PYLUA
    )
    grep -qxF "$line" "$config/hypr/hyprland.lua" || printf '\n%s\n' "$line" >> "$config/hypr/hyprland.lua"
fi
"$HOME/.local/bin/omarchy-mobile-theme" sync >/dev/null
if [[ -n $device ]]; then
    # touch1 starts the default config. It delegates to the portable named shell.
    install -m644 "$device/quickshell-bootstrap.qml" "$config/quickshell/shell.qml"
else
    "$HOME/.local/bin/omarchy-mobile-session" launch
fi
printf 'Mobile configuration backup: %s\n' "$backup"
