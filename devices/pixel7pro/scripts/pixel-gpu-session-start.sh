#!/bin/bash
# Isolated hardware-rendered session in the native Pixel root.
set -euo pipefail
[[ $(stat -f -c %T /) == tmpfs || $(cat /etc/omarchy-mobile-pixel-root 2>/dev/null) == v1 ]]
case $(uname -r) in
    *-pixel-gpu15-*|*-pixel-display16-*|*-pixel-scanout17-*|*-pixel-panel18-*|*-pixel-panel19-*) ;;
    *) echo 'Expected a validated Pixel GPU kernel.' >&2; exit 1 ;;
esac
[[ -c /dev/dri/card0 && -c /dev/dri/renderD128 ]]
[[ -f /root/pixel-hyprland.lua && -x /root/pixel-gpu-render-test ]]
[[ -f /root/pixel-desktop-prepare.sh ]]
if pgrep -x Hyprland >/dev/null; then
    echo 'Hyprland already running; refusing a second instance.' >&2
    exit 1
fi
export LD_LIBRARY_PATH=/opt/pixel-mesa/lib
export GBM_BACKENDS_PATH=/opt/pixel-mesa/lib/gbm
export __EGL_VENDOR_LIBRARY_FILENAMES=/opt/pixel-mesa/share/glvnd/egl_vendor.d/50_mesa.json
unset GBM_ALWAYS_SOFTWARE LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE
# Refuse to start the desktop if the isolated build cannot execute a shader.
startup_timeout=20
# Initial UFS bring-up uses PWM gear 1; cold library reads need more time.
[[ ! -f /etc/omarchy-mobile-pixel-root ]] || startup_timeout=180
timeout "$startup_timeout" /root/pixel-gpu-render-test /dev/dri/card0
# The desktop's account (/etc/omarchy-mobile/session, "user=NAME"); without
# that file, root, as before the move to a user account
# (docs/user-session-plan-20261002.md). The system services stay root's.
user=root
if [[ -f /etc/omarchy-mobile/session ]]; then
    user=$(sed -n 's/^user=\([a-z_][a-z0-9_-]*\)$/\1/p' /etc/omarchy-mobile/session | head -1)
fi
id "${user:-root}" >/dev/null 2>&1 || user=root
uid=$(id -u "$user") gid=$(id -g "$user") home=$(getent passwd "$user" | cut -d: -f6)
runtime=/run/user/$uid
mkdir -p "$runtime" /run/udev
chown "$uid:$gid" "$runtime"
chmod 700 "$runtime"
# Programs of the session run as its account, with its own environment. A
# command prefix, not a function: a function run in the background is a
# subshell, and $! would not be the program's PID.
as_user=(env HOME="$home" XDG_RUNTIME_DIR="$runtime")
if (( uid != 0 )); then
    as_user=(setpriv --reuid="$uid" --regid="$gid" --init-groups
             env HOME="$home" USER="$user" LOGNAME="$user" SHELL=/bin/bash XDG_RUNTIME_DIR="$runtime")
fi
if ! pgrep -x systemd-udevd >/dev/null; then
    /usr/lib/systemd/systemd-udevd --daemon
fi
# The session's devices: display and input, and what the shell writes itself
# (the panel's brightness, the torch, the charge limit, libcamera's heap:
# session/70-omarchy-mobile.rules).
for subsystem in drm input backlight leds power_supply dma_heap sound video4linux media; do
    SYSTEMD_IN_CHROOT=0 udevadm trigger --subsystem-match=$subsystem --action=add
done
SYSTEMD_IN_CHROOT=0 udevadm settle --timeout=10
if ! pgrep -x seatd >/dev/null; then
    # The seat group may open the display and input (Hyprland as the user).
    nohup env SEATD_VTBOUND=0 seatd -g seat >/run/seatd.log 2>&1 </dev/null &
fi
for n in {1..30}; do [[ -S /run/seatd.sock ]] && break; sleep 0.1; done
[[ -S /run/seatd.sock ]]
pkill -x weston || true
for n in {1..30}; do pgrep -x weston >/dev/null || break; sleep 0.1; done
! pgrep -x weston >/dev/null
export XDG_RUNTIME_DIR=$runtime XDG_SESSION_TYPE=wayland
export XDG_CURRENT_DESKTOP=Hyprland LIBSEAT_BACKEND=seatd
export SEATD_SOCK=/run/seatd.sock AQ_DRM_DEVICES=/dev/dri/card0 AQ_NO_MODIFIERS=1
unset WAYLAND_DISPLAY DISPLAY HYPRLAND_INSTANCE_SIGNATURE
export HYPRLAND_STARTED_VIA_WRAPPER=1
ulimit -c 0
# Hyprland's configuration: the Pixel's base (root's copy, which the user
# cannot read) with the shell's generated mobile.lua added by session-prepare.
install -d -o "$uid" -g "$gid" "$home/.config/hypr" "$home/.config/omarchy-mobile"
grep -vxF 'dofile("/root/.config/hypr/mobile.lua")' /root/pixel-hyprland.lua > "$home/.config/hypr/pixel-hyprland.lua"
chown "$uid:$gid" "$home/.config/hypr/pixel-hyprland.lua"
install -o "$uid" -g "$gid" -m 700 /root/pixel-desktop-prepare.sh "$home/.config/omarchy-mobile/session-prepare"
stupid=()
(( uid != 0 )) || stupid=(--i-am-really-stupid)
"${as_user[@]}" nohup Hyprland "${stupid[@]}" -c "$home/.config/hypr/pixel-hyprland.lua" \
    >"$runtime/hyprland.log" 2>&1 </dev/null &
hypr_pid=$!
for ((n=0; n<startup_timeout*10; n++)); do
    kill -0 "$hypr_pid" || { cat "$runtime/hyprland.log" >&2; exit 1; }
    hypr_socket=$(hyprctl instances 2>/dev/null | awk -v wanted="$hypr_pid" \
        '$1 == "pid:" { pid=$2 } $1 == "wl" && pid == wanted { print $3 }')
    [[ -n "$hypr_socket" && -S "$XDG_RUNTIME_DIR/$hypr_socket" ]] && break
    sleep 0.1
done
[[ -n "$hypr_socket" && -S "$XDG_RUNTIME_DIR/$hypr_socket" ]]
export WAYLAND_DISPLAY="$hypr_socket" QT_QPA_PLATFORM=wayland QSG_RHI_BACKEND=opengl
# Phone flick physics for every Qt app (overlay/mobile/platformtheme), once
# install.sh has built the plugin; Qt's generic theme otherwise.
if [[ -f $home/.local/lib/qt6/plugins/platformthemes/libomarchymobiletheme.so ]]; then
    export QT_PLUGIN_PATH="$home/.local/lib/qt6/plugins" QT_QPA_PLATFORMTHEME=omarchy-mobile
fi
"${as_user[@]}" "$home/.local/bin/omarchy-mobile-session" launch
"${as_user[@]}" "$home/.local/bin/omarchy-mobile-keyboard" start
# No terminal at start any more (October 7): the bring-up's GPU check
# ("Pixel Mali-G710") was the first app of every boot, in front of the home
# screen, and the next app opened tiled beside it.
echo "HYPRLAND_PID=$hypr_pid WAYLAND_DISPLAY=$WAYLAND_DISPLAY USER=$user"
