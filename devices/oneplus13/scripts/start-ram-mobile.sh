#!/bin/bash
# Start the installed shared mobile shell after start-ram-desktop.sh.
set -euo pipefail
case "${1:-}" in
    '') gpu=0 ;;
    --gpu) gpu=1 ;;
    *) echo 'Usage: start-ram-mobile.sh [--gpu]' >&2; exit 2 ;;
esac
[[ $(uname -r) == *-omarchy-oneplus13-probe* ]]
[[ $(tr -d '\0' </sys/firmware/devicetree/base/model) == 'OnePlus 13 CPH2655 Omarchy USB probe' ]]
case $(stat -f -c %T /) in
    tmpfs) ;;
    ext2/ext3) [[ $(cat /etc/omarchy-mobile-oneplus13-root) == oneplus13-cph2655-v1 ]] ;;
    *) exit 1 ;;
esac
export XDG_RUNTIME_DIR=/run/user/0
read -r HYPRLAND_INSTANCE_SIGNATURE WAYLAND_DISPLAY < <(
    hyprctl instances -j | python3 -c '
import json, sys
items = json.load(sys.stdin)
if len(items) != 1:
    raise SystemExit("Expected exactly one native Hyprland instance")
print(items[0]["instance"], items[0]["wl_socket"])
')
export HYPRLAND_INSTANCE_SIGNATURE WAYLAND_DISPLAY
[[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]]
export QT_QPA_PLATFORM=wayland QSG_RHI_BACKEND=opengl
if (( gpu )); then
    [[ -c /dev/dri/renderD128 ]]
    [[ -r /opt/oneplus13-mesa/lib/libEGL_mesa.so.0 ]]
    export LD_LIBRARY_PATH=/opt/oneplus13-mesa/lib
    export GBM_BACKENDS_PATH=/opt/oneplus13-mesa/lib/gbm
    export __EGL_VENDOR_LIBRARY_FILENAMES=/opt/oneplus13-mesa/share/glvnd/egl_vendor.d/50_mesa.json
    unset GBM_ALWAYS_SOFTWARE LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER
else
    export GBM_ALWAYS_SOFTWARE=1 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
fi
unset MESA_LOADER_DRIVER_OVERRIDE
"$HOME/.config/omarchy-mobile/session-prepare"
"$HOME/.local/bin/omarchy-mobile-session" launch
