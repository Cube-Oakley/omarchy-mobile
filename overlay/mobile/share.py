#!/usr/bin/env python3
"""Share text, a link or files from any app, through the shell's share sheet.

  omarchy-mobile-share [--title TITLE] --text TEXT
  omarchy-mobile-share [--title TITLE] --url URL
  omarchy-mobile-share [--title TITLE] --file PATH [--file PATH ...]
      keep the request and ask the shell to show the sheet
  show ID              the request with the targets that take it (JSON)
  run ID TARGET        hand it to a target: copy, messages, notes, browser,
                       laptop, downloads
  read ID              the request itself, for the app a target opens

A request lives in $XDG_RUNTIME_DIR/omarchy-mobile-share (the newest 20).
The shell reads requests named on its request FIFO ("share ID"); without a
shell the request is kept and nothing shows. Prints one JSON object.
"""
import json
import mimetypes
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import time
from urllib.parse import quote

MAX_TEXT = 64 * 1024
MAX_FILES = 20
KEEP = 20


def runtime():
    base = os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}'
    return Path(base)


def store():
    path = runtime() / 'omarchy-mobile-share'
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    return path


def bin_path(name):
    return str(Path.home() / '.local/bin' / name)


def request_path(ident):
    if not re.fullmatch(r'[0-9a-f]{16}', ident or ''):
        raise ValueError('Invalid share')
    return store() / f'{ident}.json'


def is_url(text):
    return re.fullmatch(r'(https?|ftp)://\S+', (text or '').strip()) is not None


def describe_file(path):
    path = Path(path).expanduser().resolve()
    if not path.is_file():
        raise ValueError(f'Not a file: {path.name}')
    mime = mimetypes.guess_type(path.name)[0] or 'application/octet-stream'
    return {'path': str(path), 'name': path.name, 'mime': mime, 'size': path.stat().st_size}


def make_request(title, text=None, url=None, files=None):
    request = {'title': (title or '').strip()[:200], 'created': time.time()}
    if url is not None:
        url = url.strip()
        if not is_url(url):
            raise ValueError('Not a web address')
        request.update(kind='url', url=url[:4096])
    elif files:
        if len(files) > MAX_FILES:
            raise ValueError(f'At most {MAX_FILES} files at once')
        request.update(kind='files', files=[describe_file(path) for path in files])
    elif text is not None:
        if not text.strip():
            raise ValueError('Nothing to share')
        if '\0' in text:
            raise ValueError('Text cannot contain NUL')
        text = text[:MAX_TEXT]
        if is_url(text):
            request.update(kind='url', url=text.strip())
        else:
            request.update(kind='text', text=text)
    else:
        raise ValueError('Nothing to share')
    return request


def save(request):
    ident = secrets.token_hex(8)
    path = request_path(ident)
    path.write_text(json.dumps(request))
    path.chmod(0o600)
    old = sorted(store().glob('*.json'), key=lambda item: item.stat().st_mtime, reverse=True)[KEEP:]
    for item in old:
        item.unlink(missing_ok=True)
    return ident


def load(ident):
    return json.loads(request_path(ident).read_text())


def ask_shell(line):
    """A line on the shell's request FIFO; False when no shell reads it."""
    fifo = runtime() / 'omarchy-mobile-shell.fifo'
    try:
        fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
    except OSError:
        return False
    try:
        os.write(fd, (line + '\n').encode())
        return True
    except OSError:
        return False
    finally:
        os.close(fd)


def body_text(request):
    """The request as text: a link with its title, or the text."""
    if request['kind'] == 'url':
        title = request.get('title', '')
        return f"{title}\n{request['url']}" if title and title != request['url'] else request['url']
    if request['kind'] == 'text':
        return request['text']
    return '\n'.join(item['path'] for item in request['files'])


def laptop_connected():
    """Paired computers the link reaches now (empty without the link)."""
    link = shutil.which('omarchy-link') or bin_path('omarchy-link')
    if not os.access(link, os.X_OK):
        return []
    try:
        result = subprocess.run([link, 'state'], capture_output=True, text=True, timeout=3)
    except (OSError, subprocess.SubprocessError):
        return []
    for line in result.stdout.splitlines():
        try:
            state = json.loads(line)
        except ValueError:
            continue
        devices = state.get('devices') or state.get('state', {}).get('devices') or []
        return [d for d in devices if d.get('paired') and d.get('reachable') and d.get('share') is True]
    return []


def targets(request):
    kind = request['kind']
    found = [{'id': 'copy', 'name': 'Copy', 'icon': '\U000F018F'}]
    if kind in ('text', 'url'):
        found.append({'id': 'messages', 'name': 'Messages', 'icon': '\U000F0369'})
        found.append({'id': 'notes', 'name': 'Notes', 'icon': '\U000F082E'})
    if kind == 'url':
        found.append({'id': 'browser', 'name': 'Browser', 'icon': '\U000F059F'})
    if kind == 'files':
        found.append({'id': 'downloads', 'name': 'Downloads', 'icon': '\U000F01DA'})
    for device in laptop_connected():
        found.append({'id': 'laptop:' + device.get('id', ''), 'name': device.get('name') or 'Computer',
                      'icon': '\U000F0322'})
    return found


def launch_app(app_id, argument=''):
    launcher = bin_path('omarchy-mobile-app')
    # The FIFO's words are split on spaces: the argument goes percent-encoded.
    if not ask_shell(f'over {app_id} {quote(argument, safe=":/.-_")}'.strip()):
        subprocess.Popen([launcher, 'launch', app_id] + ([argument] if argument else []),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)


def copy(request):
    if request['kind'] == 'files':
        uris = '\n'.join('file://' + item['path'] for item in request['files'])
        subprocess.run(['wl-copy', '--type', 'text/uri-list'], input=uris.encode(), check=True, timeout=5)
        return 'Copied'
    text = request['url'] if request['kind'] == 'url' else request['text']
    clipboard = bin_path('omarchy-mobile-clipboard')
    if os.access(clipboard, os.X_OK):
        # The history records it; copy puts it on the clipboard.
        subprocess.run([clipboard, 'record'], input=json.dumps({'text': text}).encode(),
                       capture_output=True, timeout=5)
    subprocess.run(['wl-copy', '--', text], check=True, timeout=5)
    return 'Copied'


def notes(request):
    title = request.get('title') or ('Link' if request['kind'] == 'url' else '')
    text = body_text(request)
    if title and not text.startswith(title):
        text = f'# {title}\n\n{text}'
    result = subprocess.run([bin_path('omarchy-mobile-notes'), 'save', '-'], input=text,
                            capture_output=True, text=True, timeout=10)
    note = json.loads(result.stdout or '{}')
    if not note.get('ok'):
        raise RuntimeError(note.get('error') or 'Notes did not save it')
    launch_app('notes', 'note:' + note.get('id', ''))
    return 'Saved in Notes'


def downloads(request):
    folder = Path.home() / 'Downloads'
    folder.mkdir(exist_ok=True)
    for item in request['files']:
        source = Path(item['path'])
        target = folder / source.name
        stem, suffix, n = target.stem, target.suffix, 1
        while target.exists():
            target = folder / f'{stem} ({n}){suffix}'
            n += 1
        shutil.copy2(source, target)
    return 'Saved in Downloads'


def laptop(request, device):
    link = shutil.which('omarchy-link') or bin_path('omarchy-link')
    command = {'cmd': 'share', 'device': device, 'title': request.get('title', '')}
    if request['kind'] == 'url':
        command['url'] = request['url']
    elif request['kind'] == 'text':
        command['text'] = request['text']
    else:
        command['files'] = [item['path'] for item in request['files']]
    result = subprocess.run([link, 'call', json.dumps(command)], capture_output=True, text=True, timeout=60)
    for line in result.stdout.splitlines():
        try:
            reply = json.loads(line)
        except ValueError:
            continue
        if reply.get('ok') is False:
            raise RuntimeError(reply.get('error') or 'The computer did not take it')
    if result.returncode != 0:
        raise RuntimeError('The computer did not take it')
    return 'Sent to the computer'


def run(ident, target):
    request = load(ident)
    kind = request['kind']
    if target == 'copy':
        return copy(request)
    if target == 'messages' and kind in ('text', 'url'):
        launch_app('messages', 'draft:' + ident)
        return ''
    if target == 'notes' and kind in ('text', 'url'):
        return notes(request)
    if target == 'browser' and kind == 'url':
        subprocess.Popen([bin_path('omarchy-mobile-browser'), request['url']], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)
        return ''
    if target == 'downloads' and kind == 'files':
        return downloads(request)
    if target.startswith('laptop:'):
        return laptop(request, target[7:])
    raise ValueError('That target does not take this')


def main(argv):
    try:
        if argv[:1] == ['show'] and len(argv) == 2:
            request = load(argv[1])
            result = dict(request, ok=True, id=argv[1], targets=targets(request))
        elif argv[:1] == ['run'] and len(argv) == 3:
            result = {'ok': True, 'message': run(argv[1], argv[2])}
        elif argv[:1] == ['read'] and len(argv) == 2:
            request = load(argv[1])
            result = dict(request, ok=True, id=argv[1], body=body_text(request))
        else:
            title, text, url, files = '', None, None, []
            args = list(argv)
            if not args:
                print(__doc__.strip(), file=sys.stderr)
                return 2
            while args:
                flag = args.pop(0)
                if not args:
                    print(__doc__.strip(), file=sys.stderr)
                    return 2
                value = args.pop(0)
                if flag == '--title':
                    title = value
                elif flag == '--text':
                    text = value
                elif flag == '--url':
                    url = value
                elif flag == '--file':
                    files.append(value)
                else:
                    print(__doc__.strip(), file=sys.stderr)
                    return 2
            ident = save(make_request(title, text=text, url=url, files=files))
            result = {'ok': True, 'id': ident, 'shown': ask_shell('share ' + ident)}
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result = {'ok': False, 'error': str(error)}
    print(json.dumps(result))
    return 0 if result.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
