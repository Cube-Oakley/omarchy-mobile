#!/bin/bash
# Executed inside the persistent Arch root by the recovery initramfs.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/bin HOME=/root USER=root LOGNAME=root
[[ $(cat /etc/omarchy-mobile-pixel-root) == v1 ]]
[[ $(stat -f -c %T /) == ext2/ext3 ]]
# The partitions with this phone's identity and calibration (IMEI, RF and
# sensor calibration, the modem's NV) and the modem firmware are read-only in
# the block layer from the start, so nothing here can write them by mistake.
# Reading still works (persist's calibration is read with debugfs).
for part in persist efs efs_backup modem_userdata devinfo mfg_data fips modem_a modem_b; do
    for uevent in /sys/class/block/*/uevent; do
        grep -qx "PARTNAME=$part" "$uevent" || continue
        part_dev=${uevent%/uevent}
        blockdev --setro "/dev/${part_dev##*/}" || echo "$part could not be set read-only." >&2
    done
done
# Clock floor until the RTC module loads below; it also guards against an RTC
# that reads earlier than the newest log.
clock_floor=$(stat -c %Y /etc/omarchy-mobile-pixel-root)
if [[ -f /var/log/pixel-native-boots.log ]]; then
    previous=$(stat -c %Y /var/log/pixel-native-boots.log)
    (( previous <= clock_floor )) || clock_floor=$previous
fi
if (( $(date +%s) < clock_floor )); then
    date -u -s "@$clock_floor" >/dev/null
fi
mkdir -p /run/sshd /run/omarchy-mobile
# Per-boot USB power/clock/PHY snapshot and kernel log, read-only: a normal
# (bootloader) boot lacks fastboot's running USB, so it is compared with a
# RAM boot's. Never stops the boot.
mkdir -p /var/log/pixel-boot-diag
boot=$(cat /proc/sys/kernel/random/boot_id)
# Preserve the previous kernel console before another restart replaces it.
# Logs may contain device identifiers: private directories/files, no deletion
# from pstore, and failure must not prevent telephony/desktop startup.
(
    umask 077
    mountpoint -q /sys/fs/pstore || mount -t pstore pstore /sys/fs/pstore || exit 0
    for record in /sys/fs/pstore/*; do
        [[ -f $record && ! -L $record ]] || continue
        savedir="/var/log/pixel-pstore/$boot"
        install -d -m 700 /var/log/pixel-pstore "$savedir"
        if [[ ! -e "$savedir/${record##*/}" ]]; then
            install -m 600 "$record" "$savedir/${record##*/}"
        fi
    done
) || echo 'Previous kernel console could not be archived.' >&2
/usr/local/sbin/pixel-usb-diag >"/var/log/pixel-boot-diag/$boot.usb" 2>&1 || true
dmesg >"/var/log/pixel-boot-diag/$boot.dmesg" 2>&1 || true
# Keep the newest 20 boots (about 130 KiB each).
ls -t /var/log/pixel-boot-diag/*.usb 2>/dev/null | tail -n +21 | while read -r old; do
    rm -f "$old" "${old%.usb}.dmesg"
done || true
sync
for n in {1..30}; do
    ip -4 addr show dev usb0 | grep -q '10.77.7.1/30' && break
    sleep 1
done
/usr/bin/sshd -t -f /etc/ssh/sshd_config.pixel
/usr/bin/sshd -f /etc/ssh/sshd_config.pixel -E /run/sshd-pixel.log
echo 1 >/sys/module/pixel_acpm/parameters/activate
sleep 1
# Battery-backed S2MPG12 RTC through ACPM (kernel/rtc); registering it sets the
# system clock (CONFIG_RTC_HCTOSYS). Never stops the boot.
if insmod /proc/1/root/lib/modules/pixel/pixel-rtc.ko; then
    if (( $(date +%s) < clock_floor )); then
        echo 'RTC reads earlier than the newest log; keeping the clock floor.' >&2
        date -u -s "@$clock_floor" >/dev/null
    fi
else
    echo 'RTC unavailable; keeping the clock floor.' >&2
fi
# CPU power-down idle (kernel/cpupm): the MCT becomes the tick broadcast
# device, so no CPU has to stay awake to run the broadcast. Without the
# firmware's CPU_INFORM hints every C2 entry is rejected and idle CPUs spin on
# SMCs (0.65 W). The hints include cluster power-down for the mid and big
# clusters.
# A root copy in /usr/local/lib/omarchy-mobile replaces the image's module
# without a new boot image; the image's copy remains the fallback.
pixel_module() {
    local name=$1 root=/usr/local/lib/omarchy-mobile/$1.ko image=/proc/1/root/lib/modules/pixel/$1.ko
    shift
    { [[ -f $root ]] && insmod "$root" "$@"; } || insmod "$image" "$@"
}
pixel_module pixel-mct ||
    echo 'MCT broadcast unavailable; one CPU keeps the hrtimer broadcast.' >&2
pixel_module pixel-cpupm ||
    echo 'C2 idle hints unavailable; CPUs keep spinning on rejected C2.' >&2
# The bootloader powers on the camera pipeline, TPU, codecs, G2D, EH and AUR;
# nothing here uses them (kernel/pd).
insmod /proc/1/root/lib/modules/pixel/pixel-pd-off.ko off=all ||
    echo 'Unused power domains left on.' >&2
# The thermal path must be active before enabling CPU scaling and the GPU.
zones=(/sys/class/thermal/thermal_zone*/temp)
[[ ${#zones[@]} -ge 7 ]]
for zone in "${zones[@]}"; do
    temp=$(cat "$zone")
    [[ $temp -gt 0 && $temp -lt 80000 ]]
done
# The CPU-scaling and GPU guards refuse 60 C or more. After a hot restart,
# such as a thermal reboot, wait up to five minutes for the chip to cool.
for n in {1..300}; do
    hot=0
    for zone in "${zones[@]}"; do
        (( $(cat "$zone") < 58000 )) || hot=1
    done
    (( hot )) || break
    (( n > 1 )) || echo 'Waiting for the chip to cool below 58 C.' >&2
    sleep 1
done
# pixel_acpm refuses CPU scaling (ERANGE) when ACPM reports unexpected boot
# rates. The CPUs then stay at the bootloader's rates, which is safe, so the
# desktop still starts.
cpufreq=1
if ! echo 1 >/sys/module/pixel_acpm/parameters/cpufreq; then
    echo 'CPU scaling was not started; keeping the boot rates.' >&2
    cpufreq=0
fi
echo 1 >/sys/module/pixel_gpu/parameters/domains
sleep 1
echo 1 >/sys/module/pixel_gpu/parameters/cycle
echo 1 >/sys/module/pixel_gpu/parameters/render
# GPU clock floor while rendering (the GPU still power-gates when idle): from
# 302 MHz, each animation missed frames until devfreq's first 50 ms poll
# raised the clock. pixel_gpu's thermal cap still overrides it.
echo 603000000 >/sys/class/devfreq/28000000.gpu/min_freq || echo 'GPU clock floor not set' >&2
if (( cpufreq )); then
    for policy in /sys/devices/system/cpu/cpufreq/policy*; do
        [[ -e $policy/scaling_governor ]] || continue
        echo schedutil >"$policy/scaling_governor" || echo "schedutil refused by $policy" >&2
    done
fi
# Power and volume keys on wake-up interrupts (kernel/keys); the polled power
# key (kernel/powerkey) remains the fallback.
insmod /proc/1/root/lib/modules/pixel/pixel-keys.ko ||
    insmod /proc/1/root/lib/modules/pixel/pixel-powerkey.ko
# Touch over the SPI0 controller with its attention interrupt; bit-banged GPIO
# polling is the fallback.
touch=/proc/1/root/lib/modules/pixel/pixel_touch_input.ko
if ! insmod "$touch" probe=1 seconds=0 hwspi=1 irq=1 &&
   ! insmod "$touch" probe=1 seconds=0; then
    echo 'Touch driver failed; starting the desktop with USB recovery available.' >&2
fi
# Battery state and charge control over hsi2c_13 (kernel/battery): lets a
# computer port supply 1.5 A so the phone charges while in use, and applies the
# charge limit chosen in the shell. Without it the charger keeps the
# bootloader's 500 mA input limit, and its 4.35 V float stops the cell at about
# 92 %. The root copy allows an update without rebuilding the recovery image.
battery=/usr/local/lib/omarchy-mobile/pixel-battery.ko
[[ -f $battery ]] || battery=/proc/1/root/lib/modules/pixel/pixel-battery.ko
if insmod "$battery" ||
   { [[ $battery != /proc/* ]] && insmod /proc/1/root/lib/modules/pixel/pixel-battery.ko; }; then
    XDG_STATE_HOME=/root/.local/state /root/.local/bin/omarchy-mobile-battery restore >/dev/null ||
        echo 'Charge limit not restored.' >&2
else
    echo 'Battery driver failed; the charger keeps its bootloader settings.' >&2
fi
# The battery driver owns the I2C adapter used by the Type-C data switch.
# The root copy allows an update without rebuilding the recovery image.
usb_switch=/usr/local/lib/omarchy-mobile/pixel-usb-switch.ko
[[ -f $usb_switch ]] || usb_switch=/proc/1/root/lib/modules/pixel/pixel-usb-switch.ko
if [[ -f $usb_switch ]]; then
    insmod "$usb_switch" ||
        echo 'USB cable reconnection helper unavailable.' >&2
fi
# Flashlight and vibration: hsi2c_15 and hsi2c_8 as the bootloader leaves them
# (kernel/i2c), then the LM3644 (kernel/torch) and the CS40L26 (kernel/haptics)
# on them. Never stops the boot.
if insmod /proc/1/root/lib/modules/pixel/pixel-hsi2c.ko buses=15,8; then
    # The read-only battery EEPROM selects the stock gauge profile. A gauge
    # with POR clear is preserved, including its learned state. The driver
    # requires stable USB power and a cool battery before any model writes.
    model_restore=/sys/bus/platform/devices/pixel-battery/restore_model
    if [[ -w $model_restore ]] && [[ $(cat /sys/class/power_supply/usb/online) == 1 ]]; then
        echo 1 > "$model_restore" || echo 'Battery model restoration deferred.' >&2
    fi
    insmod /proc/1/root/lib/modules/pixel/pixel-torch.ko || echo 'Flashlight unavailable.' >&2
    insmod /proc/1/root/lib/modules/pixel/pixel-haptics.ko || echo 'Vibration unavailable.' >&2
else
    echo 'Flashlight and vibration unavailable.' >&2
fi
/root/pixel-gpu-session-start.sh
# Wi-Fi after the desktop is up: the PCIe link (kernel/pcie), then the in-image
# brcmfmac stack, whose firmware is in the initramfs. The link enters L1
# substates once the firmware runs (l1ss); brcmfmac's deep sleep is on by
# default. With no watchdog, a PCIe fault would hang the SoC; the marker,
# synced before the link comes up, makes the next boot skip Wi-Fi instead of
# hanging again. Remove it to retry.
wifi_marker=/var/lib/omarchy-mobile/pixel-wifi-starting
start_wifi() {
    local modules=/proc/1/root/lib/modules/pixel module
    local wifi_driver=$modules/brcmfmac.ko
    # The opt-in modem/suspend bundle pins the matching Wi-Fi resume fix.
    # Validate it before loading any module from the persistent root.
    if [[ -f /var/lib/omarchy-mobile/modem/enabled ]]; then
        python3 /usr/local/lib/omarchy-mobile/modem/manager.py verify || return
        wifi_driver=/var/lib/omarchy-mobile/modem/brcmfmac.ko
    fi
    # brcmfmac request_module()s its firmware-vendor module from the
    # initramfs, which has no module tree; this helper insmods from it.
    echo /lib/modules/pixel/request-module >/proc/sys/kernel/modprobe
    insmod "$modules/pixel-pcie.ko" l1ss=1 || return
    for module in rfkill cfg80211 brcmutil; do
        insmod "$modules/$module.ko" || return
    done
    insmod "$wifi_driver" || return
    insmod "$modules/brcmfmac-wcc.ko" 2>/dev/null || [[ -d /sys/module/brcmfmac_wcc ]]
}
if [[ -e $wifi_marker ]]; then
    echo "Wi-Fi skipped: an earlier start did not finish ($wifi_marker)." >&2
else
    mkdir -p "${wifi_marker%/*}"
    : >"$wifi_marker"
    sync
    if start_wifi; then
        rm -f "$wifi_marker"
        sync
    else
        # A clean failure is not a hang: retry on the next boot.
        rm -f "$wifi_marker"
        sync
        echo 'Wi-Fi did not start.' >&2
    fi
fi
# NetworkManager (with wpa_supplicant, activated over the system bus) manages
# Wi-Fi; the USB link stays unmanaged (/etc/NetworkManager/conf.d/10-pixel.conf).
mkdir -p /run/dbus /run/systemd/resolve
dbus-daemon --system --fork || echo 'System bus did not start.' >&2
NetworkManager || echo 'NetworkManager did not start.' >&2
# Optional recovery access waits for Wi-Fi DHCP without delaying the desktop.
if [[ -f /etc/ssh/pixel-wifi-address ]]; then
    setsid -f sh -c 'exec /usr/local/sbin/pixel-wifi-ssh boot >>/run/pixel-wifi-ssh.log 2>&1 </dev/null'
fi
# Bluetooth (kernel/bluetooth): the BCM4389's Bluetooth core on UART18, which
# loads Google's patch firmware from the initramfs, then BlueZ. A UART fault
# cannot hang the SoC, so a failure only skips it.
start_bluetooth() {
    local modules=/proc/1/root/lib/modules/pixel module
    insmod "$modules/rfkill.ko" 2>/dev/null || [[ -d /sys/module/rfkill ]] || return
    for module in kpp ecc ecdh_generic bluetooth btbcm btqca pixel-bt hci_uart; do
        insmod "$modules/$module.ko" || return
    done
}
if start_bluetooth; then
    setsid -f sh -c 'exec /usr/lib/bluetooth/bluetoothd -n >>/run/bluetoothd.log 2>&1 </dev/null'
else
    echo 'Bluetooth did not start.' >&2
fi
# Audio (kernel/aoc, kernel/audio, kernel/gpio, kernel/spi): Google's AoC
# drivers have Trusty and the GSA load the audio DSP's firmware; the DSP runs
# the built-in microphones and the sensors (their supplies from
# pixel-aoc-power, kernel/aoc-power) and feeds the
# two CS35L41 speaker amplifiers on SPI7. The AoC ALSA service drivers and the
# amplifiers load before the firmware starts, so the card finds its codecs,
# and aoc_char_dev loads last so it does not claim the audio services. A start
# that never finished makes the next boot skip audio, as for Wi-Fi.
audio_marker=/var/lib/omarchy-mobile/pixel-audio-starting
aoc=/sys/devices/platform/19000000.aoc
aoc_online() { grep -q '^Services : [1-9]' "$aoc/services" 2>/dev/null; }
# The amplifiers' protection firmware (the stock tunings: top speaker on the
# left amplifier, bottom on the right) with the factory calibration, which
# persist:/audio/speaker.cal holds as CAL_R, CAL_STATUS, CAL_CHECKSUM and
# CAL_AMBIENT per amplifier. debugfs opens persist read-only; it is never
# mounted or written.
start_speaker_protection() {
    local dev cal=/run/pixel-speaker.cal amp p field word i
    dev=$(grep -l '^PARTNAME=persist$' /sys/class/block/*/uevent) || return
    dev=${dev%/uevent}
    debugfs -R 'cat /audio/speaker.cal' "/dev/${dev##*/}" >"$cal" 2>/dev/null
    (( $(stat -c %s "$cal") >= 48 )) || return
    for p in '' 'R '; do
        amixer -q -c0 cset name="${p}DSP1 Preload Switch" 1 || return
    done
    for i in {1..50}; do
        amixer -c0 cget name='R DSP1 Protection cd CAL_R' >/dev/null 2>&1 && break
        sleep 0.1
    done
    for amp in 0 1; do
        p=''
        (( amp )) && p='R '
        for field in CAL_R:0 CAL_STATUS:1 CAL_CHECKSUM:2 CAL_AMBIENT:3; do
            word=$(od -A n -t x4 -j $(( (amp * 6 + ${field#*:}) * 4 )) -N 4 "$cal" | tr -d ' ')
            amixer -q -c0 cset name="${p}DSP1 Protection cd ${field%:*}" \
                "0x${word:0:2},0x${word:2:2},0x${word:4:2},0x${word:6:2}" || return
        done
        amixer -q -c0 cset name="${p}PCM Source" DSP || return
        amixer -q -c0 cset name="${p}ASP TX1 Source" VMON
        amixer -q -c0 cset name="${p}ASP TX2 Source" IMON
        # The rest of the stock default path (mixer_paths.xml) for this
        # firmware: 17.5 dB of amplifier gain ("AMP PCM Gain" 17 there; the
        # reset value is 0.5 dB, 17 dB quieter), DRE, and both protection
        # inputs from the amplifier's own slot.
        amixer -q -c0 cset name="${p}Analog PCM Volume" 17 || return
        amixer -q -c0 cset name="${p}DRE Switch" on
        amixer -q -c0 cset name="${p}DSP RX1 Source" ASPRX1
        amixer -q -c0 cset name="${p}DSP RX2 Source" ASPRX1
    done
}
start_audio() {
    local modules=/proc/1/root/lib/modules/pixel module i
    for module in trusty-core trusty-log trusty-ipc trusty-virtio gsa mailbox-wc; do
        insmod "$modules/$module.ko" || return
    done
    insmod "$modules/aoc_core.ko" aoc_autoload_firmware=0 || return
    for module in soundcore snd snd-timer snd-pcm snd-compress snd-pcm-dmaengine \
                  snd-soc-core aoc_alsa_dev_util; do
        insmod "$modules/$module.ko" || return
    done
    insmod "$modules/pixel-aoc-power.ko" || echo 'Microphone and sensor supplies not switched on.' >&2
    # An enabled modem bundle supplies the shared GPIO provider before audio.
    # Otherwise retain the ordinary in-image audio-only provider.
    if [[ ! -d /sys/module/pixel_gpio ]]; then
        insmod "$modules/pixel-gpio.ko" || return
    fi
    for module in pixel-spi cs_dsp snd-soc-wm-adsp snd-soc-cs35l41-lib \
                  snd-soc-cs35l41 snd-soc-cs35l41-spi; do
        insmod "$modules/$module.ko" || { echo 'Speaker amplifiers unavailable.' >&2; break; }
    done
    insmod "$modules/aoc_alsa_dev.ko" || return
    # The AoC firmware is on this root, not in the initramfs (it does not fit
    # under the boot image's AVB boundary). The kernel resolves firmware paths
    # from PID 1's root, where this root is /run/arch; the directory holds
    # only aoc.bin, so no other firmware is looked up here.
    echo -n /run/arch/usr/lib/firmware/omarchy-mobile >/sys/module/firmware_class/parameters/path
    for i in {1..30}; do [[ -e $aoc/firmware ]] && break; sleep 1; done
    echo aoc.bin >"$aoc/firmware" || return
    for i in {1..20}; do aoc_online && break; sleep 1; done
    aoc_online || return
    # aoc_char_dev last: it takes every service no other driver claims by name
    # (aoc_channel_dev serves the sensor and context hub services).
    for module in aoc_channel_dev aoc_control_dev aoc_char_dev; do
        insmod "$modules/$module.ko" || return
    done
    for i in {1..20}; do [[ -e /proc/asound/card0 ]] && break; sleep 1; done
    [[ -e /proc/asound/card0 ]] || return
    [[ -d /sys/module/snd_soc_cs35l41_spi ]] || return 0
    if ! start_speaker_protection; then
        # Never play through the amplifiers unprotected.
        amixer -q -c0 cset name='Digital PCM Volume' 0
        amixer -q -c0 cset name='R Digital PCM Volume' 0
        echo 'Speaker protection did not start; speakers muted.' >&2
    fi
}
# Opt-in private modem installation. A failed preparation leaves audio and
# USB recovery available and a persistent marker prevents repeated starts.
modem_ready=0
modem_service=/usr/local/lib/omarchy-mobile/modem/manager.py
if [[ -f /var/lib/omarchy-mobile/modem/enabled && -f $modem_service ]]; then
    if python3 "$modem_service" prepare; then
        modem_ready=1
    else
        echo 'Modem preparation failed or skipped; inspect /run/pixel-modem.' >&2
    fi
fi
if [[ -e $audio_marker ]]; then
    echo "Audio skipped: an earlier start did not finish ($audio_marker)." >&2
else
    mkdir -p "${audio_marker%/*}"
    : >"$audio_marker"
    sync
    start_audio || echo 'Audio did not start.' >&2
    rm -f "$audio_marker"
    sync
fi
# Sensors (sensors/): the AoC runs them too. pixel-sensor-proxy serves
# iio-sensor-proxy's D-Bus API from the AoC's sensor service (USF) for the
# shell's rotation, automatic brightness and in-call proximity. On start it
# loads the AoC's sensor registry with this phone's factory calibration, read
# (never written) from persist. It needs the system bus and a running AoC; if
# it exits, it is started again after 30 s.
if aoc_online; then
    setsid -f sh -c 'while :; do
        python3 -u /usr/local/lib/omarchy-mobile/sensors/pixel-sensor-proxy.py
        sleep 30
    done >>/run/pixel-sensor-proxy.log 2>&1 </dev/null'
else
    echo 'Sensors unavailable: the AoC is not running.' >&2
fi
# The session's audio server, as root with the desktop (there are no systemd
# user services here). WirePlumber finds Bluetooth audio through BlueZ on the
# system bus and the built-in speakers and microphones through the AoC card's
# UCM profile (audio/ucm2).
if [[ -S /run/user/0/bus ]]; then
    for server in pipewire wireplumber pipewire-pulse; do
        setsid -f env XDG_RUNTIME_DIR=/run/user/0 \
            DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/0/bus \
            sh -c "exec $server >>/run/$server.log 2>&1 </dev/null"
        sleep 0.5
    done
else
    echo 'No session bus; audio server not started.' >&2
fi
if (( modem_ready )); then
    setsid -f sh -c 'umask 077; exec python3 -u /usr/local/lib/omarchy-mobile/modem/manager.py run >>/run/pixel-modem/manager.log 2>&1 </dev/null'
fi
date -u +%FT%TZ >>/var/log/pixel-native-boots.log
