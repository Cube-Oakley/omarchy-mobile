#!/usr/bin/env python3
"""Sleep while the screen is dark: the shell's suspend policy.

  run      the policy loop; the shell keeps it running
  status   JSON: whether sleep is on, the adapter, and the last wakes

The device adapter (~/.config/omarchy-mobile/suspend) does the hardware part.
`--check` says whether sleeping is safe now (it refuses on the charger, for
example). `--keep-dark --wake-after SECONDS` sleeps until a wake source fires
or the alarm, leaves the display alone, and prints what woke the phone as
JSON: power-key, modem, network, alarm or another source's name. Without an
adapter, or with sleep turned off in Settings (prefs sleep), this only waits.

The screen goes dark by the power key or the screen timeout. After GRACE
seconds of darkness, with no call, no audio playing and nothing holding it
awake, the phone sleeps. The power key lights the screen itself (the press
reaches power-button.py once the phone is awake). A call rings through the
shell, which lights the screen. A text posts its notification and the phone
sleeps again. The background alarm (prefs sleepCheck, minutes) keeps the
phone up long enough for Wi-Fi to reconnect and waiting mail or messages to
arrive.

Anything may hold the phone awake by putting a file, named for the reason,
in $XDG_RUNTIME_DIR/omarchy-mobile-sleep-inhibit/.
"""
import fcntl
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time

GRACE = 10
# Seconds to stay up after each kind of wake before sleeping again.
HOLD = {'power-key': 0, 'modem': 15, 'network': 10, 'alarm': 45}
HOLD_OTHER = 10
# A refusal (the charger, a low battery) is asked again after this long.
REFUSED_WAIT = 60
LOG_LIMIT = 256 * 1024


def runtime():
    return Path(os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}')


def config_home():
    return Path(os.environ.get('XDG_CONFIG_HOME', str(Path.home() / '.config')))


def state_home():
    return Path(os.environ.get('XDG_STATE_HOME', str(Path.home() / '.local/state'))) / 'omarchy-mobile'


def adapter():
    return config_home() / 'omarchy-mobile/suspend'


def log_path():
    return state_home() / 'sleep.log'


_prefs_module = None


def load_prefs():
    """prefs.py (installed beside this as omarchy-mobile-prefs) owns the defaults."""
    global _prefs_module
    if _prefs_module is None:
        here = Path(__file__).resolve().parent
        for name in ('omarchy-mobile-prefs', 'prefs.py'):
            path = here / name
            if path.exists():
                loader = importlib.machinery.SourceFileLoader('omarchy_mobile_prefs', str(path))
                spec = importlib.util.spec_from_loader(loader.name, loader)
                _prefs_module = importlib.util.module_from_spec(spec)
                loader.exec_module(_prefs_module)
                break
        else:
            return {'sleep': False, 'sleepCheck': 15}
    return _prefs_module.read(config_home() / 'omarchy-mobile/prefs.json')


def display_dark():
    try:
        monitors = json.loads(subprocess.check_output(['hyprctl', '-j', 'monitors'], timeout=5))
    except (OSError, subprocess.SubprocessError, ValueError):
        return False
    return bool(monitors) and not any(monitor.get('dpmsStatus') for monitor in monitors)


def in_call():
    # The phone helper keeps the calls it follows until they end.
    base = runtime() / 'omarchy-mobile-phone'
    try:
        if json.loads((base / 'calls.json').read_text()):
            return True
    except (OSError, ValueError):
        pass
    try:
        return bool(json.loads((base / 'audio.json').read_text()).get('running'))
    except (OSError, ValueError, AttributeError):
        return False


def playing_audio():
    try:
        result = subprocess.run(['pactl', '-f', 'json', 'list', 'sink-inputs'],
                                capture_output=True, text=True, timeout=5)
        streams = json.loads(result.stdout or '[]')
    except (OSError, subprocess.SubprocessError, ValueError):
        return False
    # The audio graph's own loopbacks and filters carry an app's sound; the app's stream counts.
    return any(isinstance(stream, dict) and stream.get('corked') is False
               and (stream.get('properties') or {}).get('node.virtual') != 'true'
               and (stream.get('properties') or {}).get('media.role') != 'Loopback'
               for stream in streams)


def inhibitors():
    try:
        return sorted(path.name for path in (runtime() / 'omarchy-mobile-sleep-inhibit').iterdir())
    except OSError:
        return []


def battery():
    """The first battery's charge, for the log: sleep drain between wakes."""
    for supply in sorted(Path('/sys/class/power_supply').glob('*')):
        try:
            if (supply / 'type').read_text().strip() != 'Battery':
                continue
            return {'capacity': int((supply / 'capacity').read_text()),
                    'charge': int((supply / 'charge_now').read_text())}
        except (OSError, ValueError):
            continue
    return {}


def record(entry):
    path = log_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        if path.stat().st_size > LOG_LIMIT:
            lines = path.read_text().splitlines()
            path.write_text('\n'.join(lines[len(lines) // 2:]) + '\n')
    except OSError:
        pass
    with path.open('a') as stream:
        stream.write(json.dumps(dict(time=time.time(), **entry)) + '\n')


def emit(data):
    print(json.dumps(data), flush=True)


def check():
    try:
        result = subprocess.run([str(adapter()), '--check'], capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError) as error:
        return False, str(error)
    return result.returncode == 0, (result.stdout + result.stderr).strip()


def sleep_once(check_minutes):
    """One sleep through the adapter; what woke it and for how long."""
    command = [str(adapter()), '--keep-dark']
    if check_minutes:
        command += ['--wake-after', str(check_minutes * 60)]
    try:
        result = subprocess.run(command, capture_output=True, text=True)
    except OSError as error:
        return {'woke': 'error', 'error': str(error)}
    for line in reversed(result.stdout.splitlines()):
        try:
            data = json.loads(line)
        except ValueError:
            continue
        if isinstance(data, dict) and 'woke' in data:
            return data
    return {'woke': 'error', 'error': (result.stderr or result.stdout).strip()[-300:] or f'exit {result.returncode}'}


class Policy:
    """When to sleep; the loop feeds it the time and what it observes."""

    def __init__(self):
        self.dark_since = None
        self.awake_until = 0.0
        self.refused_until = 0.0
        self.refusal = ''

    def ready(self, now, dark, busy):
        if not dark:
            self.dark_since = None
            return False
        if self.dark_since is None:
            self.dark_since = now
        return (not busy and now - self.dark_since >= GRACE
                and now >= self.awake_until and now >= self.refused_until)

    def refused(self, now, reason):
        self.refused_until = now + REFUSED_WAIT
        changed = reason != self.refusal
        self.refusal = reason
        return changed

    def woke(self, now, reason):
        self.refusal = ''
        self.awake_until = now + HOLD.get(reason, HOLD_OTHER)
        # Darkness counts again from the wake.
        self.dark_since = None


def run():
    lock = open(runtime() / 'omarchy-mobile-sleep.lock', 'w')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return 3
    policy = Policy()
    while True:
        prefs = load_prefs()
        if not prefs.get('sleep') or not os.access(adapter(), os.X_OK):
            policy.dark_since = None
            time.sleep(5)
            continue
        now = time.monotonic()
        dark = display_dark()
        busy = dark and (in_call() or playing_audio() or bool(inhibitors()))
        if not policy.ready(now, dark, busy):
            time.sleep(2)
            continue
        ok, reason = check()
        if not ok:
            if policy.refused(time.monotonic(), reason):
                record({'refused': reason})
            continue
        before = battery()
        emit({'sleeping': True})
        result = sleep_once(prefs.get('sleepCheck', 15))
        after = battery()
        entry = dict(result)
        if before and after:
            entry['battery'] = {'capacity': after['capacity'], 'used_uah': before['charge'] - after['charge']}
        record(entry)
        emit(entry)
        policy.woke(time.monotonic(), result.get('woke', 'error'))
        # A failed sleep is not retried at once.
        if result.get('woke') == 'error':
            policy.refused(time.monotonic(), result.get('error', ''))


def status():
    prefs = load_prefs()
    recent = []
    try:
        recent = [json.loads(line) for line in log_path().read_text().splitlines()[-10:]]
    except (OSError, ValueError):
        pass
    return {'sleep': prefs.get('sleep') is True, 'sleepCheck': prefs.get('sleepCheck'),
            'adapter': os.access(adapter(), os.X_OK), 'inhibitors': inhibitors(), 'recent': recent}


def main(args):
    if args == ['run']:
        return run()
    if args == ['status']:
        emit(status())
        return 0
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
