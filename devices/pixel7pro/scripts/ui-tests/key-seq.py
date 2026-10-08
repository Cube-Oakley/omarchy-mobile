#!/usr/bin/env python3
"""Synthetic hardware keys through uinput, for testing the shell's key handling.

  key-seq.py down:power wait:60 down:voldown wait:120 up:voldown up:power

Keys: power, volup, voldown. Steps run in order; wait takes milliseconds.
Needs uinput (insmod /root/tools/uinput.ko on the Pixel). Hyprland sees a
keyboard of its own, which its bindings treat like the phone's keys.
"""
import fcntl
import os
import struct
import sys
import time

UI_SET_EVBIT, UI_SET_KEYBIT = 0x40045564, 0x40045565
UI_DEV_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405c5503, 0x5501, 0x5502
EV_SYN, EV_KEY = 0, 1
KEYS = {'power': 116, 'volup': 115, 'voldown': 114}

fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
for ev in (EV_SYN, EV_KEY):
    fcntl.ioctl(fd, UI_SET_EVBIT, ev)
for code in KEYS.values():
    fcntl.ioctl(fd, UI_SET_KEYBIT, code)
fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack('HHHH80sI', 3, 0x1234, 0x5679, 1, b'synthetic phone keys', 0))
fcntl.ioctl(fd, UI_DEV_CREATE)
time.sleep(0.6)     # the compositor opens the new device


def emit(code, value):
    os.write(fd, struct.pack('qqHHi', 0, 0, EV_KEY, code, value) + struct.pack('qqHHi', 0, 0, EV_SYN, 0, 0))


try:
    for step in sys.argv[1:]:
        kind, _, arg = step.partition(':')
        if kind == 'wait':
            time.sleep(int(arg) / 1000)
        elif kind in ('down', 'up'):
            emit(KEYS[arg], 1 if kind == 'down' else 0)
        else:
            raise SystemExit(f'unknown step {step}')
    time.sleep(0.3)
finally:
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)
