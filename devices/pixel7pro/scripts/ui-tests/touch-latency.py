#!/usr/bin/env python3
"""touch-latency.py RUNS [X Y]: after 2 s idle, touch down and drag; time from
the first move to the first new panel frame (test only, root)."""
import fcntl, glob, os, struct, sys, threading, time
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT, UI_SET_PROPBIT = 0x40045564, 0x40045565, 0x40045567, 0x4004556e
UI_DEV_SETUP, UI_ABS_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405c5503, 0x401c5504, 0x5501, 0x5502
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BTN_TOUCH, INPUT_PROP_DIRECT = 0x14a, 1
ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_X, ABS_MT_Y, ABS_MT_ID = 0, 1, 0x2f, 0x35, 0x36, 0x39
runs = int(sys.argv[1]); x = int(sys.argv[2]) if len(sys.argv) > 2 else 720; y0 = int(sys.argv[3]) if len(sys.argv) > 3 else 2200
stats = glob.glob('/sys/kernel/debug/dri/*/pixel_scanout')[0]
frames, stop = [], False
def sampler():
    f = open(stats); last = None
    while not stop:
        f.seek(0); n = int(f.read().split('starts ')[1].split()[0])
        if last is not None and n != last: frames.append(time.monotonic())
        last = n; time.sleep(0.0005)
fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
for ev in (EV_SYN, EV_KEY, EV_ABS): fcntl.ioctl(fd, UI_SET_EVBIT, ev)
fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH); fcntl.ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT)
for code, top in ((ABS_X, 1439), (ABS_Y, 3119), (ABS_MT_SLOT, 9), (ABS_MT_X, 1439), (ABS_MT_Y, 3119), (ABS_MT_ID, 65535)):
    fcntl.ioctl(fd, UI_SET_ABSBIT, code)
    fcntl.ioctl(fd, UI_ABS_SETUP, struct.pack('HHiiiiii', code, 0, 0, 0, top, 0, 0, 0))
fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack('HHHH80sI', 3, 0x1234, 0x5679, 1, b'synthetic latency touchscreen', 0))
fcntl.ioctl(fd, UI_DEV_CREATE); time.sleep(2.0)
def emit(*events):
    os.write(fd, b''.join(struct.pack('qqHHi', 0, 0, t, c, v) for t, c, v in events) + struct.pack('qqHHi', 0, 0, EV_SYN, 0, 0))
threading.Thread(target=sampler, daemon=True).start()
lat = []
for i in range(runs):
    time.sleep(2.0)                      # idle: clocks fall to their floor
    direction = -1 if i % 2 == 0 else 1
    emit((EV_ABS, ABS_MT_SLOT, 0), (EV_ABS, ABS_MT_ID, 100 + i), (EV_ABS, ABS_MT_X, x), (EV_ABS, ABS_MT_Y, y0),
         (EV_KEY, BTN_TOUCH, 1), (EV_ABS, ABS_X, x), (EV_ABS, ABS_Y, y0))
    time.sleep(0.03)
    t_move = None
    for s in range(1, 61):                # 250 ms at 240 Hz, 4 px per event
        y = y0 + direction * 4 * s
        emit((EV_ABS, ABS_MT_Y, y), (EV_ABS, ABS_Y, y))
        if t_move is None: t_move = time.monotonic()
        time.sleep(1 / 240)
    emit((EV_ABS, ABS_MT_ID, -1), (EV_KEY, BTN_TOUCH, 0))
    time.sleep(0.4)
    after = [t for t in frames if t > t_move]
    lat.append((after[0] - t_move) * 1000 if after else float('nan'))
stop = True; fcntl.ioctl(fd, UI_DEV_DESTROY); os.close(fd)
lat.sort()
print('first-frame latency ms:', ' '.join(f'{v:.0f}' for v in lat), f'| median {lat[len(lat) // 2]:.0f}')
