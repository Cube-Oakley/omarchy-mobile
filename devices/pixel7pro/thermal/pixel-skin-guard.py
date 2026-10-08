#!/usr/bin/env python3
"""Skin temperature guard for the Pixel 7 Pro.

Stock Android limits the CPUs and GPU by an estimate of the phone's surface
temperature, not by the chip's: its thermal HAL (vendor
/etc/thermal_info_config.json) computes VIRTUAL-SKIN from the board
thermistors on the S2MPG13 sub-PMIC. A power-budget PID holds the CPUs to
it from about 39 C (setpoint 41 C, CPUs and GPU together 45 C), with hard
limits from 46.5 C, while the chip itself may run to about 95 C. The kernel
here caps the chip near stock's own control range (pixel_acpm from 85 C,
pixel_gpu from 85 C); this guard adds the comfort limit on top.

VIRTUAL-SKIN is the hottest of four weighted pairs (stock's coefficients and
offsets, in C):
    0.58 gnss_tcxo + 0.42 disp      - 0.450
    0.559 usb_pwr2 + 0.441 quiet     - 1.771
    0.9 quiet      + 0.1 usb_pwr2    - 1.200
    0.378 neutral  + 0.622 quiet     - 0.453

The thermistors come from pixel-odpm's parameters/temps (it switches the
NTC channels on while they are read). Limits rise a step as the estimate
passes each threshold and fall a step once it has stayed a degree below for
ten seconds. CPU limits go to scaling_max_freq; the GPU limit to pixel_gpu's
skin_max_khz, which the kernel applies only while the GPU is powered.

    pixel-skin-guard.py [--once]   (--once prints the estimate and exits)
"""
import re, signal, sys, time

TEMPS = '/sys/module/pixel_odpm/parameters/temps'
GPU_SKIN = '/sys/module/pixel_gpu/parameters/skin_max_khz'
CPUFREQ = '/sys/devices/system/cpu/cpufreq'
POLL = 3.0
SETTLE = 10.0

# (skin C, BIG kHz, MID kHz, LITTLE kHz, GPU kHz); None: no limit.
LEVELS = [
    (None, None, None, None, None),
    (41.0, 2401000, 2130000, None, None),
    (43.0, 2048000, 1999000, None, 750000),
    (45.0, 1745000, 1663000, None, 603000),
    (46.5, 1426000, 1491000, None, 470000),
    (48.0, 1106000, 1197000, 1598000, 302000),
    (52.0, 851000, 910000, 1197000, 302000),
]
POLICIES = {'BIG': 'policy6', 'MID': 'policy4', 'LITTLE': 'policy0'}


def thermistors():
    """{name: C} from pixel-odpm, or None when it cannot be read."""
    try:
        text = open(TEMPS).read()
    except OSError:
        return None
    found = {}
    for name, value in re.findall(r'ntc\d raw \S+ ([a-z0-9]+) (-?\d+\.\d) C', text):
        found[name] = float(value)
    return found


def skin(t):
    needed = ('gnss', 'disp', 'usb2', 'quiet', 'neutral')
    if not t or any(k not in t for k in needed):
        return None
    return max(
        0.58 * t['gnss'] + 0.42 * t['disp'] - 0.450,
        0.559 * t['usb2'] + 0.441 * t['quiet'] - 1.771,
        0.9 * t['quiet'] + 0.1 * t['usb2'] - 1.200,
        0.378 * t['neutral'] + 0.622 * t['quiet'] - 0.453,
    )


def write(path, value):
    try:
        with open(path, 'w') as f:
            f.write(str(value))
        return True
    except OSError as e:
        print(f'skin guard: {path}: {e}', flush=True)
        return False


gpu_written = None


def apply(level):
    """Set the level's limits where they differ. A cluster whose CPUs are all
    offline (the screen off) refuses its files with EBUSY; it is set on a
    later poll, once a CPU is back."""
    global gpu_written
    _, big, mid, little, gpu = LEVELS[level]
    for cluster, khz in (('BIG', big), ('MID', mid), ('LITTLE', little)):
        policy = f'{CPUFREQ}/{POLICIES[cluster]}'
        try:
            top = int(open(f'{policy}/cpuinfo_max_freq').read())
            want = min(top, khz) if khz else top
            if int(open(f'{policy}/scaling_max_freq').read()) != want:
                write(f'{policy}/scaling_max_freq', want)
        except OSError:
            continue
    if gpu_written != (gpu or 0) and write(GPU_SKIN, gpu or 0):
        gpu_written = gpu or 0


def main():
    if '--once' in sys.argv:
        t = thermistors()
        print(t, 'skin', None if skin(t) is None else round(skin(t), 2))
        return
    level = 0
    below_since = None
    failures = 0
    apply(0)

    def stop(*_):
        apply(0)
        sys.exit(0)
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    while True:
        s = skin(thermistors())
        if s is None:
            failures += 1
            # A minute without readings: hold a moderate limit until they return.
            if failures == 20 and level < 2:
                level = 2
                apply(level)
                print('skin guard: no thermistor readings for a minute; holding level 2', flush=True)
        else:
            failures = 0
            want = level
            while want + 1 < len(LEVELS) and s >= LEVELS[want + 1][0]:
                want += 1
            if want > level:
                below_since = None
                print(f'skin guard: {s:.1f} C, level {level} -> {want}', flush=True)
                level = want
                apply(level)
            elif level > 0 and s < LEVELS[level][0] - 1.0:
                now = time.monotonic()
                below_since = below_since or now
                if now - below_since >= SETTLE:
                    print(f'skin guard: {s:.1f} C, level {level} -> {level - 1}', flush=True)
                    level -= 1
                    below_since = None
                    apply(level)
            else:
                below_since = None
        apply(level)
        time.sleep(POLL)


if __name__ == '__main__':
    main()
