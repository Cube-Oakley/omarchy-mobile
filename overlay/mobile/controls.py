#!/usr/bin/env python3
"""The phone's physical controls for the shell: screen brightness, the
flashlight, vibration and the alert slider. Everything is found through
standard kernel interfaces (backlight, LED and input classes), so the same
shell works on any phone that has them.

  status                 JSON: backlight, torch, haptics, slider
  brightness PERCENT     set the screen's brightness (1-100) and remember it
  restore                apply the remembered brightness (session start)
  torch on|off|toggle    the rear light at flashlight strength
  vibrate MS [PERCENT]   one pulse, 10-1000 ms at 1-100% (default 70%)
  watch                  print a line per alert slider change, until killed
  serve                  apply "brightness N" lines from standard input at
                         once, and "remember N" as the final level of a drag
  auto on|off            automatic brightness from the light sensor
  auto-run               follow the light sensor while automatic brightness
                         is on: a JSON line per change, until killed
"""
import fcntl
import json
import math
import os
from pathlib import Path
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import time

SYS = Path(os.environ.get('OMARCHY_MOBILE_SYSFS', '/sys'))
DEV = Path(os.environ.get('OMARCHY_MOBILE_DEV', '/dev'))
STATE = Path(os.environ.get('XDG_STATE_HOME', str(Path.home() / '.local/state'))) / 'omarchy-mobile/controls.json'
CONFIG = Path(os.environ.get('XDG_CONFIG_HOME', str(Path.home() / '.config')))
DEVICE = CONFIG / 'omarchy-mobile/device.json'

# Brightness follows the eye: the slider's share squared, never below 1% of
# the panel's range, so the bottom of the slider is dim but not black.
MIN_SHARE = 0.01
# A flashlight drives every flash LED at half its torch range (150 mA per
# LED on the OnePlus 7 Pro), bright enough without heating the LEDs.
TORCH_SHARE = 0.5
# The sound-profile axis of alert sliders (ABS_SND_PROFILE, Linux 6.18).
ABS_SND_PROFILE = 0x22
PROFILES = {0: 'silent', 1: 'vibrate', 2: 'ring'}
EV_ABS, EV_FF, FF_RUMBLE = 0x03, 0x15, 0x50
# A light sensor under the panel may see the panel's own light: the device
# profile's panelLux is what it adds at full output. The OnePlus 7 Pro's adds
# about 190 lux (the room's 110 lux read 113 at 5%, 301 at 100%); the Pixel
# 7 Pro's AoC leaves the panel out (0).


class AutoBrightness:
    """Brightness from ambient light: a log curve in the slider's (perceptual)
    scale, shifted by the user's own preference, which is learnt whenever
    they set the slider. A change is made only when it is worth one, and
    then faded in (ramp)."""
    STEP = 4          # slider points before a change is worth making
    INTERVAL = 2.0    # seconds between changes
    SETTLE = 5.0      # seconds a slider drag is left alone

    def __init__(self, offset=None, panel_lux=0.0):
        self.offset = offset
        self.panel_lux = panel_lux
        self.level = None      # filtered log10(lux + 1)
        self.applied = None
        self.written = 0.0
        self.manual_since = None

    @staticmethod
    def base(level):
        return 10 + 17 * level

    def target(self):
        return min(100, max(1, round(self.base(self.level) + self.offset)))

    def user_set(self, percent):
        """The slider's final level: learn it as the preference here."""
        if self.level is not None:
            self.offset = percent - self.base(self.level)
        self.applied = percent
        self.manual_since = None

    def light(self, lux, share, current, now):
        """A sensor reading: lux, the panel's current output share (0-1) and
        slider percent. Returns a percent to apply, or None."""
        ambient = max(0.0, lux - self.panel_lux * share)
        level = math.log10(ambient + 1)
        if self.level is None:
            self.level = level
        else:  # brighten quickly, darken slowly
            self.level += (0.35 if level > self.level else 0.12) * (level - self.level)
        if self.offset is None:  # first use: keep the level the user has now
            self.offset = current - self.base(self.level)
        if self.applied is None:
            self.applied = current
        if abs(current - self.applied) > 1:  # someone is moving the slider
            if self.manual_since is None:
                self.manual_since = now
            if now - self.manual_since < self.SETTLE:
                return None
            self.user_set(current)
        wanted = self.target()
        if abs(wanted - self.applied) < self.STEP or now - self.written < self.INTERVAL:
            return None
        self.applied, self.written = wanted, now
        return wanted


def ramp(start, end, seconds=1.0, step_time=0.04):
    """The levels of a fade from start to end: a slider point per step, over
    at most `seconds`. Needs kernel #194 or later, which keeps panel commands
    out of frame transfers; before it, every step could flicker."""
    if start == end:
        return [end]
    steps = min(abs(end - start), max(1, int(seconds / step_time)))
    return [round(start + (end - start) * i / steps) for i in range(1, steps + 1)]


def ioc(direction, number, size):
    return (direction << 30) | (size << 16) | (ord('E') << 8) | number


def load_state():
    try:
        data = json.loads(STATE.read_text())
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def save_state(state):
    STATE.parent.mkdir(parents=True, exist_ok=True)
    temp = STATE.with_suffix('.tmp')
    temp.write_text(json.dumps(state))
    temp.replace(STATE)


def read_int(path):
    try:
        return int(path.read_text().strip())
    except (OSError, ValueError):
        return None


def backlight():
    root = SYS / 'class/backlight'
    for device in sorted(root.iterdir()) if root.is_dir() else []:
        current, maximum = read_int(device / 'brightness'), read_int(device / 'max_brightness')
        if current is not None and maximum:
            return device, current, maximum
    return None


def percent_for(raw, maximum):
    share = max(raw / maximum, MIN_SHARE)
    return max(1, min(100, round(100 * share ** 0.5)))


def raw_for(percent, maximum):
    share = max((min(max(percent, 1), 100) / 100) ** 2, MIN_SHARE)
    return max(1, round(maximum * share))


def torch_leds():
    root = SYS / 'class/leds'
    names = sorted(root.iterdir()) if root.is_dir() else []
    return [led for led in names
            if led.name.endswith(':torch') or led.name.endswith(':flash')
            or ':flash-' in led.name]


def input_devices(kind, bit):
    """Event devices whose capability bitmap `kind` has `bit` set."""
    found = []
    root = SYS / 'class/input'
    for event in sorted(root.glob('event*')) if root.is_dir() else []:
        try:
            words = (event / 'device/capabilities' / kind).read_text().split()
        except OSError:
            continue
        mask = int(''.join(word.zfill(16) for word in words) or '0', 16)
        if mask >> bit & 1:
            found.append(DEV / 'input' / event.name)
    return found


def slider_position():
    for path in input_devices('abs', ABS_SND_PROFILE):
        try:
            fd = os.open(path, os.O_RDONLY)
        except OSError:
            continue
        try:
            info = bytearray(24)
            fcntl.ioctl(fd, ioc(2, 0x40 + ABS_SND_PROFILE, 24), info)
            return PROFILES.get(struct.unpack('6i', info)[0])
        except OSError:
            continue
        finally:
            os.close(fd)
    return None


def status():
    light = backlight()
    leds = torch_leds()
    return {
        'available': True,
        'backlight': None if light is None else {
            'device': light[0].name, 'percent': percent_for(light[1], light[2])},
        'torch': {'available': bool(leds),
                  'on': any((read_int(led / 'brightness') or 0) > 0 for led in leds)},
        'haptics': bool(input_devices('ff', FF_RUMBLE)),
        'slider': slider_position(),
        'auto': load_state().get('auto_brightness') is True,
        'light': shutil.which('monitor-sensor') is not None,
    }


def set_brightness(percent, remember=True):
    light = backlight()
    if light is None:
        raise ValueError('No backlight')
    device, _, maximum = light
    (device / 'brightness').write_text(str(raw_for(percent, maximum)))
    if remember:
        state = load_state()
        state['brightness'] = min(max(percent, 1), 100)
        save_state(state)


def set_torch(action):
    leds = torch_leds()
    if not leds:
        raise ValueError('No flashlight')
    on = any((read_int(led / 'brightness') or 0) > 0 for led in leds)
    turn_on = {'on': True, 'off': False, 'toggle': not on}[action]
    for led in leds:
        maximum = read_int(led / 'max_brightness') or 1
        level = max(1, round(maximum * TORCH_SHARE)) if turn_on else 0
        (led / 'brightness').write_text(str(level))


def vibrate(length, percent):
    devices = input_devices('ff', FF_RUMBLE)
    if not devices:
        raise ValueError('No vibration motor')
    length = min(max(length, 10), 1000)
    magnitude = 0xffff * min(max(percent, 1), 100) // 100
    fd = os.open(devices[0], os.O_RDWR)
    try:
        # struct ff_effect, 48 bytes on 64-bit: type, id, direction, trigger,
        # replay (length, delay), then the rumble magnitudes at offset 16.
        effect = bytearray(struct.pack('HhHHHHH2x', FF_RUMBLE, -1, 0, 0, 0, length, 0)
                           + struct.pack('HH', magnitude, 0) + bytes(28))
        fcntl.ioctl(fd, ioc(1, 0x80, 48), effect)
        effect_id = struct.unpack_from('h', effect, 2)[0]
        try:
            os.write(fd, struct.pack('qqHHi', 0, 0, EV_FF, effect_id, 1))
            time.sleep(length / 1000)
        finally:
            fcntl.ioctl(fd, ioc(1, 0x81, 4), effect_id)
    finally:
        os.close(fd)


def watch():
    fds = []
    for path in input_devices('abs', ABS_SND_PROFILE):
        try:
            fds.append(os.open(path, os.O_RDONLY | os.O_NONBLOCK))
        except OSError:
            pass
    if not fds:
        # Nothing yet (the board may still be loading it): the shell restarts
        # this shortly after it exits, which looks again.
        time.sleep(30)
        return
    # Found: a first line makes the shell read the status straight away.
    print(json.dumps({'slider': slider_position()}), flush=True)
    while True:
        for fd in select.select(fds, [], [])[0]:
            data = os.read(fd, 24 * 64)
            for i in range(0, len(data) - 23, 24):
                _, _, kind, code, value = struct.unpack('qqHHi', data[i:i + 24])
                if kind == EV_ABS and code == ABS_SND_PROFILE:
                    print(json.dumps({'slider': PROFILES.get(value)}), flush=True)


def serve():
    """A resident writer, so a brightness drag starts no process per step."""
    for line in sys.stdin:
        words = line.split()
        if len(words) != 2 or words[0] not in ('brightness', 'remember') or not words[1].isdigit():
            continue
        try:
            set_brightness(int(words[1]), remember=words[0] == 'remember')
        except (OSError, ValueError) as error:
            print(json.dumps({'error': str(error)}), flush=True)


def die_with_parent():
    """For a child: end with this process even if it is killed outright."""
    import ctypes
    ctypes.CDLL(None, use_errno=True).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG


def panel_lux():
    """The device profile's panelLux (see AutoBrightness), 0 if it has none."""
    try:
        value = json.loads(DEVICE.read_text()).get('panelLux', 0)
    except (OSError, ValueError, AttributeError):
        return 0.0
    return float(value) if isinstance(value, (int, float)) else 0.0


def screen_on():
    """Whether Hyprland shows the screen (DPMS on); on if it cannot say."""
    try:
        out = subprocess.run(['hyprctl', '-i', '0', '-j', 'monitors'],
                             capture_output=True, text=True, timeout=10).stdout
        return any(m.get('dpmsStatus', True) for m in json.loads(out or '[]'))
    except (OSError, ValueError, AttributeError, subprocess.SubprocessError):
        return True


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


def auto_run():
    """Follow the light sensor through iio-sensor-proxy (monitor-sensor) while
    automatic brightness is on and the screen is on (the sensor is released
    while it is off); the slider's final levels, which serve remembers, teach
    it the user's preference. A reading stands until the next one, and counts
    again every second: sensors that report only changes go quiet in steady
    light, and the curve still has to settle."""
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    # End with the shell too: a killed shell leaves its children running, and
    # this one would hold its sensor.
    die_with_parent()
    state = load_state()
    if state.get('auto_brightness') is not True:
        return
    auto = AutoBrightness(state.get('auto_offset'), panel_lux())
    remembered = state.get('brightness')
    # Only readings: the value monitor-sensor starts with may be the daemon's
    # placeholder, or stale from before the screen went off.
    pattern = re.compile(r'Light changed: ([0-9.]+)')
    while True:
        if not screen_on() or not sensor_ready('HasAmbientLight'):
            time.sleep(3)
            continue
        try:
            proc = subprocess.Popen(['stdbuf', '-oL', 'monitor-sensor', '--light'],
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                    preexec_fn=die_with_parent)
        except OSError:
            time.sleep(30)
            continue
        pending, lux, checked = b'', None, time.monotonic()
        try:
            while True:
                if time.monotonic() - checked >= 3:
                    checked = time.monotonic()
                    if not screen_on():
                        break  # screen off: release the light sensor
                if select.select([proc.stdout], [], [], 1)[0]:
                    data = os.read(proc.stdout.fileno(), 4096)
                    if not data or b'vanished' in data:
                        break  # the daemon restarted: wait until it is ready again
                    *lines, pending = (pending + data).split(b'\n')
                    for line in lines:
                        match = pattern.search(line.decode(errors='replace'))
                        if match:
                            lux = float(match.group(1))
                light = backlight()
                if lux is None or light is None:
                    continue
                _, raw, maximum = light
                state = load_state()
                if state.get('auto_brightness') is not True:
                    return
                if state.get('brightness') != remembered:
                    remembered = state.get('brightness')
                    if isinstance(remembered, int):
                        auto.user_set(remembered)
                        state['auto_offset'] = auto.offset
                        save_state(state)
                current = percent_for(raw, maximum)
                wanted = auto.light(lux, raw / maximum, current, time.monotonic())
                if wanted is not None:
                    for level in ramp(current, wanted):
                        set_brightness(level, remember=False)
                        time.sleep(0.04)
                    print(json.dumps({'auto': {'lux': lux, 'percent': wanted}}), flush=True)
        finally:
            proc.terminate()
            proc.wait()
        time.sleep(1)


def main(args):
    action = args[0] if args else 'status'
    if action == 'status' and len(args) == 1 or not args:
        return status()
    if action == 'brightness' and len(args) == 2 and args[1].isdigit():
        set_brightness(int(args[1]))
        return status()
    if action == 'restore' and len(args) == 1:
        saved = load_state().get('brightness')
        if isinstance(saved, int) and backlight() is not None:
            set_brightness(saved, remember=False)
        return status()
    if action == 'torch' and len(args) == 2 and args[1] in ('on', 'off', 'toggle'):
        set_torch(args[1])
        return status()
    if action == 'vibrate' and len(args) in (2, 3) and all(a.isdigit() for a in args[1:]):
        vibrate(int(args[1]), int(args[2]) if len(args) == 3 else 70)
        return {'ok': True}
    if action == 'watch' and len(args) == 1:
        watch()
        sys.exit(0)
    if action == 'serve' and len(args) == 1:
        serve()
        sys.exit(0)
    if action == 'auto' and len(args) == 2 and args[1] in ('on', 'off'):
        state = load_state()
        state['auto_brightness'] = args[1] == 'on'
        save_state(state)
        return status()
    if action == 'auto-run' and len(args) == 1:
        auto_run()
        sys.exit(0)
    raise ValueError(__doc__.strip().split('\n\n', 1)[1])


if __name__ == '__main__':
    try:
        print(json.dumps(main(sys.argv[1:])))
    except (OSError, ValueError) as error:
        print(json.dumps({'available': False, 'error': str(error)}))
        sys.exit(1)
