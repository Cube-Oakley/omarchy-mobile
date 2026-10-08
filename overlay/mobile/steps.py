#!/usr/bin/env python3
"""Step counting, for Settings, apps and syncs (omarchy-mobile-steps).

The phone counts with its sensor hub's step counter, with no wake-ups (on the
Pixel, pixel-sensor-proxy's Steps service). The counts become rows of steps
per minute in an append-only SQLite log every user may read,
/var/lib/omarchy-mobile/steps/steps.db, table steps:
  id           random UUID, the same on every copy
  start_ms     the minute, ms since 1970 UTC; end_ms = start_ms + 60000
  count        steps in that minute
  source       what counted them
  recorded_ms  when the row was written

  status              {"enabled", "available", "today"}
  enable | disable    switch counting (the session user may)
  days [N]            steps per local day, today and the N-1 before (N = 7)
  export [SINCE_MS]   rows recorded after SINCE_MS, one JSON object a line,
                      with ISO 8601 times: a sync keeps the largest
                      recordedMs it saw and asks for the rows after it
"""
import datetime
import json
import sqlite3
import subprocess
import sys
import time

DB = '/var/lib/omarchy-mobile/steps/steps.db'
DEST = 'net.hadess.SensorProxy'
PATH = '/org/omarchy/Mobile/Steps'
IFACE = 'org.omarchy.Mobile.Steps'


def gdbus(*args):
    return subprocess.run(['gdbus', 'call', '--system', '--dest', DEST, '--object-path', PATH, *args],
                          capture_output=True, text=True, timeout=5)


def enabled():
    """True or False from the service, or None if it has no step counter."""
    r = gdbus('--method', 'org.freedesktop.DBus.Properties.Get', IFACE, 'Enabled')
    if r.returncode != 0:
        return None
    return 'true' in r.stdout


def connect():
    return sqlite3.connect(f'file:{DB}?mode=ro', uri=True)


def local_midnight_ms(day):
    return int(time.mktime(day.timetuple()) * 1000)


def day_totals(days):
    today = datetime.date.today()
    first = today - datetime.timedelta(days=days - 1)
    totals = {(first + datetime.timedelta(days=i)).isoformat(): 0 for i in range(days)}
    try:
        with connect() as db:
            rows = db.execute('SELECT start_ms, count FROM steps WHERE start_ms >= ?',
                              (local_midnight_ms(first),)).fetchall()
    except sqlite3.Error:
        rows = []
    for start_ms, count in rows:
        day = datetime.date.fromtimestamp(start_ms / 1000).isoformat()
        if day in totals:
            totals[day] += count
    return [{'date': d, 'steps': n} for d, n in totals.items()]


def iso(ms):
    return datetime.datetime.fromtimestamp(ms / 1000, datetime.timezone.utc).isoformat().replace('+00:00', 'Z')


def export(since_ms):
    try:
        with connect() as db:
            rows = db.execute('SELECT id, start_ms, end_ms, count, source, recorded_ms FROM steps '
                              'WHERE recorded_ms > ? ORDER BY recorded_ms, start_ms', (since_ms,)).fetchall()
    except sqlite3.Error as error:
        raise RuntimeError(f'No step data: {error}')
    for row_id, start, end, count, source, recorded in rows:
        print(json.dumps({'id': row_id, 'type': 'steps', 'startTime': iso(start), 'endTime': iso(end),
                          'count': count, 'source': source, 'recordedMs': recorded}))


def main(argv):
    action = argv[1] if len(argv) > 1 else 'status'
    try:
        if action == 'status' and len(argv) <= 2:
            on = enabled()
            print(json.dumps({'ok': True, 'available': on is not None, 'enabled': bool(on),
                              'today': day_totals(1)[0]['steps']}))
            return 0
        if action in ('enable', 'disable') and len(argv) == 2:
            r = gdbus('--method', IFACE + '.SetEnabled', 'true' if action == 'enable' else 'false')
            if r.returncode != 0:
                raise RuntimeError(r.stderr.strip() or 'Step counting is not available')
            print(json.dumps({'ok': True, 'enabled': action == 'enable'}))
            return 0
        if action == 'days' and len(argv) <= 3:
            days = int(argv[2]) if len(argv) == 3 else 7
            if not 1 <= days <= 366:
                raise ValueError('1 to 366 days')
            print(json.dumps({'ok': True, 'days': day_totals(days)}))
            return 0
        if action == 'export' and len(argv) <= 3:
            export(int(argv[2]) if len(argv) == 3 else 0)
            return 0
        print('Usage: omarchy-mobile-steps status|enable|disable|days [N]|export [SINCE_MS]', file=sys.stderr)
        return 2
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
