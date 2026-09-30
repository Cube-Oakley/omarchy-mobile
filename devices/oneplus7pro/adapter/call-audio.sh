#!/usr/bin/env bash
# OnePlus 7 Pro call audio, the device adapter behind omarchy-mobile-phone:
#   call-audio start | stop | mute on|off | speaker on|off
# A call runs on the ADSP (q6voice VoiceMMode1): the upper amp in its stock
# receiver profile is the earpiece, the handset microphone (AMIC4 -> SLIM TX0,
# set up by guacamole-audio-start) is the mouthpiece. Speaker adds the lower
# loudspeaker. PipeWire lets go of the device for the call and gets it back
# afterwards. Exit 3: this boot has no call audio (voice-enabled unset).
set -euo pipefail
card=Pro
here=$(dirname "$(readlink -f "$0")")
hold="$here/call-audio-hold"
state=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/omarchy-mobile-call-audio
mkdir -p "$state"
log="$state/call-audio.log"
exec 9>"$state/lock"
flock 9

say() { printf '%s %s\n' "$(date +%T)" "$*" >> "$log"; }
set_control() { amixer -q -c "$card" cset name="$1" "$2"; }
voice_pcm() {
    python3 - "$card" <<'PY'
from pathlib import Path
import re, sys
for line in Path('/proc/asound/pcm').read_text().splitlines():
    match = re.match(r'(\d+)-(\d+): VoiceMMode1 ', line)
    if match and Path(f'/proc/asound/card{int(match[1])}/id').read_text().strip() == sys.argv[1]:
        print(f'hw:{sys.argv[1]},{int(match[2])}')
        break
PY
}
upper_idle() {
    local diag
    for diag in /sys/bus/i2c/devices/*-0034/diagnostics; do
        [[ -r $diag ]] && grep -q 'active=0' "$diag" && return 0
    done
    return 1
}
# The earpiece profile can change only while the upper amp is idle.
wait_upper_idle() {
    local i
    for i in $(seq 1 40); do upper_idle && return 0; sleep 0.05; done
    return 1
}
pipewire_devices() {
    local verb=$1 value=$2 name
    command -v pactl >/dev/null || return 0
    while read -r _ name _; do
        [[ $name == alsa_* ]] && pactl "$verb" "$name" "$value" 2>/dev/null || true
    done < <(pactl list short "${3}s" 2>/dev/null)
}
holder_running() {
    local pid
    pid=$(cat "$state/hold.pid" 2>/dev/null || true)
    [[ $pid =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null
}

start() {
    holder_running && return 0
    local pcm
    pcm=$(voice_pcm)
    if [[ -z $pcm ]]; then
        echo 'Call audio is not set up on this boot' >&2
        exit 3
    fi
    say "start on $pcm"
    pipewire_devices suspend-sink 1 sink
    pipewire_devices suspend-source 1 source
    if wait_upper_idle; then
        set_control 'Earpiece Mode' Receiver
    else
        say 'upper amp busy: staying in speaker profile'
    fi
    set_control 'Loudspeaker Switch' off
    set_control 'DEC0 Volume' 88
    # The default TX vocproc (single-mic ECNS, 0x10F71) needs calibration this
    # phone does not send; without it the uplink is silent, no RTP leaves the
    # modem and the network ends the call on an RTCP timeout after ~20 s.
    # NONE (0x10F70) passes the microphone straight to the encoder.
    set_control 'VoiceMMode1 TX Topology' 69488
    set_control 'QUAT_MI2S_RX Voice Mixer VoiceMMode1' 1
    set_control 'VoiceMMode1 Capture Mixer SLIMBUS_0_TX' 1
    : > "$state/hold.out"
    # 9>&-: the holder must not keep this script's lock, or stop would wait on it.
    setsid python3 "$hold" "$pcm" >> "$state/hold.out" 2>&1 < /dev/null 9>&- &
    echo $! > "$state/hold.pid"
    local i
    for i in $(seq 1 30); do
        grep -q VOICE_HOLD_RUNNING "$state/hold.out" && break
        holder_running || break
        sleep 0.1
    done
    cat "$state/hold.out" >> "$log"
    if ! grep -q VOICE_HOLD_RUNNING "$state/hold.out" || ! holder_running; then
        say 'holder did not start'
        stop_quiet
        echo "Call audio did not start: $(grep -v VOICE_HOLD "$state/hold.out" | tail -n 1)" >&2
        exit 1
    fi
    say 'running'
}

stop_quiet() {
    local pid i
    pid=$(cat "$state/hold.pid" 2>/dev/null || true)
    if [[ $pid =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        for i in $(seq 1 30); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
        kill -KILL "$pid" 2>/dev/null || true
    fi
    rm -f "$state/hold.pid"
    set_control 'QUAT_MI2S_RX Voice Mixer VoiceMMode1' 0 || true
    set_control 'VoiceMMode1 Capture Mixer SLIMBUS_0_TX' 0 || true
    set_control 'DEC0 Volume' 88 || true
    set_control 'Loudspeaker Switch' on || true
    # Media plays through both amps in the speaker profile again.
    wait_upper_idle && set_control 'Earpiece Mode' Speaker || say 'upper amp busy: left in receiver profile'
    pipewire_devices suspend-sink 0 sink
    pipewire_devices suspend-source 0 source
}

case ${1:-} in
    start) start ;;
    stop) say stop; stop_quiet ;;
    mute)
        [[ ${2:-} == on || ${2:-} == off ]] || { echo 'Usage: call-audio mute on|off' >&2; exit 2; }
        say "mute $2"
        set_control 'DEC0 Volume' "$([[ $2 == on ]] && echo 0 || echo 88)"
        ;;
    speaker)
        [[ ${2:-} == on || ${2:-} == off ]] || { echo 'Usage: call-audio speaker on|off' >&2; exit 2; }
        say "speaker $2"
        set_control 'Loudspeaker Switch' "$2"
        ;;
    *) echo 'Usage: call-audio start|stop|mute on|off|speaker on|off' >&2; exit 2 ;;
esac
