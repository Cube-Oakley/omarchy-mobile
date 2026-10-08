#!/usr/bin/env python3
"""Synthetic touch sequence through one uinput touchscreen (test only).

touch-seq.py ACTION...   with ACTION one of
  tap:X,Y                 press and release (panel pixels, 1440x3120)
  swipe:X,Y0,Y1,MS[,HOLD] vertical move at 240 Hz, hold HOLD ms, release
  path:MS,X,Y,X,Y,...     move through the points (straight lines between
                          them) over MS ms at 240 Hz, then release
  wait:MS
"""
import fcntl, os, struct, sys, time

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT, UI_SET_PROPBIT = 0x40045564, 0x40045565, 0x40045567, 0x4004556e
UI_DEV_SETUP, UI_ABS_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405c5503, 0x401c5504, 0x5501, 0x5502
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BTN_TOUCH, INPUT_PROP_DIRECT = 0x14a, 1
ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_X, ABS_MT_Y, ABS_MT_ID = 0, 1, 0x2f, 0x35, 0x36, 0x39

fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
for ev in (EV_SYN, EV_KEY, EV_ABS):
    fcntl.ioctl(fd, UI_SET_EVBIT, ev)
fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH)
fcntl.ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT)
for code, top in ((ABS_X, 1439), (ABS_Y, 3119), (ABS_MT_SLOT, 9), (ABS_MT_X, 1439), (ABS_MT_Y, 3119), (ABS_MT_ID, 65535)):
    fcntl.ioctl(fd, UI_SET_ABSBIT, code)
    fcntl.ioctl(fd, UI_ABS_SETUP, struct.pack('HHiiiiii', code, 0, 0, 0, top, 0, 0, 0))
fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack('HHHH80sI', 3, 0x1234, 0x5678, 1, b'synthetic swipe touchscreen', 0))
fcntl.ioctl(fd, UI_DEV_CREATE)
time.sleep(2.0)

def emit(*events):
    os.write(fd, b''.join(struct.pack('qqHHi', 0, 0, t, c, v) for t, c, v in events) +
             struct.pack('qqHHi', 0, 0, EV_SYN, 0, 0))

tracking = 1
def down(x, y):
    global tracking
    tracking += 1
    emit((EV_ABS, ABS_MT_SLOT, 0), (EV_ABS, ABS_MT_ID, tracking), (EV_ABS, ABS_MT_X, x), (EV_ABS, ABS_MT_Y, y),
         (EV_KEY, BTN_TOUCH, 1), (EV_ABS, ABS_X, x), (EV_ABS, ABS_Y, y))
def up():
    emit((EV_ABS, ABS_MT_ID, -1), (EV_KEY, BTN_TOUCH, 0))

for action in sys.argv[1:]:
    kind, _, args = action.partition(':')
    v = [int(a) for a in args.split(',')] if args else []
    if kind == 'tap':
        down(v[0], v[1]); time.sleep(0.06); up(); time.sleep(0.25)
    elif kind == 'swipe':
        x, y0, y1, ms = v[:4]
        hold = v[4] if len(v) > 4 else 0
        down(x, y0)
        steps = max(1, int(ms / 1000 * 240))
        for i in range(1, steps + 1):
            time.sleep(1 / 240)
            y = int(y0 + (y1 - y0) * i / steps)
            emit((EV_ABS, ABS_MT_Y, y), (EV_ABS, ABS_Y, y))
        end = time.monotonic() + hold / 1000
        jitter = 0
        while time.monotonic() < end:
            time.sleep(1 / 240)
            jitter ^= 1
            emit((EV_ABS, ABS_MT_Y, y1 + jitter), (EV_ABS, ABS_Y, y1 + jitter))
        time.sleep(1 / 240); up()
    elif kind == 'path':
        ms, pts = v[0], list(zip(v[1::2], v[2::2]))
        down(*pts[0])
        steps = max(len(pts), int(ms / 1000 * 240))
        legs = len(pts) - 1
        for i in range(1, steps + 1):
            time.sleep(1 / 240)
            t = i / steps * legs
            leg = min(legs - 1, int(t))
            f = t - leg
            (x0, y0), (x1, y1) = pts[leg], pts[leg + 1]
            x, y = int(x0 + (x1 - x0) * f), int(y0 + (y1 - y0) * f)
            emit((EV_ABS, ABS_MT_X, x), (EV_ABS, ABS_MT_Y, y), (EV_ABS, ABS_X, x), (EV_ABS, ABS_Y, y))
        time.sleep(1 / 240); up()
    elif kind == 'wait':
        time.sleep(v[0] / 1000)
time.sleep(0.3)
fcntl.ioctl(fd, UI_DEV_DESTROY)
os.close(fd)
