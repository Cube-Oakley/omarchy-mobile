#!/usr/bin/env bash
# Build omarchy-link and install it for this user.
#   install.sh            a computer: the daemon as a user service, and the
#                         Phone Link bar widget in the Omarchy shell
#   install.sh phone      the phone: the binary only (the mobile shell runs it)
set -euo pipefail

source_dir=$(cd "$(dirname "$0")" && pwd)
target=${1:-desktop}
[[ $target == desktop || $target == phone ]] || { echo "Usage: install.sh [desktop|phone]" >&2; exit 2; }
[[ $EUID -ne 0 ]] || { echo "install.sh: run as the user, not root" >&2; exit 1; }

build=${XDG_CACHE_HOME:-$HOME/.cache}/omarchy-link/build
cmake -S "$source_dir" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build"
install -Dm755 "$build/omarchy-link" "$HOME/.local/bin/omarchy-link"
[[ $target == desktop ]] || exit 0

config=${XDG_CONFIG_HOME:-$HOME/.config}
plugin=$config/omarchy/plugins/omarchy-mobile.link
install -Dm644 -t "$plugin" "$source_dir"/plugin/manifest.json "$source_dir"/plugin/Panel.qml "$source_dir"/plugin/LinkClient.qml
install -Dm644 "$source_dir/omarchy-link.service" "$config/systemd/user/omarchy-link.service"
systemctl --user daemon-reload
systemctl --user enable omarchy-link.service
systemctl --user restart omarchy-link.service

if command -v omarchy-shell >/dev/null; then
    omarchy-shell -q shell rescanPlugins
    if ! omarchy plugin list --json 2>/dev/null | jq -e '.[] | select(.id == "omarchy-mobile.link" and .enabled == true)' >/dev/null; then
        omarchy plugin enable omarchy-mobile.link right
    fi
fi

# A firewall must let the link's ports in from the local network: UDP 1816
# and TCP 1816-1864 (KDE Connect's scheme, 100 higher; settings.json's
# "kdeconnectPorts": true uses 1716 and 1716-1764 instead).
if command -v ufw >/dev/null && systemctl is-active -q ufw; then
    echo "If not done already: sudo ufw allow 1816/udp && sudo ufw allow 1816:1864/tcp"
fi
