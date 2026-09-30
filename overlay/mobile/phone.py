#!/usr/bin/env python3
"""Omarchy Mobile phone calls over ModemManager.

  status                        modem, current calls and recent calls as JSON
  dial NUMBER                   place a call
  accept|hangup CALL            act on one call (its id from status)
  hangup-all
  dtmf CALL KEYS                touch tones during a call (0-9, *, #)
  audio mute|speaker on|off     the call audio route, through the device adapter
  history clear
  ringtone                      path of the ringtone, made on first use
  watch                         one JSON line per change. The shell runs this.

Calls use ModemManager's Voice interface through mmcli, so any modem it drives
works. One watcher at a time holds the lock and is the recorder: it writes the
recent-calls list, removes finished calls from ModemManager and starts or
stops call audio. Others only report. The recorder also posts a notification
for each missed call.

Call audio belongs to the device. The adapter at
~/.config/omarchy-mobile/call-audio takes start, stop, mute on|off and
speaker on|off. Without it calls still connect, without sound.

When the Phone app runs this (OMARCHY_MOBILE_APP=phone), its phone grant is
required. The shell's own incoming-call screen needs no grant.
"""
import fcntl
import importlib.machinery
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import selectors
import shutil
import struct
import subprocess
import sys
import time
import wave


def load_module(names, label):
    # Installed copies have no .py suffix; the source tree keeps .py for tests.
    here = Path(__file__).resolve().parent
    for name in names:
        path = here / name
        if not path.is_file():
            continue
        loader = importlib.machinery.SourceFileLoader(label, str(path))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        module = importlib.util.module_from_spec(spec)
        loader.exec_module(module)
        return module
    return None



def device_backend():
    backend = load_module(('telephony.py', 'omarchy-mobile-telephony'), 'omarchy_mobile_telephony')
    return backend if backend is not None and backend.enabled() else None


def load_apps():
    module = load_module(('app.py', 'omarchy-mobile-app'), 'omarchy_mobile_apps')
    if module is None:
        raise ImportError('omarchy-mobile-app is not installed next to the phone helper')
    return module


_contacts = False


def person(number):
    """Name, photo and initials for a number, from omarchy-mobile-contacts."""
    global _contacts
    if _contacts is False:
        try:
            _contacts = load_module(('contacts.py', 'omarchy-mobile-contacts'), 'omarchy_mobile_contacts')
        except Exception:
            _contacts = None
    found = None
    if _contacts is not None and re.sub(r'\D', '', number or ''):
        try:
            found = _contacts.lookup(number)
        except Exception:
            found = None
    if not found:
        return {'name': '', 'photo': '', 'initials': '', 'contact': ''}
    return {'name': found.get('name') or '', 'photo': found.get('photo') or '',
            'initials': found.get('initials') or '', 'contact': found.get('uid') or ''}


BUS = 'org.freedesktop.ModemManager1'
HISTORY_LIMIT = 200
# A call that is dialling, ringing out, connected or held has its audio up.
# Ringing out starts it too: the ringback tone comes from the network.
AUDIO_STATES = {'dialing', 'ringing-out', 'active', 'held'}
KEYS = re.compile(r'^[0-9*#]+$')


def state_root():
    base = os.environ.get('XDG_STATE_HOME')
    return Path(base) if base else Path.home() / '.local/state'


def config_root():
    base = os.environ.get('XDG_CONFIG_HOME')
    return Path(base) if base else Path.home() / '.config'


def cache_root():
    base = os.environ.get('XDG_CACHE_HOME')
    return Path(base) if base else Path.home() / '.cache'


def runtime_root():
    base = os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}'
    path = Path(base) / 'omarchy-mobile-phone'
    path.mkdir(parents=True, exist_ok=True)
    return path


def history_path():
    return state_root() / 'omarchy-mobile/phone/history.json'


def adapter_path():
    return config_root() / 'omarchy-mobile/call-audio'


def require_grant():
    app_id = os.environ.get('OMARCHY_MOBILE_APP')
    if not app_id:
        return
    if not load_apps().has_grant(app_id, 'phone'):
        raise PermissionError('Phone access is not granted')


def mmcli(*args, timeout=15):
    result = subprocess.run(['mmcli', *args], capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0:
        message = (result.stderr or result.stdout).strip().splitlines()
        text = message[-1] if message else 'ModemManager request failed'
        raise RuntimeError(text.removeprefix('error: '))
    return result.stdout


def mmjson(*args):
    return json.loads(mmcli(*args, '-J'))


def text(value):
    return '' if value in (None, '--') else str(value)


def call_id(path):
    match = re.search(r'/Call/(\d+)$', path or '')
    if not match:
        raise ValueError('Unknown call')
    return match.group(1)


def call_path(ident):
    if not re.fullmatch(r'\d+', str(ident)):
        raise ValueError('Unknown call')
    return f'/org/freedesktop/ModemManager1/Call/{ident}'


def modem():
    backend = device_backend()
    if backend:
        try: return backend.request("status")["modem"]
        except RuntimeError: return {"present": False}
    try:
        data = mmjson('-m', 'any')['modem']
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.TimeoutExpired):
        return {'present': False}
    generic = data.get('generic', {})
    gpp = data.get('3gpp', {})
    signal = generic.get('signal-quality', {})
    tech = [text(t) for t in generic.get('access-technologies', []) if text(t)]
    numbers = [text(n) for n in generic.get('own-numbers', []) if text(n)]
    state = text(generic.get('state'))
    return {
        'present': True,
        'state': state,
        'ready': state in ('registered', 'connected', 'connecting', 'disconnecting'),
        'operator': text(gpp.get('operator-name')),
        'tech': ', '.join(tech),
        'signal': int(text(signal.get('value')) or 0),
        'own': numbers[0] if numbers else '',
        'audio': adapter_path().is_file() and os.access(adapter_path(), os.X_OK),
    }


def calls():
    backend = device_backend()
    if backend:
        try: current = backend.request("status")["calls"]
        except RuntimeError: return None
        return [dict(c, display=pretty(c["number"]), **person(c["number"])) for c in current]
    try:
        paths = mmjson('-m', 'any', '--voice-list-calls').get('modem.voice.call', [])
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired):
        return None
    found = []
    for path in paths:
        try:
            props = mmjson('-o', path)['call']['properties']
        except (OSError, RuntimeError, ValueError, KeyError, subprocess.TimeoutExpired):
            continue
        state = text(props.get('state')) or 'unknown'
        number = text(props.get('number'))
        found.append({
            'id': call_id(path),
            'number': number,
            'display': pretty(number),
            **person(number),
            'direction': text(props.get('direction')) or 'unknown',
            'state': state,
            'reason': text(props.get('state-reason')),
            'multiparty': props.get('multiparty') == 'yes',
        })
    return found


def load_history():
    try:
        data = json.loads(history_path().read_text())
    except (OSError, ValueError):
        return []
    return [item for item in data if isinstance(item, dict)] if isinstance(data, list) else []


def save_history(items):
    path = history_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(items[:HISTORY_LIMIT]) + '\n')
    os.chmod(tmp, 0o600)
    tmp.replace(path)


def tracked_path():
    return runtime_root() / 'calls.json'


def load_tracked():
    try:
        data = json.loads(tracked_path().read_text())
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def save_tracked(data):
    tmp = tracked_path().with_suffix('.tmp')
    tmp.write_text(json.dumps(data))
    tmp.replace(tracked_path())


def audio_state_path():
    return runtime_root() / 'audio.json'


def audio_state():
    backend = device_backend()
    if backend:
        try: return backend.request("status")["audio"]
        except RuntimeError: return dict(available=False, running=False, mute=False, speaker=False, error="Cellular service is unavailable")
    try:
        data = json.loads(audio_state_path().read_text())
    except (OSError, ValueError):
        data = {}
    return {
        'available': adapter_path().is_file() and os.access(adapter_path(), os.X_OK),
        'running': data.get('running') is True,
        'mute': data.get('mute') is True,
        'speaker': data.get('speaker') is True,
        'error': str(data.get('error') or ''),
    }


def save_audio(state):
    keep = {key: state[key] for key in ('running', 'mute', 'speaker', 'error')}
    audio_state_path().write_text(json.dumps(keep))


def run_adapter(*args):
    adapter = adapter_path()
    if not (adapter.is_file() and os.access(adapter, os.X_OK)):
        return 'Call audio is not available on this phone yet'
    try:
        result = subprocess.run([str(adapter), *args], capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired) as error:
        return str(error)
    if result.returncode != 0:
        lines = (result.stderr or result.stdout).strip().splitlines()
        return lines[-1] if lines else f'call-audio {args[0]} failed'
    return ''


def audio_start():
    if device_backend(): return  # The device service owns PCM lifetime.
    state = audio_state()
    if state['running']:
        return
    state.update(running=True, mute=False, speaker=False)
    state['error'] = run_adapter('start')
    save_audio(state)


def audio_stop():
    if device_backend(): return
    state = audio_state()
    if not state['running']:
        return
    run_adapter('stop')
    save_audio({'running': False, 'mute': False, 'speaker': False, 'error': ''})


def audio_set(what, value):
    backend = device_backend()
    if backend: return backend.request("audio", what=what, value=value)
    if what not in ('mute', 'speaker') or value not in ('on', 'off'):
        raise ValueError('Usage: audio mute|speaker on|off')
    state = audio_state()
    if not state['running']:
        raise RuntimeError('No call audio is running')
    error = run_adapter(what, value)
    if error:
        raise RuntimeError(error)
    state[what] = value == 'on'
    state['error'] = ''
    save_audio(state)
    return state


def decorate(current, tracked):
    now = time.time()
    for call in current:
        entry = tracked.get(call['id'], {})
        call['since'] = entry.get('since', now)
        call['answered'] = entry.get('answered', 0)
    return current


def snapshot(current, tracked):
    return {
        'ok': True,
        'calls': decorate([dict(c) for c in current], tracked),
        'audio': audio_state(),
    }


def result_for(entry):
    if entry.get('answered'):
        return 'answered'
    if entry.get('direction') == 'incoming':
        return 'declined' if entry.get('declined') else 'missed'
    return 'failed' if entry.get('failed') else 'cancelled'


class Recorder:
    """Turns successive call lists into recent calls, audio and cleanup."""

    def __init__(self):
        self.tracked = load_tracked()

    def update(self, current):
        """Track the calls; return the recent-call entries this update added."""
        now = time.time()
        changed = False
        declined = runtime_root() / 'declined'
        for call in current:
            entry = self.tracked.get(call['id'])
            if entry is None:
                entry = {'number': call['number'], 'direction': call['direction'],
                         'state': '', 'since': now, 'started': now, 'answered': 0}
                self.tracked[call['id']] = entry
                changed = True
            if entry['state'] != call['state']:
                entry['state'] = call['state']
                entry['since'] = now
                changed = True
                if call['state'] == 'active' and not entry['answered']:
                    entry['answered'] = now
            if call['number'] and entry['number'] != call['number']:
                entry['number'] = call['number']
                changed = True
            if call['reason'] in ('refused-or-busy', 'error', 'audio-setup-failed'):
                entry['failed'] = True
            if (declined / call['id']).exists():
                entry['declined'] = True
        added = []
        for ident in list(self.tracked):
            call = next((c for c in current if c['id'] == ident), None)
            if call is not None and call['state'] != 'terminated':
                continue
            entry = self.tracked.pop(ident)
            changed = True
            item = self.record(entry, now)
            added.append(item)
            if item['result'] == 'missed':
                notify_missed(item)
            try:
                (declined / ident).unlink()
            except OSError:
                pass
            if call is not None:
                try:
                    delete_call(ident)
                except (RuntimeError, ValueError, subprocess.TimeoutExpired):
                    pass
        if changed:
            save_tracked(self.tracked)
        live = [c for c in current if c['state'] != 'terminated']
        if any(c['state'] in AUDIO_STATES for c in live):
            audio_start()
        elif not live:
            audio_stop()
        return added

    def record(self, entry, now):
        answered = entry.get('answered') or 0
        item = {
            'number': entry.get('number', ''),
            'direction': entry.get('direction', 'unknown'),
            'result': result_for(entry),
            'start': round(entry.get('started', now)),
            'duration': round(now - answered) if answered else 0,
        }
        history = load_history()
        history.insert(0, item)
        save_history(history)
        return item


def pretty(number):
    """(503) 555-0100 for North American numbers, as the Phone app shows them."""
    number = number or ''
    digits = re.sub(r'\D', '', number)
    plain = re.fullmatch(r'[+0-9()\-.\s]+', number) is not None
    if number.startswith('+'):
        nanp = number.startswith('+1') and len(digits) == 11
    else:
        nanp = len(digits) == 10 or (len(digits) == 11 and digits[0] == '1')
    if plain and nanp:
        t = digits[-10:]
        return f'({t[:3]}) {t[3:6]}-{t[6:]}'
    return number or 'Unknown number'


def notify_missed(item):
    """A missed call becomes a normal notification in the session."""
    if not shutil.which('gdbus'):
        return
    try:
        subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications',
                        '--object-path', '/org/freedesktop/Notifications',
                        '--method', 'org.freedesktop.Notifications.Notify', '--',
                        'Phone', '0', 'phone', 'Missed call',
                        person(item.get('number', ''))['name'] or pretty(item.get('number', '')),
                        '[]', "{'urgency': <byte 1>, 'category': <'x-gnome.call.unanswered'>}", '-1'],
                       capture_output=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        pass


def lock_leader(handle):
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


def emit(payload):
    sys.stdout.write(json.dumps(payload) + '\n')
    sys.stdout.flush()


def relevant(chunk):
    return any(word in chunk for word in ('/Call/', 'Voice', 'NameOwnerChanged', 'owned by'))


class Monitor:
    """gdbus monitor on ModemManager. It follows the bus name, so a restarted
    ModemManager keeps reporting. Raw reads: a buffered readline after select
    could hold back the one line that mattered."""

    def __init__(self):
        self.process = None
        self.selector = selectors.DefaultSelector()
        self.tail = ''

    def ensure(self):
        if self.process is not None and self.process.poll() is None:
            return False
        if self.process is not None:
            self.selector.unregister(self.process.stdout)
            self.process = None
        try:
            self.process = subprocess.Popen(['gdbus', 'monitor', '--system', '--dest', BUS],
                                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        except OSError:
            return False
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        return True

    def wait(self, timeout):
        """True when a call-related signal arrived within timeout seconds."""
        if self.process is None:
            # Without gdbus, polling is all there is.
            time.sleep(min(timeout, 2.0))
            return True
        hit = False
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if hit:
                # Signals come in bursts; one refresh covers the burst.
                remaining = min(remaining, 0.12)
            if remaining <= 0:
                return hit
            if not self.selector.select(remaining):
                return hit
            data = os.read(self.process.stdout.fileno(), 65536)
            if not data:
                self.process.wait()
                time.sleep(1)
                return True
            chunk = self.tail + data.decode(errors='replace')
            self.tail = chunk[-40:]
            hit = hit or relevant(chunk)


def watch():
    backend = device_backend()
    if not backend and not shutil.which('mmcli'):
        emit({'ok': False, 'calls': [], 'error': 'ModemManager is not installed'})
        return 3
    lock = open(runtime_root() / 'watch.lock', 'w')
    leader = lock_leader(lock)
    recorder = Recorder() if leader else None
    monitor = Monitor()
    last = None
    while True:
        if not backend: monitor.ensure()
        if not leader and lock_leader(lock):
            leader = True
            recorder = Recorder()
        current = calls()
        if current is None:
            payload = {'ok': False, 'calls': [], 'error': 'ModemManager is not running'}
            live = False
        else:
            added = recorder.update(current) if recorder is not None else []
            tracked = recorder.tracked if recorder is not None else load_tracked()
            payload = snapshot([c for c in current if c['state'] != 'terminated'], tracked)
            live = bool(payload['calls'])
            if added:
                payload['ended'] = added
        if payload != last:
            emit(payload)
        last = {key: value for key, value in payload.items() if key != 'ended'}
        # A live call is also polled each second, so a lost signal never
        # leaves a call on screen. Otherwise the signals do the work.
        if backend: time.sleep(0.5)
        else: monitor.wait(1.0 if live else 30.0)


def ringtone():
    """A soft rising arpeggio, twice, then a pause: one loop of the ring.
    Made here so no sound file ships; the ring volume group sets loudness."""
    path = cache_root() / 'omarchy-mobile/ringtone.wav'
    if path.is_file():
        return path
    rate = 48000
    notes = [659.25, 830.61, 987.77, 1318.51]
    step = 0.14
    frames = bytearray()
    for repeat in range(2):
        for index, freq in enumerate(notes):
            length = step if index < len(notes) - 1 else step * 3
            for n in range(int(rate * length)):
                t = n / rate
                attack = min(1.0, t / 0.008, (length - t) / 0.01)
                decay = math.exp(-t * 7.0)
                # A little of the octave gives it a mallet's body.
                value = math.sin(2 * math.pi * freq * t) + 0.25 * math.sin(4 * math.pi * freq * t)
                frames += struct.pack('<h', int(0.42 * 32767 * attack * decay * value / 1.25))
        frames += bytes(2 * int(rate * 0.25))
    frames += bytes(2 * int(rate * 1.2))
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix('.tmp')
    with wave.open(str(tmp), 'wb') as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(bytes(frames))
    tmp.replace(path)
    return path


def normalize(number):
    cleaned = re.sub(r'[\s().\-]', '', number or '')
    if not re.fullmatch(r'\+?[0-9*#]{1,32}', cleaned):
        raise ValueError('That is not a phone number')
    return cleaned


def dial(number):
    number = normalize(number)
    backend = device_backend()
    if backend: return dict(ok=True, **backend.request("dial", number=number))
    output = mmcli('-m', 'any', '--voice-create-call=number=' + number)
    match = re.search(r'(/org/freedesktop/ModemManager1/Call/\d+)', output)
    if not match:
        raise RuntimeError('ModemManager did not create the call')
    path = match.group(1)
    try:
        mmcli('-o', path, '--start', timeout=40)
    except (RuntimeError, subprocess.TimeoutExpired):
        try:
            mmcli('-m', 'any', '--voice-delete-call=' + path)
        except (RuntimeError, subprocess.TimeoutExpired):
            pass
        raise
    return {'ok': True, 'call': call_id(path), 'number': number}



def call_action(action, ident):
    backend = device_backend()
    if backend: return backend.request(action, id=str(ident))
    return mmcli('-o', call_path(ident), '--'+action, timeout=30)


def delete_call(ident):
    backend = device_backend()
    if backend: return backend.request('delete-call', id=str(ident))
    return mmcli('-m', 'any', '--voice-delete-call='+call_path(ident))


def history_view(items):
    """Recent calls with today's contact names; one lookup per number."""
    people = {}
    out = []
    for item in items:
        number = item.get('number', '')
        if number not in people:
            people[number] = person(number)
        out.append(dict(item, display=pretty(number), **people[number]))
    return out


def status():
    current = calls() or []
    return {
        'ok': True,
        'modem': modem(),
        'calls': decorate([c for c in current if c['state'] != 'terminated'], load_tracked()),
        'audio': audio_state(),
        'history': history_view(load_history()[:100]),
    }


def main(args):
    action = args[0] if args else ''
    try:
        require_grant()
        if action == 'watch' and len(args) == 1:
            return watch()
        if action == 'status' and len(args) == 1:
            result = status()
        elif action == 'dial' and len(args) == 2:
            result = dial(args[1])
        elif action == 'accept' and len(args) == 2:
            call_action('accept', args[1])
            result = {'ok': True}
        elif action == 'hangup' and len(args) == 2:
            path = call_path(args[1])
            current = next((c for c in calls() or [] if c['id'] == args[1]), None)
            if current and current['state'] in ('ringing-in', 'waiting'):
                (runtime_root() / 'declined').mkdir(exist_ok=True)
                (runtime_root() / 'declined' / args[1]).touch()
            call_action('hangup', args[1])
            result = {'ok': True}
        elif action == 'hangup-all' and len(args) == 1:
            if device_backend():
                for call in calls() or []:
                    if call['state'] != 'terminated': call_action('hangup', call['id'])
            else: mmcli('-m', 'any', '--voice-hangup-all', timeout=30)
            result = {'ok': True}
        elif action == 'dtmf' and len(args) == 3:
            if not KEYS.fullmatch(args[2]):
                raise ValueError('Touch tones are 0-9, * and #')
            backend = device_backend()
            if backend: backend.request('dtmf', id=args[1], keys=args[2])
            else:
                for key in args[2]:
                    mmcli('-o', call_path(args[1]), '--send-dtmf=' + key)
            result = {'ok': True}
        elif action == 'audio' and len(args) == 3:
            result = {'ok': True, 'audio': audio_set(args[1], args[2])}
        elif action == 'ringtone' and len(args) == 1:
            result = {'ok': True, 'path': str(ringtone())}
        elif action == 'history' and args[1:] == ['clear']:
            save_history([])
            result = {'ok': True, 'history': []}
        else:
            print(__doc__.strip(), file=sys.stderr)
            return 2
    except (OSError, ValueError, RuntimeError, PermissionError, subprocess.SubprocessError) as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1
    print(json.dumps(result))
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
