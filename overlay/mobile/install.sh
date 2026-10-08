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
install -m755 "$source_dir/report.sh" "$HOME/.local/bin/omarchy-mobile-report"
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
install -m755 "$source_dir/clock.py" "$HOME/.local/bin/omarchy-mobile-clock"
install -m755 "$source_dir/calendars.py" "$HOME/.local/bin/omarchy-mobile-calendar"
install -m755 "$source_dir/clipboard.py" "$HOME/.local/bin/omarchy-mobile-clipboard"
install -m755 "$source_dir/settings.sh" "$HOME/.local/bin/omarchy-mobile-settings"
install -m755 "$source_dir/app.py" "$HOME/.local/bin/omarchy-mobile-app"
install -m755 "$source_dir/files.py" "$HOME/.local/bin/omarchy-mobile-files"
install -m755 "$source_dir/phone.py" "$HOME/.local/bin/omarchy-mobile-phone"
install -m755 "$source_dir/telephony.py" "$HOME/.local/bin/omarchy-mobile-telephony"
install -m755 "$source_dir/messages.py" "$HOME/.local/bin/omarchy-mobile-messages"
install -m755 "$source_dir/mms.py" "$HOME/.local/bin/omarchy-mobile-mms"
install -m755 "$source_dir/screen.py" "$HOME/.local/bin/omarchy-mobile-screen"
install -m755 "$source_dir/contacts.py" "$HOME/.local/bin/omarchy-mobile-contacts"
install -m755 "$source_dir/notes.py" "$HOME/.local/bin/omarchy-mobile-notes"
install -m755 "$source_dir/share.py" "$HOME/.local/bin/omarchy-mobile-share"
install -m755 "$source_dir/hotspot.py" "$HOME/.local/bin/omarchy-mobile-hotspot"
install -m755 "$source_dir/vpn.py" "$HOME/.local/bin/omarchy-mobile-vpn"
install -m755 "$source_dir/dictate.py" "$HOME/.local/bin/omarchy-mobile-dictate"
install -m755 "$source_dir/assistant.py" "$HOME/.local/bin/omarchy-mobile-assistant"
install -m755 "$source_dir/nfc.py" "$HOME/.local/bin/omarchy-mobile-nfc"
install -m755 "$source_dir/bluetooth.py" "$HOME/.local/bin/omarchy-mobile-bluetooth"
install -m755 "$source_dir/storage.py" "$HOME/.local/bin/omarchy-mobile-storage"
install -m755 "$source_dir/display.py" "$HOME/.local/bin/omarchy-mobile-displayinfo"
install -m755 "$source_dir/controls.py" "$HOME/.local/bin/omarchy-mobile-controls"
install -m755 "$source_dir/rotation.py" "$HOME/.local/bin/omarchy-mobile-rotation"
install -m755 "$source_dir/ambient.py" "$HOME/.local/bin/omarchy-mobile-ambient"
install -m755 "$source_dir/android-media.py" "$HOME/.local/bin/omarchy-mobile-android-media"
kit="$data/omarchy-mobile/qml/OmarchyMobile"
mkdir -p "$kit" "$config/quickshell/omarchy-mobile-settings" "$data/applications"
install -m644 "$source_dir/MobileTheme.qml" "$source_dir/TouchButton.qml" \
    "$source_dir/TouchTextField.qml" "$source_dir/DetailRow.qml" "$source_dir/Avatar.qml" "$kit/"
install -m644 "$source_dir/kit/"*.qml "$source_dir/kit/qmldir" "$kit/"
# Home screen widgets (widgets/README.md); apps may install more beside these.
for widget in "$source_dir/widgets"/*/; do
    [[ -f $widget/manifest.json && -f $widget/Widget.qml ]] || continue
    install -Dm644 -t "$data/omarchy-mobile/widgets/$(basename "$widget")" "$widget"*
done
install -m644 "$source_dir/settings/shell.qml" "$config/quickshell/omarchy-mobile-settings/"
install -m644 "$source_dir/settings/omarchy-mobile-settings.desktop" "$data/applications/"
if [[ -d $source_dir/apps ]]; then
    for app in "$source_dir/apps"/*; do
        [[ -d $app && -f $app/manifest.json && -f $app/shell.qml ]] || continue
        id=$(basename "$app")
        mkdir -p "$data/omarchy-mobile/apps/$id" "$config/quickshell/omarchy-mobile-apps/$id"
        install -m644 "$app/manifest.json" "$data/omarchy-mobile/apps/$id/manifest.json"
        install -m644 "$app/shell.qml" "$config/quickshell/omarchy-mobile-apps/$id/shell.qml"
        # An app's own JavaScript libraries, beside its window.
        for script in "$app"/*.js; do
            [[ -f $script ]] || continue
            install -m644 "$script" "$config/quickshell/omarchy-mobile-apps/$id/"
        done
        for desktop in "$app"/*.desktop; do
            [[ -f $desktop ]] || continue
            install -m644 "$desktop" "$data/applications/"
        done
        # An app's own icon (one the icon theme does not have).
        for icon in "$app"/*.svg; do
            [[ -f $icon ]] || continue
            install -Dm644 "$icon" "$data/icons/hicolor/scalable/apps/$(basename "$icon")"
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
    # The cameras as webcams for the browser and Android apps
    # (camera-bridge/), on the device's v4l2loopback nodes.
    bridge_build="${XDG_CACHE_HOME:-$HOME/.cache}/omarchy-mobile/camera-bridge-build"
    nice -n 10 cmake -S "$source_dir/camera-bridge" -B "$bridge_build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    nice -n 10 cmake --build "$bridge_build" -j2 >/dev/null
    install -m755 "$bridge_build/omarchy-camera-bridge" "$HOME/.local/bin/omarchy-camera-bridge"
else
    echo "Skipping the camera engine: it needs cmake, ninja and libcamera." >&2
fi
# The keyboard (keyboard/): its Quickshell window, the key sender and swipe
# decoder built here (Wayland and xkbcommon), and the word list it decodes
# against: FrequencyWords' 50 000 most frequent English words (OpenSubtitles
# 2018, CC BY-SA 4.0, github.com/hermitdave/FrequencyWords), fetched once and
# checked. keyboard.sh falls back to wvkbd until both are in place.
mkdir -p "$config/quickshell/omarchy-mobile-keyboard" "$data/omarchy-mobile/keyboard"
install -m644 "$source_dir/keyboard/shell.qml" "$config/quickshell/omarchy-mobile-keyboard/"
if command -v wayland-scanner >/dev/null && command -v cc >/dev/null; then
    keyboard_build="${XDG_CACHE_HOME:-$HOME/.cache}/omarchy-mobile/keyboard-build"
    mkdir -p "$keyboard_build"
    proto="$source_dir/keyboard/virtual-keyboard-unstable-v1.xml"
    wayland-scanner client-header "$proto" "$keyboard_build/virtual-keyboard-unstable-v1-client-protocol.h"
    wayland-scanner private-code "$proto" "$keyboard_build/virtual-keyboard-unstable-v1-protocol.c"
    nice -n 10 cc -O2 -I"$keyboard_build" -o "$keyboard_build/omarchy-mobile-vkbd" "$source_dir/keyboard/vkbd.c" \
        "$keyboard_build/virtual-keyboard-unstable-v1-protocol.c" -lwayland-client -lxkbcommon -lm
    install -m755 "$keyboard_build/omarchy-mobile-vkbd" "$HOME/.local/bin/"
else
    echo "Skipping the keyboard's key sender: it needs wayland-scanner and a C compiler." >&2
fi
words="$data/omarchy-mobile/keyboard/en.txt"
if [[ ! -f $words ]] && command -v curl >/dev/null; then
    curl -fsSL -o "$words.new" https://raw.githubusercontent.com/hermitdave/FrequencyWords/master/content/2018/en/en_50k.txt &&
        echo "5351ff405b1126ef555791dd4d9798a48e3e9a501a9fc481a9da957752cfb458  $words.new" | sha256sum -c --quiet &&
        mv "$words.new" "$words" || { rm -f "$words.new"; echo "Swipe typing needs $words." >&2; }
fi
# The browser (browser/): a small Qt WebEngine host with its QML interface,
# built here against the Qt it runs on (Arch: qt6-webengine).
if command -v cmake >/dev/null && command -v ninja >/dev/null && [[ -d /usr/lib/qt6/qml/QtWebEngine ]]; then
    browser_build="${XDG_CACHE_HOME:-$HOME/.cache}/omarchy-mobile/browser-build"
    nice -n 10 cmake -S "$source_dir/browser" -B "$browser_build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    nice -n 10 cmake --build "$browser_build" -j2 >/dev/null
    mkdir -p "$data/omarchy-mobile/browser"
    install -m644 "$source_dir/browser/Browser.qml" "$data/omarchy-mobile/browser/"
    install -m755 "$browser_build/omarchy-mobile-browser" "$HOME/.local/bin/omarchy-mobile-browser"
    install -m644 "$source_dir/browser/omarchy-mobile-browser.desktop" "$data/applications/"
    # X and Grok come as web apps, as the browser's Add to home screen makes
    # them (each wears the theme: browser/README.md). Each is added once:
    # removed in Settings → Apps, it stays removed until this runs again.
    mkdir -p "$data/omarchy-mobile/webapps"
    for app in "x-com|X|https://x.com/|x.png" "grok-com|Grok|https://grok.com/|grok.png"; do
        IFS='|' read -r slug name url icon <<< "$app"
        entry="$data/applications/omarchy-mobile-webapp-$slug.desktop"
        [[ -e $entry ]] && continue
        install -m644 "$source_dir/webapps/$icon" "$data/omarchy-mobile/webapps/$slug.png"
        printf '[Desktop Entry]\nType=Application\nName=%s\nExec=%s --app "%s"\nIcon=%s\nCategories=Network;WebBrowser;\nX-Omarchy-Mobile-WebApp=%s\n' \
            "$name" "$HOME/.local/bin/omarchy-mobile-browser" "$url" "$data/omarchy-mobile/webapps/$slug.png" "$url" > "$entry"
    done
else
    echo "Skipping the browser: it needs cmake, ninja and qt6-webengine." >&2
fi
# Phone flick physics for Qt apps (platformtheme/): Qt's generic theme with
# Android-like fling limits. It uses Qt's private API, so it is built here
# against the installed Qt, and again after a Qt update. The session selects
# it (QT_PLUGIN_PATH, QT_QPA_PLATFORMTHEME=omarchy-mobile) when it is present.
if command -v cmake >/dev/null && command -v ninja >/dev/null && [[ -d /usr/lib/cmake/Qt6GuiPrivate ]]; then
    theme_build="${XDG_CACHE_HOME:-$HOME/.cache}/omarchy-mobile/platformtheme-build"
    nice -n 10 cmake -S "$source_dir/platformtheme" -B "$theme_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DQT_NO_PRIVATE_MODULE_WARNING=ON >/dev/null
    nice -n 10 cmake --build "$theme_build" -j2 >/dev/null
    install -Dm755 "$theme_build/libomarchymobiletheme.so" \
        "$HOME/.local/lib/qt6/plugins/platformthemes/libomarchymobiletheme.so"
else
    echo "Skipping the flick physics theme: it needs cmake, ninja and Qt's private headers." >&2
fi
# The link to a paired computer (../link): the daemon the shell runs
# (LinkAdapter.qml). The computer's half installs with ../link/install.sh.
if command -v cmake >/dev/null && command -v ninja >/dev/null && [[ -f $source_dir/../link/CMakeLists.txt ]]; then
    link_build="${XDG_CACHE_HOME:-$HOME/.cache}/omarchy-mobile/link-build"
    nice -n 10 cmake -S "$source_dir/../link" -B "$link_build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    nice -n 10 cmake --build "$link_build" -j2 >/dev/null
    install -m755 "$link_build/omarchy-link" "$HOME/.local/bin/omarchy-link"
else
    echo "Skipping the computer link: it needs cmake, ninja and ../link." >&2
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
install -m755 "$source_dir/steps.py" "$HOME/.local/bin/omarchy-mobile-steps"
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
    if [[ -f "$device/hotspot-iface.sh" ]]; then
        install -m755 "$device/hotspot-iface.sh" "$config/omarchy-mobile/hotspot-iface"
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
