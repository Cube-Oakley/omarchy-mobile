#!/usr/bin/env python3
"""Hold the VoiceMMode1 front end open in both directions until SIGTERM.

q6voice starts the modem's voice session (11C05000) once both directions of
this PCM are open, and the ADSP carries the audio between the modem, the
earpiece amp and the microphone itself: no samples pass through here. The PCM
has no buffer, so it is never written or read. Preparing starts the AFE ports
and the earpiece amp; the codec enables its SLIMbus TX channel (the
microphone) only on a capture trigger, so capture is also started, which needs
no buffer. Playback cannot start without data and does not need to. Nothing
then waits on either stream. Closing on SIGTERM ends the session.

  call-audio-hold DEVICE     e.g. hw:Pro,5
"""
import ctypes
import signal
import sys

asound = ctypes.CDLL('libasound.so.2')
asound.snd_strerror.restype = ctypes.c_char_p
PLAYBACK, CAPTURE = 0, 1
ACCESS_RW_INTERLEAVED = 3
FORMAT_S16_LE = 2
RATE, CHANNELS = 8000, 1
# q6voice's dummy hardware: 2-4 periods of 2048-4096 bytes, at most 8 KiB.
PERIOD, BUFFER = 1024, 4096


def check(ret, what):
    if ret < 0:
        raise OSError(-ret, f'{what}: {asound.snd_strerror(ret).decode()}')


def configure(pcm):
    params = ctypes.c_void_p()
    check(asound.snd_pcm_hw_params_malloc(ctypes.byref(params)), 'hw params')
    try:
        check(asound.snd_pcm_hw_params_any(pcm, params), 'hw params')
        check(asound.snd_pcm_hw_params_set_access(pcm, params, ACCESS_RW_INTERLEAVED), 'access')
        check(asound.snd_pcm_hw_params_set_format(pcm, params, FORMAT_S16_LE), 'format')
        check(asound.snd_pcm_hw_params_set_channels(pcm, params, CHANNELS), 'channels')
        check(asound.snd_pcm_hw_params_set_rate(pcm, params, RATE, 0), 'rate')
        period = ctypes.c_ulong(PERIOD)
        check(asound.snd_pcm_hw_params_set_period_size_near(pcm, params, ctypes.byref(period), None), 'period')
        buffer = ctypes.c_ulong(BUFFER)
        check(asound.snd_pcm_hw_params_set_buffer_size_near(pcm, params, ctypes.byref(buffer)), 'buffer')
        check(asound.snd_pcm_hw_params(pcm, params), 'apply hw params')
        return buffer.value
    finally:
        asound.snd_pcm_hw_params_free(params)


def open_stream(device, stream):
    pcm = ctypes.c_void_p()
    check(asound.snd_pcm_open(ctypes.byref(pcm), device.encode(), stream, 0),
          f'open {"capture" if stream else "playback"}')
    return pcm


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    device = argv[1]
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(True))
    signal.signal(signal.SIGINT, lambda *_: stop.append(True))
    playback = open_stream(device, PLAYBACK)
    capture = None
    try:
        capture = open_stream(device, CAPTURE)
        configure(playback)
        configure(capture)
        check(asound.snd_pcm_prepare(playback), 'prepare playback')
        check(asound.snd_pcm_prepare(capture), 'prepare capture')
        check(asound.snd_pcm_start(capture), 'start capture')
        print(f'VOICE_HOLD_RUNNING {device}', flush=True)
        while not stop:
            signal.pause()
    finally:
        if capture:
            asound.snd_pcm_close(capture)
        asound.snd_pcm_close(playback)
    print('VOICE_HOLD_CLOSED', flush=True)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main(sys.argv))
    except OSError as error:
        print(f'call-audio-hold: {error.strerror}', file=sys.stderr)
        raise SystemExit(1)
