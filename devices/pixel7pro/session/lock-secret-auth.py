#!/usr/bin/python3 -I
# SPDX-License-Identifier: MIT
"""PAM authentication with the lock screen's PIN or password.

Run by pam_exec (expose_authtok, seteuid) for sudo; installed as
/usr/local/lib/omarchy-mobile/lock-secret-auth. The answer is checked by the
fingerprint service (devices/pixel7pro/fingerprint), which holds the lock
credential enrolled with Trusty's Gatekeeper: Gatekeeper throttles wrong
guesses, and there is no password hash on the root. It succeeds only:
- for the session's person (/etc/omarchy-mobile/session), during auth;
- with a PIN or password set (the service accepts anything while none is);
- from the real service: the socket in a root-owned directory nobody else can
  write, its server root or the service's Android user (uid 1000).
Anything else fails, and PAM goes on to the system's own check.
"""
import json
import os
from pathlib import Path
import pwd
import socket
import stat
import struct
import sys
import time

SOCKET = Path('/run/omarchy-fingerprint/fingerprint.sock')
SESSION = Path('/etc/omarchy-mobile/session')
SERVICE_UIDS = (0, 1000)
TIMEOUT = 10
MAX_SECRET = 64  # the service's limit (fp-client.c, parse_secret)


def session_user():
    try:
        for line in SESSION.read_text().splitlines():
            key, _, value = line.partition('=')
            if key.strip() == 'user' and value.strip():
                return value.strip()
    except OSError:
        pass
    return None


def trusted_socket(path=SOCKET):
    try:
        folder, entry = path.parent.stat(), path.lstat()
    except OSError:
        return False
    return (folder.st_uid == 0 and not folder.st_mode & 0o022 and stat.S_ISSOCK(entry.st_mode)
            and entry.st_uid in SERVICE_UIDS)


def secret_from(data):
    """pam_exec writes the token and a NUL. None when unusable on the
    service's line protocol (empty, too long)."""
    secret = data.split(b'\0', 1)[0]
    return secret if 0 < len(secret) <= MAX_SECRET else None


class Service:
    def __init__(self, path=SOCKET):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(TIMEOUT)
        self.sock.connect(str(path))
        self.buffer = b''
        self.deadline = time.monotonic() + TIMEOUT

    def peer_uid(self):
        creds = self.sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize('3i'))
        return struct.unpack('3i', creds)[1]

    def event(self, name):
        """The next event called NAME (status lines go to every client)."""
        while time.monotonic() < self.deadline:
            while b'\n' in self.buffer:
                line, self.buffer = self.buffer.split(b'\n', 1)
                try:
                    data = json.loads(line)
                except ValueError:
                    continue
                if isinstance(data, dict) and data.get('event') == name:
                    return data
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            self.buffer += chunk
        raise OSError('The fingerprint service did not answer')

    def ask(self, line, name):
        self.sock.sendall(line.encode() + b'\n')
        return self.event(name)

    def close(self):
        self.sock.close()


def check(user, secret, path=SOCKET, connect=Service):
    """0 when SECRET is the lock credential of the session's USER, else 1;
    the message for the user, if any."""
    if user is None or user != session_user():
        return 1, None
    if secret is None:
        return 1, None
    if not trusted_socket(path):
        return 1, 'The lock screen service is not running.'
    service = connect(path)
    try:
        if service.peer_uid() not in SERVICE_UIDS:
            return 1, None
        status = service.ask('status', 'status')
        if status.get('credential') not in ('pin', 'password'):
            return 1, 'Set a PIN or password in Settings → Security first.'
        result = service.ask('verify ' + secret.hex(), 'verified')
    finally:
        service.close()
    if result.get('ok') is True:
        return 0, None
    if result.get('throttled'):
        return 1, 'Too many wrong tries. Wait a moment, then try again.'
    return 1, None


def main():
    if os.geteuid() != 0 or os.environ.get('PAM_TYPE') != 'auth':
        return 1
    user = os.environ.get('PAM_USER')
    try:
        pwd.getpwnam(user or '')
    except KeyError:
        return 1
    secret = secret_from(sys.stdin.buffer.read(MAX_SECRET + 2))
    try:
        code, message = check(user, secret)
    except OSError:
        code, message = 1, 'The lock screen service did not answer.'
    if message:
        print(message, flush=True)
    return code


if __name__ == '__main__':
    sys.exit(main())
