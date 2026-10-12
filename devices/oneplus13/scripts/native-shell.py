#!/usr/bin/env python3
"""Connect to the native OnePlus 13 USB shell. Ctrl-] exits an interactive session."""
# Adapted from the Pixel USB bring-up helpers; device identity/link differ.
import argparse
import fcntl
import os
from pathlib import Path
import re
import select
import shlex
import sys
import termios
import time
import tty
import uuid


def find_port():
    for p in Path('/sys/class/tty').glob('ttyACM*'):
        for parent in (p / 'device').resolve().parents:
            try:
                if ((parent / 'idVendor').read_text().strip() == '0525' and
                    (parent / 'idProduct').read_text().strip() in ('a4a7', 'a4aa') and
                    'Omarchy-OnePlus13' in (parent / 'manufacturer').read_text()):
                    return '/dev/' + p.name
            except OSError:
                pass
    return None


def write_all(fd, data, timeout=10):
    end = time.monotonic() + timeout
    while data:
        if time.monotonic() >= end:
            raise TimeoutError('Serial write timed out')
        if select.select([], [fd], [], 0.5)[1]:
            try:
                data = data[os.write(fd, data):]
            except BlockingIOError:
                pass


def receive(fd, timeout):
    if not select.select([fd], [], [], timeout)[0]:
        return b''
    try:
        data = os.read(fd, 65536)
    except BlockingIOError:
        return b''
    if not data:
        raise OSError('OnePlus 13 serial disconnected')
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--command', help='run a shell command and return its exit status')
    ap.add_argument('--wait', type=float, default=45, help='seconds to wait for USB')
    ap.add_argument('--timeout', type=float, default=30, help='command timeout')
    ap.add_argument('--log', type=Path, help='save the serial transcript')
    a = ap.parse_args()
    deadline = time.monotonic() + a.wait
    fd = None
    error = 'OnePlus 13 native serial gadget not found'
    while time.monotonic() < deadline:
        port = find_port()
        if port:
            try:
                fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError as e:
                error = str(e)
                if fd is not None:
                    os.close(fd)
                    fd = None
        time.sleep(0.2)
    if fd is None:
        raise OSError(error)
    log = a.log.open('wb') if a.log else None
    old_stdin = None

    def output(data):
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
        if log:
            log.write(data)
            log.flush()

    try:
        tty.setraw(fd, termios.TCSANOW)
        termios.tcflush(fd, termios.TCIOFLUSH)
        # Clear any partial input from the host's initial tty echo setting.
        write_all(fd, b'\x03\n')
        print(f'Connected to {port}; Ctrl-] disconnects.', file=sys.stderr)
        if a.command is None:
            if not sys.stdin.isatty():
                raise OSError('Interactive mode needs a terminal; use --command')
            old_stdin = termios.tcgetattr(sys.stdin.fileno())
            tty.setraw(sys.stdin.fileno(), termios.TCSANOW)
            while True:
                ready = select.select([fd, sys.stdin.fileno()], [], [], 1)[0]
                if fd in ready:
                    output(receive(fd, 0))
                if sys.stdin.fileno() in ready:
                    data = os.read(sys.stdin.fileno(), 4096)
                    if not data or b'\x1d' in data:
                        return 0
                    write_all(fd, data)
        data = b''
        deadline = time.monotonic() + min(a.timeout, 15)
        while b'oneplus13-linux# ' not in data:
            if time.monotonic() >= deadline:
                raise TimeoutError('No shell prompt received')
            chunk = receive(fd, 0.5)
            data += chunk
            output(chunk)
        marker = 'ONEPLUS13_DONE_' + uuid.uuid4().hex
        command = "sh -c " + shlex.quote(a.command) + "; rc=$?; printf '\\n" + marker + ":%s\\n' \"$rc\"\n"
        if len(command.encode()) > 4000:
            raise ValueError('Command exceeds canonical tty line limit')
        write_all(fd, command.encode())
        pattern = re.compile(rb'\r?\n' + marker.encode() + rb':([0-9]+)\r?\n')
        data = b''
        deadline = time.monotonic() + a.timeout
        while time.monotonic() < deadline:
            chunk = receive(fd, 0.5)
            data = (data + chunk)[-131072:]
            output(chunk)
            match = pattern.search(data)
            if match:
                return int(match[1])
        raise TimeoutError('Shell command timed out')
    finally:
        if old_stdin:
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSANOW, old_stdin)
        if log:
            log.close()
        os.close(fd)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, TimeoutError, ValueError) as e:
        print(f'oneplus13-shell: {e}', file=sys.stderr)
        sys.exit(1)
