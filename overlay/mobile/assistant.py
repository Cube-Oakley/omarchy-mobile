#!/usr/bin/env python3
"""Wisp, the phone's assistant: conversations with a language model that runs
on the phone, by text or by voice, with what it remembers about the user.

  status                      {model, models, server, voice, settings}
  conversations [QUERY]       recent conversations, or those whose title or
                              text match QUERY (with a snippet, the matches
                              between \x01 and \x02)
  messages ID                 one conversation's messages
  send [ID] [--image PATH]    the user's message on stdin, to conversation ID
                              (a new one without): JSON lines {"conversation":
                              ID, "user": MESSAGE_ID}, {"delta": TEXT} as the
                              reply comes, then {"done": true, "id": ID,
                              "text": REPLY}, or {"error": TEXT}
  rename ID TITLE | delete ID
  memories                    what Wisp remembers about the user
  remember TEXT | forget ID
  settings                    the settings; set KEY VALUE changes one
  say                         speak the text on stdin, then exit
  voice status|install|remove the voice (Kokoro, through kokoro-pi)
  voice serve                 speak JSON lines from stdin, the voice kept
                              loaded ({"say": TEXT}, {"stop": true})
  models                      the models and which are installed
  model install NAME          download a model (checked against its SHA-256)
  capture                     the camera over Wisp: {"path"} where its picture comes
  photos                      the camera roll's newest pictures
  serve                       (internal) run the model server until idle
  server stop                 stop the model server now

The model is served by llama.cpp's llama-server on 127.0.0.1, started by the
first message and stopped after SERVER_IDLE seconds without one, so it holds
no memory or CPU while Wisp is not in use. Everything stays on the phone: the
conversations in DATA/assistant.db, the models and the voice in DATA. The
phone stays awake while Wisp answers or speaks (an inhibit file).
"""
import base64
import fcntl
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import sqlite3
import subprocess
import sys
import time
import urllib.error
import urllib.request

DATA = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share') / 'omarchy-mobile/assistant'
CONFIG = Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config') / 'omarchy-mobile/assistant.json'
RUNTIME = Path(os.environ.get('XDG_RUNTIME_DIR') or '/tmp')
DB = DATA / 'assistant.db'
MODELS = DATA / 'models'
KOKORO = DATA / 'kokoro'
# Earlier voices (Piper's venv; Moonshine's engine): removed with the voice.
OLD_VOICES = (DATA / 'tts', DATA / 'voices')
PORT = 39281
SERVER_IDLE = 600
STAMP = RUNTIME / 'omarchy-mobile-assistant.used'
SERVER_LOCK = RUNTIME / 'omarchy-mobile-assistant.lock'
INHIBIT = RUNTIME / 'omarchy-mobile-sleep-inhibit'
NAME = 'Wisp'
# The models Wisp can run, from their makers' own GGUF repositories. Each
# takes pictures too (its mmproj). Measured on the Pixel 7 Pro (October 7):
# the 2B writes 12.5 tokens a second and reads 83, in 3.5 GB; the 4B 5.6 and
# 31, in 5.5 GB. A picture takes the 2B about 25 s.
CATALOG = {
    'qwen3-vl-2b': {
        'name': 'Qwen3-VL 2B', 'detail': 'Quicker: about 12 tokens a second', 'repo': 'Qwen/Qwen3-VL-2B-Instruct-GGUF',
        'file': 'Qwen3VL-2B-Instruct-Q4_K_M.gguf', 'size': 1107409952,
        'sha256': '089d75c52f4b7ffc56ba998ffc50aae89fcafc755f9e7208aacca281dca6c2ae',
        'mmproj': 'mmproj-Qwen3VL-2B-Instruct-Q8_0.gguf', 'mmproj_size': 445053216,
        'mmproj_sha256': 'f9a68fabba69c3b81e153367b2c7521030b0fa8bb0de400c9599c8e6725f9c82',
    },
    'qwen3-vl-4b': {
        'name': 'Qwen3-VL 4B', 'detail': 'Smarter, slower: about 5 tokens a second', 'repo': 'Qwen/Qwen3-VL-4B-Instruct-GGUF',
        'file': 'Qwen3VL-4B-Instruct-Q4_K_M.gguf', 'size': 2497281664,
        'sha256': '66358cb18bb6b3b1b6675aa412c7a88ef01d228f481184d13668e5201c730a0a',
        'mmproj': 'mmproj-Qwen3VL-4B-Instruct-Q8_0.gguf', 'mmproj_size': 453974304,
        'mmproj_sha256': '30ba2c7dd3127a4561b6cba9d13d0f711c91bdb38742e2f56d73c8cb596bd06d',
    },
}
# The voices Settings offers (Kokoro has more): its best rated English ones.
VOICES = {
    'kokoro_af_heart': 'Heart', 'kokoro_af_bella': 'Bella', 'kokoro_af_nicole': 'Nicole',
    'kokoro_af_aoede': 'Aoede', 'kokoro_af_kore': 'Kore', 'kokoro_af_sarah': 'Sarah',
    'kokoro_am_michael': 'Michael', 'kokoro_am_fenrir': 'Fenrir', 'kokoro_am_puck': 'Puck',
}
# kokoro-pi and kokoro-onnx (installed without their Python version pins),
# then what they use; onnx builds the int8 model.
KOKORO_PACKAGES = ['kokoro-pi==1.2.0', 'kokoro-onnx==0.4.9']
KOKORO_DEPENDENCIES = ['onnxruntime==1.30.0', 'numpy==2.5.3', 'espeakng-loader==0.2.4', 'phonemizer-fork==3.3.2',
                       'colorlog==6.12.0', 'onnx==1.23.2']
DEFAULTS = {'model': 'qwen3-vl-2b', 'speak': False, 'auto_memory': True, 'voice': 'kokoro_af_heart'}
# History sent with each message: the newest messages up to about this many
# characters (the context is 8192 tokens; the reply and the system text need
# room).
HISTORY_CHARS = 12000
CONTEXT = 8192
# Image tokens per picture: a camera photo at the model's own limit took
# minutes; this many keeps one to about 15 s on the 2B and is enough to tell
# what is in it.
IMAGE_TOKENS = 384


def emit(**fields):
    print(json.dumps(fields), flush=True)


def fail(text):
    emit(error=text)
    return 1


# Settings

def settings():
    try:
        stored = json.loads(CONFIG.read_text())
    except (OSError, ValueError):
        stored = {}
    return {**DEFAULTS, **{k: v for k, v in stored.items() if k in DEFAULTS or k == 'key'}}


def save_settings(values):
    CONFIG.parent.mkdir(parents=True, exist_ok=True)
    temporary = CONFIG.with_suffix('.new')
    temporary.write_text(json.dumps(values, indent=2) + '\n')
    os.chmod(temporary, 0o600)
    os.replace(temporary, CONFIG)


def api_key():
    """The model server's key: other programs on the phone cannot use it."""
    values = settings()
    if not values.get('key'):
        values['key'] = secrets.token_hex(16)
        save_settings(values)
    return values['key']


def set_setting(key, value):
    values = settings()
    if key == 'model':
        if value not in CATALOG:
            raise ValueError(f'unknown model {value}')
        values['model'] = value
    elif key == 'voice':
        if value not in VOICES:
            raise ValueError(f'unknown voice {value}')
        values['voice'] = value
    elif key in ('speak', 'auto_memory'):
        if value not in ('true', 'false'):
            raise ValueError(f'{key} is true or false')
        values[key] = value == 'true'
    else:
        raise ValueError(f'unknown setting {key}')
    save_settings(values)
    if key == 'model':
        stop_server()
    return values


# Storage

def connect():
    DATA.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(DB, timeout=10)
    db.row_factory = sqlite3.Row
    db.execute('PRAGMA foreign_keys = ON')
    db.executescript('''
        CREATE TABLE IF NOT EXISTS conversations (
            id INTEGER PRIMARY KEY, title TEXT NOT NULL DEFAULT '',
            created REAL NOT NULL, updated REAL NOT NULL);
        CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY,
            conversation INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
            role TEXT NOT NULL, content TEXT NOT NULL, image TEXT NOT NULL DEFAULT '',
            created REAL NOT NULL);
        CREATE INDEX IF NOT EXISTS messages_by_conversation ON messages(conversation, id);
        CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5(
            content, content='messages', content_rowid='id');
        CREATE TRIGGER IF NOT EXISTS messages_ai AFTER INSERT ON messages BEGIN
            INSERT INTO messages_fts(rowid, content) VALUES (new.id, new.content); END;
        CREATE TRIGGER IF NOT EXISTS messages_ad AFTER DELETE ON messages BEGIN
            INSERT INTO messages_fts(messages_fts, rowid, content) VALUES ('delete', old.id, old.content); END;
        CREATE TRIGGER IF NOT EXISTS messages_au AFTER UPDATE ON messages BEGIN
            INSERT INTO messages_fts(messages_fts, rowid, content) VALUES ('delete', old.id, old.content);
            INSERT INTO messages_fts(rowid, content) VALUES (new.id, new.content); END;
        CREATE TABLE IF NOT EXISTS memories (
            id INTEGER PRIMARY KEY, text TEXT NOT NULL, created REAL NOT NULL,
            source TEXT NOT NULL DEFAULT 'user');
    ''')
    os.chmod(DB, 0o600)
    return db


def fts_query(text):
    """Words of a search as FTS5 prefix terms (no FTS syntax from the user)."""
    words = re.findall(r'\w+', text.lower())
    return ' '.join(f'"{w}"*' for w in words)


def list_conversations(db, query=''):
    if query.strip():
        terms = fts_query(query)
        like = f'%{query.strip()}%'
        rows = db.execute('''
            SELECT c.id, c.title, c.updated,
                   (SELECT snippet(messages_fts, 0, char(1), char(2), '…', 10) FROM messages_fts
                     JOIN messages m ON m.id = messages_fts.rowid
                     WHERE messages_fts MATCH ? AND m.conversation = c.id LIMIT 1) AS snippet
            FROM conversations c
            WHERE c.title LIKE ? OR c.id IN (
                SELECT m.conversation FROM messages_fts JOIN messages m ON m.id = messages_fts.rowid
                WHERE messages_fts MATCH ?)
            ORDER BY c.updated DESC LIMIT 100''', (terms, like, terms)).fetchall() if terms else []
    else:
        rows = db.execute('''
            SELECT c.id, c.title, c.updated,
                   (SELECT content FROM messages WHERE conversation = c.id ORDER BY id DESC LIMIT 1) AS snippet
            FROM conversations c ORDER BY c.updated DESC LIMIT 100''').fetchall()
    return [{'id': r['id'], 'title': r['title'], 'updated': r['updated'],
             'snippet': (r['snippet'] or '')[:160]} for r in rows]


def conversation_messages(db, conversation):
    rows = db.execute('SELECT id, role, content, image, created FROM messages WHERE conversation = ? ORDER BY id',
                      (conversation,)).fetchall()
    return [dict(r) for r in rows]


def add_message(db, conversation, role, content, image=''):
    now = time.time()
    cursor = db.execute('INSERT INTO messages (conversation, role, content, image, created) VALUES (?, ?, ?, ?, ?)',
                        (conversation, role, content, image, now))
    db.execute('UPDATE conversations SET updated = ? WHERE id = ?', (now, conversation))
    db.commit()
    return cursor.lastrowid


def new_conversation(db, first_message):
    now = time.time()
    title = re.sub(r'\s+', ' ', first_message).strip() or 'Picture'
    if len(title) > 48:
        title = title[:47].rsplit(' ', 1)[0] + '…'
    cursor = db.execute('INSERT INTO conversations (title, created, updated) VALUES (?, ?, ?)', (title, now, now))
    db.commit()
    return cursor.lastrowid


def list_memories(db):
    return [dict(r) for r in db.execute('SELECT id, text, created, source FROM memories ORDER BY id')]


def add_memory(db, text, source='user'):
    text = re.sub(r'\s+', ' ', text).strip().rstrip('.') + '.'
    if len(text) < 3:
        return None
    key = text.lower()
    for memory in list_memories(db):
        old = memory['text'].lower()
        if key in old or old in key:
            return None
    cursor = db.execute('INSERT INTO memories (text, created, source) VALUES (?, ?, ?)', (text, time.time(), source))
    db.commit()
    return cursor.lastrowid


REMEMBER = re.compile(r'^\s*(?:please\s+)?remember(?:\s+that)?\s+(.{3,})$', re.I | re.S)


def explicit_memory(text):
    """'remember that X' asks the assistant to keep X."""
    match = REMEMBER.match(text)
    return match.group(1).strip() if match else ''


# Prompts

def system_prompt(memories, now=None):
    now = time.localtime(now)
    lines = [
        f'You are {NAME}, an assistant that runs entirely on the user\'s phone, offline.',
        'What you can do: talk, answer questions from what you already know, explain things, brainstorm,',
        'help write or rewrite text, remember what the user tells you, and describe pictures they share.',
        'What you cannot do yet: anything on the phone itself (messages, calls, alarms, calendar, contacts,',
        'apps, settings) or anything online. Do not offer those. Asked to set an alarm, for example, say',
        'that you cannot set alarms yet and that the Clock app can.',
        'Be warm, direct and brief: a few sentences unless asked for more. Plain text; short lists are fine.',
        f'It is {time.strftime("%A, %B %-d, %Y, %-I:%M %p", now)}.',
    ]
    if memories:
        lines.append('What you know about the user (use it when it helps; do not recite it):')
        lines += [f'- {m["text"]}' for m in memories]
    return '\n'.join(lines)


def build_messages(history, memories, user_text, image_url='', now=None):
    """The chat for the model: the system text, the newest history that fits,
    and the user's message (with its picture)."""
    chosen = []
    used = 0
    for message in reversed(history):
        text = message['content'] + (' (The user shared a picture here.)' if message.get('image') else '')
        if used + len(text) > HISTORY_CHARS and chosen:
            break
        chosen.append({'role': message['role'], 'content': text})
        used += len(text)
    chosen.reverse()
    content = user_text
    if image_url:
        content = [{'type': 'image_url', 'image_url': {'url': image_url}},
                   {'type': 'text', 'text': user_text or 'What is in this picture?'}]
    return [{'role': 'system', 'content': system_prompt(memories, now)}] + chosen + [{'role': 'user', 'content': content}]


MEMORY_PROMPT = (
    'Below is something the user said to their phone assistant. If it contains a lasting fact about the '
    'user that would help in future conversations (their name, family, pets, home town, job, preferences, '
    'plans, health needs), reply with that fact as one short sentence starting with "The user". '
    'Reply exactly NONE for questions, small talk, requests or anything temporary.\n\nThe user said: ')


# Something said about oneself has one of these; anything else is not asked.
# Not "me" or "us": "give me three ideas" is a request.
FIRST_PERSON = re.compile(r"\b(i|i'm|im|i've|i'd|i'll|my|mine|myself|we|we're|our)\b", re.I)
# What the 2B says when it has no fact but answers anyway ("The user is
# asking about a folklore myth, not a personal fact.", "The user is likely
# looking for enjoyable, indoor activities ...").
NOT_A_FACT = re.compile(r'\b(ask|asks|asking|asked|question|wants to know|curious|not a|no (?:lasting|personal)|'
                        r'likely|probably|possibly|perhaps|seems|appears|may be|might|looking for)\b', re.I)


def memory_candidate(reply):
    reply = reply.strip().splitlines()[0].strip() if reply.strip() else ''
    if (not reply or reply.upper().startswith('NONE') or not reply.lower().startswith('the user')
            or NOT_A_FACT.search(reply)):
        return ''
    return reply[:200]


# The model server

def inhibit(name, on):
    try:
        INHIBIT.mkdir(parents=True, exist_ok=True)
        path = INHIBIT / name
        if on:
            # With this process's ID: it holds only while this lives.
            path.write_text(f'{os.getpid()}\n')
        else:
            path.unlink(missing_ok=True)
    except OSError:
        pass


def server_binary():
    for candidate in (DATA / 'bin/llama-server', shutil.which('llama-server')):
        if candidate and os.access(candidate, os.X_OK):
            return str(candidate)
    return ''


def model_files(name):
    entry = CATALOG[name]
    return MODELS / entry['file'], MODELS / entry['mmproj']


def installed(name):
    model, mmproj = model_files(name)
    entry = CATALOG[name]
    return (model.is_file() and model.stat().st_size == entry['size']
            and mmproj.is_file() and mmproj.stat().st_size == entry['mmproj_size'])


def request(path, body=None, timeout=10, stream=False):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(f'http://127.0.0.1:{PORT}{path}', data=data,
                                 headers={'Content-Type': 'application/json',
                                          'Authorization': f'Bearer {api_key()}'})
    response = urllib.request.urlopen(req, timeout=timeout)
    return response if stream else json.load(response)


def server_up():
    try:
        return request('/health', timeout=2).get('status') == 'ok'
    except (OSError, ValueError):
        return False


def online_cpus():
    try:
        text = Path('/sys/devices/system/cpu/online').read_text().strip()
    except OSError:
        return set()
    cpus = set()
    for part in text.split(','):
        low, _, high = part.partition('-')
        cpus.update(range(int(low), int(high or low) + 1))
    return cpus


def thread_options():
    """Generation on the two big cores (CPUs 6-7, fastest per token), the
    prompt on big and middle (4-7); with those offline (a dark screen), on the
    little ones."""
    cpus = online_cpus()
    if {6, 7} <= cpus:
        batch = '0xf0' if {4, 5} <= cpus else '0xc0'
        return ['-t', '2', '-C', '0xc0', '-tb', '4' if batch == '0xf0' else '2', '-Cb', batch]
    return ['-t', '4', '-tb', '4']


def serve():
    """Run llama-server until it has been idle SERVER_IDLE seconds."""
    SERVER_LOCK.parent.mkdir(parents=True, exist_ok=True)
    lock = open(SERVER_LOCK, 'w')
    # One server: a stopping one (its model takes a while to let go) is waited for.
    deadline = time.time() + 60
    while True:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except OSError:
            if time.time() > deadline or server_up():
                return 0
            time.sleep(0.5)
    name = settings()['model']
    model, mmproj = model_files(name)
    binary = server_binary()
    if not binary or not installed(name):
        return 1
    log = open(DATA / 'server.log', 'w')
    child = subprocess.Popen([binary, '-m', str(model), '--mmproj', str(mmproj), '--host', '127.0.0.1',
                              '--port', str(PORT), '--api-key', api_key(), '-c', str(CONTEXT), '-np', '1',
                              '--no-webui', '--jinja', '--image-max-tokens', str(IMAGE_TOKENS)] + thread_options(),
                             stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    def stop(*_):
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
    signal.signal(signal.SIGTERM, stop)
    STAMP.touch()
    while child.poll() is None:
        try:
            idle = time.time() - STAMP.stat().st_mtime
        except OSError:
            idle = SERVER_IDLE
        if idle >= SERVER_IDLE:
            child.terminate()
            break
        time.sleep(10)
    try:
        child.wait(timeout=10)
    except subprocess.TimeoutExpired:
        child.kill()
    return 0


def ensure_server(timeout=120):
    STAMP.parent.mkdir(parents=True, exist_ok=True)
    STAMP.touch()
    if server_up():
        return True
    if not server_binary():
        raise RuntimeError('The model server (llama-server) is not installed')
    if not installed(settings()['model']):
        raise RuntimeError(f'The model is not downloaded yet (Settings in {NAME})')
    subprocess.Popen([sys.executable, os.path.abspath(__file__), 'serve'], start_new_session=True,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if server_up():
            return True
        time.sleep(0.5)
    raise RuntimeError('The model server did not start (see server.log)')


def server_pids():
    """The supervisor and its llama-server."""
    pids = []
    for pattern in (f'^{re.escape(sys.executable)} {re.escape(os.path.abspath(__file__))} serve',
                    f'^{re.escape(str(DATA))}/bin/llama-server '):
        try:
            pids += [int(p) for p in subprocess.run(['pgrep', '-f', pattern], capture_output=True,
                                                    text=True).stdout.split()]
        except (OSError, ValueError):
            pass
    return pids


def stop_server(wait=15):
    """Stops the server, and waits for it to be gone (a new model cannot load
    beside it)."""
    for pid in server_pids():
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
    deadline = time.time() + wait
    while server_pids() and time.time() < deadline:
        time.sleep(0.3)
    for pid in server_pids():
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


def stream_reply(messages, max_tokens=1024):
    """The model's reply, piece by piece."""
    body = {'messages': messages, 'stream': True, 'max_tokens': max_tokens, 'temperature': 0.7,
            'cache_prompt': True}
    with request('/v1/chat/completions', body, timeout=600, stream=True) as response:
        for raw in response:
            line = raw.decode('utf-8', 'replace').strip()
            if not line.startswith('data:'):
                continue
            data = line[5:].strip()
            if data == '[DONE]':
                return
            try:
                choice = json.loads(data)['choices'][0]
            except (ValueError, KeyError, IndexError):
                continue
            piece = (choice.get('delta') or {}).get('content') or ''
            if piece:
                yield piece
            STAMP.touch()


def ask_once(prompt, max_tokens=60):
    response = request('/v1/chat/completions', {'messages': [{'role': 'user', 'content': prompt}],
                                                'max_tokens': max_tokens, 'temperature': 0}, timeout=120)
    return response['choices'][0]['message']['content']


def image_url(path):
    data = Path(path).read_bytes()
    kind = 'png' if data[:8] == b'\x89PNG\r\n\x1a\n' else 'jpeg'
    return f'data:image/{kind};base64,' + base64.b64encode(data).decode()


def send(args):
    conversation = None
    image = ''
    rest = list(args)
    while rest:
        item = rest.pop(0)
        if item == '--image' and rest:
            image = rest.pop(0)
        elif item.isdigit():
            conversation = int(item)
        else:
            return fail(f'unknown argument {item}')
    text = sys.stdin.read().strip()
    if not text and not image:
        return fail('Nothing to send')
    if image and not Path(image).is_file():
        return fail('The picture is gone')
    if image:
        image = keep_picture(image)
    db = connect()
    if conversation is None or not db.execute('SELECT 1 FROM conversations WHERE id = ?', (conversation,)).fetchone():
        conversation = new_conversation(db, text)
    history = conversation_messages(db, conversation)
    user_id = add_message(db, conversation, 'user', text, image)
    emit(conversation=conversation, user=user_id)
    fact = explicit_memory(text)
    if fact:
        add_memory(db, f'The user asked you to remember: {fact}', 'user')
    inhibit('assistant', True)
    try:
        return answer(db, conversation, history, text, image, fact)
    finally:
        inhibit('assistant', False)


def answer(db, conversation, history, text, image, fact):
    reply = ''
    try:
        ensure_server()
        messages = build_messages(history, list_memories(db), text, image_url(image) if image else '')
        for piece in stream_reply(messages):
            reply += piece
            emit(delta=piece)
    except (OSError, RuntimeError, urllib.error.URLError) as error:
        if reply:
            add_message(db, conversation, 'assistant', reply.strip())
        return fail(str(error))
    reply = reply.strip()
    message_id = add_message(db, conversation, 'assistant', reply)
    emit(done=True, id=message_id, text=reply, conversation=conversation)
    # After the reply: anything lasting the user said, kept for next time.
    if text and not fact and settings()['auto_memory'] and len(text) > 12 and FIRST_PERSON.search(text):
        try:
            candidate = memory_candidate(ask_once(MEMORY_PROMPT + text))
            if candidate and add_memory(db, candidate, 'auto'):
                emit(memory=candidate)
        except (OSError, RuntimeError, urllib.error.URLError, KeyError, IndexError):
            pass
    return 0


# Voice: Kokoro (82M, Apache 2.0) through kokoro-pi, which runs the vocoder's
# convolutions in an int8 kernel for the CPU's dot-product instructions, in
# a venv of its own. Measured on the Pixel 7 Pro (October 8), 10 s of speech:
# 0.24 s per second of it on CPUs 4-7, 0.34 on the big two, and a short
# sentence in 0.4 s. Moonshine's engine (the one voice typing installs) ran
# the same voice at 0.8, too close to real time: a long sentence after a
# short one came late; sherpa-onnx's int8 build at 1.3, Kyutai's Pocket TTS
# at 8.

def voice_python():
    return KOKORO / 'venv/bin/python'


def voice_installed(voice=None):
    return voice_python().exists() and (KOKORO / 'models/models.json').exists() and \
        (KOKORO / 'models/kokoro-int8.onnx').exists()


def sha256(path):
    import hashlib
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def download(url, target, expected_sha, size=0, label=''):
    part = target.with_suffix(target.suffix + '.part')
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'OmarchyMobile-Assistant/1'}),
                                timeout=60) as response, open(part, 'wb') as out:
        total = size or int(response.headers.get('Content-Length') or 0)
        done = 0
        last = 0
        for block in iter(lambda: response.read(1 << 20), b''):
            out.write(block)
            done += len(block)
            if total and time.time() - last > 0.5:
                emit(progress=round(done / total, 3), file=label or target.name)
                last = time.time()
    if sha256(part) != expected_sha:
        part.unlink()
        raise RuntimeError(f'{target.name}: the download does not match its checksum')
    os.replace(part, target)


def voice_install(voice=None):
    """kokoro-pi and its models: Kokoro's export (177 MB) and voices (28 MB)
    from kokoro-onnx's release, checked against their SHA-256, from which it
    builds its int8 model here (a minute or two)."""
    if voice and voice not in VOICES:
        return fail(f'unknown voice {voice}')
    pip = [str(KOKORO / 'venv/bin/pip'), 'install', '--quiet', '--disable-pip-version-check']
    try:
        KOKORO.mkdir(parents=True, exist_ok=True)
        if not voice_python().exists():
            emit(state='Setting up')
            subprocess.run([sys.executable, '-m', 'venv', str(KOKORO / 'venv')], check=True, capture_output=True)
        emit(state='Installing the speech engine')
        # Both declare Python below 3.14 and pin each other; they run on it.
        subprocess.run(pip + ['--no-deps', '--ignore-requires-python'] + KOKORO_PACKAGES, check=True,
                       capture_output=True)
        subprocess.run(pip + KOKORO_DEPENDENCIES, check=True, capture_output=True)
        if not voice_installed():
            emit(state='Downloading and building the voice')
            models = KOKORO / 'models'
            subprocess.run([str(voice_python()), '-m', 'kokoro_pi', 'build', '--models', str(models),
                            '--threads', '4'], check=True, capture_output=True, timeout=3600)
            # Only the int8 variant is used.
            for name in ('kokoro-v1.0.fp16.onnx', 'kokoro-fused.onnx', 'calibration.npz'):
                (models / name).unlink(missing_ok=True)
    except (OSError, subprocess.SubprocessError) as error:
        detail = getattr(error, 'stderr', b'') or b''
        detail = detail.decode(errors='replace').strip().splitlines()[-1:] if isinstance(detail, bytes) else []
        return fail(f'Voice install failed: {detail[0] if detail else error}')
    emit(ok=True, voice=True)
    return 0


# The speaker, in kokoro-pi's venv. "once" speaks stdin and exits; "serve"
# keeps the voice loaded and speaks JSON lines from stdin as they come:
# {"say": TEXT} queues a piece, {"stop": true} drops what is queued and
# playing. It answers {"ready": true} once loaded and {"speaking": BOOL}
# around each burst, and exits at the end of stdin or after VOICE_IDLE
# seconds of quiet.
SPEAK = r"""
import json, os, queue, re, signal, subprocess, sys, threading
from pathlib import Path
root, voice, mode, inhibit, idle = Path(sys.argv[1]), sys.argv[2], sys.argv[3], Path(sys.argv[4]), float(sys.argv[5])
# Stopped (the app closed): the finally below lets the phone sleep again.
for number in (signal.SIGTERM, signal.SIGHUP):
    signal.signal(number, lambda *_: sys.exit(0))
# The middle and big cores when they are online (a dark screen takes them).
if {4, 5, 6, 7} <= os.sched_getaffinity(0):
    os.sched_setaffinity(0, {4, 5, 6, 7})
import numpy
from kokoro_pi.compat import Kokoro
voice = voice.removeprefix("kokoro_")
# Checked when it was built; hashing 188 MB at each start took a second.
tts = Kokoro(root, variant="int8", threads=min(4, len(os.sched_getaffinity(0))), voice=voice, verify=False)

def emit(**fields):
    print(json.dumps(fields), flush=True)

def clean(text):
    text = re.sub(r"[*_#`>|]+", " ", text)
    return re.sub(r"https?://\S+", "a link", text)

class Player:
    # pw-play for one burst of speech; the phone stays awake while it plays.
    process = None
    def write(self, samples, rate):
        if self.process is None:
            # With this process's ID: a killed speaker does not keep the
            # phone awake (omarchy-mobile-sleep drops the file).
            inhibit.write_text(str(os.getpid()))
            self.process = subprocess.Popen(["pw-play", "--raw", "--rate", str(rate), "--channels", "1",
                                             "--format", "s16", "-"], stdin=subprocess.PIPE)
            if mode == "serve":
                emit(speaking=True)
        try:
            self.process.stdin.write((numpy.clip(samples, -1, 1) * 32767).astype("<i2").tobytes())
            self.process.stdin.flush()
        except BrokenPipeError:
            self.close(drop=True)
    def close(self, drop=False):
        if self.process is None:
            return
        if drop:
            self.process.kill()
        else:
            try:
                self.process.stdin.close()
            except BrokenPipeError:
                pass
        self.process.wait()
        self.process = None
        inhibit.unlink(missing_ok=True)
        if mode == "serve":
            emit(speaking=False)

def pieces(text):
    # Sentences and clauses (180 characters at most): the first plays while
    # the next is made.
    return [piece for piece in tts.split_for_streaming(clean(text)) if re.search(r"\w", piece)]

player = Player()
try:
    if mode == "once":
        for piece in pieces(sys.stdin.read()):
            player.write(*tts.create(piece, voice=voice))
        player.close()
        sys.exit(0)
    pending = queue.Queue()
    generation = [0]
    def read():
        for line in sys.stdin:
            try:
                command = json.loads(line)
            except ValueError:
                continue
            if command.get("stop"):
                generation[0] += 1
                pending.put(None)
            elif command.get("say"):
                pending.put((generation[0], command["say"]))
        pending.put("end")
    threading.Thread(target=read, daemon=True).start()
    emit(ready=True)
    while True:
        try:
            # A pause between pieces (the model still writing) keeps the
            # stream; a longer one ends the burst.
            item = pending.get(timeout=3 if player.process else idle)
        except queue.Empty:
            if player.process is None:
                break
            player.close()
            continue
        if item == "end":
            player.close()
            break
        if item is None:
            player.close(drop=True)
            continue
        wanted, text = item
        for piece in pieces(text):
            if wanted != generation[0]:
                break
            samples, rate = tts.create(piece, voice=voice)
            if wanted != generation[0]:
                break
            player.write(samples, rate)
        if wanted != generation[0]:
            player.close(drop=True)
finally:
    inhibit.unlink(missing_ok=True)
"""
VOICE_IDLE = 300


def speak_command(mode):
    INHIBIT.mkdir(parents=True, exist_ok=True)
    return [str(voice_python()), '-c', SPEAK, str(KOKORO / 'models'), settings()['voice'], mode,
            str(INHIBIT / 'assistant-voice'), str(VOICE_IDLE)]


def say():
    if not voice_installed():
        return fail('The voice is not installed')
    child = subprocess.Popen(speak_command('once'), stdin=subprocess.PIPE)
    signal.signal(signal.SIGTERM, lambda *_: (child.kill(), sys.exit(0)))
    child.communicate(sys.stdin.read().encode())
    return child.returncode


def voice_serve():
    if not voice_installed():
        return fail('The voice is not installed')
    command = speak_command('serve')
    os.execv(command[0], command)


def model_install(name):
    if name not in CATALOG:
        return fail(f'unknown model {name}')
    entry = CATALOG[name]
    MODELS.mkdir(parents=True, exist_ok=True)
    try:
        for file, digest, size in ((entry['file'], entry['sha256'], entry['size']),
                                   (entry['mmproj'], entry['mmproj_sha256'], entry['mmproj_size'])):
            target = MODELS / file
            if target.is_file() and target.stat().st_size == size:
                continue
            download(f'https://huggingface.co/{entry["repo"]}/resolve/main/{file}', target, digest, size, file)
    except (OSError, RuntimeError, urllib.error.URLError) as error:
        return fail(f'Download failed: {error}')
    emit(ok=True, model=name)
    return 0


# Pictures

def ask_shell(request):
    """A line on the shell's request FIFO; False when no shell reads it."""
    try:
        fd = os.open(RUNTIME / 'omarchy-mobile-shell.fifo', os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    except OSError:
        return False
    try:
        return os.write(fd, (request + '\n').encode()) > 0
    except OSError:
        return False
    finally:
        os.close(fd)


def capture():
    """The camera, in its capture mode, over the assistant (as from Messages): the
    picture kept arrives at the returned path."""
    folder = RUNTIME / 'omarchy-mobile-capture'
    folder.mkdir(mode=0o700, exist_ok=True)
    path = folder / f'assistant-{int(time.time() * 1000)}.jpg'
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


def keep_picture(path):
    """A picture sent to the assistant, kept beside its conversations (a capture lives
    in the runtime directory, gone at the next boot)."""
    source = Path(path)
    if source.parent != RUNTIME / 'omarchy-mobile-capture':
        return str(source)
    folder = DATA / 'pictures'
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / source.name
    shutil.copyfile(source, target)
    return str(target)


def status():
    values = settings()
    return {'name': NAME, 'model': values['model'],
            'models': [{'id': k, 'name': v['name'], 'detail': v['detail'], 'installed': installed(k),
                        'size': v['size'] + v['mmproj_size']} for k, v in CATALOG.items()],
            'server': server_up(), 'engine': bool(server_binary()), 'voice': voice_installed(),
            'voices': [{'id': k, 'name': v, 'installed': voice_installed()} for k, v in VOICES.items()],
            'settings': {k: values[k] for k in DEFAULTS}}


def main(args):
    command = args[0] if args else 'status'
    rest = args[1:]
    try:
        if command == 'status':
            emit(**status())
        elif command == 'conversations':
            emit(conversations=list_conversations(connect(), ' '.join(rest)))
        elif command == 'messages' and len(rest) == 1 and rest[0].isdigit():
            emit(conversation=int(rest[0]), messages=conversation_messages(connect(), int(rest[0])))
        elif command == 'send':
            return send(rest)
        elif command == 'rename' and len(rest) >= 2 and rest[0].isdigit():
            db = connect()
            db.execute('UPDATE conversations SET title = ? WHERE id = ?', (' '.join(rest[1:])[:80], int(rest[0])))
            db.commit()
            emit(ok=True)
        elif command == 'delete' and len(rest) == 1 and rest[0].isdigit():
            db = connect()
            db.execute('DELETE FROM conversations WHERE id = ?', (int(rest[0]),))
            db.commit()
            emit(ok=True)
        elif command == 'memories':
            emit(memories=list_memories(connect()))
        elif command == 'remember' and rest:
            emit(ok=True, id=add_memory(connect(), ' '.join(rest), 'user'))
        elif command == 'forget' and len(rest) == 1 and rest[0].isdigit():
            db = connect()
            db.execute('DELETE FROM memories WHERE id = ?', (int(rest[0]),))
            db.commit()
            emit(ok=True)
        elif command == 'settings':
            emit(settings={k: settings()[k] for k in DEFAULTS})
        elif command == 'set' and len(rest) == 2:
            values = set_setting(*rest)
            emit(ok=True, settings={k: values[k] for k in DEFAULTS})
        elif command == 'say':
            return say()
        elif command == 'voice' and rest == ['status']:
            emit(voice=voice_installed())
        elif command == 'voice' and len(rest) in (1, 2) and rest[0] == 'install':
            return voice_install(rest[1] if len(rest) == 2 else None)
        elif command == 'voice' and rest == ['serve']:
            return voice_serve()
        elif command == 'voice' and rest == ['remove']:
            for directory in (KOKORO, *OLD_VOICES):
                shutil.rmtree(directory, ignore_errors=True)
            emit(ok=True, voice=False)
        elif command == 'models':
            emit(models=status()['models'])
        elif command == 'model' and len(rest) == 2 and rest[0] == 'install':
            return model_install(rest[1])
        elif command == 'capture':
            emit(ok=True, path=capture())
        elif command == 'photos':
            emit(ok=True, photos=photos())
        elif command == 'serve':
            return serve()
        elif command == 'server' and rest == ['stop']:
            stop_server()
            emit(ok=True)
        else:
            print(__doc__, file=sys.stderr)
            return 2
    except (ValueError, sqlite3.Error) as error:
        return fail(str(error))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
