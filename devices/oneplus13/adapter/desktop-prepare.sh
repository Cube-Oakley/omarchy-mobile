#!/bin/bash
# Native desktop adapter; requires the device-specific root marker on storage.
set -euo pipefail
[[ $(uname -r) == *-omarchy-oneplus13-probe* ]]
[[ $(tr -d '\0' </sys/firmware/devicetree/base/model) == 'OnePlus 13 CPH2655 Omarchy USB probe' ]]
case $(stat -f -c %T /) in
    tmpfs) ;;
    ext2/ext3) [[ $(cat /etc/omarchy-mobile-oneplus13-root) == oneplus13-cph2655-v1 ]] ;;
    *) exit 1 ;;
esac
config=/run/oneplus13-hyprland.lua
[[ -f $config ]]
line="dofile(\"$HOME/.config/hypr/mobile.lua\")"
if ! grep -qxF "$line" "$config"; then
    printf '\n%s\n' "$line" >> "$config"
fi
hyprctl reload
errors=$(hyprctl configerrors)
[[ -z ${errors//[[:space:]]/} ]] || { printf '%s\n' "$errors" >&2; exit 1; }
