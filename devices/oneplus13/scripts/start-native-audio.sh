#!/bin/bash
# Optional audio: AudioReach graphs on the running ADSP, WCD9395 microphones,
# AW882xx speaker amps, then the session's PipeWire. Run in the Arch root after
# the desktop starts. One attempt per boot; a failure leaves audio off.
set -euo pipefail
[[ $(cat /etc/omarchy-mobile-oneplus13-root) == oneplus13-cph2655-v1 ]]
[[ $(tr -d '\0' </sys/firmware/devicetree/base/model) == 'OnePlus 13 CPH2655 Omarchy USB probe' ]]
[[ $(tr -d '\0' </sys/firmware/devicetree/base/sound/compatible) == oneplus,cph2655-sndcard ]]
base=/opt/oneplus13-audio
[[ $(cat "$base/KERNELRELEASE") == "$(uname -r)" ]]
(cd "$base" && sha256sum --quiet -c SHA256SUMS)
[[ $(cat /sys/class/remoteproc/remoteproc0/state) == running ]]
mkdir /run/oneplus13-audio-started || {
    echo 'Audio startup already attempted this boot; inspect the log.' >&2
    exit 1
}
# The kernel's firmware loader sees the recovery root: expose the board
# topology and the amps' profile file from the verified package.
fw=/proc/1/root/lib/firmware
mkdir -p "$fw/qcom/sm8750"
for pair in qcom/sm8750/OnePlus-13-tplg.bin aw882xx_acf.bin; do
    target=/run/arch$base/firmware/$pair
    if [[ -e $fw/$pair || -L $fw/$pair ]]; then
        [[ $(readlink "$fw/$pair") == "$target" ]]
    else
        ln -s "$target" "$fw/$pair"
    fi
done
# A private module tree: udev in this root never autoloads these, so nothing
# here can race the separately packaged Wi-Fi and Bluetooth builds.
mp() { modprobe -d "$base/root" "$@"; }
mp pinctrl-sm8650-lpass-lpi
mp snd-soc-lpass-rx-macro
mp snd-soc-lpass-tx-macro
mp snd-soc-lpass-va-macro
mp snd-soc-lpass-wsa-macro
mp soundwire-qcom
mp snd-soc-wcd939x-sdw
mp snd-soc-wcd939x
mp i2c-qcom-geni
[[ ! -s $base/amp-module ]] || mp "$(cat "$base/amp-module")"
mp apr
mp q6apm-lpass-dais
mp q6apm-dai
mp q6prm-clocks
mp snd-soc-sc8280xp
mp snd-usb-audio
for n in {1..30}; do
    grep -q 'OnePlus-13' /proc/asound/cards 2>/dev/null && break
    sleep 1
done
grep -q 'OnePlus-13' /proc/asound/cards
echo 'Sound card ready'
