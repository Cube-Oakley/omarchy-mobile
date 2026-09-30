#!/usr/bin/env python3
"""Screen rotation for the shell, as a lock with a suggestion (Android's
rotate button): the screen keeps its orientation, and when the phone is held
another way the shell offers to follow it. The phone's orientation comes
from iio-sensor-proxy (monitor-sensor), the screen's from Hyprland.

  status              JSON: the orientation the screen shows now
  set ORIENTATION     show the screen normal, left-up, right-up or bottom-up
                      (iio-sensor-proxy's names: the edge that points up)
  watch               a JSON line per change of how the phone is held, while
                      the screen is on (the accelerometer is released while
                      it is off), until killed
"""
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import sys
import time

CONFIG = Path(os.environ.get('XDG_CONFIG_HOME', str(Path.home() / '.config')))
DEVICE = CONFIG / 'omarchy-mobile/device.json'
# Hyprland's output transforms (wl_output: 1 = 90°, 2 = 180°, 3 = 270°) that
# show the picture upright when the named edge of the phone points up
# (checked by hand on the OnePlus 7 Pro in both landscape directions).
TRANSFORMS = {'normal': 0, 'left-up': 1, 'bottom-up': 2, 'right-up': 3}
ORIENTATION = re.compile(r'(?:orientation changed: |orientation: )([a-z-]+)')


def hyprctl(*args):
    return subprocess.run(['hyprctl', '-i', '0', *args], capture_output=True, text=True, timeout=10).stdout


def monitor():
    monitors = json.loads(hyprctl('-j', 'monitors') or '[]')
    return monitors[0] if monitors else None


def status():
    current = monitor()
    if current is None:
        return {'available': False}
    names = {value: name for name, value in TRANSFORMS.items()}
    return {'available': True, 'rotation': names.get(current.get('transform', 0), 'normal'),
            'screen_on': bool(current.get('dpmsStatus', True))}


def monitor_rule(name, device, transform):
    """The device profile's own mode and scale (as the theme writes them in
    hypr/mobile.lua), with the transform."""
    mode = device.get('displayMode') or 'preferred'
    scale = device.get('scale') or 'auto'
    return ('hl.monitor({ output = %s, mode = %s, position = "auto", scale = %s, transform = %d })'
            % (json.dumps(name), json.dumps(mode), json.dumps(scale) if isinstance(scale, str) else scale,
               transform))


def set_rotation(orientation):
    if orientation not in TRANSFORMS:
        raise ValueError('Unknown orientation: %s' % orientation)
    current = monitor()
    if current is None:
        raise ValueError('No display')
    try:
        device = json.loads(DEVICE.read_text())
    except (OSError, ValueError):
        device = {}
    # The touchscreen does not follow the output's transform on its own: set
    # it with the output, or touches land where the unrotated picture was.
    transform = TRANSFORMS[orientation]
    hyprctl('eval', monitor_rule(current['name'], device, transform)
            + '; hl.config({ input = { touchdevice = { transform = %d } } })' % transform)
    return status()


def parse(line):
    """The orientation a monitor-sensor line reports, or None."""
    match = ORIENTATION.search(line)
    return match.group(1) if match else None


def die_with_parent():
    """For a child: end with this process even if it is killed outright."""
    import ctypes
    ctypes.CDLL(None, use_errno=True).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG


def sensor_ready(has):
    """Whether iio-sensor-proxy has opened a sensor (HasAccelerometer,
    HasAmbientLight). Version 3.9 takes its D-Bus name before opening its
    sensors, and a claim in between is counted but never started, so its
    clients wait for this, and start again whenever the daemon restarts."""
    try:
        out = subprocess.run(['busctl', '--system', 'get-property', 'net.hadess.SensorProxy',
                              '/net/hadess/SensorProxy', 'net.hadess.SensorProxy', has],
                             capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    return out.strip() == 'b true'


def watch():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    # End with the shell too: a killed shell leaves its children running, and
    # this one would hold its sensor.
    die_with_parent()
    last = None
    while True:
        if not status().get('screen_on', False) or not sensor_ready('HasAccelerometer'):
            time.sleep(3)
            continue
        try:
            proc = subprocess.Popen(['stdbuf', '-oL', 'monitor-sensor', '--accel'],
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                    preexec_fn=die_with_parent)
        except OSError:
            time.sleep(30)
            continue
        pending, checked = b'', time.monotonic()
        try:
            while True:
                # Orientation lines come only on a change: wait for output,
                # but check the screen every few seconds.
                if time.monotonic() - checked >= 3:
                    checked = time.monotonic()
                    if not status().get('screen_on', False):
                        break  # screen off: release the accelerometer
                if select.select([proc.stdout], [], [], 3)[0]:
                    data = os.read(proc.stdout.fileno(), 4096)
                    if not data:
                        break
                    *lines, pending = (pending + data).split(b'\n')
                    if b'vanished' in data:
                        break  # the daemon restarted: wait until it is ready again
                    for line in lines:
                        orientation = parse(line.decode(errors='replace'))
                        if orientation and orientation != last:
                            last = orientation
                            print(json.dumps({'orientation': orientation}), flush=True)
        finally:
            proc.terminate()
        if last is not None:
            last = None
            print(json.dumps({'orientation': 'undefined'}), flush=True)
        time.sleep(1)


def main(args):
    if args == ['status'] or not args:
        return status()
    if len(args) == 2 and args[0] == 'set':
        return set_rotation(args[1])
    if args == ['watch']:
        watch()
        sys.exit(0)
    raise ValueError(__doc__.strip().split('\n\n', 1)[1])


if __name__ == '__main__':
    try:
        print(json.dumps(main(sys.argv[1:])))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(json.dumps({'available': False, 'error': str(error)}))
        sys.exit(1)
