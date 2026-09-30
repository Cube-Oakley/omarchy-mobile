# SPDX-License-Identifier: MIT
"""Temporary AoC call routes, restored when the bounded test ends.

Uses the existing ALSA voice endpoints (PCM4 playback, PCM11 capture).
AoC and CP exchange audio directly; no host recording or SRAM mapping.
Amplifier protection, calibration and gain remain under normal audio setup.
"""
import ctypes as C
import re
import subprocess


class VoiceAudio:
    def __init__(self, *, mic_boost_db=6):
        if mic_boost_db not in (0, 6):
            raise ValueError('unsupported voice microphone boost')
        self.mic_boost_db = mic_boost_db
        self.saved = []
        self.pcms = []
        self.alsa = C.CDLL('libasound.so.2')
        self.alsa.snd_pcm_open.argtypes = [C.POINTER(C.c_void_p), C.c_char_p, C.c_int, C.c_int]
        self.alsa.snd_pcm_set_params.argtypes = [C.c_void_p, C.c_int, C.c_int,
                                               C.c_uint, C.c_uint, C.c_int, C.c_uint]
        for name in ('snd_pcm_prepare', 'snd_pcm_start', 'snd_pcm_drop', 'snd_pcm_close', 'snd_pcm_state'):
            getattr(self.alsa, name).argtypes = [C.c_void_p]

    @staticmethod
    def mixer(action, name, value=None):
        args = ['amixer', '-c', '0', action, 'name='+name]
        if value is not None:
            args.append(value)
        result = subprocess.run(args, capture_output=True, text=True, timeout=10)
        if result.returncode:
            raise RuntimeError('voice mixer operation failed: '+name)
        return result.stdout

    @staticmethod
    def check(code, operation):
        if code < 0:
            raise RuntimeError('voice PCM '+operation+' failed: '+str(code))

    def __enter__(self):
        try:
            for name, value in (
                    ('Voice Call Audio Enable', '1'), ('Voice Call Mic Source', '1'),
                    ('Voice Call Mic Mute', '0'), ('Incall Mic Mute', '0'),
                    ('Incall Sink Mute', '0'), ('AoC Modem Downlink ASRC Mode', '1'),
                    # Stock gain is 130 cB; the user-confirmed normal-call
                    # default adds 6 dB, below the 240 cB hardware maximum.
                    ('MIC HW Gain At High Power Mode (cB)', str(130 + 10*self.mic_boost_db)),
                    ('MIC Record Soft Gain (dB)', '0'),
                    ('TDM_0_RX Mixer EP5', '1'), ('EP4 TX Mixer INTERNAL_MIC_TX', '1')):
                output = self.mixer('cget', name)
                match = re.search(r'^\s*: values=([0-9,\-]+|on|off)$', output, re.M)
                if match is None:
                    raise RuntimeError('voice mixer state unavailable: '+name)
                self.saved.append((name, match[1]))
                self.mixer('cset', name, value)
            # Have capture ready before playback prepare starts the modem.
            for device, direction in ((11, 1), (4, 0)):
                pcm = C.c_void_p()
                self.check(self.alsa.snd_pcm_open(C.byref(pcm), f'hw:0,{device}'.encode(), direction, 0), 'open')
                self.pcms.append(pcm)
                # S16_LE, RW_INTERLEAVED, 2ch/48kHz, 80ms, no resampling.
                self.check(self.alsa.snd_pcm_set_params(pcm, 2, 3, 2, 48000, 0, 80000), 'params')
                # set_params normally leaves ALSA PREPARED. Avoid sending
                # duplicate TELEPHONY_MODEM_START commands through prepare.
                if self.alsa.snd_pcm_state(pcm) != 2:
                    self.check(self.alsa.snd_pcm_prepare(pcm), 'prepare')
                self.check(self.alsa.snd_pcm_start(pcm), 'start')
            print('AoC voice playback and capture prepared', flush=True)
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *args):
        failures = []
        for pcm in reversed(self.pcms):
            self.alsa.snd_pcm_drop(pcm)
            if self.alsa.snd_pcm_close(pcm) < 0:
                failures.append('PCM close')
        self.pcms.clear()
        for name, value in reversed(self.saved):
            try:
                self.mixer('cset', name, value)
            except (OSError, RuntimeError, subprocess.TimeoutExpired):
                failures.append(name)
        self.saved.clear()
        if failures:
            raise RuntimeError('voice cleanup failed: '+', '.join(failures))
        print('AoC call routes restored', flush=True)
