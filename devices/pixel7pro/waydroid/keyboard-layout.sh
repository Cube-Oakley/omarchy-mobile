#!/usr/bin/env bash
# On the phone, as root, with the Android session stopped: give Waydroid's
# keyboard (wayland_keyboard, vendor 1 product 1) Android's English (US)
# layout, so Android stops posting "Configure physical keyboard".
#   keyboard-layout.sh [english_us]
# Waydroid's hwcomposer names the layout from the first word of the XKB name,
# "English (US)" -> "english", and Android has no keyboard_layout_english, so
# it drops that layout at every boot. A layout already current for the device
# stays current. Android reads this file as text XML and writes it back as
# binary XML; the old file is kept beside it.
set -euo pipefail
layout="${1:-english_us}"
data=/home/cube/.local/share/waydroid/data/system
file="$data/input-manager-state.xml"
if lxc-info -P /var/lib/waydroid/lxc -n waydroid -sH 2>/dev/null | grep -qx RUNNING; then
    echo 'stop the Android session first (waydroid session stop, as cube)' >&2
    exit 1
fi
[ -f "$file" ] && cp -p "$file" "$file.before-layout"
cat > "$file.new" <<EOF
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<input-manager-state>
  <input-devices>
    <input-device descriptor="vendor:1,product:1">
      <keyboard-layout descriptor="com.android.inputdevices/com.android.inputdevices.InputDeviceReceiver/keyboard_layout_$layout" current="true" />
    </input-device>
  </input-devices>
</input-manager-state>
EOF
chown --reference="$data" "$file.new"
chmod 600 "$file.new"
mv "$file.new" "$file"
sync
echo "wayland_keyboard: keyboard_layout_$layout"
