#!/usr/bin/env python3
"""Switcher swipe-to-close frame timing (test only).

One synthetic touchscreen for the whole run: open the switcher from the app in
front (bottom-edge swipe and hold), then flick the front card up N times. A
thread samples pixel_scanout's `starts` counter every millisecond; each frame
interval inside a close (release .. release + 900 ms) is reported.

switcher-close-test.py N [RATE_HZ]
"""
import fcntl, glob, os, struct, sys, threading, time

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT, UI_SET_PROPBIT = 0x40045564, 0x40045565, 0x40045567, 0x4004556e
UI_DEV_SETUP, UI_ABS_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405c5503, 0x401c5504, 0x5501, 0x5502
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BTN_TOUCH, INPUT_PROP_DIRECT = 0x14a, 1
ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_X, ABS_MT_Y, ABS_MT_ID = 0, 1, 0x2f, 0x35, 0x36, 0x39

count = int(sys.argv[1]) if len(sys.argv) > 1 else 3
rate = float(sys.argv[2]) if len(sys.argv) > 2 else 240
period = 1.0 / rate
stats = glob.glob('/sys/kernel/debug/dri/*/pixel_scanout')[0]

frames, stop = [], False
def sampler():
    f = open(stats)
    last = None
    while not stop:
        f.seek(0)
        text = f.read()
        n = int(text.split('\nstarts ')[1].split()[0]) if '\nstarts ' in text else int(text.split('starts ')[1].split()[0])
        if last is not None and n != last:
            frames.append(time.monotonic())
        last = n
        time.sleep(0.001)

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
def swipe(x, y0, y1, move_ms, hold_ms):
    global tracking
    tracking += 1
    emit((EV_ABS, ABS_MT_SLOT, 0), (EV_ABS, ABS_MT_ID, tracking), (EV_ABS, ABS_MT_X, x), (EV_ABS, ABS_MT_Y, y0),
         (EV_KEY, BTN_TOUCH, 1), (EV_ABS, ABS_X, x), (EV_ABS, ABS_Y, y0))
    steps = max(1, int(move_ms / 1000 / period))
    for i in range(1, steps + 1):
        time.sleep(period)
        y = int(y0 + (y1 - y0) * i / steps)
        emit((EV_ABS, ABS_MT_Y, y), (EV_ABS, ABS_Y, y))
    end = time.monotonic() + hold_ms / 1000
    jitter = 0
    while time.monotonic() < end:
        time.sleep(period)
        jitter ^= 1
        emit((EV_ABS, ABS_MT_Y, y1 + jitter), (EV_ABS, ABS_Y, y1 + jitter))
    time.sleep(period)
    emit((EV_ABS, ABS_MT_ID, -1), (EV_KEY, BTN_TOUCH, 0))
    return time.monotonic()

thread = threading.Thread(target=sampler, daemon=True)
thread.start()
time.sleep(0.5)
opened = swipe(720, 3115, 2300, 200, 450)
time.sleep(1.5)
releases = []
for i in range(count):
    releases.append(swipe(720, 1650, 450, 140, 0))
    time.sleep(2.0)
stop = True
thread.join()
fcntl.ioctl(fd, UI_DEV_DESTROY)
os.close(fd)

def report(label, t0, span):
    ts = [t for t in frames if t0 - 0.05 <= t <= t0 + span]
    gaps = [round((b - a) * 1000, 1) for a, b in zip(ts, ts[1:])]
    long = [f'{g} at +{(ts[i] - t0) * 1000:.0f} ms' for i, g in enumerate(gaps) if g > 12]
    last = (ts[-1] - t0) * 1000 if ts else 0
    print(f'{label}: {len(ts)} frames, last at +{last:.0f} ms, worst {max(gaps) if gaps else 0} ms, over 12 ms: {long}')
    return gaps
report('open', opened - 0.65, 1.2)
for i, t in enumerate(releases):
    gaps = report(f'close {i + 1}', t, 0.9)
    print('   ', gaps)
