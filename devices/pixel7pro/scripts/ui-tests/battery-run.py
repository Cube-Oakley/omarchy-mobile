#!/usr/bin/env python3
"""Battery run: heavy-ish daily use, unattended, until the battery is low
(test only).

  battery-run.py [--stop-at PERCENT] [--profile PROFILE] [--seed N]
                 [--night HHMM-HHMM] [--on-cable]
  on the phone as root, unplugged, started with setsid: SSH over Wi-Fi drops
  whenever the phone sleeps

Profiles: day (the default), stress (bursts only), and short checks: smoke
(each activity once, two naps), naps (four timed naps), race (the screen
off and on again while the phone gets ready to sleep). --on-cable runs one
on the cable, the battery supplying everything (charging off for the run).
A reboot ends the run; it is not started again.

The phone is used as a person uses it. It is woken and unlocked (the PIN),
then X is scrolled, a video watched, web pages read, apps and Android apps
opened and closed, the camera looked through, or the screen left lit on
whatever is in front until the screen timeout darkens it. Then the screen goes
to the always-on clock (sometimes off) and the phone sleeps for a while. A
wake request (omarchy-mobile-sleep's wake.d) ends each rest, standing in for
the power key. Every couple of hours a burst of short wakes and sleeps puts
the suspend path through many more cycles; the stress profile is only bursts.
At night (--night, local time) the phone mostly rests.

Only swipes reach the user's apps (X, Discord, Telegram, Android apps): no
taps, so nothing is liked, sent or opened. Taps go only to the lock's keypad,
and only while it shows. Video and web pages are in a scratch browser profile.

A finger on the screen or a key press that the run did not make (the touch
and key interrupts; the run's own touches are synthetic) means the owner is
using the phone: the run steps aside until the screen has been dark for
USER_QUIET seconds. So does a charger, and the file PAUSE. The file STOP ends
the run.

Results: LOG, a JSON line per event with the battery's state (charge used
since the start from the gauge's coulomb counter, current, temperature).
STATE says what the run is doing, for a watcher on the laptop.
"""
import argparse
import contextlib
import importlib.util
import json
import os
import random
import signal
import subprocess
import sys
import time
from pathlib import Path

TOOLS = '/root/tools'
SOAK = Path(TOOLS) / 'soak'
LOG = SOAK / 'battery-run.jsonl'
STATE = SOAK / 'battery-run.state'
PAUSE = SOAK / 'battery-run.pause'
STOP = SOAK / 'battery-run.stop'
CUBE = 1001
RUNTIME = Path(f'/run/user/{CUBE}')
WAKE_REQUEST = RUNTIME / 'omarchy-mobile-wake.d' / 'battery-run'
# Present while the suspend adapter's call runs (pixel-suspend.py). A power
# key wakes the phone and the call returns before the screen lights; the run
# waits for the same, as lighting the screen under a call that is about to
# sleep again only has pixel-sleep refuse (display domains powered).
ADAPTER_ACTIVE = Path('/run/pixel-suspend.active')
ADAPTER_WAIT = 90
# Seconds for Wi-Fi to pass traffic after a wake before it counts as down.
WIFI_WAIT = 60
SLEEP_LOG = Path('/home/cube/.local/state/omarchy-mobile/sleep.log')
PREFS = Path('/home/cube/.config/omarchy-mobile/prefs.json')
BATTERY = Path('/sys/class/power_supply/battery')
COULOMB = Path('/sys/module/pixel_battery/parameters/coulomb')
SUSPEND_STATS = Path('/sys/power/suspend_stats')
INPUT_OFF = Path('/sys/module/pixel_battery/parameters/input_off')
ALLOW_CHARGING = Path('/run/pixel-suspend-allow-charging')
CHARGERS = [Path('/sys/class/power_supply/usb/online'), Path('/sys/class/power_supply/wireless/online')]
WORK = Path('/home/cube/.cache/omarchy-mobile/battery-run')
WWW = WORK / 'www'
HTTP_PORT = 8099
BROWSER = '/home/cube/.local/bin/omarchy-mobile-browser'
X_APP = 'omarchy-mobile-webapp-x-com'
SOCIAL = ['omarchy-mobile-webapp-discord-com', 'omarchy-mobile-webapp-web-telegram-org']
ANDROID = ['waydroid.com.whatsapp', 'waydroid.org.thoughtcrime.securesms', 'waydroid.org.fdroid.fdroid']
APPS = ['calculator', 'clock', 'notes', 'contacts', 'calendar', 'files', 'messages', 'phone',
        'omarchy-mobile-settings']
# Window titles where they differ from the app ID's.
APP_TITLES = {'omarchy-mobile-settings': 'Settings'}
# Lists that scroll with nothing a vertical swipe could change.
SCROLLABLE = {'contacts', 'messages', 'files', 'omarchy-mobile-settings'}
SITES = ['https://en.m.wikipedia.org/wiki/Special:Random', 'https://news.ycombinator.com/',
         'https://www.bbc.com/news', 'https://arstechnica.com/', 'https://www.theverge.com/',
         'https://apnews.com/', 'https://old.reddit.com/r/Android/', 'https://github.com/trending',
         'https://forecast.weather.gov/MapClick.php?lat=37.77&lon=-122.42', 'https://www.allrecipes.com/']
# Seconds of darkness, with no touch or key from the owner, before the run
# takes the phone back.
USER_QUIET = 600
# Heavy use waits while the battery is this warm (0.1 degC).
HOT = 420
# Panel pixels: a synthetic touchscreen covers 1440x3120.
HOME_SWIPE = 'swipe:720,3115,2300,200'


def load_soak():
    """function-soak.py's helpers: the shell's IPC, the display, the lock."""
    spec = importlib.util.spec_from_file_location('function_soak', f'{TOOLS}/function-soak.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


fs = load_soak()


class UserActive(Exception):
    pass


class Stopped(Exception):
    pass


# ---------------------------------------------------------------- touch

class Touch:
    """One synthetic touchscreen for the whole run (a new device per gesture
    costs two seconds and a hotplug); touch-seq.py's actions."""
    import fcntl as _fcntl
    import struct as _struct

    def __init__(self):
        f, s = self._fcntl, self._struct
        self.fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
        for ev in (0, 1, 3):
            f.ioctl(self.fd, 0x40045564, ev)
        f.ioctl(self.fd, 0x40045565, 0x14a)
        f.ioctl(self.fd, 0x4004556e, 1)
        for code, top in ((0, 1439), (1, 3119), (0x2f, 9), (0x35, 1439), (0x36, 3119), (0x39, 65535)):
            f.ioctl(self.fd, 0x40045567, code)
            f.ioctl(self.fd, 0x401c5504, s.pack('HHiiiiii', code, 0, 0, 0, top, 0, 0, 0))
        f.ioctl(self.fd, 0x405c5503, s.pack('HHHH80sI', 3, 0x1234, 0x5678, 1, b'battery-run touchscreen', 0))
        f.ioctl(self.fd, 0x5501)
        self.tracking = 1
        time.sleep(2)

    def emit(self, *events):
        p = self._struct.pack
        os.write(self.fd, b''.join(p('qqHHi', 0, 0, t, c, v) for t, c, v in events) + p('qqHHi', 0, 0, 0, 0, 0))

    def down(self, x, y):
        self.tracking += 1
        self.emit((3, 0x2f, 0), (3, 0x39, self.tracking), (3, 0x35, x), (3, 0x36, y), (1, 0x14a, 1), (3, 0, x), (3, 1, y))

    def up(self):
        self.emit((3, 0x39, -1), (1, 0x14a, 0))

    def run(self, *actions):
        for action in actions:
            kind, _, args = action.partition(':')
            v = [int(a) for a in args.split(',')] if args else []
            if kind == 'tap':
                self.down(v[0], v[1])
                time.sleep(0.06)
                self.up()
                time.sleep(0.25)
            elif kind == 'swipe':
                x, y0, y1, ms = v[:4]
                hold = v[4] if len(v) > 4 else 0
                self.down(x, y0)
                steps = max(1, int(ms / 1000 * 240))
                y = y0
                for i in range(1, steps + 1):
                    time.sleep(1 / 240)
                    y = int(y0 + (y1 - y0) * i / steps)
                    self.emit((3, 0x36, y), (3, 1, y))
                end, jitter = time.monotonic() + hold / 1000, 0
                while time.monotonic() < end:
                    time.sleep(1 / 240)
                    jitter ^= 1
                    self.emit((3, 0x36, y1 + jitter), (3, 1, y1 + jitter))
                time.sleep(1 / 240)
                self.up()
            elif kind == 'wait':
                time.sleep(v[0] / 1000)


touch = None


def gesture(*actions):
    touch.run(*actions)


fs.touch = gesture


# ---------------------------------------------------------------- state

def read(path, default=''):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return default


def coulomb():
    return int(read(COULOMB, '0 0').split()[1])


def charger():
    return any(read(p) == '1' for p in CHARGERS)


def interrupts():
    """(key presses, touch interrupts) since boot, on the CPUs online: a dark
    screen takes cores offline, and their columns leave /proc/interrupts (the
    numbers after them are the chip's, not counts)."""
    keys = touches = 0
    for line in read('/proc/interrupts').splitlines():
        line = line.rstrip()
        if line.endswith('pixel-keys') or line.endswith('pixel-touch-attn'):
            count = 0
            for field in line.split()[1:]:
                if not field.isdigit():
                    break
                count += int(field)
            if line.endswith('pixel-keys'):
                keys += count
            else:
                touches += count
    return keys, touches


class RealInput:
    """The phone's own touchscreen and keys, read without grabbing them: a
    finger down or a key pressed is the owner. The run's touches come from its
    own uinput device and never show here."""
    NAMES = ('Pixel keys', 'Pixel 7 Pro Synaptics S3908')

    def __init__(self):
        self.devices = []
        for block in read('/proc/bus/input/devices').split('\n\n'):
            name = next((line[len('N: Name="'):-1] for line in block.splitlines() if line.startswith('N: Name=')), '')
            handler = next((h for line in block.splitlines() if line.startswith('H: Handlers=')
                            for h in line.split('=', 1)[1].split() if h.startswith('event')), None)
            if name in self.NAMES and handler:
                self.devices.append(os.open(f'/dev/input/{handler}', os.O_RDONLY | os.O_NONBLOCK))
        if len(self.devices) != len(self.NAMES):
            raise RuntimeError(f'found {len(self.devices)} of the input devices {self.NAMES}')

    def news(self):
        """Fingers put down (at x, y) and keys pressed (codes) since the last
        call."""
        touches, keys = [], []
        for fd in self.devices:
            x = y = None
            down = False
            while True:
                try:
                    data = os.read(fd, 24 * 128)
                except BlockingIOError:
                    break
                if not data:
                    break
                for i in range(0, len(data) - 23, 24):
                    _, _, kind, code, value = Touch._struct.unpack('qqHHi', data[i:i + 24])
                    if kind == 3 and code == 0x35:
                        x = value
                    elif kind == 3 and code == 0x36:
                        y = value
                    elif (kind == 3 and code == 0x39 and value >= 0) or (kind == 1 and code == 0x14a and value == 1):
                        down = True
                    elif kind == 1 and value == 1:
                        keys.append(code)
                    elif kind == 0 and down:
                        touches.append((x, y))
                        down = False
        return touches, keys


def suspends():
    return int(read(SUSPEND_STATS / 'success', '0')), int(read(SUSPEND_STATS / 'fail', '0'))


class Run:
    def __init__(self, args):
        self.args = args
        self.q0 = coulomb()
        self.t0 = time.time()
        self.boot = read('/proc/sys/kernel/random/boot_id')
        self.phase = 'start'
        self.next_wake = None
        self.input = RealInput()
        self.counts = {'sessions': 0, 'rests': 0, 'bursts': 0, 'burst_cycles': 0, 'fails': 0, 'user': 0}
        self.kmsg = fs.Kmsg()
        self.log = open(LOG, 'a', buffering=1)
        self.opened = {}  # native app -> window address left open

    def used(self):
        return round((self.q0 - coulomb()) / 1e6, 1)

    def battery(self):
        return {'cap': int(read(BATTERY / 'capacity', '0')), 'used': self.used(),
                'ma': round(int(read(BATTERY / 'current_now', '0')) / 1000),
                'mv': round(int(read(BATTERY / 'voltage_now', '0')) / 1000),
                'temp': int(read(BATTERY / 'temp', '0')) / 10}

    def record(self, event, **fields):
        warnings = self.kmsg.news()
        if warnings:
            fields['kernel'] = len(warnings)
            fields['kernel_first'] = [w[:200] for w in warnings[:5]]
        entry = {'t': round(time.time(), 1), 'ev': event, **self.battery(), **fields}
        self.log.write(json.dumps(entry) + '\n')
        os.fsync(self.log.fileno())
        self.save_state()
        return entry

    def save_state(self):
        data = {'pid': os.getpid(), 'boot': self.boot, 'phase': self.phase, 'started': self.t0,
                'next_wake': self.next_wake, 'profile': self.args.profile, **self.counts, **self.battery()}
        tmp = STATE.with_suffix('.tmp')
        tmp.write_text(json.dumps(data) + '\n')
        os.replace(tmp, STATE)

    # ------------------------------------------------------------ guards

    def check(self):
        """Raises UserActive when the owner touched the phone or pressed a
        key since the last look, or plugged it in; Stopped for STOP."""
        if STOP.exists():
            raise Stopped('stop file')
        touches, keys = self.input.news()
        if touches or keys:
            raise UserActive(f'touch at {touches[:3]}, keys {keys[:3]}')
        if PAUSE.exists():
            raise UserActive('pause file')
        if charger() and not self.args.on_cable:
            raise UserActive('charger')

    def wait(self, seconds, screen_on=True):
        """Seconds of nothing, looking for the owner each second; with
        screen_on, also that the screen stays lit."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            time.sleep(min(1, max(0, end - time.monotonic())))
            self.check()
            if screen_on and fs.screen() != 'on':
                raise fs.Failed(f'screen went {fs.screen() or "?"} by itself')

    def step_aside(self, why):
        """The owner has the phone: wait until it has been dark and untouched
        for USER_QUIET seconds (and unplugged, and not paused). A wake request
        at the end of that quiet bounds the sleep the policy is in then."""
        self.counts['user'] += 1
        self.clear_wake()
        self.phase = 'user'
        q, t = coulomb(), time.time()
        self.record('user', why=why)
        quiet_since = time.time()
        self.request_wake(quiet_since + USER_QUIET)
        while time.time() - quiet_since < USER_QUIET:
            time.sleep(5)
            if STOP.exists():
                raise Stopped('stop file')
            touches, keys = self.input.news()
            if (touches or keys or fs.screen() == 'on' or PAUSE.exists()
                    or (charger() and not self.args.on_cable)):
                quiet_since = time.time()
                if self.next_wake != int(quiet_since + USER_QUIET):
                    self.request_wake(quiet_since + USER_QUIET)
        self.record('user-done', minutes=round((time.time() - t) / 60, 1),
                    mah=round((q - coulomb()) / 1e6, 1))

    # ------------------------------------------------------------ screen

    def request_wake(self, when):
        WAKE_REQUEST.parent.mkdir(exist_ok=True)
        os.chown(WAKE_REQUEST.parent, CUBE, CUBE)
        WAKE_REQUEST.write_text(f'{int(when)}\n')
        os.chown(WAKE_REQUEST, CUBE, CUBE)
        self.next_wake = int(when)

    def clear_wake(self):
        WAKE_REQUEST.unlink(missing_ok=True)
        self.next_wake = None

    def wake_screen(self):
        """The screen lit as a power key's wake lights it: once the suspend
        adapter's call has returned. Seconds waited for it."""
        start = time.time()
        while ADAPTER_ACTIVE.exists() and time.time() - start < ADAPTER_WAIT:
            time.sleep(0.5)
            self.check()
        fs.ensure_lit()
        self.input.news()
        return round(time.time() - start, 1)

    def light(self, unlock=True):
        if fs.screen() != 'on':
            self.wake_screen()
        fs.ensure_lit()
        if unlock:
            fs.unlock()

    def dark(self, mode):
        fs.display(mode)
        fs.until(lambda: fs.screen() == mode, 10, f'screen did not go {mode}')

    def wifi_check(self):
        """Wi-Fi back and passing traffic after a wake (it reloads with the
        screen): seconds it took. A dead one is recorded with the driver's
        last words, then restarted so the run goes on."""
        start = time.monotonic()
        while time.monotonic() - start < WIFI_WAIT:
            if fs.run('ping', '-c', '1', '-W', '2', '-I', fs.wifi_device(), '9.9.9.9', timeout=10)[0] == 0:
                return round(time.monotonic() - start, 1)
            self.wait(2)
        self.counts['wifi_down'] = self.counts.get('wifi_down', 0) + 1
        code, state = fs.run('nmcli', '-t', '-f', 'DEVICE,TYPE,STATE', 'device', timeout=10)
        code, dmesg = fs.run('dmesg', timeout=10)
        driver = [line[:200] for line in dmesg.splitlines()
                  if 'brcmf' in line or 'Bluetooth' in line or 'PM: suspend' in line][-25:]
        self.record('wifi-down', seconds=WIFI_WAIT, nmcli=state.splitlines(), driver=driver)
        code, out = fs.run('/usr/local/sbin/pixel-wifi-restart', timeout=150)
        back = False
        for _ in range(20):
            self.wait(3)
            if fs.run('ping', '-c', '1', '-W', '2', '-I', fs.wifi_device(), '9.9.9.9', timeout=10)[0] == 0:
                back = True
                break
        self.record('wifi-restart', exit=code, back=back)
        return None

    def rest(self, seconds, mode=None, why='rest'):
        """The screen dark (always-on clock or off) for seconds, the phone
        asleep as the policy decides, then lit again (still locked) once the
        wake request has woken it."""
        mode = mode or ('ambient' if random.random() < 0.75 else 'off')
        if fs.screen() == 'on':
            self.dark(mode)
        mode = fs.screen()
        self.counts['rests'] += 1
        self.phase = why
        start, q = time.time(), coulomb()
        stats = suspends()
        wake_at = start + seconds
        self.request_wake(wake_at)
        self.record(f'{why}-start', mode=mode, minutes=round(seconds / 60, 1))
        try:
            while time.time() < wake_at:
                time.sleep(1)
                self.check()
                if fs.screen() == 'on':
                    raise UserActive('screen lit by something else')
            late = round(time.time() - wake_at, 1)
            waited = self.wake_screen()
        finally:
            self.clear_wake()
        took = time.time() - start
        after = suspends()
        sleep = sleep_summary(start, time.time())
        self.record(f'{why}-end', mode=mode, minutes=round(took / 60, 1), late=late, adapter_wait=waited,
                    mah=round((q - coulomb()) / 1e6, 2), avg_ma=round((q - coulomb()) / 1e6 * 3600 / took, 1),
                    suspends=after[0] - stats[0], suspend_fails=after[1] - stats[1], **sleep)

    # ------------------------------------------------------------ apps

    def front(self, app, seconds=30):
        """An app with one window (web app, Android) brought to the front,
        opened if it was not: its address."""
        if fs.ipc('launchApp', app) != 'true':
            raise fs.Failed(f'{app}: unknown app')
        found = {}

        def shown():
            for address, c in fs.windows().items():
                if c.get('class') == app and fs.in_front(c):
                    found['address'] = address
                    return True
            return False
        fs.until(shown, seconds, f'{app}: not in front')
        return found['address']

    def home(self):
        gesture(HOME_SWIPE)
        time.sleep(1)

    def scroll(self, seconds, x_range=(450, 990), fling=0.5, back=0.12, dwell=(2, 8)):
        """Reading: a swipe up, a pause to read, now and then back up."""
        end = time.monotonic() + seconds
        swipes = 0
        while time.monotonic() < end:
            x = random.randint(*x_range)
            if random.random() < back:
                gesture(f'swipe:{x},{random.randint(900, 1200)},{random.randint(1900, 2300)},'
                        f'{random.randint(250, 400)},120')
            else:
                hold = 0 if random.random() < fling else random.randint(80, 160)
                gesture(f'swipe:{x},{random.randint(2100, 2500)},{random.randint(700, 1100)},'
                        f'{random.randint(180, 420)},{hold}')
            swipes += 1
            self.wait(min(random.uniform(*dwell), max(0.5, end - time.monotonic())))
        return swipes

    def scratch_browser(self, url, port):
        """A browser window on url with a scratch profile (the owner's tabs
        and profile untouched) and DevTools on port: its address."""
        profile = WORK / 'profile'
        data = profile / 'data/omarchy-mobile/browser'
        data.mkdir(parents=True, exist_ok=True)
        qml = Path('/home/cube/.local/share/omarchy-mobile/browser/Browser.qml')
        (data / 'Browser.qml').write_bytes(qml.read_bytes())
        subprocess.run(['chown', '-R', f'{CUBE}:{CUBE}', str(WORK)], check=False)
        before = fs.windows()
        xdg = ' '.join(f'XDG_{k}_HOME={profile}/{k.lower()}' for k in ('DATA', 'STATE', 'CONFIG', 'CACHE'))
        fs.cube('sh', '-c', f'{xdg} QTWEBENGINE_REMOTE_DEBUGGING=127.0.0.1:{port} setsid -f {BROWSER} '
                f'--app {url} >{WORK}/browser-{port}.log 2>&1')
        new = {}

        def opened():
            new.update({a: c for a, c in fs.windows().items() if a not in before})
            return bool(new)
        fs.until(opened, 30, f'scratch browser on {url}: no window')
        return next(iter(new))

    def close_browser(self, address):
        try:
            fs.close([address])
        finally:
            # The process ends with its window; one that lingers is ended.
            # Found by its scratch profile: the owner's web apps run the
            # same program.
            time.sleep(2)
            for pid in scratch_pids():
                with contextlib.suppress(OSError):
                    os.kill(pid, signal.SIGTERM)

    def cdp(self, port, method, params=None):
        code, out = fs.run('python3', f'{TOOLS}/cdp.py', str(port), 'page', method,
                           json.dumps(params or {}), timeout=20)
        if code:
            raise fs.Failed(f'DevTools {method} failed: {out[-200:]}')
        return json.loads(out) if out else {}

    # ------------------------------------------------------------ activities

    def act_x(self, seconds):
        restart = random.random() < 0.15
        if restart:
            for address, c in fs.windows().items():
                if c.get('class') == X_APP:
                    fs.close([address])
            time.sleep(2)
        self.front(X_APP, 45)
        self.wait(random.uniform(2, 5))
        swipes = self.scroll(seconds - 5)
        return {'swipes': swipes, 'restarted': restart}

    def act_video(self, seconds):
        http_server()
        port = 9341
        address = self.scratch_browser(f'http://127.0.0.1:{HTTP_PORT}/video.html', port)
        try:
            self.wait(4)
            # As a tap on play would (muted autoplay may wait for a gesture).
            self.cdp(port, 'Runtime.evaluate', {'expression': 'document.querySelector("video").play(), 1',
                                                'userGesture': True})
            self.wait(6)
            first = self.video_state(port)
            self.wait(max(5, seconds - 12))
            last = self.video_state(port)
        finally:
            self.close_browser(address)
        frames = (last.get('frames', 0) - first.get('frames', 0))
        span = max(1, last.get('time', 0) - first.get('time', 0))
        if frames <= 0:
            raise fs.Failed(f'video did not play: {first} -> {last}')
        return {'frames': frames, 'dropped': last.get('dropped', 0) - first.get('dropped', 0),
                'played_s': round(span, 1)}

    def video_state(self, port):
        expression = ('(() => { const v = document.querySelector("video"); const q = v.getVideoPlaybackQuality();'
                      ' return JSON.stringify({time: v.currentTime, frames: q.totalVideoFrames,'
                      ' dropped: q.droppedVideoFrames, paused: v.paused}); })()')
        result = self.cdp(port, 'Runtime.evaluate', {'expression': expression})
        try:
            return json.loads(result['result']['value'])
        except (KeyError, TypeError, ValueError):
            return {}

    def act_web(self, seconds):
        port = 9342
        sites = random.sample(SITES, k=min(len(SITES), max(2, int(seconds // 50))))
        address = self.scratch_browser(sites[0], port)
        pages = 0
        try:
            end = time.monotonic() + seconds
            for n, url in enumerate(sites):
                if n:
                    self.cdp(port, 'Page.navigate', {'url': url})
                pages += 1
                self.wait(random.uniform(3, 6))
                self.scroll(min(random.uniform(20, 60), max(5, end - time.monotonic())), dwell=(3, 9))
                if time.monotonic() >= end:
                    break
        finally:
            self.close_browser(address)
        return {'pages': pages}

    def open_app(self, app, seconds=20):
        """One of the phone's apps in front: ({address: client}, seconds).
        Its window may be the one it had: Settings and Files bring an open
        window forward instead of opening another, which fs.launch (a new
        window only) took for a failure (October 9: five "no window")."""
        title = APP_TITLES.get(app, app.title())
        before = fs.windows()
        if fs.ipc('launchApp', app) != 'true':
            raise fs.Failed(f'{app}: unknown app')
        found = {}

        def shown():
            for address, c in fs.windows().items():
                if fs.in_front(c) and not fs.is_android(c) and (address not in before or c.get('title') == title):
                    found[address] = c
                    return True
            return False
        return found, fs.until(shown, seconds, f'{app}: no window in front')

    def act_apps(self, seconds):
        end = time.monotonic() + seconds
        names = []
        for app in random.sample(APPS, k=len(APPS)):
            if time.monotonic() >= end:
                break
            old = self.opened.pop(app, None)
            if old and old in fs.windows():
                fs.close([old])
            new, took = self.open_app(app)
            names.append(f'{app} {took:.1f}')
            if app in SCROLLABLE:
                self.scroll(random.uniform(4, 12), fling=0.3, dwell=(1, 3))
            else:
                self.wait(random.uniform(4, 12))
            if random.random() < 0.7:
                fs.close(list(new))
            else:
                self.opened[app] = next(iter(new))
                self.home()
        return {'apps': names}

    def act_social(self, seconds):
        app = random.choice(SOCIAL)
        self.front(app, 45)
        self.wait(random.uniform(4, 8))
        self.scroll(max(5, seconds - 8), fling=0.3, dwell=(3, 8))
        self.home()
        return {'app': app}

    def act_android(self, seconds):
        app = random.choice(ANDROID)
        new, took = fs.launch(app, 60)
        self.wait(random.uniform(4, 8))
        self.scroll(max(5, seconds - took - 8), fling=0.3, dwell=(3, 8))
        if random.random() < 0.4:
            fs.close(list(new))
        else:
            self.home()
        return {'app': app, 'launch_s': round(took, 1)}

    def act_camera(self, seconds):
        new, took = fs.launch('camera', 30)
        try:
            self.wait(max(5, seconds - took))
        finally:
            fs.close(list(new))
        return {'launch_s': round(took, 1)}

    def act_idle(self, seconds):
        """Left lit on whatever is in front: the screen timeout darkens it.
        The session ends with it."""
        timeout = json.loads(read(PREFS, '{}') or '{}').get('screenTimeout', 600)
        start = time.monotonic()
        while fs.screen() == 'on':
            time.sleep(1)
            self.check()
            if time.monotonic() - start > timeout + 120:
                raise fs.Failed(f'screen still lit {timeout + 120} s after the last touch')
        return {'darkened_after': round(time.monotonic() - start), 'to': fs.screen()}

    ACTIVITIES = {
        # name: (weight, shortest, longest seconds, heavy)
        'x': (26, 90, 360, True),
        'video': (12, 180, 600, True),
        'web': (14, 120, 360, True),
        'apps': (12, 50, 120, False),
        'social': (9, 40, 120, False),
        'android': (8, 40, 120, True),
        'camera': (4, 20, 50, True),
        'idle': (10, 0, 0, False),
    }

    def activity(self, name, seconds):
        self.phase = name
        q, t = coulomb(), time.monotonic()
        # The touch chip's interrupts, logged: on October 9 bursts of them
        # (42, 142) came with nobody touching the phone.
        irqs = interrupts()[1]
        try:
            detail = getattr(self, f'act_{name}')(seconds)
            ok = True
        except (fs.Failed, subprocess.TimeoutExpired, OSError, ValueError, KeyError) as error:
            detail, ok = {'error': f'{type(error).__name__}: {error}'}, False
            self.counts['fails'] += 1
        except UserActive as error:
            took = time.monotonic() - t
            self.record(name, ok=None, s=round(took), interrupted=str(error),
                        touch_irqs=interrupts()[1] - irqs)
            raise
        took = time.monotonic() - t
        self.record(name, ok=ok, s=round(took), mah=round((q - coulomb()) / 1e6, 2),
                    avg_ma=round((q - coulomb()) / 1e6 * 3600 / max(1, took), 1),
                    touch_irqs=interrupts()[1] - irqs, **detail)
        if not ok:
            self.recover()
        return ok

    def recover(self):
        """After a failed step: back to a lit, unlocked home screen."""
        try:
            fs.ensure_lit()
            fs.unlock()
            self.home()
        except (fs.Failed, subprocess.TimeoutExpired, OSError, ValueError, KeyError) as error:
            self.record('recover-failed', error=f'{type(error).__name__}: {error}')

    def pick(self, allow_idle=True):
        hot = int(read(BATTERY / 'temp', '0')) >= HOT
        names = [n for n, (w, lo, hi, heavy) in self.ACTIVITIES.items()
                 if (allow_idle or n != 'idle') and not (hot and heavy)]
        weights = [self.ACTIVITIES[n][0] for n in names]
        name = random.choices(names, weights)[0]
        _, lo, hi, _ = self.ACTIVITIES[name]
        return name, random.uniform(lo, hi)

    # ------------------------------------------------------------ plans

    def session(self, minutes):
        """Woken (already, by the rest) and used for about minutes; True
        when it ended dark (left to the screen timeout)."""
        self.counts['sessions'] += 1
        self.record('session-start', minutes=round(minutes, 1))
        self.light()
        took = self.wifi_check()
        if took is not None and took > 10:
            self.record('wifi-slow', seconds=took)
        end = time.monotonic() + minutes * 60
        while time.monotonic() < end:
            name, seconds = self.pick()
            seconds = min(seconds, max(20, end - time.monotonic())) if name != 'idle' else 0
            self.activity(name, seconds)
            if name == 'idle':
                return True
        return False

    def burst(self, minutes):
        """Short wakes and sleeps, as a restless owner makes them."""
        self.counts['bursts'] += 1
        self.record('burst-start', minutes=minutes)
        end = time.time() + minutes * 60
        cycles = 0
        while time.time() < end:
            self.stop_if_low()
            kind = random.random()
            self.phase = 'burst'
            try:
                if kind < 0.25:
                    # A look at the clock and notifications, no unlock.
                    fs.ensure_lit()
                    self.wait(random.uniform(2, 6))
                elif kind < 0.4:
                    # Power key twice in a row, then a look.
                    fs.ensure_lit()
                    self.dark(random.choice(['ambient', 'off']))
                    time.sleep(random.uniform(0.5, 2))
                    fs.ensure_lit()
                    fs.unlock()
                    self.wait(random.uniform(2, 8))
                elif kind < 0.55:
                    self.relight()
                else:
                    self.light()
                    name = random.choice(['x', 'x', 'apps', 'social', 'camera', 'web'])
                    self.activity(name, random.uniform(15, 50))
            except fs.Failed as error:
                self.counts['fails'] += 1
                self.record('burst-fail', error=str(error))
                self.recover()
            cycles += 1
            self.counts['burst_cycles'] += 1
            self.rest(random.uniform(30, 150), why='nap')
        self.record('burst-end', cycles=cycles)

    def relight(self, delay=None):
        """Off, then on again while the phone gets ready to sleep (the
        adapter's 35-45 s of moving push to mobile data): the power key does
        not wait for it. On October 9 a sleep tried under the lit screen cut
        through Wi-Fi's reload and left it dead."""
        fs.ensure_lit()
        self.dark(random.choice(['ambient', 'off']))
        delay = random.uniform(11, 40) if delay is None else delay
        # The adapter may be asleep before the delay is up (a shorter push
        # move): the request ends that sleep, as the key would.
        on_at = time.time() + delay
        self.request_wake(on_at)
        try:
            while time.time() < on_at:
                time.sleep(0.5)
                self.check()
        finally:
            self.clear_wake()
        fs.ensure_lit()
        fs.unlock()
        self.wait(random.uniform(3, 8))
        took = self.wifi_check()
        self.record('relight', delay=round(delay, 1), wifi_s=took, adapter=ADAPTER_ACTIVE.exists())

    def race(self):
        """The relight at four delays, then a nap."""
        self.light()
        for delay in (12, 18, 25, 35):
            self.relight(delay)
        self.rest(90, mode='ambient', why='nap')

    def stop_if_low(self):
        if int(read(BATTERY / 'capacity', '100')) <= self.args.stop_at:
            raise Stopped(f'battery at {self.args.stop_at}%')

    def night(self):
        """Seconds until the night ends, when it is night now, else 0."""
        if not self.args.night:
            return 0
        a, b = self.args.night.split('-')
        now = time.localtime()
        minute = now.tm_hour * 60 + now.tm_min
        start, stop = int(a[:2]) * 60 + int(a[2:]), int(b[:2]) * 60 + int(b[2:])
        inside = start <= minute < stop if start < stop else minute >= start or minute < stop
        if not inside:
            return 0
        return ((stop - minute) % 1440) * 60

    def day(self):
        last_burst = time.time()
        burst_every = random.uniform(100, 140) * 60
        dark = False
        while True:
            self.stop_if_low()
            night = self.night()
            if night:
                # A glance at the phone or two in the night, otherwise asleep.
                nap = min(night, random.uniform(90, 200) * 60)
                self.rest(nap, mode='ambient' if random.random() < 0.5 else 'off', why='night')
                self.wait(random.uniform(5, 20))
                continue
            if self.args.profile == 'day':
                if time.time() - last_burst > burst_every:
                    self.burst(random.uniform(15, 25))
                    last_burst, burst_every = time.time(), random.uniform(100, 140) * 60
                    continue
                dark = self.session(random.uniform(4, 12))
                minutes = random.choice([random.uniform(10, 40)] * 9 + [random.uniform(60, 120)])
                self.rest(minutes * 60, mode=None if not dark else fs.screen())
            else:
                self.burst(30)

    def naps(self):
        """Short rests only, lit in between: the wake request's timing."""
        self.light()
        for mode in ('ambient', 'off', 'ambient', 'off'):
            self.rest(100, mode=mode, why='nap')
            fs.unlock()
            self.wait(10)

    def smoke(self):
        """Each piece once, briefly: a check before a long run."""
        self.light()
        for name, seconds in (('x', 25), ('video', 25), ('web', 40), ('apps', 30), ('social', 25),
                              ('android', 30), ('camera', 15)):
            self.activity(name, seconds)
        self.rest(75, mode='ambient', why='nap')
        self.rest(75, mode='off', why='nap')
        fs.ensure_lit()


def sleep_summary(start, end):
    """The sleep log between start and end: time asleep, wakes by reason,
    refusals and errors."""
    slept, wakes, problems = 0.0, {}, []
    try:
        lines = SLEEP_LOG.read_text().splitlines()[-400:]
    except OSError:
        lines = []
    for line in lines:
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        if not start <= entry.get('time', 0) <= end + 5:
            continue
        if 'woke' in entry:
            woke = entry['woke']
            wakes[woke] = wakes.get(woke, 0) + 1
            slept += entry.get('slept', 0) or 0
            if woke == 'error':
                problems.append(entry.get('error', '')[:120])
        elif 'refused' in entry:
            problems.append(str(entry['refused'])[:120])
    return {'asleep_pct': round(100 * slept / max(1, end - start)), 'wakes': wakes,
            **({'problems': problems[:5]} if problems else {})}


def scratch_pids():
    """The browser processes started with the run's scratch profile."""
    marker = f'XDG_DATA_HOME={WORK}/profile/data'.encode()
    pids = []
    for entry in os.listdir('/proc'):
        if not entry.isdigit():
            continue
        try:
            with open(f'/proc/{entry}/cmdline', 'rb') as f:
                if not f.read().startswith(BROWSER.encode() + b'\0'):
                    continue
            with open(f'/proc/{entry}/environ', 'rb') as f:
                if marker in f.read().split(b'\0'):
                    pids.append(int(entry))
        except OSError:
            continue
    return pids


def http_server():
    found = subprocess.run(['pgrep', '-f', f'^python3 -m http.server {HTTP_PORT}'], capture_output=True)
    if found.returncode:
        fs.cube('sh', '-c', f'cd {WWW} && setsid -f python3 -m http.server {HTTP_PORT} --bind 127.0.0.1 '
                f'>/dev/null 2>&1')
        time.sleep(1)


def terminated(*_):
    raise Stopped('SIGTERM')


def main():
    global touch
    parser = argparse.ArgumentParser()
    parser.add_argument('--stop-at', type=int, default=10)
    parser.add_argument('--profile', choices=('day', 'stress', 'smoke', 'naps', 'race'), default='day')
    parser.add_argument('--seed', type=int, default=None)
    parser.add_argument('--night', default='0030-0730')
    parser.add_argument('--on-cable', action='store_true',
                        help='a check on the cable: the battery supplies everything and the phone sleeps there')
    args = parser.parse_args()
    if args.night in ('', 'none'):
        args.night = None
    seed = args.seed if args.seed is not None else int(time.time())
    random.seed(seed)
    SOAK.mkdir(parents=True, exist_ok=True)
    for path in (STOP, PAUSE):
        path.unlink(missing_ok=True)
    if not os.path.exists('/dev/uinput'):
        subprocess.run(['insmod', f'{TOOLS}/uinput.ko'], check=False)
    touch = Touch()
    if args.on_cable:
        INPUT_OFF.write_text('1\n')
        ALLOW_CHARGING.touch()
    run = Run(args)
    signal.signal(signal.SIGTERM, terminated)
    run.record('start', profile=args.profile, seed=seed, stop_at=args.stop_at, night=args.night,
               prefs=json.loads(read(PREFS, '{}') or '{}'))
    why = 'done'
    try:
        while True:
            try:
                try:
                    {'smoke': run.smoke, 'naps': run.naps, 'race': run.race}.get(args.profile, run.day)()
                    break
                except UserActive as error:
                    run.step_aside(str(error))
            except Stopped:
                raise
            except Exception as error:  # The run goes on; the log says what broke.
                run.counts['fails'] += 1
                run.record('error', error=f'{type(error).__name__}: {error}')
                time.sleep(10)
                run.recover()
    except Stopped as error:
        why = str(error)
    run.clear_wake()
    if args.on_cable:
        ALLOW_CHARGING.unlink(missing_ok=True)
        INPUT_OFF.write_text('0\n')
    run.phase = 'finished'
    run.record('finish', why=why, hours=round((time.time() - run.t0) / 3600, 2), **run.counts)
    subprocess.run(['pkill', '-f', f'^python3 -m http.server {HTTP_PORT}'], check=False)
    return 0


if __name__ == '__main__':
    sys.exit(main())
