#!/usr/bin/env python3
"""Functional soak: the phone's everyday paths, over and over (test only).

  function-soak.py [ROUNDS] [STEP...]   on the phone as root, at a desk: it
                                        drives the screen, the lock and touch

STEPS (default all): display ambient lock apps android over wifi bluetooth report
Each step runs once a round and checks its outcome: the screen state that was
asked for, the lock that goes and comes, each app's window opening and
closing, the radio connected again, the shell answering throughout. Kernel
messages at warning level or worse during a step are logged with it.
Results: /root/tools/soak/function-soak.log, a line per step and a summary.

Needs /root/tools/as-cube.sh, /root/tools/touch-seq.py and the uinput module
(the lock step taps the PIN). The phone is left unlocked on a lit screen
when the run ends; lock it afterwards.
"""
import json
import os
import subprocess
import sys
import time

TOOLS = '/root/tools'
SHELL = '/home/cube/.config/quickshell/omarchy-mobile/shell.qml'
SCREEN = '/run/user/1001/omarchy-mobile/screen'
LOG = f'{TOOLS}/soak/function-soak.log'
# The lock's PIN, as its digits in a file on the phone (never in the repo),
# tapped on the keypad: 1-9 in three rows from (360, 1896), 288 px apart,
# then 0 below the 8.
PIN_FILE = f'{TOOLS}/soak-pin'
KEYS = {str(n): (360 + (n - 1) % 3 * 360, 1896 + (n - 1) // 3 * 288) for n in range(1, 10)} | {'0': (720, 2760)}


def pin_taps():
    with open(PIN_FILE) as f:
        return [f'tap:{KEYS[d][0]},{KEYS[d][1]}' for d in f.read().strip()]
APPS = ['calculator', 'clock', 'notes', 'contacts', 'calendar']
ANDROID = 'waydroid.org.fdroid.fdroid'


class Failed(Exception):
    pass


def run(*cmd, timeout=30):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout.strip()


def cube(*cmd, timeout=30):
    return run(f'{TOOLS}/as-cube.sh', *cmd, timeout=timeout)


def ipc(fn, *args):
    code, out = cube('quickshell', 'ipc', '-p', SHELL, 'call', 'mobile', fn, *args, timeout=10)
    if code:
        raise Failed(f'shell did not answer {fn}')
    return out


def hypr_json(what):
    code, out = cube('hyprctl', what, '-j', timeout=10)
    if code:
        raise Failed(f'hyprctl {what} failed')
    return json.loads(out)


def display(action):
    code, out = cube('omarchy-mobile-display', action, timeout=30)
    if code:
        raise Failed(f'display {action} exited {code}')


def screen():
    try:
        with open(SCREEN) as f:
            return f.read().strip()
    except OSError:
        return ''


def until(check, seconds, what):
    """Polls check() for up to seconds; the time it took, or Failed(what)."""
    start = time.monotonic()
    while True:
        value = check()
        if value:
            return time.monotonic() - start
        if time.monotonic() - start > seconds:
            raise Failed(what)
        time.sleep(0.2)


def lock_state():
    return json.loads(ipc('lockState'))


def dpms_on():
    return all(m.get('dpmsStatus') for m in hypr_json('monitors'))


def touch(*actions):
    code, out = run('python3', f'{TOOLS}/touch-seq.py', *actions, timeout=60)
    if code:
        raise Failed(f'touch failed: {out[-200:]}')


def ensure_lit():
    if screen() != 'on' or not dpms_on():
        display('on')
        until(lambda: screen() == 'on' and dpms_on(), 10, 'screen did not light')


def unlock():
    state = lock_state()
    if not state['locked']:
        return 0.0
    ipc('lockKeypad')
    until(lambda: lock_state()['bouncer'], 5, 'keypad did not show')
    # Taps only on the keypad (a stray PIN on an unlocked phone opens apps).
    if not lock_state()['locked']:
        return 0.0
    touch(*pin_taps())
    return until(lambda: not lock_state()['locked'], 8, 'PIN did not unlock')


def step_display():
    ensure_lit()
    display('off')
    until(lambda: screen() == 'off' and not dpms_on(), 10, 'screen did not go dark')
    time.sleep(2)
    display('on')
    took = until(lambda: screen() == 'on' and dpms_on(), 10, 'screen did not light again')
    unlock()
    return f'lit in {took:.1f} s'


def step_ambient():
    ensure_lit()
    display('ambient')
    until(lambda: screen() == 'ambient', 10, 'always-on display did not show')
    time.sleep(3)
    display('on')
    took = until(lambda: screen() == 'on' and dpms_on(), 10, 'screen did not leave the always-on display')
    unlock()
    return f'back in {took:.1f} s'


def step_lock():
    ensure_lit()
    unlock()
    ipc('lock')
    until(lambda: lock_state()['locked'], 5, 'did not lock')
    took = unlock()
    return f'unlocked in {took:.1f} s'


def windows():
    return {c['address']: c for c in hypr_json('clients') if c.get('mapped', True)}


def close(addresses):
    for address in addresses:
        cube('hyprctl', 'dispatch', f'hl.dsp.window.close({{window = "address:{address}"}})', timeout=10)
    until(lambda: not set(addresses) & set(windows()), 10, 'window did not close')


def in_front(c):
    return c.get('workspace', {}).get('id') == 1


def is_android(c):
    return (c.get('class') or '').startswith('waydroid.')


def launch(app, seconds):
    """Launches app and waits for its window in front: ({address: client},
    seconds until it showed).
    Android's window may be an old one given the app's class (Waydroid reuses
    its sleep screen's); the phone's own apps are org.quickshell, so theirs is
    the new non-Android window."""
    before = windows()
    if ipc('launchApp', app) != 'true':
        raise Failed(f'{app}: unknown app')
    if app.startswith('waydroid.'):
        match = lambda a, c: c.get('class') == app and in_front(c)
    else:
        match = lambda a, c: a not in before and not is_android(c)
    new = {}

    def opened():
        new.update({a: c for a, c in windows().items() if match(a, c)})
        return bool(new)
    took = until(opened, seconds, f'{app}: no window')
    # The shell keeps Android windows apart within half a second: the app
    # launched must be the one left in front.
    time.sleep(1.5)
    now = windows()
    if not any(in_front(now.get(a, {})) for a in new):
        raise Failed(f'{app}: opened, but not in front')
    return new, took


def open_close(app, seconds):
    # An app already open is closed first: a launch would only bring it up.
    # The phone's own apps are org.quickshell windows titled with their name.
    if app.startswith('waydroid.'):
        close([a for a, c in windows().items() if c.get('class') == app])
    else:
        close([a for a, c in windows().items()
               if c.get('class') == 'org.quickshell' and c.get('title') == app.title()])
    new, took = launch(app, seconds)
    time.sleep(1)
    close(list(new))
    return took


def android_answers():
    code, out = run('lxc-attach', '-P', '/var/lib/waydroid/lxc', '-n', 'waydroid', '--clear-env', '--',
                    '/system/bin/dumpsys', 'window', 'displays', timeout=15)
    if code:
        raise Failed("Android's window manager does not answer")


def step_apps():
    ensure_lit()
    unlock()
    times = []
    for app in APPS:
        times.append(f'{app} {open_close(app, 15):.1f}')
        time.sleep(1)
    ipc('lockState')
    return ', '.join(times)


def step_android():
    ensure_lit()
    unlock()
    took = open_close(ANDROID, 60)
    android_answers()
    return f'window in {took:.1f} s'


def step_over():
    """One of the phone's apps opened over an Android app comes to the
    front, and the Android app goes back to it after."""
    ensure_lit()
    unlock()
    android, _ = launch(ANDROID, 60)
    calculator, _ = launch('calculator', 15)
    close(list(calculator))
    time.sleep(1)
    android_answers()
    close(list(android))
    return 'calculator in front of Android'


def wifi_device():
    code, out = run('nmcli', '-t', '-f', 'DEVICE,TYPE', 'device', timeout=10)
    return next((line.split(':')[0] for line in out.splitlines() if line.endswith(':wifi')), 'wlan0')


def connected(device_type):
    code, out = run('nmcli', '-t', '-f', 'TYPE,STATE', 'device', timeout=10)
    return any(line.startswith(device_type + ':connected') for line in out.splitlines())


def step_wifi():
    if not connected('wifi'):
        raise Failed('Wi-Fi not connected at the start')
    run('nmcli', 'radio', 'wifi', 'off', timeout=20)
    until(lambda: not connected('wifi'), 10, 'Wi-Fi did not go off')
    time.sleep(3)
    run('nmcli', 'radio', 'wifi', 'on', timeout=20)
    took = until(lambda: connected('wifi'), 45, 'Wi-Fi did not reconnect')
    device = wifi_device()
    online = until(lambda: run('ping', '-c', '1', '-W', '2', '-I', device, '9.9.9.9', timeout=10)[0] == 0,
                   20, 'Wi-Fi connected but no route out')
    return f'reconnected in {took:.1f} s, online {online:.1f} s later'


def bt_powered():
    code, out = run('bluetoothctl', 'show', timeout=10)
    return 'Powered: yes' in out


def step_bluetooth():
    was = bt_powered()
    code, out = cube('omarchy-mobile-bluetooth', 'power', 'off', timeout=30)
    until(lambda: not bt_powered(), 10, 'Bluetooth did not power off')
    time.sleep(2)
    code, out = cube('omarchy-mobile-bluetooth', 'power', 'on', timeout=60)
    took = until(bt_powered, 20, 'Bluetooth did not power on')
    if not was:
        cube('omarchy-mobile-bluetooth', 'power', 'off', timeout=30)
    return f'on again in {took:.1f} s'


def step_report():
    code, out = cube('omarchy-mobile-report', timeout=120)
    if code or not out:
        raise Failed(f'report exited {code}')
    path = out.splitlines()[-1]
    files = os.listdir(path)
    root = os.listdir(os.path.join(path, 'root')) if 'root' in files else []
    if 'dmesg.txt' not in root:
        raise Failed(f'report has no root half ({len(files)} files)')
    return f'{len(files)} + {len(root)} files'


STEPS = {'display': step_display, 'ambient': step_ambient, 'lock': step_lock, 'apps': step_apps,
         'android': step_android, 'over': step_over, 'wifi': step_wifi, 'bluetooth': step_bluetooth, 'report': step_report}


class Kmsg:
    """Kernel messages at warning level or worse since the last read."""
    def __init__(self):
        self.fd = os.open('/dev/kmsg', os.O_RDONLY | os.O_NONBLOCK)
        os.lseek(self.fd, 0, os.SEEK_END)

    def news(self):
        lines = []
        while True:
            try:
                record = os.read(self.fd, 8192).decode(errors='replace')
            except BlockingIOError:
                return lines
            except BrokenPipeError:
                continue
            head, _, text = record.partition(';')
            if int(head.split(',')[0]) & 7 <= 4:
                lines.append(text.splitlines()[0] if text else '')


def main(args):
    rounds = int(args[0]) if args and args[0].isdigit() else 3
    names = [a for a in args if not a.isdigit()] or list(STEPS)
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    kmsg = Kmsg()
    results = {name: [0, 0] for name in names}
    warnings = 0
    with open(LOG, 'a', buffering=1) as log:
        log.write(f'=== {time.strftime("%F %T")} {rounds} rounds: {" ".join(names)}\n')
        for n in range(1, rounds + 1):
            for name in names:
                start = time.monotonic()
                try:
                    detail, ok = STEPS[name](), True
                except (Failed, subprocess.TimeoutExpired, OSError, ValueError, KeyError) as error:
                    detail, ok = f'FAIL {type(error).__name__}: {error}', False
                results[name][0 if ok else 1] += 1
                news = kmsg.news()
                warnings += len(news)
                log.write(f'{time.strftime("%T")} {n}/{rounds} {name}: {"ok" if ok else "FAIL"} '
                          f'{time.monotonic() - start:.1f} s, {detail}'
                          f'{f", {len(news)} kernel warnings" if news else ""}\n')
                for line in news[:20]:
                    log.write(f'    kernel: {line}\n')
                time.sleep(1)
        summary = ', '.join(f'{k} {v[0]}/{v[0] + v[1]}' for k, v in results.items())
        log.write(f'{time.strftime("%T")} done: {summary}; {warnings} kernel warnings\n')
    print(summary, f'{warnings} kernel warnings')
    return 0 if all(v[1] == 0 for v in results.values()) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
