#!/usr/bin/env bash
# On the phone, as root: install the modules from pack-modules.sh and keep
# Waydroid's container away from hardware that only our drivers should drive.
#   install.sh waydroid-modules-RELEASE.tar.gz
# Safe to run again. Undo: rm -r /lib/modules/RELEASE, and reinstall the
# waydroid package to restore lxc.py.
set -euo pipefail
tarball="$1"
release="$(uname -r)"
case "$(basename "$tarball")" in
    "waydroid-modules-$release.tar.gz") ;;
    *) echo "$tarball is not for the running kernel ($release)" >&2; exit 1 ;;
esac

mkdir -p /lib/modules
tar xzf "$tarball" -C /lib/modules
depmod -a "$release"

# lxc.py binds every /dev/video* (pixel-camera's four lenses), /dev/fb* and
# /sys/kernel/debug into the container. Android has no driver for our camera
# nodes, and the phone's debug files can reach registers that hang the SoC
# (see docs on bus hangs), so leave all three out.
lxc=/usr/lib/waydroid/tools/helpers/lxc.py
python3 - "$lxc" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
cuts = [
    '    for n in glob.glob("/dev/fb*"):\n        make_entry(n)\n',
    '    for n in glob.glob("/dev/video*"):\n        make_entry(n)\n',
    '    make_entry("/sys/kernel/debug", options="rbind,create=dir,optional 0 0")\n',
]
changed = False
for cut in cuts:
    if cut in s:
        s = s.replace(cut, "".join("    # omarchy-mobile: " + l.lstrip() + "\n"
                                   for l in cut.splitlines()), 1)
        changed = True
if changed:
    open(p, "w").write(s)
print("lxc.py patched" if changed else "lxc.py already patched")
EOF
# Android gets the camera bridge's two webcams (overlay/mobile/camera-bridge,
# v4l2loopback /dev/video20 and 21) for its external camera HAL, and only
# those: the container manager also opened every /dev/video* to everyone
# (mode 777) at each start, the raw sensors and the video decoder included.
python3 - "$lxc" /usr/lib/waydroid/tools/actions/container_manager.py <<'EOF'
import sys
lxc, manager = sys.argv[1], sys.argv[2]
s = open(lxc).read()
marker = "    # omarchy-mobile: the camera bridge's webcams\n"
if marker not in s:
    anchor = '    for n in glob.glob("/dev/dma_heap/*"):\n'
    s = s.replace(anchor, marker + '    for n in ("/dev/video20", "/dev/video21"):\n'
                  '        make_entry(n)\n' + anchor, 1)
    open(lxc, "w").write(s)
s = open(manager).read()
old = '        perm_list.extend(glob.glob("/dev/video*"))\n'
if old in s:
    s = s.replace(old, '        perm_list.extend(glob.glob("/dev/video2[01]"))  # omarchy-mobile: the webcams only\n', 1)
    open(manager, "w").write(s)
print("webcams passed to Android")
EOF

# A session started by `waydroid app launch` keeps that launch as its
# "Android is ready" callback and ran it at every ready, so each time
# Android restarted (a crash) the first app opened again by itself, over
# whatever was in front. Only the first ready launches it now.
users=/usr/lib/waydroid/tools/services/user_manager.py
python3 - "$users" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = "        if unlocked_cb:\n            unlocked_cb()\n"
new = ("        # omarchy-mobile: the launch that started the session runs once.\n"
       "        nonlocal unlocked_cb\n"
       "        if unlocked_cb:\n"
       "            callback, unlocked_cb = unlocked_cb, None\n"
       "            callback()\n")
if old in s:
    open(p, "w").write(s.replace(old, new, 1))
    print("user_manager.py patched")
elif new in s:
    print("user_manager.py already patched")
else:
    sys.exit("user_manager.py: the ready callback was not found; check the Waydroid version")
EOF

# Android asks to suspend once none of its windows show, and Waydroid then
# freezes the container (or stops the session with suspend_action = stop),
# so messaging apps receive nothing in the background. suspend_action = none
# leaves Android running: it dozes by itself, and its notifications reach
# the shell through Waydroid's notification bridge.
hardware=/usr/lib/waydroid/tools/services/hardware_manager.py
python3 - "$hardware" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = ('        if cfg["waydroid"]["suspend_action"] == "stop":\n'
       '            tools.actions.session_manager.stop(args)\n')
new = ('        # omarchy-mobile: "none" keeps Android running (background apps).\n'
       '        if cfg["waydroid"]["suspend_action"] == "none":\n'
       '            return\n' + old)
if new in s:
    print("hardware_manager.py already patched")
elif old in s:
    open(p, "w").write(s.replace(old, new, 1))
    print("hardware_manager.py patched")
else:
    sys.exit("hardware_manager.py: the suspend action was not found; check the Waydroid version")
EOF

# In single-window mode (waydroid-container.sh) an app launch sets Android's
# policy to hide only the status bar, so Android's navigation bar (a back
# arrow) took the bottom of every app, under the shell's own gestures. Hide
# both, as in multi-window mode.
apps=/usr/lib/waydroid/tools/actions/app_manager.py
python3 - "$apps" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = '"policy_control", "immersive.status=*")'
new = '"policy_control", "immersive.full=*")  # omarchy-mobile'
if old in s:
    open(p, "w").write(s.replace(old, new))
    print("app_manager.py patched")
elif new in s:
    print("app_manager.py already patched")
else:
    sys.exit("app_manager.py: the immersive policy was not found; check the Waydroid version")
EOF
sync
modprobe -a bridge veth overlay fuse
echo "modules installed for $release"
