#!/usr/bin/env bash
# Manual-only bring-up. Does not play, record or install startup hooks.
# "speakers" exposes the experimental output and microphone routes; no stream
# is started.
set -euo pipefail
BASE=/root/audio-bringup
[[ $(uname -r) == 6.17.0-sm8150-codex-native5-g379d8fe35c7c-dirty ]]
exec 9>/run/guacamole-audio-test.lock
flock -n 9
cd "$BASE"
case ${1:-status} in
    status) ;;
    start|speakers)
        sha256sum -c AUDIO-SHA256SUMS >/dev/null
        [[ -d /sys/module/qcom_q6v5_pas && -d /sys/module/qcom_pd_mapper ]]
        modem_running=0
        for r in /sys/class/remoteproc/*; do
            if [[ $(cat "$r/name") == modem && $(cat "$r/state") == running ]]; then modem_running=1; fi
        done
        [[ $modem_running == 1 ]]
        cp -an firmware/. /lib/firmware/qcom/sm8150/oneplus/guacamole/
        for file in firmware/*; do cmp "$file" "/lib/firmware/qcom/sm8150/oneplus/guacamole/${file##*/}"; done
        # Call audio is opt-in (voice-enabled). Its ADSP services must exist
        # before the ADSP starts, since APR lists services only then.
        voice=0
        if [[ $1 == speakers && -f voice-enabled && -f voice/guacamole_voice_services.ko ]]; then
            (cd voice && sha256sum -c SHA256SUMS >/dev/null) && voice=1
        fi
        if [[ $voice == 1 && ! -d /sys/module/guacamole_voice_services ]]; then
            if [[ -d /sys/module/guacamole_adsp_test ]] || ! insmod ./voice/guacamole_voice_services.ko; then
                echo 'Call audio skipped: the ADSP started before its voice services' >&2
                voice=0
            fi
        fi
        [[ -d /sys/module/guacamole_adsp_test ]] || insmod ./guacamole_adsp_test.ko
        adsp_running=0
        for attempt in $(seq 1 15); do
            for r in /sys/class/remoteproc/*; do
                if [[ $(cat "$r/name") == adsp && $(cat "$r/state") == running ]]; then adsp_running=1; fi
            done
            [[ $adsp_running == 1 ]] && break
            sleep 1
        done
        [[ $adsp_running == 1 ]]
        [[ -d /sys/module/guacamole_audio_card ]] || insmod ./guacamole_audio_card.ko
        if [[ $1 == speakers ]]; then
            [[ -d /sys/module/guacamole_amplifiers ]] || insmod ./guacamole_amplifiers.ko
            # Fresh boot only: do not alter topology under a registered card.
            [[ -d /sys/module/guacamole_speaker_route ]] || insmod ./guacamole_speaker_route.ko
            # Adds dai@1 to q6asmdai, so it must precede q6asm-dai's probe.
            [[ -d /sys/module/guacamole_microphone ]] || insmod ./guacamole_microphone.ko
            if [[ $voice == 1 ]]; then
                [[ -d /sys/module/guacamole_voice_link ]] || insmod ./voice/guacamole_voice_link.ko
            fi
        fi
        python3 phone-audio-modules.py
        [[ -r /proc/asound/cards ]] && grep -q 'OnePlus 7 Pro' /proc/asound/cards
        ;;
    *) echo 'Usage: phone-audio-test.sh start|speakers|status' >&2; exit 2 ;;
esac
for r in /sys/class/remoteproc/*; do printf '%s: ' "$(cat "$r/name")"; cat "$r/state"; done
cat /proc/asound/cards /proc/asound/pcm
cat /sys/kernel/debug/devices_deferred
