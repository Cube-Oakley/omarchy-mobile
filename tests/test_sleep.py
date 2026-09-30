"""Sleep policy decisions and the adapter protocol, without sleeping anything."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('sleep_policy', Path(__file__).resolve().parents[1] / 'overlay/mobile/sleep.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PolicyTests(unittest.TestCase):
    def test_sleeps_only_after_the_grace_period_of_darkness(self):
        policy = module.Policy()
        self.assertFalse(policy.ready(100, dark=True, busy=False))
        self.assertFalse(policy.ready(100 + module.GRACE - 1, dark=True, busy=False))
        self.assertTrue(policy.ready(100 + module.GRACE, dark=True, busy=False))

    def test_light_restarts_the_count(self):
        policy = module.Policy()
        policy.ready(100, dark=True, busy=False)
        policy.ready(105, dark=False, busy=False)
        self.assertFalse(policy.ready(100 + module.GRACE, dark=True, busy=False))

    def test_a_call_or_audio_keeps_it_awake(self):
        policy = module.Policy()
        policy.ready(100, dark=True, busy=True)
        self.assertFalse(policy.ready(200, dark=True, busy=True))
        self.assertTrue(policy.ready(201, dark=True, busy=False))

    def test_each_wake_holds_the_phone_up_for_its_kind(self):
        policy = module.Policy()
        policy.woke(1000, 'alarm')
        policy.ready(1000, dark=True, busy=False)
        self.assertFalse(policy.ready(1000 + module.HOLD['alarm'] - 1, dark=True, busy=False))
        self.assertTrue(policy.ready(1000 + module.HOLD['alarm'], dark=True, busy=False))
        policy.woke(2000, 'something-new')
        policy.ready(2000, dark=True, busy=False)
        self.assertTrue(policy.ready(2000 + max(module.HOLD_OTHER, module.GRACE), dark=True, busy=False))

    def test_a_refusal_waits_and_is_logged_once(self):
        policy = module.Policy()
        self.assertTrue(policy.refused(100, 'Unplug before sleeping'))
        self.assertFalse(policy.refused(100 + module.REFUSED_WAIT, 'Unplug before sleeping'))
        policy.ready(100, dark=True, busy=False)
        self.assertFalse(policy.ready(100 + module.REFUSED_WAIT, dark=True, busy=False))
        self.assertTrue(policy.ready(100 + 2 * module.REFUSED_WAIT, dark=True, busy=False))


class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = patch.dict(os.environ, {'XDG_RUNTIME_DIR': str(self.root), 'XDG_CONFIG_HOME': str(self.root),
                                           'XDG_STATE_HOME': str(self.root)})
        self.env.start()
        self.addCleanup(self.env.stop)

    def test_sleep_passes_the_background_interval_and_reads_the_wake(self):
        done = subprocess.CompletedProcess([], 0, 'noise\n{"woke": "modem", "slept": 812.4}\n', '')
        with patch.object(module.subprocess, 'run', return_value=done) as run:
            result = module.sleep_once(15)
        self.assertEqual(run.call_args.args[0][1:], ['--keep-dark', '--wake-after', '900'])
        self.assertEqual(result, {'woke': 'modem', 'slept': 812.4})

    def test_no_background_wake_when_turned_off(self):
        done = subprocess.CompletedProcess([], 0, '{"woke": "power-key", "slept": 3.0}\n', '')
        with patch.object(module.subprocess, 'run', return_value=done) as run:
            module.sleep_once(0)
        self.assertEqual(run.call_args.args[0][1:], ['--keep-dark'])

    def test_a_failed_sleep_is_reported(self):
        done = subprocess.CompletedProcess([], 1, '', 'A device failed to resume')
        with patch.object(module.subprocess, 'run', return_value=done):
            result = module.sleep_once(15)
        self.assertEqual(result['woke'], 'error')
        self.assertIn('failed to resume', result['error'])

    def test_calls_and_inhibitors_are_seen(self):
        self.assertFalse(module.in_call())
        phone = self.root / 'omarchy-mobile-phone'
        phone.mkdir()
        (phone / 'calls.json').write_text('{}')
        self.assertFalse(module.in_call())
        (phone / 'calls.json').write_text(json.dumps({'3': {'state': 'ringing-in'}}))
        self.assertTrue(module.in_call())
        self.assertEqual(module.inhibitors(), [])
        (self.root / 'omarchy-mobile-sleep-inhibit').mkdir()
        (self.root / 'omarchy-mobile-sleep-inhibit' / 'ssh').touch()
        self.assertEqual(module.inhibitors(), ['ssh'])

    def test_playing_audio_means_an_uncorked_stream(self):
        loopback = {'index': 3, 'corked': False, 'properties': {'node.virtual': 'true', 'media.role': 'Loopback'}}
        done = subprocess.CompletedProcess([], 0, json.dumps([loopback]), '')
        with patch.object(module.subprocess, 'run', return_value=done):
            self.assertFalse(module.playing_audio())
        streams = [{'index': 1, 'corked': True}, {'index': 2, 'corked': False}]
        done = subprocess.CompletedProcess([], 0, json.dumps(streams), '')
        with patch.object(module.subprocess, 'run', return_value=done):
            self.assertTrue(module.playing_audio())
        done = subprocess.CompletedProcess([], 0, json.dumps(streams[:1]), '')
        with patch.object(module.subprocess, 'run', return_value=done):
            self.assertFalse(module.playing_audio())


if __name__ == '__main__':
    unittest.main()
