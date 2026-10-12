#!/bin/bash
# Optional vibration motor (PMIH0108 haptics, FF_RUMBLE); run in the Arch root.
set -euo pipefail
[[ $(cat /etc/omarchy-mobile-oneplus13-root) == oneplus13-cph2655-v1 ]]
[[ $(tr -d '\0' </sys/firmware/devicetree/base/model) == 'OnePlus 13 CPH2655 Omarchy USB probe' ]]
base=/opt/oneplus13-haptics
[[ $(cat "$base/KERNELRELEASE") == "$(uname -r)" ]]
(cd "$base" && sha256sum --quiet -c SHA256SUMS)
[[ -d /sys/module/ff_memless ]] || insmod "$base/ff-memless.ko"
[[ -d /sys/module/oneplus13_haptics ]] || insmod "$base/oneplus13-haptics.ko"
grep -qx 'N: Name="pmih0108-haptics"' /proc/bus/input/devices
echo 'Native vibration motor ready'
