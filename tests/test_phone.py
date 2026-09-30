import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import wave

ROOT = Path(__file__).resolve().parents[1] / 'overlay/mobile'


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeModem:
    """Just enough of mmcli: a call list, call details, create/start/delete."""

    def __init__(self):
        self.calls = {}
        self.log = []
        self.fail_start = False

    def set(self, ident, state, direction='incoming', number='+15035550100', reason='--'):
        self.calls[str(ident)] = {'state': state, 'direction': direction, 'number': number, 'state-reason': reason}

    def __call__(self, *args, timeout=15):
        self.log.append(args)
        args = list(args)
        if args[-1] == '-J':
            args = args[:-1]
        if args == ['-m', 'any', '--voice-list-calls']:
            paths = [f'/org/freedesktop/ModemManager1/Call/{i}' for i in self.calls]
            return json.dumps({'modem.voice.call': paths})
        if args[0] == '-o' and len(args) == 2:
            ident = args[1].rsplit('/', 1)[1]
            return json.dumps({'call': {'properties': dict(self.calls[ident], multiparty='no')}})
        if args[:2] == ['-m', 'any'] and args[2].startswith('--voice-create-call=number='):
            ident = str(len(self.calls) + 10)
            self.set(ident, '--', 'outgoing', args[2].split('=', 2)[2])
            return f'Successfully created new call: /org/freedesktop/ModemManager1/Call/{ident}\n'
        if args[0] == '-o' and args[2] == '--start':
            if self.fail_start:
                raise RuntimeError('Cannot start the call: not registered')
            self.calls[args[1].rsplit('/', 1)[1]]['state'] = 'dialing'
            return ''
        if args[:2] == ['-m', 'any'] and args[2].startswith('--voice-delete-call='):
            self.calls.pop(args[2].rsplit('/', 1)[1], None)
            return ''
        if args[0] == '-o' and args[2] in ('--hangup', '--accept') or args[2].startswith('--send-dtmf='):
            return ''
        raise AssertionError(f'unexpected mmcli {args}')


class PhoneTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        base = Path(self.temporary.name)
        self.saved = dict(os.environ)
        for name in ('state', 'data', 'config', 'cache', 'run', 'home'):
            (base / name).mkdir()
        os.environ.update(XDG_STATE_HOME=str(base / 'state'), XDG_DATA_HOME=str(base / 'data'),
                          XDG_CONFIG_HOME=str(base / 'config'), XDG_CACHE_HOME=str(base / 'cache'),
                          XDG_RUNTIME_DIR=str(base / 'run'), HOME=str(base / 'home'))
        os.environ.pop('OMARCHY_MOBILE_APP', None)
        app = base / 'data/omarchy-mobile/apps/phone'
        app.mkdir(parents=True)
        (app / 'manifest.json').write_text(json.dumps({'id': 'phone', 'name': 'Phone', 'permissions': ['phone']}))
        self.phone = load('phone')
        self.modem = FakeModem()
        self.patches = [patch.object(self.phone, 'mmcli', self.modem),
                        patch.object(self.phone, 'notify_missed')]
        for item in self.patches:
            item.start()

    def tearDown(self):
        for item in self.patches:
            item.stop()
        os.environ.clear()
        os.environ.update(self.saved)
        self.temporary.cleanup()

    def adapter(self):
        path = Path(os.environ['XDG_CONFIG_HOME']) / 'omarchy-mobile/call-audio'
        path.parent.mkdir(parents=True, exist_ok=True)
        log = path.parent / 'audio.log'
        path.write_text(f'#!/bin/sh\necho "$@" >> {log}\n')
        path.chmod(0o755)
        return log

    def step(self, recorder):
        current = self.phone.calls()
        return recorder.update(current)

    def test_numbers_are_formatted_like_the_app(self):
        self.assertEqual(self.phone.pretty('+15035550100'), '(503) 555-0100')
        self.assertEqual(self.phone.pretty('5035550100'), '(503) 555-0100')
        self.assertEqual(self.phone.pretty('+442079460123'), '+442079460123')
        self.assertEqual(self.phone.pretty('*611'), '*611')
        self.assertEqual(self.phone.pretty(''), 'Unknown number')

    def test_answered_call_is_recorded_with_audio_and_removed(self):
        log = self.adapter()
        recorder = self.phone.Recorder()
        self.modem.set(1, 'ringing-in')
        self.assertEqual(self.step(recorder), [])
        self.assertFalse(log.exists(), 'audio waits for the call to be answered')
        self.modem.set(1, 'active')
        self.step(recorder)
        self.assertTrue(self.phone.audio_state()['running'])
        recorder.tracked['1']['answered'] -= 42
        self.modem.set(1, 'terminated')
        added = self.step(recorder)
        self.assertEqual(len(added), 1)
        self.assertEqual(added[0]['result'], 'answered')
        self.assertEqual(added[0]['duration'], 42)
        self.assertNotIn('1', self.modem.calls, 'the finished call is deleted from ModemManager')
        self.assertEqual(log.read_text().split('\n')[:2], ['start', 'stop'])
        self.assertFalse(self.phone.audio_state()['running'])
        self.assertEqual(self.phone.load_history()[0]['result'], 'answered')

    def test_unanswered_call_is_missed_and_notified(self):
        recorder = self.phone.Recorder()
        self.modem.set(2, 'ringing-in')
        self.step(recorder)
        del self.modem.calls['2']
        added = self.step(recorder)
        self.assertEqual(added[0]['result'], 'missed')
        self.phone.notify_missed.assert_called_once()

    def test_missed_call_notification_arguments_follow_a_separator(self):
        # Without --, gdbus reads the -1 expiry as an option and posts nothing.
        phone = load('phone')
        with patch.object(phone.shutil, 'which', return_value='/usr/bin/gdbus'), \
                patch.object(phone.subprocess, 'run') as run:
            phone.notify_missed({'number': '+442079460123'})
        args = run.call_args.args[0]
        self.assertEqual(args[args.index('--method') + 2], '--')
        self.assertEqual(args[-1], '-1')

    def test_declined_call_is_not_missed(self):
        recorder = self.phone.Recorder()
        self.modem.set(3, 'ringing-in')
        self.step(recorder)
        with patch('sys.stdout', new_callable=io.StringIO):
            self.assertEqual(self.phone.main(['hangup', '3']), 0)
        self.modem.set(3, 'terminated')
        added = self.step(recorder)
        self.assertEqual(added[0]['result'], 'declined')
        self.phone.notify_missed.assert_not_called()

    def test_outgoing_calls_record_cancelled_and_failed(self):
        recorder = self.phone.Recorder()
        self.modem.set(4, 'ringing-out', 'outgoing')
        self.step(recorder)
        self.modem.set(4, 'terminated', 'outgoing')
        self.assertEqual(self.step(recorder)[0]['result'], 'cancelled')
        self.modem.set(5, 'dialing', 'outgoing')
        self.step(recorder)
        self.modem.set(5, 'terminated', 'outgoing', reason='refused-or-busy')
        self.assertEqual(self.step(recorder)[0]['result'], 'failed')

    def test_dial_cleans_number_and_deletes_a_call_that_fails(self):
        result = self.phone.dial('(503) 555-0100')
        self.assertEqual(result['number'], '5035550100')
        self.assertEqual(self.modem.calls[result['call']]['state'], 'dialing')
        self.modem.fail_start = True
        with self.assertRaises(RuntimeError):
            self.phone.dial('5035550101')
        self.assertEqual([c['number'] for c in self.modem.calls.values()], ['5035550100'])
        with self.assertRaises(ValueError):
            self.phone.dial('call me')

    def test_touch_tones_are_checked(self):
        self.modem.set(6, 'active')
        with patch('sys.stdout', new_callable=io.StringIO) as out:
            self.assertEqual(self.phone.main(['dtmf', '6', '12;rm']), 1)
        self.assertIn('Touch tones', out.getvalue())
        with patch('sys.stdout', new_callable=io.StringIO):
            self.assertEqual(self.phone.main(['dtmf', '6', '1#']), 0)
        self.assertIn(('-o', '/org/freedesktop/ModemManager1/Call/6', '--send-dtmf=#'), self.modem.log)

    def test_app_needs_the_phone_grant(self):
        os.environ['OMARCHY_MOBILE_APP'] = 'phone'
        with patch('sys.stdout', new_callable=io.StringIO) as out:
            self.assertEqual(self.phone.main(['status']), 1)
        self.assertIn('not granted', out.getvalue())
        self.phone.load_apps().grant('phone', 'phone')
        with patch('sys.stdout', new_callable=io.StringIO) as out, \
                patch.object(self.phone, 'modem', return_value={'present': True}):
            self.assertEqual(self.phone.main(['status']), 0)
        self.assertTrue(json.loads(out.getvalue())['ok'])

    def test_audio_controls_need_a_running_call(self):
        self.adapter()
        with self.assertRaises(RuntimeError):
            self.phone.audio_set('mute', 'on')
        self.phone.audio_start()
        self.assertTrue(self.phone.audio_set('speaker', 'on')['speaker'])
        with self.assertRaises(ValueError):
            self.phone.audio_set('volume', 'on')

    def test_ringtone_is_made_once(self):
        path = self.phone.ringtone()
        with wave.open(str(path)) as sound:
            self.assertEqual(sound.getframerate(), 48000)
            self.assertGreater(sound.getnframes() / 48000, 2)
        stamp = path.stat().st_mtime_ns
        self.assertEqual(self.phone.ringtone().stat().st_mtime_ns, stamp)


if __name__ == '__main__':
    unittest.main()
