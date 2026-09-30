#!/usr/bin/env python3
"""Omarchy Mobile text messages over ModemManager.

  threads                       conversations, newest first
  thread KEY [LIMIT]            one conversation's messages, oldest first
  read KEY                      mark a conversation read
  send NUMBER TEXT              send a text (long texts go as several parts)
  retry ID                      send a failed message again
  delete ID | delete-thread KEY
  unread                        number of unread messages
  segments TEXT                 SMS parts and characters left, for the composer
  watch                         one JSON line per change. The shell runs this.

Messages live in a SQLite database under $XDG_DATA_HOME/omarchy-mobile/messages.
ModemManager holds a received text only until it is copied here: the watcher
that holds the lock (the recorder) copies each new text, removes it from the
modem, posts a notification and follows delivery reports for sent texts.
Other watchers only report changes. Numbers are matched to contacts through
omarchy-mobile-contacts when it is installed.

When the Messages app runs this (OMARCHY_MOBILE_APP=messages), its messages
grant is required. The shell needs none.
"""
import fcntl
import hashlib
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import sqlite3
import subprocess
import sys
import time
import uuid
from datetime import datetime

BUS = 'org.freedesktop.ModemManager1'
MESSAGING = 'org.freedesktop.ModemManager1.Modem.Messaging'
SMS = 'org.freedesktop.ModemManager1.Sms'
# Sent texts stay in ModemManager this long for their delivery report.
DELIVERY_WAIT = 3600
MAX_TEXT = 1600
GSM = set('@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞÆæßÉ !"#¤%&\'()*+,-./0123456789:;<=>?¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§¿abcdefghijklmnopqrstuvwxyzäöñüà')
GSM_EXT = set('^{}\\[~]|€\f')


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
        raise ImportError('omarchy-mobile-app is not installed next to the messages helper')
    return module


_contacts = False


def contacts():
    global _contacts
    if _contacts is False:
        try:
            _contacts = load_module(('contacts.py', 'omarchy-mobile-contacts'), 'omarchy_mobile_contacts')
        except Exception:
            _contacts = None
    return _contacts


def data_root():
    base = os.environ.get('XDG_DATA_HOME')
    return Path(base) if base else Path.home() / '.local/share'


def runtime_root():
    base = os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}'
    path = Path(base) / 'omarchy-mobile-messages'
    path.mkdir(parents=True, exist_ok=True)
    return path


def require_grant():
    app_id = os.environ.get('OMARCHY_MOBILE_APP')
    if app_id and not load_apps().has_grant(app_id, 'messages'):
        raise PermissionError('Messages access is not granted')


def database():
    path = data_root() / 'omarchy-mobile/messages/messages.db'
    path.parent.mkdir(parents=True, exist_ok=True)
    fresh = not path.exists()
    db = sqlite3.connect(path, timeout=10)
    db.row_factory = sqlite3.Row
    if fresh:
        os.chmod(path, 0o600)
    db.execute('PRAGMA journal_mode=WAL')
    if db.execute('PRAGMA user_version').fetchone()[0] < 1:
        db.executescript('''
            CREATE TABLE IF NOT EXISTS messages (
                id INTEGER PRIMARY KEY,
                thread TEXT NOT NULL,
                number TEXT NOT NULL,
                direction TEXT NOT NULL,
                body TEXT NOT NULL,
                time REAL NOT NULL,
                state TEXT NOT NULL,
                read INTEGER NOT NULL DEFAULT 0,
                modem TEXT NOT NULL DEFAULT '',
                sent REAL NOT NULL DEFAULT 0,
                error TEXT NOT NULL DEFAULT '',
                fingerprint TEXT NOT NULL DEFAULT '',
                updated REAL NOT NULL
            );
            CREATE INDEX IF NOT EXISTS messages_thread ON messages (thread, time);
            CREATE INDEX IF NOT EXISTS messages_fingerprint ON messages (fingerprint);
            PRAGMA user_version = 1;
        ''')
    return db


def thread_key(number):
    """One conversation per correspondent: the last ten digits of a phone
    number (+1 503… and 503… are the same person), else the sender's name."""
    number = (number or '').strip()
    digits = re.sub(r'\D', '', number)
    if digits and re.fullmatch(r'\+?[0-9 ().\-]+', number):
        return digits[-10:] if len(digits) >= 10 else digits
    return number.casefold() or 'unknown'


def pretty(number):
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
    return number or 'Unknown'


def person(number):
    """Name and photo for a number, from contacts when they are installed."""
    found = None
    module = contacts()
    if module is not None and re.sub(r'\D', '', number or ''):
        try:
            found = module.lookup(number)
        except Exception:
            found = None
    if found:
        return {'name': found.get('name') or pretty(number), 'photo': found.get('photo') or '',
                'initials': found.get('initials') or '', 'contact': found.get('uid') or ''}
    label = pretty(number)
    initials = '' if re.sub(r'\D', '', number or '') else label[:1].upper()
    return {'name': label, 'photo': '', 'initials': initials, 'contact': ''}


def segments(text):
    """SMS parts: 160 GSM-7 characters (153 per part when split), or 70
    UCS-2 characters (67 per part) once any character is outside GSM-7."""
    gsm = all(c in GSM or c in GSM_EXT for c in text)
    if gsm:
        units = sum(2 if c in GSM_EXT else 1 for c in text)
        single, part = 160, 153
    else:
        units = sum(2 if ord(c) > 0xFFFF else 1 for c in text)
        single, part = 70, 67
    if units <= single:
        return {'parts': 1 if text else 0, 'left': single - units, 'unicode': not gsm}
    parts = -(-units // part)
    return {'parts': parts, 'left': parts * part - units, 'unicode': not gsm}


def row(item):
    return {
        'id': item['id'], 'thread': item['thread'], 'number': item['number'],
        'direction': item['direction'], 'body': item['body'], 'time': item['time'],
        'state': item['state'], 'read': bool(item['read']), 'error': item['error'],
    }


def threads(db):
    items = db.execute('''
        SELECT m.*, (SELECT COUNT(*) FROM messages u WHERE u.thread = m.thread AND u.read = 0) AS unread
        FROM messages m
        WHERE m.id = (SELECT id FROM messages l WHERE l.thread = m.thread ORDER BY time DESC, id DESC LIMIT 1)
        ORDER BY m.time DESC
    ''').fetchall()
    out = []
    for item in items:
        entry = row(item)
        entry.update(person(item['number']))
        entry['unread'] = item['unread']
        entry['display'] = pretty(item['number'])
        out.append(entry)
    return out


def thread(db, key, limit=500):
    items = db.execute('SELECT * FROM (SELECT * FROM messages WHERE thread = ? ORDER BY time DESC, id DESC LIMIT ?) '
                       'ORDER BY time, id', (key, limit)).fetchall()
    if not items:
        return {'ok': True, 'thread': key, 'messages': [], **person(key), 'number': key, 'display': pretty(key)}
    number = items[-1]['number']
    return {'ok': True, 'thread': key, 'number': number, 'display': pretty(number),
            'messages': [row(item) for item in items], **person(number)}


def unread(db):
    return db.execute('SELECT COUNT(*) FROM messages WHERE read = 0').fetchone()[0]


def version(db):
    count, updated, waiting = db.execute('SELECT COUNT(*), MAX(updated), SUM(read = 0) FROM messages').fetchone()
    return f'{count}:{updated or 0}:{waiting or 0}'


# ModemManager access. Reads use mmcli's JSON; writes use busctl, whose
# arguments are separate words, so a text never needs quoting.

def mmcli(*args, timeout=20):
    result = subprocess.run(['mmcli', *args], capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0:
        lines = (result.stderr or result.stdout).strip().splitlines()
        raise RuntimeError((lines[-1] if lines else 'ModemManager request failed').removeprefix('error: '))
    return result.stdout


def busctl(*args, timeout=60):
    result = subprocess.run(['busctl', '--system', '--json=short', f'--timeout={timeout}', *args],
                            capture_output=True, text=True, timeout=timeout + 5)
    if result.returncode != 0:
        lines = (result.stderr or result.stdout).strip().splitlines()
        raise RuntimeError(lines[-1] if lines else 'ModemManager request failed')
    return json.loads(result.stdout) if result.stdout.strip() else {}


def modem_path():
    return json.loads(mmcli('-m', 'any', '-J'))['modem']['dbus-path']


def modem_texts():
    backend = device_backend()
    if backend:
        try: return backend.request("sms-list")
        except RuntimeError: return None
    """ModemManager's texts as dicts, or None when it is not running."""
    try:
        paths = json.loads(mmcli('-m', 'any', '--messaging-list-sms', '-J')).get('modem.messaging.sms', [])
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.TimeoutExpired):
        return None
    found = []
    for path in paths:
        try:
            sms = json.loads(mmcli('-s', path, '-J'))['sms']
        except (OSError, RuntimeError, ValueError, KeyError, subprocess.TimeoutExpired):
            continue
        content, props = sms.get('content', {}), sms.get('properties', {})
        clean = lambda v: '' if v in (None, '--') else str(v)
        found.append({
            'path': path,
            'number': clean(content.get('number')),
            'text': clean(content.get('text')),
            'data': clean(content.get('data')),
            'pdu': clean(props.get('pdu-type')),
            'state': clean(props.get('state')),
            'timestamp': clean(props.get('timestamp')),
            'delivery': clean(props.get('delivery-state')),
        })
    return found


def delete_modem_text(path):
    backend = device_backend()
    if backend:
        try: backend.request("sms-delete", id=path)
        except RuntimeError: pass
        return
    try:
        busctl('call', BUS, modem_path(), MESSAGING, 'Delete', 'o', path, timeout=20)
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.TimeoutExpired):
        pass


def when(stamp):
    try:
        return datetime.fromisoformat(stamp).timestamp()
    except (TypeError, ValueError):
        return time.time()


def insert(db, number, direction, body, stamp, state, read, modem='', fingerprint=''):
    now = time.time()
    cursor = db.execute(
        'INSERT INTO messages (thread, number, direction, body, time, state, read, modem, sent, fingerprint, updated) '
        'VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)',
        (thread_key(number), number, direction, body, stamp, state, int(read), modem,
         now if direction == 'out' else 0, fingerprint, now))
    db.commit()
    return cursor.lastrowid


def ingest(db, texts=None, quiet=False):
    """Copy received texts out of ModemManager; return the new messages.
    quiet files them as already read (the first import of old texts)."""
    texts = modem_texts() if texts is None else texts
    if texts is None:
        return []
    added = []
    for sms in texts:
        if sms['pdu'] not in ('deliver', 'cdma-deliver') or sms['state'] != 'received':
            continue
        body = sms['text']
        if not body and sms['data']:
            # A picture message arrives as a WAP push. MMS is not supported yet.
            body = 'Sent a picture message, which this phone cannot show yet.'
        if not body:
            delete_modem_text(sms['path'])
            continue
        stamp = when(sms['timestamp'])
        fingerprint = f"{thread_key(sms['number'])}|{sms['timestamp']}|{hash_text(body)}"
        if db.execute('SELECT 1 FROM messages WHERE fingerprint = ?', (fingerprint,)).fetchone() is None:
            ident = insert(db, sms['number'], 'in', body, stamp, 'received', quiet, fingerprint=fingerprint)
            added.append(row(db.execute('SELECT * FROM messages WHERE id = ?', (ident,)).fetchone()))
        delete_modem_text(sms['path'])
    return added


def hash_text(text):
    return hashlib.sha1(text.encode()).hexdigest()[:16]


def follow_delivery(db, texts=None):
    """Delivery reports for sent texts; returns True when something changed."""
    backend = device_backend()
    if backend:
        changed = False
        pending = db.execute("SELECT id, modem FROM messages WHERE direction='out' AND modem LIKE 'pixel:%' AND state IN ('sending','unknown')").fetchall()
        for item in pending:
            try:
                result = backend.request('sms-status', request_id=item['modem'][6:])
            except RuntimeError:
                continue
            if result.get('state') in ('sent', 'failed'):
                db.execute('UPDATE messages SET state=?, error=?, modem=?, updated=? WHERE id=?',
                           (result['state'], result.get('error', ''), '', time.time(), item['id']))
                changed = True
        db.commit()
        return changed
    waiting = db.execute("SELECT id, modem, sent FROM messages WHERE direction = 'out' AND modem != '' "
                         "AND state IN ('sent', 'sending')").fetchall()
    if not waiting:
        return False
    texts = modem_texts() if texts is None else texts
    if texts is None:
        return False
    by_path = {sms['path']: sms for sms in texts}
    changed = False
    now = time.time()
    for item in waiting:
        sms = by_path.get(item['modem'])
        delivered = sms is not None and sms['delivery'] in ('completed-received', 'completed-replaced-by-sc')
        failed = sms is not None and sms['delivery'].startswith(('permanent', 'temporary-fatal'))
        if delivered or failed:
            db.execute("UPDATE messages SET state = ?, modem = '', updated = ? WHERE id = ?",
                       ('delivered' if delivered else 'failed', now, item['id']))
            changed = True
            delete_modem_text(item['modem'])
        elif sms is None or now - item['sent'] > DELIVERY_WAIT:
            # No report is coming: the text was sent, which is what we know.
            db.execute("UPDATE messages SET modem = '', updated = ? WHERE id = ?", (now, item['id']))
            changed = True
            if sms is not None:
                delete_modem_text(item['modem'])
    db.commit()
    return changed


def normalize(number):
    cleaned = re.sub(r'[\s().\-]', '', number or '')
    if not re.fullmatch(r'\+?[0-9]{3,20}', cleaned):
        raise ValueError('That is not a phone number')
    return cleaned


def transmit(db, ident, number, text):
    """Send through the selected modem backend and record the result."""
    backend = device_backend()
    if backend:
        request_id = uuid.uuid4().hex
        db.execute('UPDATE messages SET modem = ?, updated = ? WHERE id = ?',
                   ('pixel:'+request_id, time.time(), ident))
        db.commit()
        try:
            result = backend.request('sms-send', request_id=request_id, number=number, text=text)
            state, error = result['state'], result.get('error', '')
        except backend.RequestRejected as rejection:
            state, error = 'failed', str(rejection)
        except RuntimeError:
            # A transport failure may follow modem acceptance. Never label it
            # failed/retryable without knowing whether it was sent.
            state, error = 'unknown', 'Delivery uncertain; check with the recipient before sending again'
        db.execute('UPDATE messages SET state = ?, error = ?, modem = ?, updated = ? WHERE id = ?',
                   (state, error, 'pixel:'+request_id if state == 'unknown' else '', time.time(), ident))
        db.commit()
        if state != 'sent': raise RuntimeError(error or 'Could not send the message')
        return
    try:
        path = busctl('call', BUS, modem_path(), MESSAGING, 'Create', 'a{sv}', '3',
                      'number', 's', number, 'text', 's', text,
                      'delivery-report-request', 'b', 'true', timeout=20)['data'][0]
    except (OSError, RuntimeError, ValueError, KeyError, IndexError, subprocess.TimeoutExpired) as error:
        db.execute("UPDATE messages SET state = 'failed', error = ?, updated = ? WHERE id = ?",
                   (str(error), time.time(), ident))
        db.commit()
        raise RuntimeError(f'Could not send: {error}')
    db.execute('UPDATE messages SET modem = ?, updated = ? WHERE id = ?', (path, time.time(), ident))
    db.commit()
    try:
        busctl('call', BUS, path, SMS, 'Send', timeout=90)
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        db.execute("UPDATE messages SET state = 'failed', modem = '', error = ?, updated = ? WHERE id = ?",
                   (str(error), time.time(), ident))
        db.commit()
        delete_modem_text(path)
        raise RuntimeError(f'Could not send: {error}')
    db.execute("UPDATE messages SET state = 'sent', error = '', updated = ? WHERE id = ?", (time.time(), ident))
    db.commit()


def send(db, number, text):
    number = normalize(number)
    text = text.replace('\r\n', '\n')
    if not text.strip():
        raise ValueError('The message is empty')
    if len(text) > MAX_TEXT:
        raise ValueError(f'Messages are limited to {MAX_TEXT} characters')
    ident = insert(db, number, 'out', text, time.time(), 'sending', True)
    transmit(db, ident, number, text)
    return row(db.execute('SELECT * FROM messages WHERE id = ?', (ident,)).fetchone())


def retry(db, ident):
    item = db.execute("SELECT * FROM messages WHERE id = ? AND direction = 'out' AND state = 'failed'",
                      (ident,)).fetchone()
    if item is None:
        raise ValueError('Only a failed message can be sent again')
    db.execute("UPDATE messages SET state = 'sending', error = '', time = ?, sent = ?, updated = ? WHERE id = ?",
               (time.time(), time.time(), time.time(), ident))
    db.commit()
    transmit(db, ident, item['number'], item['body'])
    return row(db.execute('SELECT * FROM messages WHERE id = ?', (ident,)).fetchone())


def adopt_sent(db, texts):
    """Texts sent before this app existed (mmcli, other tools) join the history."""
    for sms in texts or []:
        if sms['pdu'] != 'submit' or sms['state'] != 'sent' or not sms['text']:
            continue
        if db.execute('SELECT 1 FROM messages WHERE modem = ?', (sms['path'],)).fetchone():
            continue
        fingerprint = f"{thread_key(sms['number'])}|out|{hash_text(sms['text'])}|{sms['path']}"
        if db.execute('SELECT 1 FROM messages WHERE fingerprint = ?', (fingerprint,)).fetchone() is None:
            insert(db, sms['number'], 'out', sms['text'], when(sms['timestamp']) if sms['timestamp'] else time.time(),
                   'sent', True, fingerprint=fingerprint)
        delete_modem_text(sms['path'])


def notify(message):
    """A received text becomes a notification; its Open button shows the thread."""
    if not shutil.which('gdbus'):
        return 0
    who = person(message['number'])
    body = message['body'] if len(message['body']) <= 240 else message['body'][:239] + '…'
    hints = "{'urgency': <byte 1>, 'category': <'im.received'>, 'desktop-entry': <'messages'>}"
    try:
        result = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications',
                                 '--object-path', '/org/freedesktop/Notifications',
                                 '--method', 'org.freedesktop.Notifications.Notify', '--',
                                 'Messages', '0', 'mail-unread', who['name'], body,
                                 "['open', 'Open']", hints, '-1'],
                                capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return 0
    match = re.search(r'uint32 (\d+)', result.stdout)
    return int(match.group(1)) if match else 0


def launch_app(key):
    """Open the Messages app on a conversation (a notification's Open)."""
    launcher = shutil.which('omarchy-mobile-app') or str(Path.home() / '.local/bin/omarchy-mobile-app')
    try:
        subprocess.Popen([launcher, 'launch', 'messages', 'thread:' + key], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)
    except OSError:
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


class Monitors:
    """ModemManager signals on the system bus and notification actions on the
    session bus, through gdbus. Raw reads, as in the phone helper."""

    def __init__(self, leader):
        self.selector = selectors.DefaultSelector()
        self.processes = {}
        self.tails = {}
        self.leader = leader

    def ensure(self):
        # Followers only read the database; the recorder alone talks to the modem.
        if not self.leader:
            return
        wanted = {} if device_backend() else {'modem': ['gdbus', 'monitor', '--system', '--dest', BUS]}
        if self.leader and os.environ.get('DBUS_SESSION_BUS_ADDRESS'):
            wanted['actions'] = ['gdbus', 'monitor', '--session', '--dest', 'org.freedesktop.Notifications']
        for name, command in wanted.items():
            process = self.processes.get(name)
            if process is not None and process.poll() is None:
                continue
            if process is not None:
                self.selector.unregister(process.stdout)
                self.processes.pop(name)
            try:
                process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            except OSError:
                continue
            self.processes[name] = process
            self.tails[name] = ''
            self.selector.register(process.stdout, selectors.EVENT_READ, name)

    def wait(self, timeout):
        """Returns (modem signal seen, [notification ids whose Open was pressed])."""
        if not self.processes:
            time.sleep(min(timeout, 2.0))
            return True, []
        hit, opened = False, []
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if hit:
                remaining = min(remaining, 0.15)
            if remaining <= 0:
                return hit, opened
            events = self.selector.select(remaining)
            if not events:
                return hit, opened
            for key, _ in events:
                name = key.data
                data = os.read(key.fileobj.fileno(), 65536)
                if not data:
                    self.processes[name].wait()
                    return True, opened
                chunk = self.tails[name] + data.decode(errors='replace')
                if name == 'modem':
                    self.tails[name] = chunk[-60:]
                    hit = hit or any(w in chunk for w in ('Messaging', '/SMS/', 'owned by', 'NameOwnerChanged'))
                else:
                    complete, _, rest = chunk.rpartition('\n')
                    self.tails[name] = rest
                    for match in re.finditer(r"ActionInvoked \(uint32 (\d+), '([^']*)'\)", complete):
                        if match.group(2) in ('open', 'default'):
                            opened.append(int(match.group(1)))


def watch():
    backend = device_backend()
    if not backend and not shutil.which('mmcli'):
        emit({'ok': False, 'error': 'ModemManager is not installed'})
        return 3
    lock = open(runtime_root() / 'watch.lock', 'w')
    leader = lock_leader(lock)
    monitors = Monitors(leader)
    db = database()
    last = None
    notified = {}
    pending = True
    # Texts already waiting in the modem when the database is new are old:
    # they are filed as read, without a notification.
    first = db.execute('SELECT COUNT(*) FROM messages').fetchone()[0] == 0
    next_modem = 0.0
    while True:
        monitors.ensure()
        if not leader and lock_leader(lock):
            leader = True
            monitors = Monitors(True)
            monitors.ensure()
            pending = True
        added = []
        if leader and (pending or time.monotonic() >= next_modem):
            texts = modem_texts()
            if texts is not None:
                added = ingest(db, texts, quiet=first)
                adopt_sent(db, texts)
                follow_delivery(db, texts)
                if first:
                    added, first = [], False
            for message in added:
                ident = notify(message)
                if ident:
                    notified[ident] = message['thread']
            # Signals do the work; a slow poll covers anything missed.
            next_modem = time.monotonic() + (1 if backend else 60)
        current = version(db)
        if current != last:
            emit({'ok': True, 'unread': unread(db), 'version': current,
                  'new': [dict(m, **person(m['number'])) for m in added]})
            last = current
        # Sent texts waiting for a delivery report are checked every 20 s.
        waiting = leader and db.execute("SELECT 1 FROM messages WHERE modem != '' LIMIT 1").fetchone()
        hit, opened = monitors.wait(1.0 if backend else 20.0 if waiting else (60.0 if leader else 1.0))
        pending = hit
        for ident in opened:
            if ident in notified:
                launch_app(notified.pop(ident))


def main(args):
    action = args[0] if args else ''
    try:
        require_grant()
        if action == 'watch' and len(args) == 1:
            return watch()
        if action == 'segments' and len(args) == 2:
            print(json.dumps({'ok': True, **segments(args[1])}))
            return 0
        db = database()
        if action == 'threads' and len(args) == 1:
            result = {'ok': True, 'threads': threads(db), 'unread': unread(db)}
        elif action == 'thread' and len(args) in (2, 3):
            limit = int(args[2]) if len(args) == 3 else 500
            result = thread(db, thread_key(args[1]) if re.sub(r'\D', '', args[1]) else args[1], max(1, min(limit, 5000)))
        elif action == 'read' and len(args) == 2:
            db.execute('UPDATE messages SET read = 1, updated = ? WHERE thread = ? AND read = 0', (time.time(), args[1]))
            db.commit()
            result = {'ok': True, 'unread': unread(db)}
        elif action == 'send' and len(args) == 3:
            result = {'ok': True, 'message': send(db, args[1], args[2])}
        elif action == 'retry' and len(args) == 2:
            result = {'ok': True, 'message': retry(db, int(args[1]))}
        elif action == 'delete' and len(args) == 2:
            db.execute('DELETE FROM messages WHERE id = ?', (int(args[1]),))
            db.execute('UPDATE messages SET updated = ? WHERE id = (SELECT MAX(id) FROM messages)', (time.time(),))
            db.commit()
            result = {'ok': True}
        elif action == 'delete-thread' and len(args) == 2:
            db.execute('DELETE FROM messages WHERE thread = ?', (args[1],))
            db.execute('UPDATE messages SET updated = ? WHERE id = (SELECT MAX(id) FROM messages)', (time.time(),))
            db.commit()
            result = {'ok': True}
        elif action == 'unread' and len(args) == 1:
            result = {'ok': True, 'unread': unread(db)}
        else:
            print(__doc__.strip(), file=sys.stderr)
            return 2
    except (OSError, ValueError, RuntimeError, PermissionError, sqlite3.Error, subprocess.SubprocessError) as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1
    print(json.dumps(result))
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
