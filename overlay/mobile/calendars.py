#!/usr/bin/env python3
"""Omarchy Mobile calendars: iCalendar files, one folder per calendar.

  calendars                     the calendars, with names and colours
  range FROM TO                 occurrences from day FROM through day TO (YYYY-MM-DD)
  get UID                       one event, as the editor reads it
  save                          create or update the JSON event on stdin
  delete UID                    an event, every occurrence
  skip UID START                one occurrence of a repeating event (START: epoch seconds)
  calendar-save                 create or rename a calendar: {id?, name, color} on stdin
  calendar-delete ID            a calendar and its events
  import PATH [CALENDAR]        the events of an .ics file
  importable                    .ics files in ~/Downloads, ~/Documents and ~
  watch                         for the shell: reminders as notifications

Calendars live in ~/.local/share/omarchy-mobile/calendars/<id>/ as <uid>.ics,
with `displayname` and `color` files beside them: the vdir layout that
vdirsyncer syncs over CalDAV (Google, iCloud, Fastmail, Nextcloud). A file
holds one event and its changed occurrences (RECURRENCE-ID). Properties this
helper does not model (attendees, the organizer...) stay as they came.

Repeating events follow RRULE's common forms: DAILY, WEEKLY, MONTHLY and
YEARLY with INTERVAL, COUNT, UNTIL, BYDAY (ordinals too: 2TU, -1FR),
BYMONTHDAY and BYMONTH, and EXDATE. A rule with other parts (BYSETPOS,
BYHOUR...) is kept in the file and expanded without them.

Reminders (VALARM) become notifications: the shell runs `watch`, which
writes the next reminder's time to $XDG_RUNTIME_DIR, where
omarchy-mobile-sleep reads it so a sleeping phone wakes for it.

When an app runs this (OMARCHY_MOBILE_APP set), its calendar grant is
required, and files.home too for import and importable.
"""
import contextlib
import fcntl
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid
from datetime import date, datetime, timedelta, timezone
import zoneinfo

PRODID = '-//Omarchy Mobile//Calendar//EN'
WEEKDAYS = ['MO', 'TU', 'WE', 'TH', 'FR', 'SA', 'SU']
FREQS = ('DAILY', 'WEEKLY', 'MONTHLY', 'YEARLY')
FILE_ACTIONS = {'import', 'importable'}
# How far ahead the watcher looks for reminders, and how late one may still
# be shown (a phone that was off longer has missed it).
LOOKAHEAD = 3 * 86400
STALE = 3600
# A rule is expanded at most this many periods: a daily event from 1970 is
# still quick, and a broken rule cannot spin.
MAX_PERIODS = 40000
DEFAULT_CALENDAR = ('personal', 'Personal', '#7aa2f7')


def data_root():
    base = os.environ.get('XDG_DATA_HOME')
    return Path(base) if base else Path.home() / '.local/share'


def runtime_root():
    return Path(os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}')


def calendars_root():
    return data_root() / 'omarchy-mobile/calendars'


def next_file():
    return runtime_root() / 'omarchy-mobile-calendar.next'


def local_zone():
    """The system's zone by name, so recurring times keep their wall time
    across summer time."""
    name = os.environ.get('TZ', '').lstrip(':')
    if not name:
        with contextlib.suppress(OSError):
            target = os.readlink('/etc/localtime')
            if 'zoneinfo/' in target:
                name = target.split('zoneinfo/', 1)[1]
    try:
        return zoneinfo.ZoneInfo(name) if name else zoneinfo.ZoneInfo('UTC')
    except (zoneinfo.ZoneInfoNotFoundError, ValueError):
        return zoneinfo.ZoneInfo('UTC')


def zone(name):
    try:
        return zoneinfo.ZoneInfo(name) if name else local_zone()
    except (zoneinfo.ZoneInfoNotFoundError, ValueError):
        # A zone named some other way (Outlook's "Pacific Standard Time"):
        # the phone's own is the best guess.
        return local_zone()


# iCalendar text

def unfold(text):
    lines = []
    for raw in re.split(r'\r\n|\r|\n', text):
        if raw[:1] in (' ', '\t') and lines:
            lines[-1] += raw[1:]
        elif raw:
            lines.append(raw)
    return lines


def fold(line):
    """Lines of at most 75 octets, never splitting a UTF-8 sequence."""
    out, current, size = [], [], 0
    for char in line:
        width = len(char.encode())
        if size + width > 75:
            out.append(''.join(current))
            current, size = [' '], 1
        current.append(char)
        size += width
    out.append(''.join(current))
    return '\r\n'.join(out)


def escape(text):
    return (text.replace('\\', '\\\\').replace('\n', '\\n')
            .replace(',', '\\,').replace(';', '\\;'))


def unescape(text):
    return re.sub(r'\\(.)', lambda m: '\n' if m.group(1) in 'nN' else m.group(1), text, flags=re.S)


def split_line(line):
    """(NAME, {PARAM: value}, value): the colon after any quoted parameter."""
    quoted, cut = False, len(line)
    for index, char in enumerate(line):
        if char == '"':
            quoted = not quoted
        elif char == ':' and not quoted:
            cut = index
            break
    head, value = line[:cut], line[cut + 1:]
    parts = re.findall(r'(?:[^;"]|"[^"]*")+', head)
    params = {}
    for part in parts[1:]:
        key, _, val = part.partition('=')
        params[key.upper()] = val.strip('"')
    return (parts[0] if parts else '').upper(), params, value


class Component:
    """BEGIN..END: its property lines as they came, and its components."""

    def __init__(self, name):
        self.name = name
        self.lines = []
        self.subs = []

    def props(self, name):
        for line in self.lines:
            key, params, value = split_line(line)
            if key == name:
                yield params, value

    def first(self, name):
        return next(self.props(name), (None, None))

    def text(self, name):
        _, value = self.first(name)
        return unescape(value) if value is not None else ''

    def drop(self, *names):
        self.lines = [line for line in self.lines if split_line(line)[0] not in names]

    def set(self, name, value, params=''):
        self.drop(name)
        self.lines.append(f'{name}{params}:{value}')

    def serialize(self):
        out = [f'BEGIN:{self.name}']
        out += self.lines
        for sub in self.subs:
            out += sub.serialize()
        out.append(f'END:{self.name}')
        return out


def parse(text):
    """The top-level components of an iCalendar text."""
    roots, stack = [], []
    for line in unfold(text):
        upper = line.upper()
        if upper.startswith('BEGIN:'):
            component = Component(line[6:].strip().upper())
            (stack[-1].subs if stack else roots).append(component)
            stack.append(component)
        elif upper.startswith('END:'):
            if stack:
                stack.pop()
        elif stack:
            stack[-1].lines.append(line)
    return roots


def dump(calendar):
    return '\r\n'.join(fold(line) for line in calendar.serialize()) + '\r\n'


# Times

def parse_time(params, value):
    """A DATE (all day) as a date; a DATE-TIME as an aware datetime: UTC
    for Z, its TZID, or the phone's zone when floating."""
    value = (value or '').strip()
    params = params or {}
    if params.get('VALUE', '').upper() == 'DATE' or re.fullmatch(r'\d{8}', value):
        return datetime.strptime(value[:8], '%Y%m%d').date()
    match = re.fullmatch(r'(\d{8}T\d{4,6})(Z?)', value)
    if not match:
        raise ValueError(f'Not an iCalendar time: {value}')
    stamp = match.group(1).ljust(15, '0')
    naive = datetime.strptime(stamp, '%Y%m%dT%H%M%S')
    if match.group(2):
        return naive.replace(tzinfo=timezone.utc)
    return naive.replace(tzinfo=zone(params.get('TZID')))


def format_time(value, tzname=None):
    """(params, value) for DTSTART and friends."""
    if not isinstance(value, datetime):
        return ';VALUE=DATE', value.strftime('%Y%m%d')
    if tzname:
        return f';TZID={tzname}', value.astimezone(zone(tzname)).strftime('%Y%m%dT%H%M%S')
    return '', value.astimezone(timezone.utc).strftime('%Y%m%dT%H%M%SZ')


def epoch(value):
    """Seconds for a time; a day starts at its local midnight."""
    if isinstance(value, datetime):
        return value.timestamp()
    return datetime(value.year, value.month, value.day, tzinfo=local_zone()).timestamp()


def parse_duration(text):
    match = re.fullmatch(r'([+-]?)P(?:(\d+)W)?(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?)?', (text or '').strip())
    if not match:
        return None
    sign = -1 if match.group(1) == '-' else 1
    weeks, days, hours, minutes, seconds = (int(g or 0) for g in match.groups()[1:])
    return sign * timedelta(weeks=weeks, days=days, hours=hours, minutes=minutes, seconds=seconds)


def format_duration(delta):
    seconds = int(delta.total_seconds())
    sign = '-' if seconds < 0 else ''
    seconds = abs(seconds)
    days, rest = divmod(seconds, 86400)
    hours, rest = divmod(rest, 3600)
    minutes = rest // 60
    out = sign + 'P' + (f'{days}D' if days else '')
    if hours or minutes or not days:
        out += 'T' + (f'{hours}H' if hours else '') + (f'{minutes}M' if minutes or not hours else '')
    return out


def times(event):
    """(start, end, all_day) of a VEVENT; end is exclusive."""
    params, value = event.first('DTSTART')
    start = parse_time(params, value)
    all_day = not isinstance(start, datetime)
    params, value = event.first('DTEND')
    if value:
        end = parse_time(params, value)
    else:
        duration = parse_duration(event.first('DURATION')[1])
        end = start + (duration if duration is not None else (timedelta(days=1) if all_day else timedelta()))
    if all_day and isinstance(end, datetime):
        end = end.date()
    if all_day and end <= start:
        end = start + timedelta(days=1)
    return start, end, all_day


# Recurrence

def parse_rule(text):
    rule = {'FREQ': None, 'INTERVAL': 1, 'COUNT': None, 'UNTIL': None, 'BYDAY': [], 'BYMONTHDAY': [],
            'BYMONTH': [], 'other': []}
    for part in (text or '').split(';'):
        key, _, value = part.partition('=')
        key = key.upper().strip()
        if key == 'FREQ':
            rule['FREQ'] = value.upper() if value.upper() in FREQS else None
        elif key == 'INTERVAL':
            rule['INTERVAL'] = max(1, int(value or 1))
        elif key == 'COUNT':
            rule['COUNT'] = max(0, int(value))
        elif key == 'UNTIL':
            rule['UNTIL'] = parse_time({}, value)
        elif key == 'BYDAY':
            for item in value.upper().split(','):
                match = re.fullmatch(r'([+-]?\d{1,2})?(MO|TU|WE|TH|FR|SA|SU)', item.strip())
                if match:
                    rule['BYDAY'].append((int(match.group(1)) if match.group(1) else None,
                                          WEEKDAYS.index(match.group(2))))
        elif key == 'BYMONTHDAY':
            rule['BYMONTHDAY'] = [int(v) for v in value.split(',') if v.strip().lstrip('+-').isdigit()]
        elif key == 'BYMONTH':
            rule['BYMONTH'] = [int(v) for v in value.split(',') if v.strip().isdigit()]
        elif key and key != 'WKST':
            rule['other'].append(key)
    return rule


def month_days(year, month):
    following = date(year + month // 12, month % 12 + 1, 1)
    return (following - timedelta(days=1)).day


def nth_weekday(year, month, ordinal, weekday):
    """The ordinal-th weekday of a month (negative: from its end), or None."""
    days = [d for d in range(1, month_days(year, month) + 1) if date(year, month, d).weekday() == weekday]
    if ordinal is None:
        return days
    index = ordinal - 1 if ordinal > 0 else ordinal
    return [days[index]] if -len(days) <= index < len(days) else []


def month_candidates(rule, year, month, start_day):
    count = month_days(year, month)
    found = set()
    if rule['BYMONTHDAY']:
        for day in rule['BYMONTHDAY']:
            actual = day if day > 0 else count + day + 1
            if 1 <= actual <= count:
                found.add(actual)
    elif rule['BYDAY']:
        for ordinal, weekday in rule['BYDAY']:
            found.update(nth_weekday(year, month, ordinal, weekday))
    elif start_day <= count:
        found.add(start_day)
    return [date(year, month, day) for day in sorted(found)]


def candidate_days(rule, first):
    """Days of the rule from the period holding `first`, in order."""
    freq, interval = rule['FREQ'], rule['INTERVAL']
    if freq == 'DAILY':
        day = first
        for _ in range(MAX_PERIODS):
            if (not rule['BYMONTH'] or day.month in rule['BYMONTH']) and \
                    (not rule['BYDAY'] or day.weekday() in [w for _, w in rule['BYDAY']]):
                yield day
            day += timedelta(days=interval)
    elif freq == 'WEEKLY':
        weekdays = sorted({w for _, w in rule['BYDAY']}) or [first.weekday()]
        monday = first - timedelta(days=first.weekday())
        for _ in range(MAX_PERIODS):
            for weekday in weekdays:
                yield monday + timedelta(days=weekday)
            monday += timedelta(weeks=interval)
    elif freq == 'MONTHLY':
        year, month = first.year, first.month
        for _ in range(MAX_PERIODS):
            if not rule['BYMONTH'] or month in rule['BYMONTH']:
                yield from month_candidates(rule, year, month, first.day)
            month += interval
            year, month = year + (month - 1) // 12, (month - 1) % 12 + 1
    elif freq == 'YEARLY':
        year = first.year
        months = sorted(rule['BYMONTH']) or [first.month]
        for _ in range(MAX_PERIODS):
            for month in months:
                if rule['BYDAY'] or rule['BYMONTHDAY']:
                    yield from month_candidates(rule, year, month, first.day)
                elif first.day <= month_days(year, month):
                    yield date(year, month, first.day)
            year += interval
            if year > 9999 - interval:
                return


def expand(start, rule, until_epoch, exdates=()):
    """Occurrence starts (dates, or aware datetimes at the start's wall time
    in its zone) from `start` until `until_epoch`, honouring COUNT, UNTIL
    and EXDATE (epoch seconds)."""
    if not rule['FREQ']:
        yield start
        return
    timed = isinstance(start, datetime)
    first = start.date() if timed else start
    limit = epoch(rule['UNTIL']) if rule['UNTIL'] is not None else None
    if limit is not None and isinstance(rule['UNTIL'], date) and not isinstance(rule['UNTIL'], datetime):
        limit += 86400 - 1
    excluded = set(int(e) for e in exdates)
    emitted = 0
    for day in candidate_days(rule, first):
        if day < first:
            continue
        at = datetime.combine(day, start.timetz()) if timed else day
        if timed:
            at = at.replace(tzinfo=start.tzinfo)
        seconds = epoch(at)
        if limit is not None and seconds > limit:
            return
        if rule['COUNT'] is not None and emitted >= rule['COUNT']:
            return
        emitted += 1
        if seconds > until_epoch:
            return
        if int(seconds) not in excluded:
            yield at


def exdates(event):
    out = []
    for params, value in event.props('EXDATE'):
        for item in value.split(','):
            with contextlib.suppress(ValueError):
                out.append(epoch(parse_time(params, item)))
    return out


def reminders(event, all_day):
    """Minutes before the start of each display reminder (negative: after;
    for a day, from its midnight), and absolute ones as ('at', epoch)."""
    out = []
    for alarm in event.subs:
        if alarm.name != 'VALARM':
            continue
        params, value = alarm.first('TRIGGER')
        if value is None:
            continue
        if (params or {}).get('VALUE', '').upper() == 'DATE-TIME':
            with contextlib.suppress(ValueError):
                out.append(('at', epoch(parse_time({}, value))))
            continue
        delta = parse_duration(value)
        if delta is not None:
            out.append(int(-delta.total_seconds() // 60))
    return out


# The store

def calendar_dirs():
    root = calendars_root()
    if not root.is_dir() or not any(p.is_dir() for p in root.iterdir()):
        ident, name, color = DEFAULT_CALENDAR
        folder = root / ident
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'displayname').write_text(name + '\n')
        (folder / 'color').write_text(color + '\n')
    return sorted(p for p in root.iterdir() if p.is_dir() and not p.name.startswith('.'))


def calendar_info(folder):
    def read(name, default):
        with contextlib.suppress(OSError):
            text = (folder / name).read_text().strip()
            if text:
                return text
        return default
    color = read('color', '#7aa2f7')
    if not re.fullmatch(r'#[0-9A-Fa-f]{6}([0-9A-Fa-f]{2})?', color):
        color = '#7aa2f7'
    return {'id': folder.name, 'name': read('displayname', folder.name), 'color': color[:7],
            'events': sum(1 for _ in folder.glob('*.ics'))}


def safe_name(text):
    return re.sub(r'[^A-Za-z0-9@._-]', '_', text)[:120] or uuid.uuid4().hex


def write_file(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temp = tempfile.mkstemp(dir=path.parent, prefix='.', suffix='.tmp')
    with os.fdopen(handle, 'w', newline='') as out:
        out.write(text)
    os.replace(temp, path)


def load_file(path):
    """(VCALENDAR, master VEVENT, {recurrence-id epoch: override VEVENT})."""
    roots = parse(path.read_text(errors='replace'))
    calendar = next((c for c in roots if c.name == 'VCALENDAR'), None)
    if calendar is None:
        raise ValueError('Not an iCalendar file')
    master, overrides = None, {}
    for event in calendar.subs:
        if event.name != 'VEVENT':
            continue
        params, value = event.first('RECURRENCE-ID')
        if value:
            with contextlib.suppress(ValueError):
                overrides[int(epoch(parse_time(params, value)))] = event
        elif master is None:
            master = event
    if master is None and overrides:
        master = next(iter(overrides.values()))
        overrides = {}
    if master is None:
        raise ValueError('No event in the file')
    return calendar, master, overrides


def event_files():
    for folder in calendar_dirs():
        info = calendar_info(folder)
        for path in sorted(folder.glob('*.ics')):
            yield info, path


def find_event(uid):
    for info, path in event_files():
        with contextlib.suppress(OSError, ValueError):
            _, master, _ = load_file(path)
            if master.text('UID') == uid or path.stem == safe_name(uid):
                return info, path
    raise ValueError('No such event')


def occurrence_row(info, event, start, end, all_day, uid, recurring, original):
    return {'uid': uid, 'calendar': info['id'], 'color': info['color'], 'title': event.text('SUMMARY') or '(No title)',
            'location': event.text('LOCATION'), 'start': epoch(start), 'end': epoch(end), 'allDay': all_day,
            'recurring': recurring, 'original': original,
            'cancelled': event.text('STATUS').upper() == 'CANCELLED'}


def occurrences_in(info, path, low, high):
    """Occurrences of one file overlapping [low, high) (epoch seconds), with
    their reminders as (epoch, row)."""
    _, master, overrides = load_file(path)
    uid = master.text('UID') or path.stem
    start, end, all_day = times(master)
    length = epoch(end) - epoch(start)
    rule_text = master.first('RRULE')[1]
    rule = parse_rule(rule_text) if rule_text else parse_rule('')
    rows = []
    for at in expand(start, rule, high, exdates(master)):
        original = int(epoch(at))
        if original in overrides:
            continue
        begins = epoch(at)
        if begins + max(length, 1) <= low:
            continue
        stop = at + timedelta(seconds=length) if isinstance(at, datetime) else at + timedelta(days=round(length / 86400) or 1)
        rows.append((master, occurrence_row(info, master, at, stop, all_day, uid, bool(rule['FREQ']), original), all_day))
    for original, event in overrides.items():
        with contextlib.suppress(ValueError):
            o_start, o_end, o_all = times(event)
            if epoch(o_end) > low and epoch(o_start) < high:
                rows.append((event, occurrence_row(info, event, o_start, o_end, o_all, uid, True, original), o_all))
    return [(event, row, all_day) for event, row, all_day in rows if not row['cancelled']]


def day_bounds(text):
    return datetime.strptime(text, '%Y-%m-%d').date()


def occurrences(low, high):
    rows = []
    for info, path in event_files():
        with contextlib.suppress(OSError, ValueError):
            rows += [row for _, row, _ in occurrences_in(info, path, low, high)]
    rows.sort(key=lambda r: (r['start'], not r['allDay'], r['title'].lower()))
    return rows


def upcoming_reminders(low, high):
    """(time, row) of reminders falling in [low, high)."""
    found = []
    for info, path in event_files():
        with contextlib.suppress(OSError, ValueError):
            for event, row, all_day in occurrences_in(info, path, low - 86400, high + 8 * 86400):
                for item in reminders(event, all_day):
                    at = item[1] if isinstance(item, tuple) else row['start'] - item * 60
                    if low <= at < high:
                        found.append((at, row))
    found.sort(key=lambda pair: pair[0])
    return found


# The editor's view of an event

def wall(value, tzname):
    if isinstance(value, datetime):
        return value.astimezone(zone(tzname)).strftime('%Y-%m-%dT%H:%M')
    return value.strftime('%Y-%m-%d')


def describe(info, path):
    _, master, overrides = load_file(path)
    start, end, all_day = times(master)
    params, _ = master.first('DTSTART')
    tzname = (params or {}).get('TZID') or getattr(local_zone(), 'key', 'UTC')
    if all_day:
        end = end - timedelta(days=1)
    rule_text = master.first('RRULE')[1]
    repeat = None
    if rule_text:
        rule = parse_rule(rule_text)
        repeat = {'freq': (rule['FREQ'] or '').lower(), 'interval': rule['INTERVAL'],
                  'days': sorted({w for _, w in rule['BYDAY']}), 'count': rule['COUNT'],
                  'until': rule['UNTIL'].strftime('%Y-%m-%d') if rule['UNTIL'] is not None else None,
                  'text': rule_text,
                  # What the editor cannot show: kept unless the repeat is changed.
                  'custom': bool(rule['other'] or rule['BYMONTHDAY'] or rule['BYMONTH']
                                 or any(o is not None for o, _ in rule['BYDAY'])
                                 or (rule['FREQ'] != 'WEEKLY' and rule['BYDAY']))}
    return {'uid': master.text('UID') or path.stem, 'calendar': info['id'], 'color': info['color'],
            'calendarName': info['name'], 'title': master.text('SUMMARY'), 'location': master.text('LOCATION'),
            'notes': master.text('DESCRIPTION'), 'allDay': all_day, 'start': wall(start, tzname),
            'end': wall(end, tzname), 'tz': tzname, 'repeat': repeat,
            'reminders': [r for r in reminders(master, all_day) if not isinstance(r, tuple)],
            'changedOccurrences': len(overrides)}


def read_wall(text, all_day, tzname):
    if all_day:
        return datetime.strptime(text[:10], '%Y-%m-%d').date()
    return datetime.strptime(text[:16], '%Y-%m-%dT%H:%M').replace(tzinfo=zone(tzname))


def rule_text(repeat, start):
    if not repeat or not repeat.get('freq'):
        return None
    if repeat.get('custom') and repeat.get('text'):
        return repeat['text']
    freq = repeat['freq'].upper()
    if freq not in FREQS:
        raise ValueError('Repeat is daily, weekly, monthly or yearly')
    parts = [f'FREQ={freq}']
    interval = int(repeat.get('interval') or 1)
    if interval > 1:
        parts.append(f'INTERVAL={interval}')
    if freq == 'WEEKLY' and repeat.get('days'):
        parts.append('BYDAY=' + ','.join(WEEKDAYS[int(d) % 7] for d in sorted(set(repeat['days']))))
    if repeat.get('count'):
        parts.append(f"COUNT={int(repeat['count'])}")
    elif repeat.get('until'):
        until = datetime.strptime(repeat['until'], '%Y-%m-%d').date()
        if isinstance(start, datetime):
            last = datetime.combine(until, start.timetz()).replace(tzinfo=start.tzinfo)
            parts.append('UNTIL=' + last.astimezone(timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
        else:
            parts.append('UNTIL=' + until.strftime('%Y%m%d'))
    return ';'.join(parts)


def stamp():
    return datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')


def save(data):
    title = str(data.get('title') or '').strip()[:500]
    all_day = data.get('allDay') is True
    tzname = data.get('tz') or getattr(local_zone(), 'key', 'UTC')
    zone(tzname)
    start = read_wall(str(data['start']), all_day, tzname)
    end = read_wall(str(data.get('end') or data['start']), all_day, tzname)
    if all_day:
        end = max(end, start) + timedelta(days=1)
    elif end < start:
        raise ValueError('The event ends before it starts')
    folders = {f.name: f for f in calendar_dirs()}
    target = folders.get(data.get('calendar')) or next(iter(folders.values()))
    uid = data.get('uid') or ''
    if uid:
        info, path = find_event(uid)
        calendar, master, _ = load_file(path)
        master.set('SEQUENCE', str(int(master.first('SEQUENCE')[1] or 0) + 1))
    else:
        uid = f'{uuid.uuid4()}@omarchy-mobile'
        path = None
        calendar = Component('VCALENDAR')
        calendar.lines = ['VERSION:2.0', 'PRODID:' + PRODID]
        master = Component('VEVENT')
        calendar.subs.append(master)
        master.lines = ['UID:' + uid, 'CREATED:' + stamp()]
    master.set('DTSTAMP', stamp())
    master.set('LAST-MODIFIED', stamp())
    for name, value in (('SUMMARY', title), ('LOCATION', str(data.get('location') or '').strip()),
                        ('DESCRIPTION', str(data.get('notes') or '').strip())):
        if value:
            master.set(name, escape(value))
        else:
            master.drop(name)
    params, value = format_time(start, None if all_day else tzname)
    master.set('DTSTART', value, params)
    params, value = format_time(end, None if all_day else tzname)
    master.drop('DURATION')
    master.set('DTEND', value, params)
    rule = rule_text(data.get('repeat'), start)
    if rule:
        master.set('RRULE', rule)
    else:
        master.drop('RRULE', 'EXDATE')
    if 'reminders' in data:
        master.subs = [s for s in master.subs if s.name != 'VALARM']
        for minutes in sorted({int(m) for m in data.get('reminders') or []}):
            alarm = Component('VALARM')
            alarm.lines = ['ACTION:DISPLAY', 'DESCRIPTION:' + escape(title or 'Reminder'),
                           'TRIGGER:' + format_duration(timedelta(minutes=-minutes))]
            master.subs.append(alarm)
    destination = target / (safe_name(uid) + '.ics')
    write_file(destination, dump(calendar))
    if path is not None and path != destination:
        path.unlink(missing_ok=True)
    return describe(calendar_info(target), destination)


def skip(uid, original):
    """One occurrence off a repeating event: an EXDATE, its change gone."""
    _, path = find_event(uid)
    calendar, master, overrides = load_file(path)
    start, _, all_day = times(master)
    params, _ = master.first('DTSTART')
    tzname = None if all_day else (params or {}).get('TZID')
    moment = datetime.fromtimestamp(original, timezone.utc)
    value = moment.astimezone(local_zone()).date() if all_day else moment
    if not all_day and start.tzinfo is timezone.utc:
        tzname = None
    params, text = format_time(value, tzname)
    master.lines.append(f'EXDATE{params}:{text}')
    if original in overrides:
        calendar.subs.remove(overrides[original])
    master.set('LAST-MODIFIED', stamp())
    write_file(path, dump(calendar))


def import_ics(path, calendar_id=None):
    """Each event of the file (with its changed occurrences) into its own
    file; an event already there by UID is replaced."""
    text = Path(path).read_text(errors='replace')
    roots = parse(text)
    source = next((c for c in roots if c.name == 'VCALENDAR'), None)
    if source is None:
        raise ValueError('Not an iCalendar file')
    folders = {f.name: f for f in calendar_dirs()}
    target = folders.get(calendar_id) or next(iter(folders.values()))
    zones = [c for c in source.subs if c.name == 'VTIMEZONE']
    groups = {}
    for event in source.subs:
        if event.name == 'VEVENT':
            uid = event.text('UID') or str(uuid.uuid4())
            groups.setdefault(uid, []).append(event)
    for uid, events in groups.items():
        calendar = Component('VCALENDAR')
        calendar.lines = ['VERSION:2.0', 'PRODID:' + PRODID]
        calendar.subs = zones + events
        write_file(target / (safe_name(uid) + '.ics'), dump(calendar))
    return len(groups)


def importable():
    home = os.path.realpath(Path.home())
    found = {}
    for folder in (Path(home) / 'Downloads', Path(home) / 'Documents', Path(home)):
        with contextlib.suppress(OSError):
            for entry in os.scandir(folder):
                if entry.name.startswith('.') or not entry.name.lower().endswith('.ics'):
                    continue
                real = os.path.realpath(entry.path)
                if os.path.commonpath([home, real]) != home or not os.path.isfile(real):
                    continue
                found[real] = {'path': real, 'name': entry.name,
                               'modified': time.strftime('%d %b %Y', time.localtime(os.stat(real).st_mtime))}
    return sorted(found.values(), key=lambda f: f['name'].lower())


def calendar_save(data):
    name = str(data.get('name') or '').strip()[:80]
    if not name:
        raise ValueError('A calendar needs a name')
    color = str(data.get('color') or '#7aa2f7')
    if not re.fullmatch(r'#[0-9A-Fa-f]{6}', color):
        raise ValueError('A colour is #RRGGBB')
    ident = data.get('id') or re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-') or uuid.uuid4().hex[:8]
    folder = calendars_root() / safe_name(ident)
    if not data.get('id') and folder.exists():
        folder = calendars_root() / (safe_name(ident) + '-' + uuid.uuid4().hex[:4])
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'displayname').write_text(name + '\n')
    (folder / 'color').write_text(color + '\n')
    return calendar_info(folder)


def calendar_delete(ident):
    folder = calendars_root() / safe_name(ident)
    if not folder.is_dir() or folder.parent != calendars_root():
        raise ValueError('No such calendar')
    if len(calendar_dirs()) <= 1:
        raise ValueError('The last calendar stays')
    shutil.rmtree(folder)


# Reminders

def notify(at, row):
    if not shutil.which('gdbus'):
        return
    when = 'All day' if row['allDay'] else time.strftime('%H:%M', time.localtime(row['start']))
    if not row['allDay'] and row['end'] > row['start']:
        when += '–' + time.strftime('%H:%M', time.localtime(row['end']))
    if time.localtime(row['start']).tm_yday != time.localtime(at).tm_yday:
        when = time.strftime('%a ', time.localtime(row['start'])) + when
    body = when + (' · ' + row['location'] if row['location'] else '')
    hints = "{'urgency': <byte 1>, 'category': <'x-omarchy.calendar'>, 'desktop-entry': <'calendar'>}"
    with contextlib.suppress(OSError, subprocess.TimeoutExpired):
        subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications',
                        '--object-path', '/org/freedesktop/Notifications',
                        '--method', 'org.freedesktop.Notifications.Notify', '--',
                        'Calendar', '0', 'x-office-calendar', row['title'], body, '[]', hints, '-1'],
                       capture_output=True, timeout=5)


def next_due(now=None):
    """For omarchy-mobile-sleep: the next reminder, as the watcher last wrote
    it (reading every calendar each time would be slow), or None."""
    now = time.time() if now is None else now
    with contextlib.suppress(OSError, ValueError):
        due = float(next_file().read_text())
        if due >= now - 60:
            return due
    return None


def tell_watcher():
    """The watcher looks again (SIGUSR1), if the PID is really it."""
    with contextlib.suppress(OSError, ValueError):
        pid = int((runtime_root() / 'omarchy-mobile-calendar.pid').read_text())
        command = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
        if b'watch' in command and any(p.endswith((b'omarchy-mobile-calendar', b'calendars.py')) for p in command):
            os.kill(pid, signal.SIGUSR1)


def watch():
    lock = open(runtime_root() / 'omarchy-mobile-calendar.watch.lock', 'w')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return 3
    (runtime_root() / 'omarchy-mobile-calendar.pid').write_text(f'{os.getpid()}\n')
    wake_r, wake_w = os.pipe()
    os.set_blocking(wake_r, False)
    os.set_blocking(wake_w, False)
    signal.set_wakeup_fd(wake_w)
    signal.signal(signal.SIGUSR1, lambda *_: None)
    timer = os.timerfd_create(time.CLOCK_REALTIME, flags=os.TFD_CLOEXEC | os.TFD_NONBLOCK)
    waits = select.poll()
    waits.register(timer, select.POLLIN)
    waits.register(wake_r, select.POLLIN)
    waits.register(sys.stdout.fileno(), 0)
    # Reminders up to this time are done (shown, or too old to show).
    done = time.time()
    while True:
        now = time.time()
        for at, row in upcoming_reminders(done, now + 1):
            if now - at <= STALE:
                notify(at, row)
        done = now + 1
        ahead = upcoming_reminders(done, done + LOOKAHEAD)
        due = ahead[0][0] if ahead else None
        with contextlib.suppress(OSError):
            next_file().write_text(f'{due}\n' if due else '')
        print(json.dumps({'next': due}), flush=True)
        # Calendars a sync tool changes are looked at again each hour.
        wake = min(due, now + 3600) if due else now + 3600
        os.timerfd_settime(timer, flags=os.TFD_TIMER_ABSTIME | os.TFD_TIMER_CANCEL_ON_SET, initial=wake)
        events = waits.poll()
        if any(fd == sys.stdout.fileno() for fd, _ in events):
            with contextlib.suppress(OSError):
                next_file().write_text('')
            return 0
        for fd, _ in events:
            with contextlib.suppress(BlockingIOError, InterruptedError, OSError):
                os.read(fd, 64)


# Commands

def load_apps():
    here = Path(__file__).resolve().parent
    for name in ('app.py', 'omarchy-mobile-app'):
        path = here / name
        if path.is_file():
            loader = importlib.machinery.SourceFileLoader('omarchy_mobile_apps', str(path))
            spec = importlib.util.spec_from_loader(loader.name, loader)
            module = importlib.util.module_from_spec(spec)
            loader.exec_module(module)
            return module
    raise RuntimeError('omarchy-mobile-app is not installed')


def require_grant(action):
    app_id = os.environ.get('OMARCHY_MOBILE_APP')
    if not app_id:
        return
    apps = load_apps()
    if not apps.has_grant(app_id, 'calendar'):
        raise PermissionError('Calendar access is not granted')
    if action in FILE_ACTIONS and not apps.has_grant(app_id, 'files.home'):
        raise PermissionError('Home folder access is not granted')


def home_path(raw):
    home = os.path.realpath(Path.home())
    path = os.path.realpath(os.path.expanduser(raw))
    if os.path.commonpath([home, path]) != home:
        raise PermissionError('Only files in the home folder can be used')
    return Path(path)


def main(argv):
    if not argv:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    action, args = argv[0], argv[1:]
    try:
        require_grant(action)
        if action == 'watch' and not args:
            return watch()
        if action == 'calendars' and not args:
            result = {'calendars': [calendar_info(f) for f in calendar_dirs()]}
        elif action == 'range' and len(args) == 2:
            low = epoch(day_bounds(args[0]))
            high = epoch(day_bounds(args[1]) + timedelta(days=1))
            if not 0 < high - low <= 400 * 86400:
                raise ValueError('A range is one day to about a year')
            result = {'events': occurrences(low, high), 'calendars': [calendar_info(f) for f in calendar_dirs()]}
        elif action == 'get' and len(args) == 1:
            result = {'event': describe(*find_event(args[0]))}
        elif action == 'save' and not args:
            result = {'event': save(json.load(sys.stdin))}
            tell_watcher()
        elif action == 'delete' and len(args) == 1:
            find_event(args[0])[1].unlink()
            result = {}
            tell_watcher()
        elif action == 'skip' and len(args) == 2:
            skip(args[0], int(float(args[1])))
            result = {}
            tell_watcher()
        elif action == 'calendar-save' and not args:
            result = {'calendar': calendar_save(json.load(sys.stdin))}
        elif action == 'calendar-delete' and len(args) == 1:
            calendar_delete(args[0])
            result = {}
            tell_watcher()
        elif action == 'import' and len(args) in (1, 2):
            result = {'imported': import_ics(home_path(args[0]), args[1] if len(args) == 2 else None)}
            tell_watcher()
        elif action == 'importable' and not args:
            result = {'files': importable()}
        else:
            print(__doc__.strip(), file=sys.stderr)
            return 2
    except (OSError, ValueError, KeyError, TypeError, PermissionError, RuntimeError, json.JSONDecodeError) as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1
    print(json.dumps(dict(ok=True, **result)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
