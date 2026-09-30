#!/usr/bin/env python3
"""Runs on the phone: tap and type through a virtual touchscreen, for testing
the UI without a finger. Needs uinput (out/input/uinput.ko on kernel #194).

  phone-touch.py STEP...     steps run in order:
    tap X Y                  a short touch at panel pixels (1440 x 3120)
    hold X Y MS              a touch held MS milliseconds
    swipe X1 Y1 X2 Y2 MS     a drag
    type DIGITS              digits, *, # and Enter (as "e") as key presses
    wait SECONDS

The device appears as "omarchy-test-touch" and is removed at exit. It gives
the same absolute coordinates the panel's own touchscreen reports.
"""
import fcntl
import os
import struct
import sys
import time

WIDTH, HEIGHT = 1440, 3120
EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
BTN_TOUCH = 0x14a
ABS_X, ABS_Y = 0x00, 0x01
ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x2f, 0x35, 0x36, 0x39
INPUT_PROP_DIRECT = 0x01
KEYS = {str(d): d + 1 for d in range(1, 10)}   # KEY_1 = 2 ... KEY_9 = 10
KEYS.update({'0': 11, 'e': 28})


def ioc(direction, kind, number, size):
    return (direction << 30) | (size << 16) | (ord(kind) << 8) | number


UI_SET_EVBIT = ioc(1, 'U', 100, 4)
UI_SET_KEYBIT = ioc(1, 'U', 101, 4)
UI_SET_ABSBIT = ioc(1, 'U', 103, 4)
UI_SET_PROPBIT = ioc(1, 'U', 110, 4)
UI_DEV_SETUP = ioc(1, 'U', 3, 92)
UI_ABS_SETUP = ioc(1, 'U', 4, 28)
UI_DEV_CREATE = ioc(0, 'U', 1, 0)
UI_DEV_DESTROY = ioc(0, 'U', 2, 0)


class Device:
    def __init__(self):
        self.fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
        for ev in (EV_SYN, EV_KEY, EV_ABS):
            fcntl.ioctl(self.fd, UI_SET_EVBIT, ev)
        for key in [BTN_TOUCH] + sorted(set(KEYS.values())):
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, key)
        fcntl.ioctl(self.fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT)
        for code, maximum in ((ABS_X, WIDTH - 1), (ABS_Y, HEIGHT - 1), (ABS_MT_SLOT, 9),
                              (ABS_MT_POSITION_X, WIDTH - 1), (ABS_MT_POSITION_Y, HEIGHT - 1),
                              (ABS_MT_TRACKING_ID, 65535)):
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, code)
            fcntl.ioctl(self.fd, UI_ABS_SETUP, struct.pack('<HH6i', code, 0, 0, 0, maximum, 0, 0, 0))
        name = b'omarchy-test-touch'.ljust(80, b'\0')
        fcntl.ioctl(self.fd, UI_DEV_SETUP, struct.pack('<4H80sI', 0x06, 0x1d6b, 0x0104, 1, name, 0))
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        self.tracking = 1
        time.sleep(1.0)  # let libinput and the compositor pick it up

    def emit(self, kind, code, value):
        now = time.time()
        os.write(self.fd, struct.pack('<qqHHi', int(now), int(now % 1 * 1e6), kind, code, value))

    def sync(self):
        self.emit(EV_SYN, SYN_REPORT, 0)

    def down(self, x, y):
        self.tracking += 1
        self.emit(EV_ABS, ABS_MT_SLOT, 0)
        self.emit(EV_ABS, ABS_MT_TRACKING_ID, self.tracking)
        self.move(x, y, touch=True)

    def move(self, x, y, touch=False):
        self.emit(EV_ABS, ABS_MT_POSITION_X, x)
        self.emit(EV_ABS, ABS_MT_POSITION_Y, y)
        if touch:
            self.emit(EV_KEY, BTN_TOUCH, 1)
        self.emit(EV_ABS, ABS_X, x)
        self.emit(EV_ABS, ABS_Y, y)
        self.sync()

    def up(self):
        self.emit(EV_ABS, ABS_MT_TRACKING_ID, -1)
        self.emit(EV_KEY, BTN_TOUCH, 0)
        self.sync()

    def key(self, code):
        self.emit(EV_KEY, code, 1)
        self.sync()
        time.sleep(0.03)
        self.emit(EV_KEY, code, 0)
        self.sync()
        time.sleep(0.05)

    def close(self):
        fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        os.close(self.fd)


def main(args):
    if not args:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    device = Device()
    try:
        while args:
            step, args = args[0], args[1:]
            stamp = f'{time.monotonic():.3f}'
            if step == 'tap':
                x, y, args = int(args[0]), int(args[1]), args[2:]
                device.down(x, y)
                time.sleep(0.06)
                device.up()
                print(stamp, 'tap', x, y, flush=True)
            elif step == 'hold':
                x, y, ms, args = int(args[0]), int(args[1]), int(args[2]), args[3:]
                device.down(x, y)
                time.sleep(ms / 1000)
                device.up()
                print(stamp, 'hold', x, y, ms, flush=True)
            elif step == 'swipe':
                x1, y1, x2, y2, ms, args = *map(int, args[:5]), args[5:]
                steps = max(2, ms // 16)
                device.down(x1, y1)
                for i in range(1, steps + 1):
                    time.sleep(ms / 1000 / steps)
                    device.move(x1 + (x2 - x1) * i // steps, y1 + (y2 - y1) * i // steps)
                device.up()
                print(stamp, 'swipe', x1, y1, x2, y2, flush=True)
            elif step == 'type':
                text, args = args[0], args[1:]
                for ch in text:
                    device.key(KEYS[ch])
                print(stamp, 'type', text, flush=True)
            elif step == 'wait':
                seconds, args = float(args[0]), args[1:]
                time.sleep(seconds)
            else:
                raise SystemExit(f'Unknown step {step}')
    finally:
        device.close()
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
