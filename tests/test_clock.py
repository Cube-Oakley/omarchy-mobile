import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1] / 'overlay/mobile'


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / (name + '.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def local(year, month, day, hour=0, minute=0, second=0):
    return time.mktime((year, month, day, hour, minute, second, 0, 0, -1))


class ClockTests(unittest.TestCase):
    def setUp(self):
        self.m = module('clock')
        self.tz = os.environ.get('TZ')
        os.environ['TZ'] = 'America/Los_Angeles'
        time.tzset()

    def tearDown(self):
        if self.tz is None:
            os.environ.pop('TZ', None)
        else:
            os.environ['TZ'] = self.tz
        time.tzset()

    def alarm(self, hour, minute, days=(), after=None, **extra):
        return dict({'id': 'a1', 'hour': hour, 'minute': minute, 'days': list(days), 'label': 'Up',
                     'vibrate': True, 'enabled': True, 'after': after}, **extra)

    def test_occurrence_today_tomorrow_and_weekdays(self):
        # Friday 2 October 2026, 06:00.
        now = local(2026, 10, 2, 6, 0)
        self.assertEqual(self.m.occurrence(self.alarm(7, 30), now), local(2026, 10, 2, 7, 30))
        self.assertEqual(self.m.occurrence(self.alarm(5, 0), now), local(2026, 10, 3, 5, 0))
        # Weekdays only: Friday 07:30, then Monday after that.
        weekdays = self.alarm(7, 30, days=range(5))
        self.assertEqual(self.m.occurrence(weekdays, now), local(2026, 10, 2, 7, 30))
        self.assertEqual(self.m.occurrence(weekdays, local(2026, 10, 2, 7, 30)), local(2026, 10, 5, 7, 30))

    def test_occurrence_across_summer_time(self):
        # US summer time ends 1 November 2026: 07:00 local stays 07:00.
        after = local(2026, 10, 31, 12, 0)
        at = self.m.occurrence(self.alarm(7, 0), after)
        self.assertEqual(time.localtime(at)[3:5], (7, 0))
        self.assertEqual(time.localtime(at)[2], 1)

    def test_alarm_rings_once_and_a_one_time_alarm_turns_off(self):
        state = self.m.blank()
        state['alarms'] = [self.alarm(7, 30, after=local(2026, 10, 2, 6, 0))]
        due = local(2026, 10, 2, 7, 30)
        self.assertEqual(self.m.next_wake(state, due - 60), due)
        changed, missed = self.m.advance(state, due + 1)
        self.assertTrue(changed)
        self.assertEqual(missed, [])
        self.assertEqual(len(state['ringing']), 1)
        self.assertEqual(state['ringing'][0]['kind'], 'alarm')
        self.assertFalse(state['alarms'][0]['enabled'])
        # Nothing new on the next pass.
        changed, _ = self.m.advance(state, due + 2)
        self.assertFalse(changed)
        self.assertEqual(len(state['ringing']), 1)

    def test_unanswered_alarm_stops_after_the_ring_window_as_missed(self):
        state = self.m.blank()
        state['alarms'] = [self.alarm(7, 30, days=range(7), after=local(2026, 10, 2, 6, 0))]
        due = local(2026, 10, 2, 7, 30)
        self.m.advance(state, due)
        self.assertEqual(self.m.next_wake(state, due + 5), due + 600)
        changed, missed = self.m.advance(state, due + 601)
        self.assertTrue(changed)
        self.assertEqual(state['ringing'], [])
        self.assertEqual(len(missed), 1)
        # A repeating alarm stays on, for tomorrow.
        self.assertTrue(state['alarms'][0]['enabled'])
        self.assertEqual(self.m.next_wake(state, due + 601), local(2026, 10, 3, 7, 30))

    def test_alarm_missed_while_off_for_days_is_reported_once(self):
        state = self.m.blank()
        state['alarms'] = [self.alarm(7, 30, days=range(7), after=local(2026, 9, 25, 6, 0))]
        now = local(2026, 10, 2, 12, 0)
        _, missed = self.m.advance(state, now)
        self.assertEqual(len(missed), 1)
        self.assertEqual(state['ringing'], [])
        _, missed = self.m.advance(state, now + 1)
        self.assertEqual(missed, [])
        self.assertEqual(self.m.next_wake(state, now + 1), local(2026, 10, 3, 7, 30))

    def test_snooze_and_dismiss(self):
        state = self.m.blank()
        state['alarms'] = [self.alarm(7, 30, days=range(7), after=local(2026, 10, 2, 6, 0))]
        due = local(2026, 10, 2, 7, 30)
        self.m.advance(state, due)
        key = state['ringing'][0]['key']
        self.m.apply(state, 'snooze', [key], '', due + 20)
        self.assertEqual(state['ringing'], [])
        self.assertEqual(self.m.next_wake(state, due + 20), due + 20 + 600)
        self.m.advance(state, due + 620)
        self.assertEqual(len(state['ringing']), 1)
        self.m.apply(state, 'dismiss', [state['ringing'][0]['key']], '', due + 630)
        self.assertEqual(state['ringing'], [])
        self.assertEqual(state['snoozes'], [])

    def test_timer_pause_resume_ring_and_add(self):
        state = self.m.blank()
        self.m.apply(state, 'timer-start', ['300', 'Tea'], '', 1000.0)
        timer = state['timers'][0]
        self.assertEqual(self.m.next_wake(state, 1000.0), 1300.0)
        self.m.apply(state, 'timer-pause', [timer['id']], '', 1100.0)
        self.assertEqual(timer['left'], 200.0)
        self.assertIsNone(self.m.next_wake(state, 1100.0))
        self.m.apply(state, 'timer-resume', [timer['id']], '', 2000.0)
        self.assertEqual(timer['end'], 2200.0)
        self.m.advance(state, 2200.0)
        self.assertEqual(state['ringing'][0]['key'], 'timer:' + timer['id'])
        self.m.apply(state, 'timer-add', [timer['id'], '60'], '', 2210.0)
        self.assertEqual(state['ringing'], [])
        self.assertEqual(timer['end'], 2270.0)
        self.m.advance(state, 2270.0)
        self.m.apply(state, 'dismiss', ['timer:' + timer['id']], '', 2280.0)
        self.assertEqual(timer['state'], 'stopped')
        self.assertEqual(timer['left'], 300)

    def test_stopwatch_laps(self):
        state = self.m.blank()
        self.m.apply(state, 'stopwatch', ['start'], '', 100.0)
        self.m.apply(state, 'stopwatch', ['lap'], '', 110.0)
        self.m.apply(state, 'stopwatch', ['pause'], '', 125.0)
        self.assertEqual(state['stopwatch']['elapsed'], 25.0)
        self.assertEqual(state['stopwatch']['laps'], [10.0])
        self.m.apply(state, 'stopwatch', ['start'], '', 200.0)
        self.m.apply(state, 'stopwatch', ['lap'], '', 205.0)
        self.assertEqual(state['stopwatch']['laps'], [10.0, 30.0])

    def test_alarm_save_validates_and_counts_from_now(self):
        state = self.m.blank()
        now = local(2026, 10, 2, 8, 0)
        self.m.apply(state, 'alarm-save', [], json.dumps({'hour': 7, 'minute': 0, 'days': []}), now)
        alarm = state['alarms'][0]
        self.assertEqual(alarm['after'], now)
        self.assertEqual(self.m.next_wake(state, now), local(2026, 10, 3, 7, 0))
        with self.assertRaises(ValueError):
            self.m.apply(state, 'alarm-save', [], json.dumps({'hour': 24, 'minute': 0}), now)
        with self.assertRaises(ValueError):
            self.m.apply(state, 'alarm-save', [], json.dumps({'hour': 7, 'minute': 0, 'days': [7]}), now)

    def test_next_due_from_the_store_and_commands(self):
        with tempfile.TemporaryDirectory() as d:
            env = {'XDG_DATA_HOME': d, 'XDG_RUNTIME_DIR': d}
            with patch.dict(os.environ, env):
                with patch('sys.stdin', io.StringIO(json.dumps({'hour': 23, 'minute': 59}))), \
                        patch('sys.stdout', io.StringIO()) as out:
                    self.assertEqual(self.m.main(['alarm-save']), 0)
                result = json.loads(out.getvalue())
                self.assertTrue(result['ok'])
                self.assertEqual(len(result['alarms']), 1)
                due = self.m.next_due()
                self.assertIsNotNone(due)
                self.assertEqual(time.localtime(due)[3:5], (23, 59))
                with patch('sys.stdout', io.StringIO()) as out:
                    self.assertEqual(self.m.main(['alarm-delete', 'nope']), 1)
                self.assertFalse(json.loads(out.getvalue())['ok'])

    def test_zones(self):
        rows = self.m.zone_rows(['Europe/London', 'Not/AZone'], now=local(2026, 7, 1, 12, 0))
        self.assertEqual([r['city'] for r in rows], ['London'])
        self.assertEqual(rows[0]['offset'], 3600)
        self.assertIn('Tokyo', [r['city'] for r in self.m.search_zones('tokyo')])


if __name__ == '__main__':
    unittest.main()
