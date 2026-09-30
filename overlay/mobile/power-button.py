#!/usr/bin/env python3
"""Tap to change display state; hold two seconds for the power menu.

Sleeping is not done here. The sleep policy (omarchy-mobile-sleep) suspends
through the device adapter once the screen has stayed dark for a few seconds,
and a power-key wake reaches this as an ordinary press on a dark screen.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import time

HOLD_SECONDS = 2.0


class PowerButton:
    def __init__(self):
        self.runtime = Path(os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}'))
        config = Path(os.environ.get('XDG_CONFIG_HOME', str(Path.home() / '.config')))
        self.display = str(Path.home() / '.local/bin/omarchy-mobile-display')
        self.prefs = config / 'omarchy-mobile/prefs.json'
        self.shell = config / 'quickshell/omarchy-mobile/shell.qml'
        # Present while the always-on display shows (omarchy-mobile-display).
        self.ambient = self.runtime / 'omarchy-mobile-ambient'
        self.state = self.runtime / 'omarchy-mobile-power.json'
        self.log = Path(os.environ.get('XDG_STATE_HOME', str(Path.home() / '.local/state'))) / 'omarchy-mobile/power-button.log'

    def record(self, action, **details):
        self.log.parent.mkdir(parents=True, exist_ok=True)
        with self.log.open('a') as stream:
            stream.write(json.dumps(dict(time=time.time(), action=action, **details)) + '\n')

    def load(self):
        try:
            return json.loads(self.state.read_text())
        except (OSError, ValueError):
            return {}

    def save(self, state):
        self.state.write_text(json.dumps(state))

    def display_on(self):
        monitors = json.loads(subprocess.check_output(['hyprctl', '-j', 'monitors'], timeout=5))
        if not monitors:
            raise RuntimeError('No compositor monitor available')
        return any(monitor['dpmsStatus'] for monitor in monitors)

    def set_display(self, action):
        subprocess.run([self.display, action], check=True, timeout=15)

    def start_display(self, action):
        return subprocess.Popen([self.display, action])

    def always_on(self):
        try:
            return json.loads(self.prefs.read_text()).get('alwaysOn') is True
        except (OSError, ValueError, AttributeError):
            return False

    def act(self):
        if self.ambient.exists():
            self.set_display('on')
            self.record('ambient-wake')
            return
        if not self.display_on():
            self.set_display('on')
            self.record('display-on')
            return
        # The always-on display keeps the panel lit, so it never suspends.
        if self.always_on():
            self.set_display('ambient')
            self.record('ambient')
            return
        self.start_display('off').wait(timeout=8)
        self.record('display-off')

    def start_hold(self, token):
        subprocess.Popen([sys.executable, str(Path(__file__).resolve()), 'hold', str(token)],
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)

    def show_menu(self):
        self.set_display('wake')
        subprocess.run(['quickshell', 'ipc', '-n', '-p', str(self.shell),
                        'call', 'mobile', 'powerMenu'], check=True, timeout=5)
        self.record('power-menu')

    def hold(self, token):
        time.sleep(HOLD_SECONDS)
        with (self.runtime / 'omarchy-mobile-power.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            state = self.load()
            if state.get('pressed') != token or state.get('held'):
                return
            if not HOLD_SECONDS <= time.monotonic() - token < 60:
                return
            state['held'] = True
            self.save(state)
            self.show_menu()

    def event(self, event):
        # One process per key event. The press that wakes the phone and its
        # release arrive together once it is awake, so each waits its turn
        # for the lock, and a release that finds no press yet gives the press
        # of its pair a moment to be recorded.
        for attempt in range(2):
            with (self.runtime / 'omarchy-mobile-power.lock').open('w') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                state = self.load()
                now = time.monotonic()
                if event == 'press':
                    if 'pressed' in state and 0 <= now - state['pressed'] < 60:
                        return  # Ignore repeat events while the key is held.
                    self.save({'pressed': now})
                    self.start_hold(now)
                    return
                if 'pressed' in state and 0 <= now - state['pressed'] < 60:
                    self.save({})
                    if not state.get('held'):
                        if now - state['pressed'] >= HOLD_SECONDS:
                            self.show_menu()  # Timer was delayed by scheduling.
                        else:
                            self.act()
                    return
            if attempt == 0:
                time.sleep(0.4)
        self.record('ignored-unpaired-release')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('event', choices=('press', 'release', 'hold'))
    parser.add_argument('token', nargs='?', type=float)
    args = parser.parse_args()
    button = PowerButton()
    try:
        if args.event == 'hold':
            if args.token is None: parser.error('hold requires a press token')
            button.hold(args.token)
        else:
            button.event(args.event)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        button.record('error', detail=str(error))
        # Errors should leave a usable screen, never silently reattempt sleep.
        button.set_display('on')
        raise SystemExit(str(error))


if __name__ == '__main__':
    main()
