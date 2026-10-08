#!/bin/bash
# Development adapter for the verified Pixel native session. No hardware writes.
set -euo pipefail
[[ $(stat -f -c %T /) == tmpfs || $(cat /etc/omarchy-mobile-pixel-root 2>/dev/null) == v1 ]]
case $(uname -r) in
    *-pixel-drm11-*|*-pixel-gpu15-*|*-pixel-display16-*|*-pixel-scanout17-*|*-pixel-panel18-*|*-pixel-panel19-*) ;;
    *) echo 'Expected a validated Pixel desktop kernel.' >&2; exit 1 ;;
esac
# The session's own copy of the Pixel's base (pixel-gpu-session-start.sh
# writes it in the home of whichever account runs the desktop).
config=$HOME/.config/hypr/pixel-hyprland.lua
[[ -f $config ]]
line="dofile(\"$HOME/.config/hypr/mobile.lua\")"
if ! grep -qxF "$line" "$config"; then
    cp -a "$config" "$config.before-mobile"
    printf '\n%s\n' "$line" >> "$config"
fi
hyprctl reload
errors=$(hyprctl configerrors)
[[ -z ${errors//[[:space:]]/} ]] || { printf '%s\n' "$errors" >&2; exit 1; }
