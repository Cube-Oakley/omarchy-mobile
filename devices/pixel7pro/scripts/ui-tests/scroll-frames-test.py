#!/usr/bin/env python3
"""Frames the panel shows while a page scrolls (test only).

A synthetic touchscreen drags the page at a steady speed, then flicks it.
A thread samples pixel_scanout's `starts` counter every millisecond (it
counts frames sent to the panel, which only happen when something on screen
changed), so the intervals are what the eye gets: 8.3 ms is every refresh
at 120 Hz, 16.7 ms is a refresh without a new frame.

scroll-frames-test.py [drag|fling|both] [RUNS] [X] [TOUCH_HZ]
Coordinates are panel pixels (1440x3120); the drag and flick go upwards
(the page scrolls down) between y 2500 and 900.
"""
import fcntl, glob, os, struct, sys, threading, time

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT, UI_SET_PROPBIT = 0x40045564, 0x40045565, 0x40045567, 0x4004556e
UI_DEV_SETUP, UI_ABS_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405c5503, 0x401c5504, 0x5501, 0x5502
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BTN_TOUCH, INPUT_PROP_DIRECT = 0x14a, 1
ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_X, ABS_MT_Y, ABS_MT_ID = 0, 1, 0x2f, 0x35, 0x36, 0x39

mode = sys.argv[1] if len(sys.argv) > 1 else 'both'
runs = int(sys.argv[2]) if len(sys.argv) > 2 else 3
x = int(sys.argv[3]) if len(sys.argv) > 3 else 720
rate = float(sys.argv[4]) if len(sys.argv) > 4 else 240
period = 1.0 / rate
if not glob.glob('/sys/kernel/debug/dri'):
    os.system('mount -t debugfs none /sys/kernel/debug')
stats = glob.glob('/sys/kernel/debug/dri/*/pixel_scanout')[0]

frames, stop = [], False
def sampler():
    f = open(stats)
    last = None
    while not stop:
        f.seek(0)
        text = f.read()
        n = int(text.split('starts ')[1].split()[0])
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
fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack('HHHH80sI', 3, 0x1234, 0x5678, 1, b'synthetic scroll touchscreen', 0))
fcntl.ioctl(fd, UI_DEV_CREATE)
time.sleep(2.0)

def emit(*events):
    os.write(fd, b''.join(struct.pack('qqHHi', 0, 0, t, c, v) for t, c, v in events) +
             struct.pack('qqHHi', 0, 0, EV_SYN, 0, 0))

tracking = 1
def stroke(y0, y1, move_ms, hold_ms):
    """Touch at y0, move to y1 in move_ms at the touch rate, hold, lift.
    Returns (time the move started, time of the lift)."""
    global tracking
    tracking += 1
    emit((EV_ABS, ABS_MT_SLOT, 0), (EV_ABS, ABS_MT_ID, tracking), (EV_ABS, ABS_MT_X, x), (EV_ABS, ABS_MT_Y, y0),
         (EV_KEY, BTN_TOUCH, 1), (EV_ABS, ABS_X, x), (EV_ABS, ABS_Y, y0))
    time.sleep(0.05)
    start = time.monotonic()
    steps = max(1, int(move_ms / 1000 / period))
    for i in range(1, steps + 1):
        # Steady steps on the touch clock, as a panel scanning at RATE.
        target = start + i * period
        time.sleep(max(0, target - time.monotonic()))
        y = int(y0 + (y1 - y0) * i / steps)
        emit((EV_ABS, ABS_MT_Y, y), (EV_ABS, ABS_Y, y))
    time.sleep(hold_ms / 1000)
    emit((EV_ABS, ABS_MT_ID, -1), (EV_KEY, BTN_TOUCH, 0))
    return start, time.monotonic()

def report(label, t0, t1):
    ts = [t for t in frames if t0 <= t <= t1]
    gaps = [round((b - a) * 1000, 1) for a, b in zip(ts, ts[1:])]
    span = (ts[-1] - ts[0]) * 1000 if len(ts) > 1 else 0
    missed = sum(max(0, round(g / 8.333) - 1) for g in gaps)
    hist = {}
    for g in gaps:
        k = min(4, max(1, round(g / 8.333)))
        hist[k] = hist.get(k, 0) + 1
    shown = ', '.join(f'{k}x:{hist[k]}' for k in sorted(hist))
    print(f'{label}: {len(ts)} frames over {span:.0f} ms, {missed} refreshes without a new frame ({shown})')
    return gaps

thread = threading.Thread(target=sampler, daemon=True)
thread.start()
time.sleep(0.5)
results = []
for i in range(runs):
    if mode in ('drag', 'both'):
        # 1600 px in 800 ms: a steady 2000 px/s (about 670 dp/s) drag.
        start, lift = stroke(2500, 900, 800, 60)
        results.append(('drag', start + 0.1, start + 0.8))
        time.sleep(1.0)
        stroke(900, 2500, 300, 300)  # back down, held still so it does not fling
        time.sleep(1.2)
    if mode in ('fling', 'both'):
        start, lift = stroke(2500, 900, 110, 0)
        results.append(('fling', lift, lift + 1.5))
        time.sleep(2.5)
        stroke(900, 2700, 250, 300)
        stroke(900, 2700, 250, 300)
        time.sleep(1.5)
stop = True
thread.join()
fcntl.ioctl(fd, UI_DEV_DESTROY)
os.close(fd)
for label, t0, t1 in results:
    report(label, t0, t1)
