#!/usr/bin/env python3
"""Omarchy Mobile text messages over ModemManager.

  threads                       conversations, newest first
  thread KEY [LIMIT]            one conversation's messages, oldest first
  read KEY                      mark a conversation read
  send NUMBER TEXT              send a text (long texts go as several parts)
  send-mms NUMBER TEXT FILE...  send a picture message (pictures made small enough)
  capture                       open the camera to take a picture to send
  photos                        the newest pictures in the camera roll
  retry ID                      send a failed message again
  delete ID | delete-thread KEY
  unread                        number of unread messages
  download ID                   fetch a picture message again (the watcher does it)
  segments TEXT                 SMS parts and characters left, for the composer
  watch                         one JSON line per change. The shell runs this.

Messages live in a SQLite database under $XDG_DATA_HOME/omarchy-mobile/messages,
a picture message's attachments in mms/ID beside it. A picture message (MMS)
arrives as a notification in a WAP push; the recorder files it as
"downloading", fetches it over mobile data (omarchy-mobile-mms) and only then
posts a notification, trying three times before it gives up.
ModemManager holds a received text only until it is copied here: the watcher
that holds the lock (the recorder) copies each new text, removes it from the
modem, posts a notification and follows delivery reports for sent texts.
Other watchers only report changes. Numbers are matched to contacts through
omarchy-mobile-contacts when it is installed.

When the Messages app runs this (OMARCHY_MOBILE_APP=messages), its messages
grant is required. The shell needs none.
"""
import ast
import ctypes
import fcntl
import hashlib
import importlib.machinery
import importlib.util
import json
import mimetypes
import os
from pathlib import Path
import re
import selectors
import shutil
import signal
import sqlite3
import subprocess
import sys
import threading
import queue
import time
import uuid
from datetime import datetime

LIBC = ctypes.CDLL(None)
PR_SET_PDEATHSIG = 1


def die_with_parent():
    """For a gdbus monitor (preexec_fn): it ends with this process. It writes
    only when a signal comes, so one left behind by a restarted watcher would
    never get the SIGPIPE and stay for good (900 piled up on October 2)."""
    LIBC.prctl(PR_SET_PDEATHSIG, signal.SIGTERM)


BUS = 'org.freedesktop.ModemManager1'
MESSAGING = 'org.freedesktop.ModemManager1.Modem.Messaging'
SMS = 'org.freedesktop.ModemManager1.Sms'
# Sent texts stay in ModemManager this long for their delivery report.
DELIVERY_WAIT = 3600
# Picture messages: tries, and the wait before the next one (times the tries).
MMS_TRIES = 3
MMS_RETRY = 60
# What a picture message may weigh: carriers take about 1 MB (T-Mobile's,
# Tello's), less the headers' share.
MMS_LIMIT = 900 * 1024
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


_mms = False


def mms_module():
    global _mms
    if _mms is False:
        _mms = load_module(('mms.py', 'omarchy-mobile-mms'), 'omarchy_mobile_mms')
    return _mms


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
    if db.execute('PRAGMA user_version').fetchone()[0] < 2:
        # Picture messages: kind 'mms', the files they brought ([{path, type,
        # name, size}]) and what the notification said (JSON).
        db.executescript('''
            ALTER TABLE messages ADD COLUMN kind TEXT NOT NULL DEFAULT 'sms';
            ALTER TABLE messages ADD COLUMN attachments TEXT NOT NULL DEFAULT '[]';
            ALTER TABLE messages ADD COLUMN mms TEXT NOT NULL DEFAULT '';
            PRAGMA user_version = 2;
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
    attachments = json.loads(item['attachments'] or '[]')
    info = json.loads(item['mms'] or '{}')
    return {
        'id': item['id'], 'thread': item['thread'], 'number': item['number'],
        'direction': item['direction'], 'body': item['body'], 'time': item['time'],
        'state': item['state'], 'read': bool(item['read']), 'error': item['error'],
        'kind': item['kind'], 'attachments': attachments, 'group': info.get('group', []),
        'preview': preview(item, attachments),
    }


def preview(item, attachments):
    """A message in one line: its text, else what it brought."""
    if item['body']:
        return item['body']
    if item['kind'] != 'mms':
        return ''
    if item['state'] == 'downloading':
        return 'Picture message, downloading…'
    if item['state'] == 'download-failed':
        return 'Picture message, not downloaded'
    if item['state'] == 'expired':
        return 'Picture message, expired'
    kinds = [a['type'].split('/')[0] for a in attachments]
    if kinds and all(k == 'image' for k in kinds):
        return 'Photo' if len(kinds) == 1 else f'{len(kinds)} photos'
    if 'video' in kinds:
        return 'Video'
    if 'audio' in kinds:
        return 'Audio'
    return 'Attachment' if kinds else 'Picture message'



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


def insert(db, number, direction, body, stamp, state, read, modem='', fingerprint='', kind='sms', mms=''):
    now = time.time()
    cursor = db.execute(
        'INSERT INTO messages (thread, number, direction, body, time, state, read, modem, sent, fingerprint, updated, '
        'kind, mms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)',
        (thread_key(number), number, direction, body, stamp, state, int(read), modem,
         now if direction == 'out' else 0, fingerprint, now, kind, mms))
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
            # A picture message arrives as a WAP push: filed to download.
            notice = mms_notice(sms['data'])
            if notice is not None:
                file_mms(db, sms, notice, quiet)
                delete_modem_text(sms['path'])
                continue
            body = 'Sent a message this phone cannot show.'
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


def mms_notice(data):
    """The MMS message in a WAP push's bytes (hex), or None."""
    module = mms_module()
    if module is None:
        return None
    try:
        pdu = module.push(bytes.fromhex(re.sub(r'[^0-9a-fA-F]', '', data)))
        return module.decode(pdu) if pdu is not None else None
    except ValueError:
        return None


def file_mms(db, sms, notice, quiet):
    """A picture message's notification becomes a message to download (a
    delivery report for one sent is only noted)."""
    kind = notice.get('message-type')
    if kind == 'delivery-ind':
        if notice.get('status') == 0x81 and notice.get('message-id'):
            db.execute("UPDATE messages SET state = 'delivered', updated = ? WHERE kind = 'mms' AND direction = 'out' "
                       "AND json_extract(mms, '$.id') = ?", (time.time(), notice['message-id']))
            db.commit()
        return
    if kind != 'notification-ind' or not notice.get('content-location'):
        return
    transaction = notice.get('transaction-id') or notice['content-location']
    fingerprint = 'mms|' + transaction
    if db.execute('SELECT 1 FROM messages WHERE fingerprint = ?', (fingerprint,)).fetchone():
        return
    info = {'location': notice['content-location'], 'transaction': transaction,
            'expiry': notice.get('expiry', 0), 'size': notice.get('message-size', 0),
            'subject': notice.get('subject', ''), 'tries': 0, 'tried': 0}
    insert(db, notice.get('from') or sms['number'], 'in', '', when(sms['timestamp']), 'downloading', quiet,
           fingerprint=fingerprint, kind='mms', mms=json.dumps(info))


def attachment_folder(ident):
    return data_root() / 'omarchy-mobile/messages/mms' / str(int(ident))


def save_parts(ident, message, module):
    """A retrieved message's parts: its texts, and its files saved."""
    folder = attachment_folder(ident)
    folder.mkdir(parents=True, exist_ok=True)
    os.chmod(folder, 0o700)
    texts, attachments = [], []
    for index, part in enumerate(message['parts']):
        kind = part['type']
        if kind == 'application/smil':
            continue
        if kind == 'text/plain':
            text = module.part_text(part).strip()
            if text:
                texts.append(text)
            continue
        name = re.sub(r'[^\w.\-]+', '_', Path(part['name'] or '').name).strip('._')
        if not name:
            name = f'part{index}' + (mimetypes.guess_extension(kind) or '')
        elif not Path(name).suffix:
            name += mimetypes.guess_extension(kind) or ''
        path = folder / name
        n = 1
        while path.exists():
            path = folder / f'{Path(name).stem} ({n}){Path(name).suffix}'
            n += 1
        path.write_bytes(part['data'])
        os.chmod(path, 0o600)
        attachments.append({'path': str(path), 'type': kind, 'name': path.name, 'size': len(part['data'])})
    return texts, attachments


def fetch_mms(ident):
    """Download a picture message; its row when there is news for the user
    (it came, or the last try failed), else None."""
    module = mms_module()
    db = database()
    try:
        item = db.execute('SELECT * FROM messages WHERE id = ?', (ident,)).fetchone()
        if item is None or item['kind'] != 'mms' or item['state'] not in ('downloading', 'download-failed'):
            return None
        info = json.loads(item['mms'] or '{}')
        now = time.time()
        try:
            if module is None:
                raise RuntimeError('omarchy-mobile-mms is not installed')
            if info.get('expiry') and info['expiry'] < now:
                raise TimeoutError('The message expired before it was downloaded')
            message = module.retrieve(info['location'])
            texts, attachments = save_parts(ident, message, module)
        except (OSError, ValueError, RuntimeError, TimeoutError) as error:
            expired = isinstance(error, TimeoutError)
            info.update(tries=info.get('tries', 0) + 1, tried=now)
            db.execute('UPDATE messages SET state = ?, error = ?, mms = ?, updated = ? WHERE id = ?',
                       ('expired' if expired else 'download-failed', str(error), json.dumps(info), time.time(), ident))
            db.commit()
            print(f'messages: picture message {ident} not downloaded: {error}', file=sys.stderr)
            final = expired or info['tries'] >= MMS_TRIES
            return row(db.execute('SELECT * FROM messages WHERE id = ?', (ident,)).fetchone()) if final else None
        subject = message.get('subject') or info.get('subject') or ''
        if subject and subject not in texts:
            texts.insert(0, subject)
        info['group'] = sorted({a for a in message.get('to', []) + message.get('cc', []) if a})
        db.execute("UPDATE messages SET body = ?, attachments = ?, state = 'received', error = '', mms = ?, "
                   "updated = ? WHERE id = ?",
                   ('\n'.join(texts), json.dumps(attachments), json.dumps(info), time.time(), ident))
        db.commit()
        try:
            module.acknowledge(info['transaction'])
        except (OSError, ValueError, RuntimeError) as error:
            print(f'messages: the MMS server was not told of {ident}: {error}', file=sys.stderr)
        return row(db.execute('SELECT * FROM messages WHERE id = ?', (ident,)).fetchone())
    finally:
        db.close()


class Downloads:
    """Picture messages being fetched, each on a thread of its own, so the
    watcher never waits on the network."""

    def __init__(self):
        self.running = set()
        self.done = queue.Queue()

    def start(self, db):
        now = time.time()
        for item in db.execute("SELECT id, state, mms FROM messages WHERE kind = 'mms' "
                               "AND state IN ('downloading', 'download-failed')").fetchall():
            if item['id'] in self.running:
                continue
            info = json.loads(item['mms'] or '{}')
            tries = info.get('tries', 0)
            if item['state'] == 'download-failed' and (tries >= MMS_TRIES or now - info.get('tried', 0) < MMS_RETRY * tries):
                continue
            self.running.add(item['id'])
            threading.Thread(target=self.fetch, args=(item['id'],), daemon=True).start()

    def fetch(self, ident):
        try:
            result = fetch_mms(ident)
        except Exception as error:      # a bug must not end the watcher
            print(f'messages: picture message {ident}: {error}', file=sys.stderr)
            result = None
        self.done.put((ident, result))

    def finished(self):
        found = []
        while True:
            try:
                ident, result = self.done.get_nowait()
            except queue.Empty:
                return found
            self.running.discard(ident)
            if result is not None:
                found.append(result)


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


def shrink(source, budget, folder, index):
    """A file to send: as it is when it fits, a picture made smaller (a
    JPEG, by ffmpeg) when it does not. (name, type, bytes)."""
    source = Path(source)
    data = source.read_bytes()
    kind = mimetypes.guess_type(source.name)[0] or 'application/octet-stream'
    if len(data) <= budget:
        return source.name, kind, data
    if not kind.startswith('image/') or not shutil.which('ffmpeg'):
        raise ValueError(f'{source.name} is too large for a picture message')
    target = folder / f'.shrink{index}.jpg'
    try:
        for edge in (1600, 1280, 1024, 800, 640, 480):
            for quality in (3, 6, 10):
                subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(source), '-frames:v', '1', '-vf',
                                f"scale='min({edge},iw)':'min({edge},ih)':force_original_aspect_ratio=decrease",
                                '-q:v', str(quality), str(target)], check=True, capture_output=True, timeout=60)
                out = target.read_bytes()
                if len(out) <= budget:
                    return source.stem + '.jpg', 'image/jpeg', out
    finally:
        target.unlink(missing_ok=True)
    raise ValueError(f'{source.name} could not be made small enough')


def transmit_mms(db, ident, number, text, parts):
    """Post a picture message to the carrier's server and record the result."""
    module = mms_module()
    if module is None:
        raise RuntimeError('omarchy-mobile-mms is not installed')
    try:
        message_id = module.send([number], text, parts)
    except module.Uncertain as error:
        state, failure = 'unknown', 'Delivery uncertain; check with the recipient before sending again'
        print(f'messages: picture message {ident}: {error}', file=sys.stderr)
    except (OSError, ValueError, RuntimeError) as error:
        state, failure = 'failed', str(error)
    else:
        state, failure = 'sent', ''
        info = json.loads(db.execute('SELECT mms FROM messages WHERE id = ?', (ident,)).fetchone()[0] or '{}')
        info['id'] = message_id
        db.execute('UPDATE messages SET mms = ? WHERE id = ?', (json.dumps(info), ident))
    db.execute('UPDATE messages SET state = ?, error = ?, updated = ? WHERE id = ?', (state, failure, time.time(), ident))
    db.commit()
    if state != 'sent':
        raise RuntimeError(failure or 'Could not send the message')


def send_mms(db, number, text, files):
    """A picture message: its files kept beside the database, then sent."""
    number = normalize(number)
    text = text.replace('\r\n', '\n').strip()
    if not files:
        raise ValueError('A picture message needs a picture')
    if len(files) > 10:
        raise ValueError('Ten pictures at most')
    for name in files:
        if not Path(name).is_file():
            raise ValueError(f'{Path(name).name} is not there')
    ident = insert(db, number, 'out', text, time.time(), 'sending', True, kind='mms', mms='{}')
    folder = attachment_folder(ident)
    folder.mkdir(parents=True, exist_ok=True)
    os.chmod(folder, 0o700)
    budget = (MMS_LIMIT - len(text.encode()) - 2048) // len(files)
    parts, attachments = [], []
    try:
        for index, name in enumerate(files):
            title, kind, data = shrink(name, budget, folder, index)
            path = folder / title
            n = 1
            while path.exists():
                path = folder / f'{Path(title).stem} ({n}){Path(title).suffix}'
                n += 1
            path.write_bytes(data)
            os.chmod(path, 0o600)
            parts.append((path.name, kind, data))
            attachments.append({'path': str(path), 'type': kind, 'name': path.name, 'size': len(data)})
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        db.execute("UPDATE messages SET state = 'failed', error = ?, attachments = ?, updated = ? WHERE id = ?",
                   (str(error), json.dumps(attachments), time.time(), ident))
        db.commit()
        raise ValueError(str(error)) from error
    db.execute('UPDATE messages SET attachments = ?, updated = ? WHERE id = ?', (json.dumps(attachments), time.time(), ident))
    db.commit()
    transmit_mms(db, ident, number, text, parts)
    return row(db.execute('SELECT * FROM messages WHERE id = ?', (ident,)).fetchone())


def ask_shell(request):
    """A line on the shell's request FIFO; False when no shell reads it."""
    base = os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}'
    try:
        fd = os.open(Path(base) / 'omarchy-mobile-shell.fifo', os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    except OSError:
        return False
    try:
        return os.write(fd, (request + '\n').encode()) > 0
    except OSError:
        return False
    finally:
        os.close(fd)


def capture():
    """The camera, in its capture mode, over Messages: the picture kept
    arrives at the returned path."""
    base = os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}'
    folder = Path(base) / 'omarchy-mobile-capture'
    folder.mkdir(mode=0o700, exist_ok=True)
    path = folder / f'photo-{int(time.time() * 1000)}.jpg'
    if not ask_shell(f'over camera capture:{path}'):
        launcher = shutil.which('omarchy-mobile-app') or str(Path.home() / '.local/bin/omarchy-mobile-app')
        subprocess.Popen([launcher, 'launch', 'camera', f'capture:{path}'], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)
    return str(path)


def photos(count=30):
    """The camera roll's newest pictures."""
    folder = Path(os.environ.get('XDG_PICTURES_DIR') or Path.home() / 'Pictures') / 'Camera'
    found = [p for p in folder.glob('*') if p.suffix.lower() in ('.jpg', '.jpeg', '.png') and p.is_file()]
    found.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return [str(p) for p in found[:count]]


def retry(db, ident):
    item = db.execute("SELECT * FROM messages WHERE id = ? AND direction = 'out' AND state = 'failed'",
                      (ident,)).fetchone()
    if item is None:
        raise ValueError('Only a failed message can be sent again')
    db.execute("UPDATE messages SET state = 'sending', error = '', time = ?, sent = ?, updated = ? WHERE id = ?",
               (time.time(), time.time(), time.time(), ident))
    if item['kind'] == 'mms':
        db.commit()
        parts = [(a['name'], a['type'], Path(a['path']).read_bytes()) for a in json.loads(item['attachments'] or '[]')]
        transmit_mms(db, ident, item['number'], item['body'], parts)
        return row(db.execute('SELECT * FROM messages WHERE id = ?', (ident,)).fetchone())
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


def gvariant_string(text):
    """A string in GVariant's text format, for gdbus."""
    return "'" + text.replace('\\', '\\\\').replace("'", "\\'").replace('\n', '\\n') + "'"


def notify(message):
    """A received text becomes a notification: Open shows the thread, Reply
    answers it from the notification (the server's inline reply), and Mark as
    read files it."""
    if not shutil.which('gdbus'):
        return 0
    who = person(message['number'])
    text = message.get('preview') or message['body']
    body = text if len(text) <= 240 else text[:239] + '…'
    hints = ("{'urgency': <byte 1>, 'category': <'im.received'>, 'desktop-entry': <'messages'>, "
             "'x-kde-reply-placeholder-text': <" + gvariant_string('Text ' + who['name']) + ">}")
    try:
        result = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications',
                                 '--object-path', '/org/freedesktop/Notifications',
                                 '--method', 'org.freedesktop.Notifications.Notify', '--',
                                 'Messages', '0', 'mail-unread', who['name'], body,
                                 "['open', 'Open', 'inline-reply', 'Reply', 'read', 'Mark as read']", hints, '-1'],
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
                process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                           preexec_fn=die_with_parent)
            except OSError:
                continue
            self.processes[name] = process
            self.tails[name] = ''
            self.selector.register(process.stdout, selectors.EVENT_READ, name)

    def wait(self, timeout):
        """Returns (modem signal seen, [(notification id, what)]), what being
        'open', 'read' or ('reply', text)."""
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
                            opened.append((int(match.group(1)), 'open'))
                        elif match.group(2) == 'read':
                            opened.append((int(match.group(1)), 'read'))
                    # The server's inline reply (KDE's NotificationReplied
                    # signal), the text in GVariant's text format.
                    for match in re.finditer(r"""NotificationReplied \(uint32 (\d+), ('(?:[^'\\]|\\.)*'|"(?:[^"\\]|\\.)*")\)""",
                                             complete):
                        try:
                            text = ast.literal_eval(match.group(2))
                        except (ValueError, SyntaxError):
                            continue
                        opened.append((int(match.group(1)), ('reply', text)))


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
    downloads = Downloads()
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
            # Signals do the work; a slow poll covers anything missed.
            next_modem = time.monotonic() + (1 if backend else 60)
        if leader:
            # Picture messages are told of once they are here (or given up on).
            downloads.start(db)
            added += [m for m in downloads.finished() if not m['read']]
            for message in added:
                ident = notify(message)
                if ident:
                    notified[ident] = (message['thread'], message['number'])
        current = version(db)
        if current != last:
            emit({'ok': True, 'unread': unread(db), 'version': current,
                  'new': [dict(m, **person(m['number'])) for m in added]})
            last = current
        # Sent texts waiting for a delivery report are checked every 20 s.
        waiting = leader and db.execute("SELECT 1 FROM messages WHERE modem != '' LIMIT 1").fetchone()
        timeout = 1.0 if backend or downloads.running else 20.0 if waiting else (60.0 if leader else 1.0)
        hit, opened = monitors.wait(timeout)
        pending = hit
        for ident, what in opened:
            if ident not in notified:
                continue
            key, number = notified.pop(ident)
            if what == 'open':
                launch_app(key)
                continue
            if isinstance(what, tuple) and what[1].strip():
                try:
                    send(db, number, what[1])
                except (ValueError, RuntimeError, OSError, subprocess.SubprocessError, sqlite3.Error) as error:
                    print(f'messages: the reply to {key} was not sent: {error}', file=sys.stderr)
            # A reply or Mark as read files the conversation as read.
            db.execute('UPDATE messages SET read = 1, updated = ? WHERE thread = ? AND read = 0', (time.time(), key))
            db.commit()


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
        elif action == 'send-mms' and len(args) >= 4:
            result = {'ok': True, 'message': send_mms(db, args[1], args[2], args[3:])}
        elif action == 'capture' and len(args) == 1:
            result = {'ok': True, 'path': capture()}
        elif action == 'photos' and len(args) == 1:
            result = {'ok': True, 'photos': photos()}
        elif action == 'retry' and len(args) == 2:
            result = {'ok': True, 'message': retry(db, int(args[1]))}
        elif action == 'download' and len(args) == 2:
            item = db.execute("SELECT mms FROM messages WHERE id = ? AND kind = 'mms' "
                              "AND state IN ('download-failed', 'expired')", (int(args[1]),)).fetchone()
            if item is None:
                raise ValueError('There is no picture message to download')
            info = dict(json.loads(item['mms'] or '{}'), tries=0, tried=0)
            db.execute("UPDATE messages SET state = 'downloading', error = '', mms = ?, updated = ? WHERE id = ?",
                       (json.dumps(info), time.time(), int(args[1])))
            db.commit()
            result = {'ok': True}
        elif action == 'delete' and len(args) == 2:
            shutil.rmtree(attachment_folder(args[1]), ignore_errors=True)
            db.execute('DELETE FROM messages WHERE id = ?', (int(args[1]),))
            db.execute('UPDATE messages SET updated = ? WHERE id = (SELECT MAX(id) FROM messages)', (time.time(),))
            db.commit()
            result = {'ok': True}
        elif action == 'delete-thread' and len(args) == 2:
            for item in db.execute("SELECT id FROM messages WHERE thread = ? AND kind = 'mms'", (args[1],)).fetchall():
                shutil.rmtree(attachment_folder(item['id']), ignore_errors=True)
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
