import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('pixel_suspend',
    Path(__file__).resolve().parents[1] / 'devices/pixel7pro/scripts/pixel-suspend.py')
sleep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sleep)


class AlarmTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / 'since_epoch').write_text('1000')
        self.alarm = self.root / 'wakealarm'
        self.alarm.write_text('')
        self.override = patch.object(sleep, 'RTC', self.root)
        self.override.start()

    def tearDown(self):
        self.override.stop()
        self.temp.cleanup()

    def test_existing_earlier_alarm_is_untouched(self):
        self.alarm.write_text('1020')
        alarm = sleep.Alarm(60)
        self.assertEqual(self.alarm.read_text(), '1020')
        alarm.restore()
        self.assertEqual(self.alarm.read_text(), '1020')

    def test_later_alarm_is_restored_after_our_wake(self):
        self.alarm.write_text('1200')
        alarm = sleep.Alarm(60)
        self.assertEqual(self.alarm.read_text().strip(), '1060')
        self.alarm.write_text('')  # hardware fired our alarm
        (self.root / 'since_epoch').write_text('1060')
        alarm.restore()
        self.assertEqual(self.alarm.read_text().strip(), '1200')

    def test_new_alarm_from_another_owner_is_preserved(self):
        alarm = sleep.Alarm(60)
        self.alarm.write_text('1090')
        alarm.restore()
        self.assertEqual(self.alarm.read_text(), '1090')


class TransitionTests(unittest.TestCase):
    def test_live_call_or_audio_refuses_sleep(self):
        for calls, audio in (([dict(state='ringing-in')], False), ([], True)):
            with patch.object(sleep, 'request', return_value=dict(
                    modem=dict(ready=True), calls=calls, audio=dict(running=audio))):
                with self.assertRaises(RuntimeError):
                    sleep.idle_modem()

    def test_terminated_call_does_not_inhibit(self):
        with patch.object(sleep, 'request', return_value=dict(modem=dict(ready=True),
                calls=[dict(state='terminated')], audio=dict(running=False))):
            sleep.idle_modem()

    def test_pending_wake_restores_alarm_modem_and_display(self):
        with patch.object(sleep, 'prerequisites'), patch.object(sleep, 'display') as display, \
             patch.object(sleep, 'request') as request, patch.object(sleep.os, 'sync'), \
             patch.object(sleep, 'Alarm') as alarm, \
             patch.object(sleep, 'wake_count', side_effect=TimeoutError('active')):
            with self.assertRaises(TimeoutError):
                sleep.suspend(60, False, True)
            alarm.return_value.restore.assert_called_once()
            self.assertEqual([c.args for c in display.call_args_list], [(False,), (True,)])
            self.assertEqual([c.kwargs for c in request.call_args_list], [dict(on=False), dict(on=True)])

    def test_partial_modem_failure_attempts_foreground_restore(self):
        with patch.object(sleep, 'prerequisites'), patch.object(sleep, 'display'), \
             patch.object(sleep, 'request', side_effect=[TimeoutError(), {}]) as request:
            with self.assertRaises(TimeoutError):
                sleep.suspend(60, True, True)
            self.assertEqual([c.kwargs for c in request.call_args_list], [dict(on=False), dict(on=True)])


if __name__ == '__main__':
    unittest.main()
