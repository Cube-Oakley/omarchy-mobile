#!/bin/sh
# Explicit RAM-only start of the verified CPH2655 ADSP and battery read profile.
set -eu
case "$(uname -r)" in *-omarchy-oneplus13-probe*) ;; *) exit 1;; esac
[ "$(tr -d '\000' </sys/firmware/devicetree/base/model)" = 'OnePlus 13 CPH2655 Omarchy USB probe' ]
[ "$(stat -f -c %T /)" = tmpfs ]
[ -r /lib/firmware/qcom/oneplus13/SHA256SUMS ]
sha256sum -c /lib/firmware/qcom/oneplus13/SHA256SUMS
load() {
    name=$(printf '%s' "$1" | tr '-' '_')
    if ! grep -q "^$name " /proc/modules; then insmod "/$1.ko"; fi
}
for module in qrtr qmi_helpers qcom_pdr_msg pdr_interface qcom_glink_smem \
              qrtr-smd qcom_sysmon qcom_q6v5 qcom_common qcom_pil_info \
              qcom_pd_mapper qcom_q6v5_pas; do
    load "$module"
done
for n in 1 2 3 4 5 6 7 8 9 10; do
    [ "$(cat /sys/class/remoteproc/remoteproc0/state 2>/dev/null)" = running ] && break
    sleep 1
done
[ "$(cat /sys/class/remoteproc/remoteproc0/name)" = adsp ]
case "$(readlink -f /sys/class/remoteproc/remoteproc0)" in
    */6800000.remoteproc/remoteproc/remoteproc0) ;;
    *) exit 1 ;;
esac
[ "$(cat /sys/class/remoteproc/remoteproc0/state)" = running ]
load pmic_glink
load qcom_battmgr
echo 'ADSP running; battery telemetry still requires validation.'
