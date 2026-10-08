import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from datetime import date, datetime
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1] / 'overlay/mobile'


def module():
    spec = importlib.util.spec_from_file_location('calendars', ROOT / 'calendars.py')
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def local(year, month, day, hour=0, minute=0):
    return time.mktime((year, month, day, hour, minute, 0, 0, 0, -1))


def ics(*events, zones=''):
    body = '\r\n'.join(['BEGIN:VCALENDAR', 'VERSION:2.0', 'PRODID:test'] + ([zones] if zones else [])
                       + list(events) + ['END:VCALENDAR'])
    return body + '\r\n'


def vevent(*lines):
    return '\r\n'.join(['BEGIN:VEVENT'] + list(lines) + ['END:VEVENT'])


class CalendarTests(unittest.TestCase):
    def setUp(self):
        self.m = module()
        self.tz = os.environ.get('TZ')
        os.environ['TZ'] = 'America/Los_Angeles'
        time.tzset()
        self.dir = tempfile.TemporaryDirectory()
        self.env = patch.dict(os.environ, {'XDG_DATA_HOME': self.dir.name, 'XDG_RUNTIME_DIR': self.dir.name,
                                           'HOME': self.dir.name})
        self.env.start()
        os.environ.pop('OMARCHY_MOBILE_APP', None)

    def tearDown(self):
        self.env.stop()
        self.dir.cleanup()
        if self.tz is None:
            os.environ.pop('TZ', None)
        else:
            os.environ['TZ'] = self.tz
        time.tzset()

    def put(self, name, text, calendar='personal'):
        self.m.calendar_dirs()
        path = self.m.calendars_root() / calendar / name
        path.write_text(text)
        return path

    def starts(self, low, high):
        return [time.localtime(r['start'])[:5] for r in self.m.occurrences(local(*low), local(*high))]

    def test_weekly_on_days_with_count(self):
        self.put('a.ics', ics(vevent('UID:a', 'SUMMARY:Gym', 'DTSTART;TZID=America/Los_Angeles:20261005T070000',
                                     'DTEND;TZID=America/Los_Angeles:20261005T080000',
                                     'RRULE:FREQ=WEEKLY;BYDAY=MO,WE;COUNT=3')))
        self.assertEqual(self.starts((2026, 10, 1), (2026, 11, 1)),
                         [(2026, 10, 5, 7, 0), (2026, 10, 7, 7, 0), (2026, 10, 12, 7, 0)])

    def test_weekly_keeps_wall_time_across_summer_time(self):
        self.put('a.ics', ics(vevent('UID:a', 'DTSTART;TZID=America/Los_Angeles:20261026T090000',
                                     'DTEND;TZID=America/Los_Angeles:20261026T093000', 'RRULE:FREQ=WEEKLY')))
        rows = self.m.occurrences(local(2026, 10, 25), local(2026, 11, 10))
        self.assertEqual([time.localtime(r['start'])[3:5] for r in rows], [(9, 0)] * 3)
        self.assertEqual([round((r['end'] - r['start']) / 60) for r in rows], [30] * 3)

    def test_monthly_ordinal_weekdays_and_month_days(self):
        self.put('a.ics', ics(vevent('UID:a', 'DTSTART:20261013T180000Z', 'RRULE:FREQ=MONTHLY;BYDAY=2TU')))
        self.put('b.ics', ics(vevent('UID:b', 'DTSTART;VALUE=DATE:20261030', 'RRULE:FREQ=MONTHLY;BYDAY=-1FR')))
        self.put('c.ics', ics(vevent('UID:c', 'DTSTART;VALUE=DATE:20261031', 'RRULE:FREQ=MONTHLY;BYMONTHDAY=-1')))
        rows = self.m.occurrences(local(2026, 11, 1), local(2026, 12, 1))
        days = sorted((r['uid'], time.localtime(r['start'])[2]) for r in rows)
        self.assertEqual(days, [('a', 10), ('b', 27), ('c', 30)])

    def test_monthly_on_the_31st_skips_short_months(self):
        self.put('a.ics', ics(vevent('UID:a', 'DTSTART;VALUE=DATE:20260131', 'RRULE:FREQ=MONTHLY')))
        rows = self.m.occurrences(local(2026, 1, 1), local(2026, 6, 1))
        self.assertEqual([time.localtime(r['start'])[1] for r in rows], [1, 3, 5])

    def test_yearly_birthday_and_thanksgiving(self):
        self.put('a.ics', ics(vevent('UID:a', 'DTSTART;VALUE=DATE:19900315', 'RRULE:FREQ=YEARLY')))
        self.put('b.ics', ics(vevent('UID:b', 'DTSTART;VALUE=DATE:20201126', 'RRULE:FREQ=YEARLY;BYMONTH=11;BYDAY=4TH')))
        rows = self.m.occurrences(local(2026, 1, 1), local(2027, 1, 1))
        self.assertEqual(sorted((r['uid'], time.localtime(r['start'])[1:3]) for r in rows),
                         [('a', (3, 15)), ('b', (11, 26))])
        self.assertTrue(all(r['allDay'] for r in rows))

    def test_until_exdate_and_a_changed_occurrence(self):
        self.put('a.ics', ics(
            vevent('UID:a', 'SUMMARY:Standup', 'DTSTART;TZID=America/Los_Angeles:20261005T090000',
                   'DURATION:PT15M', 'RRULE:FREQ=DAILY;UNTIL=20261009T160000Z',
                   'EXDATE;TZID=America/Los_Angeles:20261007T090000'),
            vevent('UID:a', 'SUMMARY:Standup (late)', 'RECURRENCE-ID;TZID=America/Los_Angeles:20261008T090000',
                   'DTSTART;TZID=America/Los_Angeles:20261008T110000',
                   'DTEND;TZID=America/Los_Angeles:20261008T111500')))
        rows = self.m.occurrences(local(2026, 10, 1), local(2026, 10, 31))
        self.assertEqual([(time.localtime(r['start'])[2:4], r['title']) for r in rows],
                         [((5, 9), 'Standup'), ((6, 9), 'Standup'), ((8, 11), 'Standup (late)'), ((9, 9), 'Standup')])

    def test_save_describe_round_trip_and_moves(self):
        self.m.calendar_save({'name': 'Work', 'color': '#9ece6a'})
        event = self.m.save({'title': 'Dentist, again', 'location': 'Main St; suite 2', 'notes': 'Bring\ncard',
                             'start': '2026-10-06T14:30', 'end': '2026-10-06T15:15',
                             'repeat': {'freq': 'weekly', 'interval': 2, 'days': [1, 3]}, 'reminders': [15, 60]})
        again = self.m.describe(*self.m.find_event(event['uid']))
        self.assertEqual((again['title'], again['location'], again['notes']), ('Dentist, again', 'Main St; suite 2', 'Bring\ncard'))
        self.assertEqual((again['start'], again['end'], again['tz']), ('2026-10-06T14:30', '2026-10-06T15:15', 'America/Los_Angeles'))
        self.assertEqual((again['repeat']['freq'], again['repeat']['interval'], again['repeat']['days']), ('weekly', 2, [1, 3]))
        self.assertFalse(again['repeat']['custom'])
        self.assertEqual(sorted(again['reminders']), [15, 60])
        moved = self.m.save(dict(again, calendar='work', repeat=None))
        self.assertEqual(moved['calendar'], 'work')
        self.assertIsNone(moved['repeat'])
        self.assertEqual(len(list(self.m.calendars_root().glob('*/*.ics'))), 1)

    def test_all_day_event_spans_its_days(self):
        event = self.m.save({'title': 'Trip', 'allDay': True, 'start': '2026-10-09', 'end': '2026-10-11'})
        rows = self.m.occurrences(local(2026, 10, 1), local(2026, 11, 1))
        self.assertEqual(len(rows), 1)
        self.assertEqual(round((rows[0]['end'] - rows[0]['start']) / 86400), 3)
        self.assertEqual(self.m.describe(*self.m.find_event(event['uid']))['end'], '2026-10-11')

    def test_unmodelled_properties_and_custom_rules_survive_an_edit(self):
        self.put('a.ics', ics(vevent('UID:a', 'SUMMARY:Board', 'ORGANIZER;CN="Chair, Board":mailto:chair@example.org',
                                     'DTSTART;TZID=America/New_York:20261001T100000',
                                     'DTEND;TZID=America/New_York:20261001T110000',
                                     'RRULE:FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1')))
        event = self.m.describe(*self.m.find_event('a'))
        self.assertTrue(event['repeat']['custom'])
        self.assertEqual(event['tz'], 'America/New_York')
        self.m.save(dict(event, title='Board meeting'))
        text = (self.m.calendars_root() / 'personal' / 'a.ics').read_text()
        self.assertIn('ORGANIZER;CN="Chair, Board":mailto:chair@example.org', text)
        self.assertIn('RRULE:FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1', text)
        self.assertIn('SUMMARY:Board meeting', text)

    def test_skip_one_occurrence(self):
        event = self.m.save({'title': 'Run', 'start': '2026-10-05T06:00', 'end': '2026-10-05T07:00',
                             'repeat': {'freq': 'daily'}})
        rows = self.m.occurrences(local(2026, 10, 5), local(2026, 10, 8))
        self.m.skip(event['uid'], int(rows[1]['original']))
        rows = self.m.occurrences(local(2026, 10, 5), local(2026, 10, 8))
        self.assertEqual([time.localtime(r['start'])[2] for r in rows], [5, 7])

    def test_reminders_for_timed_and_all_day_events(self):
        self.m.save({'title': 'Call', 'start': '2026-10-05T10:00', 'end': '2026-10-05T10:30', 'reminders': [10]})
        self.m.save({'title': 'Holiday', 'allDay': True, 'start': '2026-10-06', 'end': '2026-10-06',
                     'reminders': [-540, 900]})
        found = self.m.upcoming_reminders(local(2026, 10, 4), local(2026, 10, 7))
        self.assertEqual([(time.localtime(at)[2:4], row['title']) for at, row in found],
                         [((5, 9), 'Holiday'), ((5, 9), 'Call'), ((6, 9), 'Holiday')])

    def test_import_groups_changed_occurrences_and_replaces_by_uid(self):
        source = Path(self.dir.name) / 'export.ics'
        source.write_text(ics(vevent('UID:x', 'SUMMARY:One', 'DTSTART:20261005T170000Z'),
                              vevent('UID:y', 'SUMMARY:Two', 'DTSTART:20261006T170000Z', 'RRULE:FREQ=DAILY;COUNT=3'),
                              vevent('UID:y', 'SUMMARY:Two moved', 'RECURRENCE-ID:20261007T170000Z',
                                     'DTSTART:20261007T190000Z')))
        self.assertEqual(self.m.import_ics(source), 2)
        self.assertEqual(self.m.import_ics(source), 2)
        rows = self.m.occurrences(local(2026, 10, 1), local(2026, 10, 31))
        self.assertEqual([r['title'] for r in rows], ['One', 'Two', 'Two moved', 'Two'])

    def test_commands_and_the_grant(self):
        with patch('sys.stdin', io.StringIO(json.dumps({'title': 'X', 'start': '2026-10-05T10:00'}))), \
                patch('sys.stdout', io.StringIO()) as out:
            self.assertEqual(self.m.main(['save']), 0)
        self.assertTrue(json.loads(out.getvalue())['ok'])
        with patch('sys.stdout', io.StringIO()) as out:
            self.assertEqual(self.m.main(['range', '2026-10-01', '2026-10-31']), 0)
        self.assertEqual([e['title'] for e in json.loads(out.getvalue())['events']], ['X'])
        with patch.dict(os.environ, {'OMARCHY_MOBILE_APP': 'calendar'}), \
                patch.object(self.m, 'load_apps') as apps, patch('sys.stdout', io.StringIO()) as out:
            apps.return_value.has_grant.return_value = False
            self.assertEqual(self.m.main(['calendars']), 1)
        self.assertIn('not granted', json.loads(out.getvalue())['error'])

    def test_next_due_reads_the_watchers_file(self):
        self.assertIsNone(self.m.next_due(1000))
        self.m.next_file().write_text('1500.0\n')
        self.assertEqual(self.m.next_due(1000), 1500.0)
        self.assertIsNone(self.m.next_due(2000))


if __name__ == '__main__':
    unittest.main()
