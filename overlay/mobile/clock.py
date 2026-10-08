#!/usr/bin/env python3
"""Alarms, timers, the stopwatch and world clocks, for the Clock app and the shell.

Everything is kept in $XDG_DATA_HOME/omarchy-mobile/clock.json. The shell runs
`watch`, which rings what comes due: it waits on a wall-clock timer (timerfd,
CLOCK_REALTIME), which fires on time while awake and at once after a resume
whose sleep passed its time. omarchy-mobile-sleep asks next_due() and never
sleeps past it, so the phone's RTC wakes it for an alarm. While anything
rings, the phone is held awake (inhibit file "clock"). An alarm that rang
unanswered for ring_minutes, or came due while nothing watched, is reported
as missed in a notification.

Commands, each printing the state as JSON:
  status
  alarm-save                   an alarm on stdin: {id?, hour, minute, days, label, vibrate, enabled}
  alarm-delete ID
  alarm-enable ID on|off
  snooze KEY | dismiss KEY     a ringing alarm or timer
  timer-start SECONDS [LABEL]
  timer-pause ID | timer-resume ID | timer-reset ID | timer-delete ID
  timer-add ID SECONDS
  stopwatch start|pause|reset|lap
  zones-set                    a list of IANA zone names on stdin
  zones-search TEXT
  tone                         path of the alarm tone, made on first use
  watch                        for the shell: a JSON line whenever what rings changes
"""
import contextlib
import fcntl
import json
import math
import os
from pathlib import Path
import select
import shutil
import signal
import struct
import subprocess
import sys
import time
import uuid
import wave
from datetime import datetime, timedelta, timezone
import zoneinfo

# Days are Python's weekday(): 0 is Monday.
DAYS = range(7)
DEFAULT_SNOOZE = 10
DEFAULT_RING = 10


def data_root():
    base = os.environ.get('XDG_DATA_HOME')
    return Path(base) if base else Path.home() / '.local/share'


def runtime_root():
    return Path(os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}')


def cache_root():
    base = os.environ.get('XDG_CACHE_HOME')
    return Path(base) if base else Path.home() / '.cache'


def store_path():
    return data_root() / 'omarchy-mobile/clock.json'


def blank():
    return {'alarms': [], 'snoozes': [], 'timers': [], 'ringing': [],
            'stopwatch': {'running': False, 'started': 0.0, 'elapsed': 0.0, 'laps': []},
            'zones': [], 'snooze_minutes': DEFAULT_SNOOZE, 'ring_minutes': DEFAULT_RING}


def load(path=None):
    path = path or store_path()
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError):
        data = {}
    state = blank()
    if isinstance(data, dict):
        for key, default in state.items():
            if isinstance(data.get(key), type(default)):
                state[key] = data[key]
    return state


def save(state, path=None):
    path = path or store_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix('.tmp')
    temp.write_text(json.dumps(state))
    temp.replace(path)


@contextlib.contextmanager
def locked(path=None):
    """The store for a read and a write by one process at a time."""
    path = path or store_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path.with_suffix('.lock'), 'w') as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        yield


def occurrence(alarm, after):
    """The alarm's first time strictly after `after` (epoch seconds), in local
    time (mktime settles summer time), or None."""
    start = datetime.fromtimestamp(after).date()
    for offset in range(8):
        day = start + timedelta(days=offset)
        if alarm.get('days') and day.weekday() not in alarm['days']:
            continue
        at = time.mktime((day.year, day.month, day.day, alarm['hour'], alarm['minute'], 0, 0, 0, -1))
        if at > after:
            return at
    return None


def ring_window(state):
    return 60 * max(1, int(state.get('ring_minutes') or DEFAULT_RING))


def advance(state, now):
    """Brings the state to `now`: what came due starts ringing, what rang too
    long stops. Returns (changed, missed alarms)."""
    changed, missed = False, []
    window = ring_window(state)
    ringing = {item['key'] for item in state['ringing']}

    def ring(key, kind, ident, due, label, vibrate):
        nonlocal changed
        changed = True
        if key not in ringing:
            ringing.add(key)
            state['ringing'].append({'key': key, 'kind': kind, 'id': ident, 'due': due, 'since': now,
                                     'label': label, 'vibrate': vibrate})

    for alarm in state['alarms']:
        if not alarm.get('enabled'):
            continue
        at = occurrence(alarm, alarm.get('after', now))
        if at is None or at > now:
            continue
        changed = True
        if not alarm.get('days'):
            alarm['enabled'] = False
        if now - at <= window:
            alarm['after'] = at
            ring(f"alarm:{alarm['id']}:{int(at)}", 'alarm', alarm['id'], at, alarm.get('label', ''),
                 alarm.get('vibrate', True))
        else:
            # Missed while nothing watched (the phone off): reported once,
            # however many days went by.
            alarm['after'] = now
            missed.append({'label': alarm.get('label', ''), 'due': at})
    for snooze in list(state['snoozes']):
        if snooze['at'] <= now:
            state['snoozes'].remove(snooze)
            changed = True
            if now - snooze['at'] <= window:
                ring(snooze['key'], 'alarm', snooze['id'], snooze['at'], snooze.get('label', ''),
                     snooze.get('vibrate', True))
            else:
                missed.append({'label': snooze.get('label', ''), 'due': snooze['at']})
    for timer in state['timers']:
        if timer.get('state') == 'running' and timer['end'] <= now:
            timer['state'] = 'ringing'
            ring(f"timer:{timer['id']}", 'timer', timer['id'], timer['end'], timer.get('label', ''), True)
    for item in list(state['ringing']):
        if now - item['since'] > window:
            state['ringing'].remove(item)
            changed = True
            if item['kind'] == 'alarm':
                missed.append({'label': item.get('label', ''), 'due': item['due']})
            else:
                stop_timer(state, item['id'])
    return changed, missed


def next_wake(state, now):
    """The next time anything is due (epoch), ringing items' end included."""
    times = []
    for alarm in state['alarms']:
        if alarm.get('enabled'):
            at = occurrence(alarm, alarm.get('after', now))
            if at is not None:
                times.append(at)
    times += [snooze['at'] for snooze in state['snoozes']]
    times += [timer['end'] for timer in state['timers'] if timer.get('state') == 'running']
    times += [item['since'] + ring_window(state) for item in state['ringing']]
    return min(times) if times else None


def next_due(now=None, path=None):
    """For omarchy-mobile-sleep: the next alarm or timer (epoch), or None."""
    now = time.time() if now is None else now
    state = load(path)
    advance(state, now)
    if state['ringing']:
        return now
    return next_wake(state, now)


def next_alarm(state, now):
    times = [occurrence(a, a.get('after', now)) for a in state['alarms'] if a.get('enabled')]
    times += [snooze['at'] for snooze in state['snoozes']]
    times = [t for t in times if t is not None]
    return min(times) if times else None


def stop_timer(state, ident):
    for timer in state['timers']:
        if timer['id'] == ident:
            timer['state'] = 'stopped'
            timer['left'] = timer['duration']
            timer.pop('end', None)


def find(items, ident):
    for item in items:
        if item['id'] == ident:
            return item
    raise ValueError('No such item')


def short_id():
    return uuid.uuid4().hex[:8]


def zone_rows(names, now=None):
    now = time.time() if now is None else now
    moment = datetime.fromtimestamp(now, timezone.utc)
    rows = []
    for name in names:
        try:
            offset = moment.astimezone(zoneinfo.ZoneInfo(name)).utcoffset()
        except (zoneinfo.ZoneInfoNotFoundError, ValueError):
            continue
        rows.append({'zone': name, 'city': name.rsplit('/', 1)[-1].replace('_', ' '),
                     'region': name.split('/', 1)[0] if '/' in name else '',
                     'offset': int(offset.total_seconds())})
    return rows


def public(state, now=None):
    """The state as the app and shell read it."""
    now = time.time() if now is None else now
    alarms = []
    for alarm in sorted(state['alarms'], key=lambda a: (a['hour'], a['minute'])):
        row = dict(alarm)
        row['next'] = occurrence(alarm, alarm.get('after', now)) if alarm.get('enabled') else None
        alarms.append(row)
    local = datetime.now().astimezone().utcoffset()
    return {'ok': True, 'now': now, 'local_offset': int(local.total_seconds()), 'alarms': alarms,
            'snoozes': state['snoozes'], 'timers': state['timers'], 'ringing': state['ringing'],
            'stopwatch': state['stopwatch'], 'zones': zone_rows(state['zones'], now),
            'next_alarm': next_alarm(state, now), 'snooze_minutes': state['snooze_minutes']}


def clean_alarm(data, existing=None):
    try:
        hour, minute = int(data['hour']), int(data['minute'])
    except (KeyError, TypeError, ValueError):
        raise ValueError('An alarm needs an hour and a minute')
    if not (0 <= hour <= 23 and 0 <= minute <= 59):
        raise ValueError('That is not a time')
    days = sorted({int(d) for d in data.get('days') or []})
    if any(d not in DAYS for d in days):
        raise ValueError('Days are 0 (Monday) to 6')
    alarm = dict(existing or {'id': short_id()})
    alarm.update(hour=hour, minute=minute, days=days, label=str(data.get('label') or '')[:60],
                 vibrate=data.get('vibrate') is not False, enabled=data.get('enabled') is not False)
    return alarm


def apply(state, command, args, stdin, now):
    """One change to the state."""
    if command == 'alarm-save':
        data = json.loads(stdin or '{}')
        existing = None
        if data.get('id'):
            existing = find(state['alarms'], data['id'])
        alarm = clean_alarm(data, existing)
        # Times from now on count: an alarm set for 07:00 at 08:00 rings tomorrow.
        alarm['after'] = now
        if existing:
            state['alarms'][state['alarms'].index(existing)] = alarm
        else:
            state['alarms'].append(alarm)
        state['snoozes'] = [s for s in state['snoozes'] if s['id'] != alarm['id']]
    elif command == 'alarm-delete' and len(args) == 1:
        state['alarms'].remove(find(state['alarms'], args[0]))
        state['snoozes'] = [s for s in state['snoozes'] if s['id'] != args[0]]
    elif command == 'alarm-enable' and len(args) == 2 and args[1] in ('on', 'off'):
        alarm = find(state['alarms'], args[0])
        alarm['enabled'] = args[1] == 'on'
        alarm['after'] = now
        if args[1] == 'off':
            state['snoozes'] = [s for s in state['snoozes'] if s['id'] != args[0]]
    elif command in ('snooze', 'dismiss') and len(args) == 1:
        item = next((i for i in state['ringing'] if i['key'] == args[0]), None)
        if item is None:
            return
        state['ringing'].remove(item)
        if item['kind'] == 'timer':
            stop_timer(state, item['id'])
        elif command == 'snooze':
            minutes = int(state.get('snooze_minutes') or DEFAULT_SNOOZE)
            state['snoozes'].append({'key': f"snooze:{item['id']}:{int(now)}", 'id': item['id'],
                                     'label': item.get('label', ''), 'vibrate': item.get('vibrate', True),
                                     'at': now + 60 * minutes})
    elif command == 'timer-start' and len(args) in (1, 2):
        seconds = int(args[0])
        if not 1 <= seconds <= 100 * 3600:
            raise ValueError('A timer runs one second to 100 hours')
        state['timers'].append({'id': short_id(), 'label': (args[1] if len(args) == 2 else '')[:60],
                                'duration': seconds, 'state': 'running', 'end': now + seconds})
    elif command == 'timer-pause' and len(args) == 1:
        timer = find(state['timers'], args[0])
        if timer['state'] == 'running':
            timer.update(state='paused', left=max(0.0, timer['end'] - now))
            timer.pop('end', None)
    elif command == 'timer-resume' and len(args) == 1:
        timer = find(state['timers'], args[0])
        if timer['state'] in ('paused', 'stopped'):
            timer.update(state='running', end=now + timer.get('left', timer['duration']))
    elif command == 'timer-reset' and len(args) == 1:
        find(state['timers'], args[0])
        state['ringing'] = [i for i in state['ringing'] if i['key'] != f'timer:{args[0]}']
        stop_timer(state, args[0])
    elif command == 'timer-delete' and len(args) == 1:
        state['timers'].remove(find(state['timers'], args[0]))
        state['ringing'] = [i for i in state['ringing'] if i['key'] != f'timer:{args[0]}']
    elif command == 'timer-add' and len(args) == 2:
        timer, seconds = find(state['timers'], args[0]), int(args[1])
        if timer['state'] == 'running':
            timer['end'] += seconds
        elif timer['state'] == 'ringing':
            state['ringing'] = [i for i in state['ringing'] if i['key'] != f'timer:{args[0]}']
            timer.update(state='running', end=now + seconds)
        else:
            timer['left'] = timer.get('left', timer['duration']) + seconds
    elif command == 'stopwatch' and len(args) == 1:
        watch = state['stopwatch']
        total = watch['elapsed'] + (now - watch['started'] if watch['running'] else 0)
        if args[0] == 'start' and not watch['running']:
            watch.update(running=True, started=now)
        elif args[0] == 'pause' and watch['running']:
            watch.update(running=False, elapsed=total)
        elif args[0] == 'reset':
            watch.update(running=False, started=0.0, elapsed=0.0, laps=[])
        elif args[0] == 'lap' and watch['running']:
            watch['laps'].append(total)
        elif args[0] not in ('start', 'pause', 'lap'):
            raise ValueError('stopwatch start|pause|reset|lap')
    elif command == 'zones-set':
        names = json.loads(stdin or '[]')
        known = zoneinfo.available_timezones()
        if not isinstance(names, list) or any(name not in known for name in names):
            raise ValueError('Unknown time zone')
        state['zones'] = list(dict.fromkeys(names))[:30]
    else:
        raise ValueError('Unknown command')


def search_zones(text):
    words = text.lower().replace('_', ' ').split()
    names = sorted(name for name in zoneinfo.available_timezones()
                   if '/' in name and not name.startswith(('Etc/', 'SystemV/', 'posix/', 'right/'))
                   and all(word in name.lower().replace('_', ' ') for word in words))
    return zone_rows(names[:40])


def tell_watcher():
    """The watcher rereads the store (SIGUSR1). The PID is checked first: a
    stale one could name another process, which SIGUSR1 would end."""
    try:
        pid = int((runtime_root() / 'omarchy-mobile-clock.pid').read_text())
        command = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
        if b'watch' in command and any(part.endswith((b'omarchy-mobile-clock', b'clock.py')) for part in command):
            os.kill(pid, signal.SIGUSR1)
    except (OSError, ValueError):
        pass


def tone():
    """A bright four-note figure twice, then a pause: one loop of the alarm.
    Made here so no sound file ships; the alarm volume group sets loudness."""
    path = cache_root() / 'omarchy-mobile/alarm.wav'
    if path.is_file():
        return path
    rate = 48000
    frames = bytearray()
    for _ in range(2):
        for freq in (1046.5, 1318.5, 1046.5, 1568.0):
            length = 0.12
            for n in range(int(rate * length)):
                t = n / rate
                envelope = min(1.0, t / 0.005, (length - t) / 0.02) * math.exp(-t * 4.0)
                value = math.sin(2 * math.pi * freq * t) + 0.3 * math.sin(4 * math.pi * freq * t)
                frames += struct.pack('<h', int(0.45 * 32767 * envelope * value / 1.3))
            frames += bytes(2 * int(rate * 0.04))
        frames += bytes(2 * int(rate * 0.18))
    frames += bytes(2 * int(rate * 0.6))
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix('.tmp')
    with wave.open(str(temp), 'wb') as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(bytes(frames))
    temp.replace(path)
    return path


def notify_missed(item):
    if not shutil.which('gdbus'):
        return
    when = datetime.fromtimestamp(item['due']).strftime('%H:%M')
    body = when + (' · ' + item['label'] if item.get('label') else '')
    hints = "{'urgency': <byte 1>, 'category': <'alarm.missed'>, 'desktop-entry': <'clock'>}"
    with contextlib.suppress(OSError, subprocess.TimeoutExpired):
        subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications',
                        '--object-path', '/org/freedesktop/Notifications',
                        '--method', 'org.freedesktop.Notifications.Notify', '--',
                        'Clock', '0', 'alarm-clock', 'Missed alarm', body, '[]', hints, '-1'],
                       capture_output=True, timeout=5)


def hold_awake(on):
    """While anything rings, omarchy-mobile-sleep keeps the phone up."""
    folder = runtime_root() / 'omarchy-mobile-sleep-inhibit'
    hold = folder / 'clock'
    with contextlib.suppress(OSError):
        if on:
            folder.mkdir(parents=True, exist_ok=True)
            hold.touch()
        else:
            hold.unlink(missing_ok=True)


def watch():
    lock = open(runtime_root() / 'omarchy-mobile-clock.watch.lock', 'w')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return 3
    (runtime_root() / 'omarchy-mobile-clock.pid').write_text(f'{os.getpid()}\n')
    # SIGUSR1 (a change from the app) and the timer both end the wait.
    wake_r, wake_w = os.pipe()
    os.set_blocking(wake_r, False)
    os.set_blocking(wake_w, False)
    signal.set_wakeup_fd(wake_w)
    signal.signal(signal.SIGUSR1, lambda *_: None)
    timer = os.timerfd_create(time.CLOCK_REALTIME, flags=os.TFD_CLOEXEC | os.TFD_NONBLOCK)
    # The shell gone (its end of stdout closed) ends the watch: a watcher
    # left over would hold the lock and ring where nobody shows it.
    waits = select.poll()
    waits.register(timer, select.POLLIN)
    waits.register(wake_r, select.POLLIN)
    waits.register(sys.stdout.fileno(), 0)
    last = None
    while True:
        now = time.time()
        with locked():
            state = load()
            changed, missed = advance(state, now)
            if changed:
                save(state)
        for item in missed:
            notify_missed(item)
        hold_awake(bool(state['ringing']))
        line = json.dumps({'ringing': state['ringing'], 'next_alarm': next_alarm(state, now),
                           'snooze_minutes': state['snooze_minutes']})
        if line != last:
            print(line, flush=True)
            last = line
        due = next_wake(state, now)
        # Absolute wall time: it fires after a resume if its time passed in
        # sleep, and a clock change (TFD_TIMER_CANCEL_ON_SET) ends the wait too.
        os.timerfd_settime(timer, flags=os.TFD_TIMER_ABSTIME | os.TFD_TIMER_CANCEL_ON_SET,
                           initial=max(due, now + 0.01) if due else 0)
        events = waits.poll()
        if any(fd == sys.stdout.fileno() for fd, _ in events):
            hold_awake(False)
            return 0
        # Only what woke the wait: both are non-blocking anyway.
        for fd, _ in events:
            with contextlib.suppress(BlockingIOError, InterruptedError, OSError):
                os.read(fd, 64)


def main(argv):
    if not argv:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    command, args = argv[0], argv[1:]
    try:
        if command == 'watch' and not args:
            return watch()
        if command == 'tone' and not args:
            print(json.dumps({'ok': True, 'path': str(tone())}))
            return 0
        if command == 'zones-search' and len(args) == 1:
            print(json.dumps({'ok': True, 'zones': search_zones(args[0])}))
            return 0
        now = time.time()
        # Only the watcher advances the state (rings, reports missed alarms).
        with locked():
            state = load()
            if command != 'status':
                stdin = sys.stdin.read() if command in ('alarm-save', 'zones-set') else ''
                apply(state, command, args, stdin, now)
            save(state)
        if command != 'status':
            tell_watcher()
        print(json.dumps(public(state, now)))
        return 0
    except (OSError, ValueError, TypeError, KeyError) as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
