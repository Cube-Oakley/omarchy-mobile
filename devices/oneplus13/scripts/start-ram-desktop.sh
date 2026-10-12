#!/bin/bash
# Start the desktop inside the verified OnePlus native Arch root.
set -euo pipefail
case "${1:-}" in
    '') gpu=0 ;;
    --gpu) gpu=1 ;;
    *) echo 'Usage: start-ram-desktop.sh [--gpu]' >&2; exit 2 ;;
esac
[[ $(uname -r) == *-omarchy-oneplus13-probe* ]]
[[ $(tr -d '\0' </sys/firmware/devicetree/base/model) == 'OnePlus 13 CPH2655 Omarchy USB probe' ]]
case $(stat -f -c %T /) in
    tmpfs) ;;
    ext2/ext3) [[ $(cat /etc/omarchy-mobile-oneplus13-root) == oneplus13-cph2655-v1 ]] ;;
    *) exit 1 ;;
esac
display_card=''
for node in /sys/class/drm/card*; do
    [[ ${node##*/} =~ ^card[0-9]+$ ]] || continue
    driver=$(basename "$(readlink -f "$node/device/driver")")
    if [[ $driver == oneplus13-scanout ]]; then
        display_card=/dev/dri/${node##*/}
        break
    elif [[ $driver == simple-framebuffer ]]; then
        display_card=/dev/dri/${node##*/}
    fi
done
[[ -n $display_card && -c $display_card ]]
! pgrep -x Hyprland >/dev/null
mkdir -p /run/user/0 /run/udev
chmod 700 /run/user/0
export XDG_RUNTIME_DIR=/run/user/0
export LIBSEAT_BACKEND=seatd SEATD_SOCK=/run/seatd.sock
if ! pgrep -x systemd-udevd >/dev/null; then
    /usr/lib/systemd/systemd-udevd --daemon
fi
SYSTEMD_IN_CHROOT=0 udevadm trigger --subsystem-match=drm --action=add
SYSTEMD_IN_CHROOT=0 udevadm trigger --subsystem-match=input --action=add
SYSTEMD_IN_CHROOT=0 udevadm settle --timeout=10
if ! pgrep -x seatd >/dev/null; then
    nohup env SEATD_VTBOUND=0 seatd >/run/seatd.log 2>&1 </dev/null &
fi
for n in {1..30}; do [[ -S /run/seatd.sock ]] && break; sleep 0.1; done
[[ -S /run/seatd.sock ]]
cat >/run/oneplus13-hyprland.lua <<'EOF'
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = 3 })
hl.config({
    general = { gaps_in = 0, gaps_out = 0, border_size = 1 },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
    animations = { enabled = false },
    cursor = { invisible = true, no_hardware_cursors = 1 },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true,
             disable_watchdog_warning = true },
})
EOF
export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=Hyprland
export AQ_DRM_DEVICES=$display_card AQ_NO_MODIFIERS=1
if (( gpu )); then
    [[ -c /dev/dri/renderD128 ]]
    [[ $(basename "$(readlink -f /sys/class/drm/renderD128/device/driver)") == adreno ]]
    [[ -r /opt/oneplus13-mesa/lib/libEGL_mesa.so.0 ]]
    export LD_LIBRARY_PATH=/opt/oneplus13-mesa/lib
    export GBM_BACKENDS_PATH=/opt/oneplus13-mesa/lib/gbm
    export __EGL_VENDOR_LIBRARY_FILENAMES=/opt/oneplus13-mesa/share/glvnd/egl_vendor.d/50_mesa.json
    unset GBM_ALWAYS_SOFTWARE LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER
else
    export GBM_ALWAYS_SOFTWARE=1 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
fi
unset MESA_LOADER_DRIVER_OVERRIDE WAYLAND_DISPLAY DISPLAY HYPRLAND_INSTANCE_SIGNATURE
export HYPRLAND_STARTED_VIA_WRAPPER=1
ulimit -c 0
nohup Hyprland --i-am-really-stupid -c /run/oneplus13-hyprland.lua \
    >/run/hyprland-oneplus13.log 2>&1 </dev/null &
pid=$!
for n in {1..150}; do
    kill -0 "$pid" || { cat /run/hyprland-oneplus13.log >&2; exit 1; }
    instances=$(hyprctl instances -j 2>/dev/null || echo '[]')
    if printf '%s' "$instances" | python3 -c 'import json,sys; sys.exit(not json.load(sys.stdin))' 2>/dev/null; then
        printf '%s\n' "$instances"
        exit 0
    fi
    sleep 0.2
done
echo 'Hyprland startup deadline exceeded; inspect /run/hyprland-oneplus13.log' >&2
exit 1
