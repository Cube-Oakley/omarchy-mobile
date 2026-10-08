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
The adapter may read OMARCHY_MOBILE_CHECK_NETWORK (prefs sleepCheckNetwork):
"wifi" brings Wi-Fi up for the background check, "data" leaves it to mobile
data alone; an adapter without that distinction ignores it.

The screen goes dark by the power key or the screen timeout. After GRACE
seconds of darkness, with no call, no audio playing and nothing holding it
awake, the phone sleeps. The power key lights the screen itself (the press
reaches power-button.py once the phone is awake). A call rings through the
shell, which lights the screen. A text posts its notification and the phone
sleeps again. The background alarm (prefs sleepCheck, minutes) keeps the
phone up until the network is back (NetworkManager connected for Wi-Fi; a
default route for mobile data), at most the alarm's hold. Then it writes the
time to $XDG_RUNTIME_DIR/omarchy-mobile-check and stays up ONLINE_HOLD more
seconds. An app with something to fetch watches that file and holds the phone
awake until it is done. The alarm runs only while something uses it: an app
registered with a file in $XDG_RUNTIME_DIR/omarchy-mobile-check.d/, or the
weather under the always-on clock (prefs ambientWeather).

Anything may hold the phone awake by putting a file, named for the reason,
in $XDG_RUNTIME_DIR/omarchy-mobile-sleep-inhibit/, and removing it after. A
file holding a process ID holds only while that process lives: one left by a
killed process is removed (October 8: Wisp's speaker, killed with its app
mid-sentence, left one).

Alarms and timers (omarchy-mobile-clock) and calendar reminders
(omarchy-mobile-calendar) end a sleep: it lasts no longer than until the
next one is due (purpose "clock"), and with one due within CLOCK_LEAD
seconds the phone stays up. Their watchers ring or notify.

Under the always-on clock, a device whose profile sets ambientSleep (its
panel keeps showing the clock while the phone sleeps) sleeps as if dark. The
adapter keeps the clock: given OMARCHY_MOBILE_MINUTE_TICK, a file the shell
watches, it wakes a second after each minute, writes the file (the clock
redraws; Qt's timers stop in suspend) and sleeps again, without returning.
The call ends at the background check, a second past its minute
(OMARCHY_MOBILE_WAKE_PURPOSE "check"), or with checks off within the hour
("ambient": the phone sleeps again after AMBIENT_HOLD).
"""
import contextlib
import fcntl
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

GRACE = 10
# Seconds to stay up after each kind of wake before sleeping again. A
# network wake is a packet for an app (on the Pixel, a push over mobile
# data): the messenger it is for fetches its message and posts it.
HOLD = {'power-key': 0, 'modem': 15, 'network': 15, 'alarm': 45}
HOLD_OTHER = 10
# Wakes that leave the screen dark.
DARK_WAKES = ('alarm', 'modem', 'network', 'bluetooth', 'unknown')
# After the background alarm, this long once the network is up: time for an
# app watching check_file() to claim the phone with an inhibit file. Each
# second costs about 0.3 mA averaged at one check per 15 minutes.
ONLINE_HOLD = 3
# Seconds awake after an hour of minute ticks under the always-on clock
# with the background check off: the redraw.
AMBIENT_HOLD = 1
# The adapter's longest sleep call, and its own when given none.
MAX_WAKE = 3600
ADAPTER_WAKE = 900
# An alarm or timer this close (seconds) keeps the phone up instead.
CLOCK_LEAD = 15
# A refusal (the charger, a low battery) is asked again after this long.
REFUSED_WAIT = 60
# A refusal that lasts is logged again this often: logged only when it
# changed, one on October 3 kept the phone up 14 hours with a single line.
REFUSAL_RELOG = 1800
# One refusal keeping a dark phone up this long on the battery is shown to
# the user (once per refusal), unless it is the charger's.
REFUSAL_ALERT = 600
EXPECTED_REFUSALS = ('Unplug before automatic sleep',)
# The same inhibit files keeping a dark phone up this long on the battery are
# shown too: a program that died while holding the phone (a weather fetch,
# an update) left its file, and the phone never slept again.
INHIBIT_ALERT = 1800
# An adapter killed by a signal refused nothing: sleep again after this long.
CRASH_RETRY = 5
LOG_LIMIT = 256 * 1024
# Settings → Battery's history: a line every HISTORY_EVERY seconds while awake,
# at each change of the screen and after each sleep (the sleep log has the
# sleeps themselves). It rides on this loop: nothing wakes the phone for it.
HISTORY_EVERY = 300
HISTORY_LIMIT = 8 * 1024 * 1024
# The device's per-rail energy meter, when it has one (Pixel: kernel/odpm):
# each rail's energy (uJ) since the last read. Only this history reads it.
RAIL_ENERGY = Path('/sys/module/pixel_odpm/parameters/energy')
# How many processes, wake sources and interrupts a line keeps (the rest are
# summed as "other").
TOP = 12


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


_scheduled = {}


def helper_module(names):
    """A helper installed beside this (omarchy-mobile-NAME) or, in the
    source tree, its .py file; None when neither is there."""
    if names not in _scheduled:
        _scheduled[names] = None
        here = Path(__file__).resolve().parent
        for name in names:
            path = here / name
            if path.exists():
                label = 'omarchy_mobile_' + re.sub(r'\W', '_', name)
                loader = importlib.machinery.SourceFileLoader(label, str(path))
                spec = importlib.util.spec_from_loader(loader.name, loader)
                module = importlib.util.module_from_spec(spec)
                loader.exec_module(module)
                _scheduled[names] = module
                break
    return _scheduled[names]


def clock_due():
    """The next alarm, timer or calendar reminder (omarchy-mobile-clock and
    omarchy-mobile-calendar; epoch seconds), or None."""
    due = []
    for names in (('omarchy-mobile-clock', 'clock.py'), ('omarchy-mobile-calendar', 'calendars.py')):
        try:
            module = helper_module(names)
            when = module.next_due() if module else None
        except (OSError, ValueError, KeyError, TypeError, ImportError, SyntaxError):
            when = None
        if when is not None:
            due.append(when)
    return min(due) if due else None


def clock_wake(wake, purpose, check_minutes, due, wall):
    """(seconds, purpose) for a sleep that ends at the next alarm or timer
    when that comes first: the RTC then wakes the phone for it."""
    if due is None:
        return wake, purpose
    seconds = int(due - wall)
    limit = wake or (check_minutes * 60 if check_minutes else ADAPTER_WAKE)
    if seconds < limit:
        return max(5, seconds), 'clock'
    return wake, purpose


def ambient_sleeps():
    """Whether the device's panel keeps the always-on clock through sleep."""
    try:
        return json.loads((config_home() / 'omarchy-mobile/device.json').read_text()).get('ambientSleep') is True
    except (OSError, ValueError, AttributeError):
        return False


def ambient_shown():
    """The always-on clock is up and lit (not off face down or in a pocket)."""
    return ((runtime() / 'omarchy-mobile-ambient').exists()
            and not (runtime() / 'omarchy-mobile-ambient-asleep').exists())


def minute_tick():
    """The file whose change redraws the always-on clock (AmbientDisplay)."""
    return runtime() / 'omarchy-mobile-minute-tick'


def check_file():
    """Written at each background check once the network is up."""
    return runtime() / 'omarchy-mobile-check'


def check_dir():
    """Apps that fetch at the background check register here while they
    run: a file each, named for the app."""
    return runtime() / 'omarchy-mobile-check.d'


def check_minutes(prefs):
    """The background check's interval (prefs sleepCheck) while anything
    uses it: the weather under the always-on clock (prefs ambientWeather), or
    an app registered in check_dir(). Otherwise 0: each check brings the
    network up and holds the phone awake (about 30 s for Wi-Fi) for nobody."""
    minutes = prefs.get('sleepCheck', 15)
    if not minutes:
        return 0
    if prefs.get('ambientWeather') is True and prefs.get('alwaysOn') is True:
        return minutes
    try:
        if any(check_dir().iterdir()):
            return minutes
    except OSError:
        pass
    return 0


def announce_check():
    try:
        check_file().write_text(f'{int(time.time())}\n')
    except OSError:
        pass


def ambient_tick():
    try:
        minute_tick().write_text(f'{int(time.time())}\n')
    except OSError:
        pass


def seconds_to_minute(now=None):
    """Seconds until a second past the next minute, at least 5."""
    now = time.time() if now is None else now
    wait = 61 - int(now) % 60
    return wait if wait >= 5 else wait + 60


def display_dark():
    try:
        monitors = json.loads(subprocess.check_output(['hyprctl', '-j', 'monitors'], timeout=5))
    except (OSError, subprocess.SubprocessError, ValueError):
        return False
    return bool(monitors) and not any(monitor.get('dpmsStatus') for monitor in monitors)


def network_online(network='wifi'):
    if network == 'data':
        command = ['ip', '-j', 'route', 'show', 'default']
    else:
        command = ['nmcli', '-t', '-f', 'STATE', 'general']
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        return False
    if network == 'data':
        try:
            return bool(json.loads(result.stdout or '[]'))
        except ValueError:
            return False
    return result.stdout.strip() == 'connected'


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
    """The programs with an audio stream playing (empty when none), for the
    battery history's reasons too."""
    try:
        result = subprocess.run(['pactl', '-f', 'json', 'list', 'sink-inputs'],
                                capture_output=True, text=True, timeout=5)
        streams = json.loads(result.stdout or '[]')
    except (OSError, subprocess.SubprocessError, ValueError):
        return []
    # The audio graph's own loopbacks and filters carry an app's sound; the app's stream counts.
    names = set()
    for stream in streams:
        if not isinstance(stream, dict) or stream.get('corked') is not False:
            continue
        properties = stream.get('properties') or {}
        if properties.get('node.virtual') == 'true' or properties.get('media.role') == 'Loopback':
            continue
        names.add(properties.get('application.process.binary') or properties.get('application.name') or 'stream')
    return sorted(names)


def inhibitors():
    try:
        files = sorted((runtime() / 'omarchy-mobile-sleep-inhibit').iterdir())
    except OSError:
        return []
    held = []
    for path in files:
        try:
            owner = path.read_text().strip()
        except OSError:
            owner = ''
        if owner.isdigit() and not Path(f'/proc/{owner}').exists():
            with contextlib.suppress(OSError):
                path.unlink()
            continue
        held.append(path.name)
    return held


def notify_cannot_sleep(reason, seconds):
    """A notification: the phone has stayed up in the dark this long because
    the device refuses to sleep, or inhibit files hold it (the shell serves
    the notification)."""
    body = f'Kept awake {round(seconds / 60)} min: {reason}'[:200]
    hints = "{'urgency': <byte 1>, 'category': <'x-omarchy.sleep'>}"
    try:
        subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications',
                        '--object-path', '/org/freedesktop/Notifications',
                        '--method', 'org.freedesktop.Notifications.Notify', '--',
                        'Battery', '0', 'battery-caution', "Phone can't sleep", body, '[]', hints, '-1'],
                       capture_output=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        pass


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


def history_path():
    return state_home() / 'battery-history.jsonl'


def battery_now(root=Path('/sys/class/power_supply')):
    """The battery for the history: capacity (%), charge (µAh), current (µA,
    positive charging), voltage (µV), status, and whether a charger is in."""
    found, plugged = {}, False
    for supply in sorted(root.glob('*')):
        try:
            kind = (supply / 'type').read_text().strip()
        except OSError:
            continue
        if kind == 'Battery' and not found:
            for key, name in (('capacity', 'capacity'), ('charge', 'charge_now'),
                              ('current', 'current_now'), ('voltage', 'voltage_now')):
                try:
                    found[key] = int((supply / name).read_text())
                except (OSError, ValueError):
                    pass
            try:
                found['status'] = (supply / 'status').read_text().strip()
            except OSError:
                pass
        elif kind != 'Battery':
            try:
                plugged = plugged or (supply / 'online').read_text().strip() == '1'
            except OSError:
                pass
    if found:
        found['plugged'] = plugged
    return found


def rail_energy(path=RAIL_ENERGY):
    """Each metered rail's energy (uJ) since the last read and the seconds the
    meter covered, or {} (no meter, or the first read, which only starts it)."""
    try:
        text = path.read_text()
    except OSError:
        return {}
    rails, seconds = {}, []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0] == 'seconds':
            seconds.append(float(parts[2]))
        elif len(parts) == 2 and parts[1].isdigit() and parts[0] != 'baseline':
            rails[parts[0]] = int(parts[1])
    return {'rails': rails, 'metered': min(seconds) if seconds else 0.0} if rails else {}


TICKS = os.sysconf('SC_CLK_TCK') if hasattr(os, 'sysconf') else 100
KERNEL_GROUPS = ('kworker', 'ksoftirqd', 'migration', 'cpuhp', 'rcu', 'irq', 'kswapd', 'jbd2')


def process_label(pid, comm):
    """A name for a process that says what it is: the app for quickshell, the
    script for python, the group for kernel threads."""
    for group in KERNEL_GROUPS:
        if comm.startswith(group):
            return group
    if comm not in ('quickshell', 'python3', 'python', 'sh', 'bash', '.quickshell-wra'):
        return comm
    try:
        args = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
    except OSError:
        return comm
    args = [a.decode(errors='replace') for a in args if a]
    if comm.startswith(('quickshell', '.quickshell')):
        path = args[args.index('-p') + 1] if '-p' in args[:-1] else ''
        if '/omarchy-mobile-apps/' in path:
            return path.split('/omarchy-mobile-apps/', 1)[1].split('/', 1)[0] + ' app'
        if '/omarchy-mobile-settings/' in path:
            return 'settings'
        return 'shell' if '/omarchy-mobile/' in path else 'quickshell'
    for arg in args[1:]:
        if not arg.startswith('-'):
            return Path(arg).name
    return comm


def process_ticks(root=Path('/proc')):
    """CPU time (clock ticks, user and system) of every process, by PID and
    start time (a reused PID is another process), with its command name."""
    found = {}
    try:
        entries = list(os.scandir(root))
    except OSError:
        return found
    for entry in entries:
        if not entry.name.isdigit():
            continue
        try:
            stat = Path(entry.path, 'stat').read_text()
        except OSError:
            continue
        head, _, rest = stat.rpartition(')')
        fields = rest.split()
        if len(fields) < 20 or '(' not in head:
            continue
        found[(entry.name, fields[19])] = (int(fields[11]) + int(fields[12]), head.split('(', 1)[1])
    return found


def counters():
    """The kernel's running totals the history differences: wake sources
    (wakeups, active ms), interrupts, network bytes, time at each CPU
    frequency."""
    wakeups = {}
    for folder in Path('/sys/class/wakeup').glob('wakeup*'):
        try:
            name = (folder / 'name').read_text().strip()
            count = int((folder / 'wakeup_count').read_text())
            active = int((folder / 'active_time_ms').read_text())
        except (OSError, ValueError):
            continue
        old = wakeups.get(name, (0, 0))
        wakeups[name] = (old[0] + count, old[1] + active)
    irqs = {}
    try:
        for line in Path('/proc/interrupts').read_text().splitlines()[1:]:
            fields = line.split()
            counts = [int(f) for f in fields[1:] if f.isdigit()]
            if not counts:
                continue
            words = [f for f in fields[1:] if not f.isdigit()]
            # A numbered line ends in the device's name; a summary line
            # ("IPI0: ... Rescheduling interrupts") is its kind.
            if fields[0].rstrip(':').isdigit():
                name = words[-1] if words else fields[0].rstrip(':')
            else:
                name = ' '.join([fields[0].rstrip(':')] + words[:3])
            irqs[name] = irqs.get(name, 0) + sum(counts)
    except OSError:
        pass
    net = {}
    for folder in Path('/sys/class/net').glob('*'):
        if folder.name == 'lo':
            continue
        try:
            net[folder.name] = (int((folder / 'statistics/rx_bytes').read_text()),
                                int((folder / 'statistics/tx_bytes').read_text()))
        except (OSError, ValueError):
            continue
    freq = {}
    for policy in Path('/sys/devices/system/cpu/cpufreq').glob('policy*'):
        try:
            rows = (policy / 'stats/time_in_state').read_text().split('\n')
        except OSError:
            continue
        freq[policy.name] = {r.split()[0]: int(r.split()[1]) for r in rows if len(r.split()) == 2}
    return {'wakeups': wakeups, 'irqs': irqs, 'net': net, 'freq': freq}


def top(values, count=TOP):
    """The largest entries, the rest summed as "other"."""
    ranked = sorted(values.items(), key=lambda item: item[1], reverse=True)
    kept = dict(ranked[:count])
    rest = sum(v for _, v in ranked[count:])
    if rest:
        kept['other'] = rest
    return kept


class Usage:
    """What happened between two history lines: CPU seconds by process,
    wake sources, interrupts, network bytes and CPU frequency residency."""

    def __init__(self):
        self.ticks = None
        self.totals = None

    def since_last(self):
        ticks, totals = process_ticks(), counters()
        if self.ticks is None:
            self.ticks, self.totals = ticks, totals
            return {}
        cpu = {}
        for key, (now, comm) in ticks.items():
            spent = now - self.ticks.get(key, (0, comm))[0]
            if spent > 0:
                label = process_label(key[0], comm)
                cpu[label] = cpu.get(label, 0) + spent
        old = self.totals
        wakeups = {}
        for name, (count, active) in totals['wakeups'].items():
            before = old['wakeups'].get(name, (0, 0))
            if count - before[0] > 0 or active - before[1] > 0:
                wakeups[name] = [max(0, count - before[0]), max(0, active - before[1])]
        irqs = {name: n - old['irqs'].get(name, 0) for name, n in totals['irqs'].items()
                if n - old['irqs'].get(name, 0) > 0}
        net = {}
        for name, (rx, tx) in totals['net'].items():
            before = old['net'].get(name, (rx, tx))
            if rx - before[0] > 0 or tx - before[1] > 0:
                net[name] = [max(0, rx - before[0]), max(0, tx - before[1])]
        freq = {}
        for policy, rows in totals['freq'].items():
            spent = {khz: (t - old['freq'].get(policy, {}).get(khz, 0)) / 100 for khz, t in rows.items()}
            spent = {khz: round(s, 2) for khz, s in spent.items() if s > 0}
            if spent:
                freq[policy] = spent
        self.ticks, self.totals = ticks, totals
        ranked_wakeups = dict(sorted(wakeups.items(), key=lambda item: item[1][0], reverse=True)[:TOP])
        return {'cpu': {k: round(v / TICKS, 2) for k, v in top(cpu).items()},
                'wakeups': ranked_wakeups, 'irqs': top(irqs), 'net': net, 'freq': freq,
                'radios': {'wifi': Path('/sys/module/brcmfmac').exists(),
                           'bluetooth': Path('/sys/module/hci_uart').exists()},
                'brightness': backlight_level()}


def backlight_level():
    for device in sorted(Path('/sys/class/backlight').glob('*')):
        try:
            return round(100 * int((device / 'brightness').read_text()) / int((device / 'max_brightness').read_text()))
        except (OSError, ValueError, ZeroDivisionError):
            continue
    return None


class History:
    """The battery history's writer: a line at most every HISTORY_EVERY
    seconds unless the screen changed or a wake asks for one. Each line also
    has what was used since the line before: the rails' energy (when the
    device meters them) and the Usage counters."""

    def __init__(self, path=None, usage=None):
        self.path = path
        self.last = 0.0
        self.screen = None
        self.usage = usage if usage is not None else Usage()
        # Seconds awake since the last line, by why (see Policy.waiting).
        self.awake = {}

    def awake_for(self, reason, seconds):
        if reason and seconds > 0:
            self.awake[reason] = self.awake.get(reason, 0.0) + seconds

    def note(self, screen, event=None, now=None):
        now = time.time() if now is None else now
        if event is None and screen == self.screen and now - self.last < HISTORY_EVERY:
            return False
        self.last, self.screen = now, screen
        entry = dict(time=round(now, 1), screen=screen, **battery_now())
        if event:
            entry['event'] = event
        entry.update(rail_energy())
        entry.update(self.usage.since_last())
        if self.awake:
            entry['awake'] = {reason: round(seconds) for reason, seconds in
                              sorted(self.awake.items(), key=lambda item: -item[1]) if seconds >= 0.5}
            self.awake = {}
        path = self.path or history_path()
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.exists() and path.stat().st_size > HISTORY_LIMIT:
                lines = path.read_text().splitlines()
                path.write_text('\n'.join(lines[len(lines) // 2:]) + '\n')
            with path.open('a') as stream:
                stream.write(json.dumps(entry) + '\n')
        except OSError:
            return False
        return True


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


def sleep_once(check_minutes, network='wifi', wake_seconds=None, purpose='check', tick=None):
    """One sleep through the adapter; what woke it and for how long. With
    tick, the adapter redraws the always-on clock each minute meanwhile."""
    command = [str(adapter()), '--keep-dark']
    if wake_seconds:
        command += ['--wake-after', str(wake_seconds)]
    elif check_minutes:
        command += ['--wake-after', str(check_minutes * 60)]
    env = dict(os.environ, OMARCHY_MOBILE_CHECK_NETWORK=network, OMARCHY_MOBILE_WAKE_PURPOSE=purpose)
    if tick:
        env['OMARCHY_MOBILE_MINUTE_TICK'] = str(tick)
    try:
        result = subprocess.run(command, capture_output=True, text=True, env=env)
    except OSError as error:
        return {'woke': 'error', 'error': str(error)}
    for line in reversed(result.stdout.splitlines()):
        try:
            data = json.loads(line)
        except ValueError:
            continue
        if isinstance(data, dict) and 'woke' in data:
            return data
    error = {'woke': 'error', 'error': (result.stderr or result.stdout).strip()[-300:] or f'exit {result.returncode}'}
    if result.returncode < 0:
        error['crashed'] = True
    return error


class Policy:
    """When to sleep; the loop feeds it the time and what it observes."""

    def __init__(self):
        self.dark_since = None
        self.awake_until = 0.0
        self.refused_until = 0.0
        self.refusal = ''
        # An alarm wake whose hold ends ONLINE_HOLD after the network is up.
        self.until_online = False
        # Wall time of the last background check (minute wakes count apart).
        self.last_check = time.time()
        # After a minute wake the adapter's check of a moment ago still holds.
        self.quick = False
        # What the current hold is for (the last wake).
        self.held_by = ''
        # When the current refusal began and was last logged, and whether the
        # user was told.
        self.refusal_since = 0.0
        self.refusal_logged = 0.0
        self.refusal_alerted = False
        # The inhibit files holding a dark phone, since when, and whether the
        # user was told.
        self.inhibited_by = ''
        self.inhibited_since = 0.0
        self.inhibit_alerted = False

    def ready(self, now, dark, busy):
        if not dark:
            self.dark_since = None
            return False
        if self.dark_since is None:
            self.dark_since = now
        return (not busy and now - self.dark_since >= GRACE
                and now >= self.awake_until and now >= self.refused_until)

    def retry(self, now):
        """After an adapter crash: the screen is still dark and nothing
        refused, so sleep again shortly, without the grace period."""
        self.awake_until = now + CRASH_RETRY
        self.dark_since = now - GRACE
        self.refusal = ''

    def refused(self, now, reason):
        """Whether to log this refusal: a new one, or one lasting
        REFUSAL_RELOG since it was last logged."""
        self.refused_until = now + REFUSED_WAIT
        log = reason != self.refusal
        if log:
            self.refusal_since = self.refusal_logged = now
            self.refusal_alerted = False
        elif now - self.refusal_logged >= REFUSAL_RELOG:
            self.refusal_logged = now
            log = True
        self.refusal = reason
        return log

    def alert_due(self, now, on_battery):
        """Whether to tell the user the phone cannot sleep (once a refusal
        has kept it up REFUSAL_ALERT on the battery)."""
        if (self.refusal_alerted or not on_battery or not self.refusal
                or self.refusal.startswith(EXPECTED_REFUSALS) or now - self.refusal_since < REFUSAL_ALERT):
            return False
        self.refusal_alerted = True
        return True

    def inhibit_alert_due(self, now, held, on_battery):
        """Whether to tell the user that inhibit files (held, '' for none)
        have kept a dark phone up INHIBIT_ALERT on the battery: once while
        the same ones hold."""
        if held != self.inhibited_by:
            self.inhibited_by, self.inhibited_since, self.inhibit_alerted = held, now, False
        if not held or self.inhibit_alerted or not on_battery or now - self.inhibited_since < INHIBIT_ALERT:
            return False
        self.inhibit_alerted = True
        return True

    def waiting(self, now):
        """Why a dark phone that nothing keeps busy is not asleep yet."""
        if self.dark_since is not None and now - self.dark_since < GRACE:
            return 'grace'
        if now < self.refused_until:
            return 'refused:' + (self.refusal or '?')[:48]
        if now < self.awake_until:
            return 'hold:' + (self.held_by or '?')
        return 'checking'

    def woke(self, now, reason, purpose='check'):
        self.refusal = ''
        self.held_by = 'minute' if reason == 'alarm' and purpose == 'ambient' else str(reason)
        self.quick = reason == 'alarm' and purpose == 'ambient'
        if self.quick:
            # The clock's minute: redraw, then straight back to sleep.
            self.awake_until = now + AMBIENT_HOLD
            self.until_online = False
            self.dark_since = now - GRACE
            return
        if reason == 'alarm':
            self.last_check = time.time()
        self.awake_until = now + HOLD.get(reason, HOLD_OTHER)
        self.until_online = reason == 'alarm'
        # A wake that lights the screen (the power key, a touch, anything
        # unknown) counts darkness again from the wake: the screen takes a
        # moment to light. One that leaves it dark (the check, the modem,
        # Bluetooth) is held only by its HOLD; the grace there kept the
        # phone up 10 s more at every check (October 2).
        self.dark_since = now - GRACE if reason in DARK_WAKES else None

    def wake_plan(self, under_clock, check_minutes, wall=None):
        """(seconds or None, purpose) for the next sleep. Under the clock the
        adapter wakes each minute itself; the call ends a second past the
        minute the background check is due, or with checks off within the
        hour."""
        wall = time.time() if wall is None else wall
        if not under_clock:
            return None, 'check'
        first = seconds_to_minute(wall)
        longest = first + 60 * ((MAX_WAKE - first) // 60)
        if not check_minutes:
            return longest, 'ambient'
        due = self.last_check + check_minutes * 60 - wall
        minutes = max(0, -(-(due - first) // 60))
        return min(longest, first + 60 * int(minutes)), 'check'

    def online(self, now):
        if self.until_online:
            self.until_online = False
            self.awake_until = min(self.awake_until, now + ONLINE_HOLD)


def run():
    lock = open(runtime() / 'omarchy-mobile-sleep.lock', 'w')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return 3
    policy = Policy()
    history = History()
    # Each stretch awake goes to the battery history with its reason: the
    # one found at the start of the stretch.
    stretch_reason, stretch_start = None, time.monotonic()

    def account(why):
        nonlocal stretch_reason, stretch_start
        moment = time.monotonic()
        history.awake_for(stretch_reason, min(moment - stretch_start, 60))
        stretch_reason, stretch_start = why, moment

    while True:
        prefs = load_prefs()
        if not prefs.get('sleep') or not os.access(adapter(), os.X_OK):
            policy.dark_since = None
            dark = display_dark()
            account('sleep off' if dark else None)
            history.note('off' if dark else 'ambient' if ambient_shown() else 'on')
            time.sleep(5)
            continue
        now = time.monotonic()
        if policy.until_online and network_online(prefs.get('sleepCheckNetwork', 'wifi')):
            policy.online(now)
            announce_check()
        dark = display_dark()
        screen = 'off' if dark else 'ambient' if ambient_shown() else 'on'
        history.note(screen)
        under_clock = not dark and ambient_shown() and ambient_sleeps()
        dark = dark or under_clock
        due = clock_due() if dark else None
        soon = due is not None and due - time.time() < CLOCK_LEAD
        busy = None
        if dark:
            calling = in_call()
            audio = [] if calling else playing_audio()
            held = [] if calling or audio else inhibitors()
            busy = ('call' if calling else 'audio:' + ','.join(audio) if audio
                    else 'inhibit:' + ','.join(held) if held else 'clock' if soon else None)
        if not policy.ready(now, dark, bool(busy)):
            # The screen lit (on, or the clock while it cannot sleep) is no
            # reason to record: only a dark phone that stays up.
            account((busy or policy.waiting(now)) if dark else 'ambient' if screen == 'ambient' else None)
            held = busy[len('inhibit:'):] if busy and busy.startswith('inhibit:') else ''
            if policy.inhibit_alert_due(now, held, bool(held) and not battery_now().get('plugged', True)):
                notify_cannot_sleep(f'held by {held}', now - policy.inhibited_since)
            time.sleep(2)
            continue
        account('checking')
        ok, reason = (True, '') if under_clock and policy.quick else check()
        if not ok:
            moment = time.monotonic()
            if policy.refused(moment, reason):
                lasted = round(moment - policy.refusal_since)
                record({'refused': reason, **({'for': lasted} if lasted else {})})
            if policy.alert_due(moment, not battery_now().get('plugged', True)):
                notify_cannot_sleep(reason, moment - policy.refusal_since)
            continue
        before = battery()
        emit({'sleeping': True})
        minutes = check_minutes(prefs)
        wake, purpose = clock_wake(*policy.wake_plan(under_clock, minutes), minutes, due, time.time())
        account(None)
        result = sleep_once(minutes, prefs.get('sleepCheckNetwork', 'wifi'), wake, purpose,
                            minute_tick() if under_clock else None)
        # The time asleep is not awake time.
        stretch_start = time.monotonic()
        if under_clock:
            ambient_tick()
        after = battery()
        entry = dict(result)
        if before and after:
            entry['battery'] = {'capacity': after['capacity'], 'used_uah': before['charge'] - after['charge']}
        record(entry)
        emit(entry)
        history.note(screen, event='wake:' + str(result.get('woke', 'error')))
        if result.get('crashed'):
            policy.retry(time.monotonic())
            continue
        policy.woke(time.monotonic(), result.get('woke', 'error'), purpose)
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
            'checkMinutes': check_minutes(prefs),
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
