# SPDX-License-Identifier: MIT
import unittest
from voice_audio import VoiceAudio


class AudioCleanupTests(unittest.TestCase):
    def fixture(self, fail_params=False):
        class Alsa:
            opened = 0
            closed = []
            def snd_pcm_open(self, pointer, *args):
                self.opened += 1
                pointer._obj.value = self.opened
                return 0
            def snd_pcm_set_params(self, pcm, *args):
                return -5 if fail_params and pcm.value == 2 else 0
            def snd_pcm_prepare(self, pcm):
                return 0
            def snd_pcm_state(self, pcm):
                return 2
            def snd_pcm_start(self, pcm):
                return 0
            def snd_pcm_drop(self, pcm):
                return 0
            def snd_pcm_close(self, pcm):
                self.closed.append(pcm.value)
                return 0
        audio = VoiceAudio.__new__(VoiceAudio)
        audio.saved = []
        audio.pcms = []
        audio.mic_boost_db = 0
        audio.alsa = Alsa()
        changed = []
        def mixer(action, name, value=None):
            if action == 'cget':
                return '  : values=0\n'
            changed.append((name, value))
            return ''
        audio.mixer = mixer
        return audio, changed

    def test_partial_open_failure_closes_both_pcms_and_restores_all_routes(self):
        audio, changed = self.fixture(fail_params=True)
        with self.assertRaisesRegex(RuntimeError, 'params failed'):
            audio.__enter__()
        self.assertEqual(audio.alsa.closed, [2, 1])
        self.assertEqual(changed[10:], [(name, '0') for name, _ in reversed(changed[:10])])
        self.assertFalse(audio.saved)
        self.assertFalse(audio.pcms)

    def test_body_failure_still_restores_routes(self):
        audio, changed = self.fixture()
        with self.assertRaisesRegex(RuntimeError, 'call failed'):
            with audio:
                raise RuntimeError('call failed')
        self.assertEqual(audio.alsa.closed, [2, 1])
        self.assertEqual(len(changed), 20)

    def test_microphone_boost_is_bounded_and_restored(self):
        audio, changed = self.fixture()
        audio.mic_boost_db = 6
        with audio:
            self.assertIn(('MIC HW Gain At High Power Mode (cB)', '190'), changed)
        self.assertIn(('MIC HW Gain At High Power Mode (cB)', '0'), changed[10:])
        with self.assertRaises(ValueError):
            VoiceAudio(mic_boost_db=12)
