#!/usr/bin/env python3
"""Two omarchy-link daemons over loopback, one as the phone and one as the
computer, each with its own home and XDG directories and a private session
bus (so no real notification is posted). Checks pairing, the pairing code,
texts, notifications, ringing and theme following.

  tests/link/test_loopback.py BUILD/omarchy-link
"""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent


class Client:
    def __init__(self, path):
        deadline = time.time() + 10
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX)
                self.sock.connect(str(path))
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        self.buffer = b''
        self.events = []

    def send(self, **command):
        self.sock.sendall(json.dumps(command).encode() + b'\n')

    def _read(self, timeout):
        self.sock.settimeout(timeout)
        try:
            data = self.sock.recv(65536)
        except socket.timeout:
            return
        if not data:
            raise EOFError('daemon closed the socket')
        self.buffer += data
        while b'\n' in self.buffer:
            line, self.buffer = self.buffer.split(b'\n', 1)
            self.events.append(json.loads(line))

    def wait(self, test, timeout=10, what='event'):
        deadline = time.time() + timeout
        while True:
            for index, event in enumerate(self.events):
                if test(event):
                    del self.events[:index + 1]
                    return event
            remaining = deadline - time.time()
            if remaining <= 0:
                raise AssertionError(f'timed out waiting for {what}; last events: {self.events[-5:]}')
            self._read(min(remaining, 0.5))

    def call(self, **command):
        self.send(**command)
        return self.wait(lambda e: e.get('reply') == command['cmd'], what=f'reply to {command["cmd"]}')

    def state(self):
        self.send(cmd='state')
        return self.wait(lambda e: e.get('event') == 'state', what='state')


def device_in(state, **match):
    for device in state['devices']:
        if all(device.get(k) == v for k, v in match.items()):
            return device
    return None


def main():
    binary = Path(sys.argv[1]).resolve()
    root = Path(tempfile.mkdtemp(prefix='omarchy-link-test-'))
    processes = []
    try:
        bus = subprocess.Popen(['dbus-daemon', '--session', '--nofork', '--print-address=1'],
                               stdout=subprocess.PIPE, text=True)
        processes.append(bus)
        address = bus.stdout.readline().strip()

        def environment(name, role, udp, peer):
            home = root / name
            env = {k: v for k, v in os.environ.items() if not k.startswith(('XDG_', 'OMARCHY_'))}
            env.update(HOME=str(home), XDG_CONFIG_HOME=str(home / '.config'), XDG_DATA_HOME=str(home / '.local/share'),
                       XDG_STATE_HOME=str(home / '.local/state'), XDG_CACHE_HOME=str(home / '.cache'),
                       XDG_RUNTIME_DIR=str(home / 'run'), DBUS_SESSION_BUS_ADDRESS=address,
                       OMARCHY_LINK_TEST='1', OMARCHY_LINK_ROLE=role,
                       OMARCHY_LINK_UDP_PORT=str(udp), OMARCHY_LINK_PEER_UDP_PORT=str(peer),
                       QT_LOGGING_RULES='*.debug=false')
            for key in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_STATE_HOME', 'XDG_RUNTIME_DIR'):
                Path(env[key]).mkdir(parents=True, exist_ok=True)
            os.chmod(env['XDG_RUNTIME_DIR'], 0o700)
            return env

        phone_env = environment('phone', 'phone', 18716, 18717)
        desk_env = environment('desk', 'desktop', 18717, 18716)

        # The phone's helpers: fake messages, and a theme helper that logs and
        # records the choice as the real one does.
        bin_dir = Path(phone_env['HOME']) / '.local/bin'
        bin_dir.mkdir(parents=True)
        shutil.copy(HERE / 'fake-messages.py', bin_dir / 'omarchy-mobile-messages')
        theme_log = root / 'theme.log'
        (bin_dir / 'omarchy-mobile-theme').write_text(
            '#!/bin/sh\necho "$@" >> ' + str(theme_log) + '\n'
            'if [ "$1" = set ]; then echo "$2" > "$XDG_STATE_HOME/omarchy-mobile/selected-theme"; fi\n')
        (bin_dir / 'omarchy-mobile-theme').chmod(0o755)
        state_dir = Path(phone_env['XDG_STATE_HOME']) / 'omarchy-mobile'
        state_dir.mkdir(parents=True)
        (state_dir / 'selected-theme').write_text('tokyo-night\n')
        os.utime(state_dir / 'selected-theme', (time.time() - 3600, time.time() - 3600))
        # A stock theme the phone already has.
        stock = Path(phone_env['XDG_DATA_HOME']) / 'omarchy-mobile/themes/kanagawa'
        stock.mkdir(parents=True)
        (stock / 'colors.toml').write_text('')

        # The computer's theme: universo with a background of its own and one
        # the user added, chosen just now. The phone does not have it.
        theme = Path(desk_env['XDG_CONFIG_HOME']) / 'omarchy/themes/universo'
        (theme / 'backgrounds').mkdir(parents=True)
        (theme / 'colors.toml').write_text('accent = "#123456"\n')
        (theme / 'backgrounds/universo-4.png').write_bytes(os.urandom(3_000_000))
        subprocess.run(['git', 'init', '-q', str(theme)], check=True)
        mine = Path(desk_env['XDG_CONFIG_HOME']) / 'omarchy/backgrounds/universo'
        mine.mkdir(parents=True)
        (mine / 'mine.jpg').write_bytes(b'jpeg')
        current = Path(desk_env['XDG_STATE_HOME']) / 'omarchy/current'
        current.mkdir(parents=True)
        (current / 'theme.name').write_text('universo\n')
        os.symlink(theme / 'backgrounds/universo-4.png', current / 'background')

        logs = {}
        for name, env in (('phone', phone_env), ('desk', desk_env)):
            logs[name] = open(root / f'{name}.log', 'w')
            processes.append(subprocess.Popen([str(binary), 'daemon'], env=env, stdout=logs[name], stderr=subprocess.STDOUT))
        phone = Client(Path(phone_env['XDG_RUNTIME_DIR']) / 'omarchy-link.sock')
        desk = Client(Path(desk_env['XDG_RUNTIME_DIR']) / 'omarchy-link.sock')
        shell = Client(Path(phone_env['XDG_RUNTIME_DIR']) / 'omarchy-link.sock')
        shell.send(cmd='shell.hello')
        desk.send(cmd='subscribe')
        phone.send(cmd='subscribe')

        phone_id = phone.state()['self']['id']
        desk_id = desk.state()['self']['id']
        desk.wait(lambda e: e.get('event') == 'state' and device_in(e, id=phone_id, reachable=True), what='phone reachable')
        print('found each other')

        reply = desk.call(cmd='pair', device=phone_id)
        request = phone.wait(lambda e: e.get('event') == 'pairRequest', what='pair request')
        assert request['code'] and request['code'] == reply['code'], (request, reply)
        phone.call(cmd='accept', device=desk_id)
        desk.wait(lambda e: e.get('event') == 'paired', what='paired')
        print('paired, both show', request['code'])

        # Battery and the theme come as soon as the link is paired.
        desk.wait(lambda e: e.get('event') == 'state' and (device_in(e, id=phone_id) or {}).get('battery') is not None,
                  what='battery', timeout=5) if Path('/sys/class/power_supply').exists() else None
        result = desk.wait(lambda e: e.get('event') == 'theme.result', what='theme result', timeout=30)
        assert result['name'] == 'universo' and not result['error'], result
        installed = Path(phone_env['XDG_CONFIG_HOME']) / 'omarchy/themes/universo'
        assert (installed / 'colors.toml').read_text() == 'accent = "#123456"\n'
        assert (installed / 'backgrounds/universo-4.png').read_bytes() == (theme / 'backgrounds/universo-4.png').read_bytes()
        assert not (installed / '.git').exists()
        assert (Path(phone_env['XDG_CONFIG_HOME']) / 'omarchy/backgrounds/universo/mine.jpg').read_bytes() == b'jpeg'
        assert theme_log.read_text().splitlines() == ['set universo', 'background set universo-4.png'], theme_log.read_text()
        print('the phone got universo over the link (3 MB, with the user background) and switched to it')

        # A new background picked on the computer, one the phone lacks: its
        # files come again and the phone switches background only.
        (theme / 'backgrounds/universo-5.png').write_bytes(b'png')
        time.sleep(1.1)
        os.remove(current / 'background')
        os.symlink(theme / 'backgrounds/universo-5.png', current / 'background')
        result = desk.wait(lambda e: e.get('event') == 'theme.result', what='background result', timeout=15)
        assert not result['error'], result
        assert theme_log.read_text().splitlines()[-1] == 'background set universo-5.png', theme_log.read_text()
        print('a background picked later on the computer was followed')

        # A theme picked later on the computer is followed at once.
        kanagawa = Path(desk_env['XDG_CONFIG_HOME']) / 'omarchy/themes/kanagawa'
        kanagawa.mkdir(parents=True)
        (kanagawa / 'colors.toml').write_text('')
        time.sleep(1.1)
        (current / 'theme.name').write_text('kanagawa\n')
        os.remove(current / 'background')
        result = desk.wait(lambda e: e.get('event') == 'theme.result' and e['name'] == 'kanagawa', what='second theme', timeout=15)
        assert not result['error'], result
        assert theme_log.read_text().splitlines()[-1] == 'set kanagawa', theme_log.read_text()
        print('a theme picked later on the computer was followed')

        # An older theme as Purple Haze is: alacritty.toml but no colors.toml,
        # and backgrounds that are links into its author's home.
        haze = Path(desk_env['XDG_CONFIG_HOME']) / 'omarchy/themes/haze'
        (haze / 'backgrounds').mkdir(parents=True)
        (haze / 'alacritty.toml').write_text(
            '[colors.primary]\nbackground = "#1e1a2e"\nforeground = "#e0d0ff"\n'
            '[colors.normal]\nblack = "#1e1a2e"\nred = "#ff5c8a"\ngreen = "#9cff8a"\nyellow = "#ffd36e"\n'
            'blue = "#8a9cff"\nmagenta = "#c38aff"\ncyan = "#8affe8"\nwhite = "#e0d0ff"\n')
        (haze / 'backgrounds/BG01.png').write_bytes(b'png')
        os.symlink('/home/someone-else/wallpaper.jpg', haze / 'backgrounds/background')
        time.sleep(1.1)
        (current / 'theme.name').write_text('haze\n')
        os.symlink(haze / 'backgrounds/BG01.png', current / 'background')
        result = desk.wait(lambda e: e.get('event') == 'theme.result' and e['name'] == 'haze', what='haze', timeout=15)
        assert not result['error'], result
        got = Path(phone_env['XDG_CONFIG_HOME']) / 'omarchy/themes/haze'
        if shutil.which('omarchy-theme-colors-from-alacritty'):
            assert 'e0d0ff' in (got / 'colors.toml').read_text().lower(), (got / 'colors.toml').read_text()
        assert (got / 'backgrounds/BG01.png').exists() and not (got / 'backgrounds/background').exists()
        assert theme_log.read_text().splitlines()[-2:] == ['set haze', 'background set BG01.png'], theme_log.read_text()
        print('an older theme (no colors.toml, broken links) came over with a colors.toml made for it')

        conversations = desk.call(cmd='sms.conversations', device=phone_id)
        update = desk.wait(lambda e: e.get('event') == 'sms.conversations' and len(e['conversations']) == 2, what='conversations')
        first = update['conversations'][0]
        assert first['thread_id'] == 5035550100 and first['omarchy']['name'] == 'Ada', first
        assert update['conversations'][1]['thread_id'] > 10**12
        print('conversations:', [c['body'] for c in update['conversations']])

        desk.call(cmd='sms.thread', device=phone_id, thread=5035550100)
        thread = desk.wait(lambda e: e.get('event') == 'sms.messages' and e.get('threadID') == 5035550100, what='thread')
        assert [m['body'] for m in thread['messages']] == ['Hi there', 'Hello!'], thread
        assert [m['type'] for m in thread['messages']] == [1, 2]
        print('thread:', [m['body'] for m in thread['messages']])

        sent = desk.call(cmd='sms.send', device=phone_id, address='+15035550100', text='From the laptop')
        assert sent['ok'] and not sent['queued'], sent
        pushed = desk.wait(lambda e: e.get('event') == 'sms.messages' and any(m['body'] == 'From the laptop' for m in e['messages']),
                           what='sent text pushed back')
        print('sent a text; the phone pushed it back:', pushed['messages'][0]['body'])

        shell.send(cmd='shell.notification', id='42', appName='Signal', title='Bob', text='Lunch?', time=1759600000000,
                   actions=[{'id': 'reply', 'label': 'Reply'}, {'id': 'read', 'label': 'Mark read'}], clearable=True)
        notes = desk.wait(lambda e: e.get('event') == 'notifications' and e['notifications'], what='notification')
        assert notes['notifications'][0]['title'] == 'Bob' and notes['notifications'][0]['actions'] == ['Reply', 'Mark read'], notes
        desk.call(cmd='notification.action', device=phone_id, id='42', action='Mark read')
        action = shell.wait(lambda e: e.get('event') == 'shell.action', what='shell action')
        assert action['id'] == '42' and action['action'] == 'read', action
        desk.call(cmd='notification.dismiss', device=phone_id, id='42')
        shell.wait(lambda e: e.get('event') == 'shell.dismiss' and e['id'] == '42', what='shell dismiss')
        print('notification mirrored; its action and dismissal reached the phone shell')

        desk.call(cmd='ring', device=phone_id)
        shell.wait(lambda e: e.get('event') == 'shell.ring', what='ring')
        print('ring reached the phone shell')

        # Unpairing on one end unpairs both.
        desk.call(cmd='unpair', device=phone_id)
        phone.wait(lambda e: e.get('event') == 'unpaired', what='unpaired')
        print('unpaired')
        print('PASS')
        return 0
    finally:
        for process in reversed(processes):
            process.terminate()
            try:
                process.wait(5)
            except subprocess.TimeoutExpired:
                process.kill()
        if os.environ.get('KEEP'):
            print('kept', root)
        else:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == '__main__':
    raise SystemExit(main())
