#!/usr/bin/env python3
"""The shell's display hook on the Pixel: `on` or `off` as the screen lights or
goes dark. It tells the modem service (SIT 0902 screen state and the 0928
indication filter, as stock's RIL does on a display change), so the modem stops
optional signal and dormancy reports while the screen is dark: otherwise it
sends one about every 1.25 s, each waking its PCIe link. Calls, SMS and
registration changes still arrive. It also holds the touch controller in reset
while the screen is dark (pixel_touch_input's sleep parameter; about 5 mA in
system sleep). With deep sleep, which unloads Wi-Fi and Bluetooth, it loads
them again in the background when the screen lights, and it resumes the
sensors pixel-suspend paused, in case a sleep ended without its cleanup.

`ambient` is the always-on display once its clock shows: as `off` for the
modem and touch, then the panel's AOD mode (pixel_scanout.ambient: the panel
shows its memory and the pipeline powers off between frames) and its VCI and
VDDD held on through sleep (pixel_pmic_opmode.panel_on). `ambient-exit` leaves
AOD at once, before the clock fades out; `on` and `off` leave it too.
When the shell asks for the fingerprint sensor in the always-on display
($XDG_RUNTIME_DIR/omarchy-mobile-ambient-touch), touch is not held in reset
there: the controller is in its gesture mode (pixel_touch_input.gesture), and
its interrupt may wake the phone from deep sleep (pixel_sleep.touch_wake).

While the screen is dark (`off`, `ambient`) the mid and big clusters, CPUs
4-7, are offline; pixel_cpupm then powers both clusters down. Idle with the
always-on display that was about 55 mW less (October 2). They stay offline
through deep sleep and come back first thing on `on` or `ambient-exit`, in
about 75 ms. Taking them offline takes about 200 ms, so it runs in the
background (`cores`), under a lock, and stops as soon as the screen lights
again. Installed as
/usr/local/sbin/pixel-display-hook, called by
~/.config/omarchy-mobile/display-hook."""
import fcntl
import json
import os
from pathlib import Path
import pwd
import socket
import subprocess
import sys
import time

SOCKET = '/run/omarchy-mobile-telephony/socket'
TOUCH_SLEEP = Path('/sys/module/pixel_touch_input/parameters/sleep')
TOUCH_GESTURE = Path('/sys/module/pixel_touch_input/parameters/gesture')
TOUCH_WAKE = Path('/sys/module/pixel_sleep/parameters/touch_wake')
WIFI_RESTART = '/usr/local/sbin/pixel-wifi-restart'
BT_RESTART = '/usr/local/sbin/pixel-bt-restart'
BT_SLEEPING = Path('/run/pixel-bt-sleeping')
AMBIENT = Path('/sys/module/pixel_scanout/parameters/ambient')
PANEL_ON = Path('/sys/module/pixel_pmic_opmode/parameters/panel_on')
# The clock's fade-in (AmbientDisplay.qml, 300 ms) ends before AOD freezes it.
AMBIENT_SETTLE = 0.6
# AOD low: 10 nit (2: high, 50 nit).
AMBIENT_LEVEL = '1'
CPUS = Path('/sys/devices/system/cpu')
DARK_CPUS = (4, 5, 6, 7)
# Present while the screen is dark: the cores should be offline.
CORES_DARK = Path('/run/pixel-cores-dark')
CORES_LOCK = '/run/pixel-cores.lock'
# CPUs a service needs online even while dark (the fingerprint service: its
# trusted app pins itself to CPUs 4 and 6), as numbers separated by spaces.
CORES_HOLD = Path('/run/pixel-cores-hold')


def held_cpus():
    try:
        return {int(cpu) for cpu in CORES_HOLD.read_text().split()}
    except (OSError, ValueError):
        return set()


def write(path, value):
    try:
        path.write_text(value)
        return True
    except OSError:
        return False


def ambient(on):
    """AOD on or off; leaving it first restores the panel, then its rails."""
    if on:
        time.sleep(AMBIENT_SETTLE)
        if write(AMBIENT, AMBIENT_LEVEL):
            write(PANEL_ON, '1')
    else:
        write(AMBIENT, '0')
        write(PANEL_ON, '0')


# The desktop's account, named in this file ("user=NAME"); without it, root's.
# This runs as root (through sudo, which drops the caller's environment), so
# the account's paths come from here, never from the caller.
SESSION = Path('/etc/omarchy-mobile/session')


def session_account():
    name = 'root'
    try:
        for line in SESSION.read_text().splitlines():
            key, _, value = line.partition('=')
            if key.strip() == 'user' and value.strip():
                name = value.strip()
    except OSError:
        pass
    try:
        return pwd.getpwnam(name)
    except KeyError:
        return pwd.getpwnam('root')


def session_runtime():
    return Path('/run/user') / str(session_account().pw_uid)


def ambient_touch():
    """Whether the shell wants touch in the always-on display: its flag in
    the session's runtime directory, which the user owns (no link followed)."""
    try:
        fd = os.open(session_runtime() / 'omarchy-mobile-ambient-touch', os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except OSError:
        return False
    with os.fdopen(fd) as flag:
        return flag.read(8).strip() == '1'


def background(command):
    subprocess.Popen(['setsid', '-f', *command], stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def cores():
    """Bring CPUs 4-7 to the state CORES_DARK asks for, leaving held CPUs
    online. The flags are read again before each CPU, so a screen that lights
    mid-way stops the offlining."""
    with open(CORES_LOCK, 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for cpu in DARK_CPUS if not CORES_DARK.exists() else reversed(DARK_CPUS):
            want = '0' if CORES_DARK.exists() and cpu not in held_cpus() else '1'
            online = CPUS / f'cpu{cpu}' / 'online'
            try:
                if online.read_text().strip() != want:
                    online.write_text(want)
            except OSError:
                pass  # a CPU that refuses stays as it is


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ('on', 'off', 'ambient', 'ambient-exit', 'cores'):
        raise SystemExit('usage: pixel-display-hook on|off|ambient|ambient-exit')
    if sys.argv[1] == 'cores':
        cores()
        return 0
    if sys.argv[1] in ('on', 'ambient-exit'):
        CORES_DARK.unlink(missing_ok=True)
        cores()
    else:
        CORES_DARK.touch()
        background([sys.executable, __file__, 'cores'])
    lit = sys.argv[1] in ('on', 'ambient-exit')
    # Touch: on while lit; gestures only, able to wake the phone, under the
    # always-on display with its fingerprint sensor; otherwise in reset. A
    # module that is not loaded is skipped (write returns False).
    gesture = sys.argv[1] == 'ambient' and ambient_touch()
    write(TOUCH_WAKE, '1' if gesture else '0')
    write(TOUCH_GESTURE, '1' if gesture else '0')
    write(TOUCH_SLEEP, '0' if lit or gesture else '1')
    if sys.argv[1] == 'ambient-exit':
        ambient(False)
        return 0
    if sys.argv[1] != 'ambient':
        ambient(False)
    if (sys.argv[1] == 'on' and Path('/sys/module/pixel_sleep').exists()
            and not Path('/sys/module/brcmfmac').exists()):
        background([WIFI_RESTART, '--start'])
    if sys.argv[1] == 'on' and BT_SLEEPING.exists():
        background([BT_RESTART])
    if sys.argv[1] == 'on':
        background(['dbus-send', '--system', '--dest=net.hadess.SensorProxy', '/net/hadess/SensorProxy',
                    'org.omarchy.Mobile.SensorPower.Resume'])
    try:
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(8)
            client.connect(SOCKET)
            client.sendall(json.dumps(dict(action='screen-state', on=lit)).encode() + b'\n')
            with client.makefile() as stream:
                reply = json.loads(stream.readline(65536))
    except (OSError, ValueError) as error:
        if sys.argv[1] == 'ambient':
            ambient(True)
        raise SystemExit(f'modem service unavailable: {error}')
    if sys.argv[1] == 'ambient':
        ambient(True)
    # During a call the service keeps reports on and says so; that is fine.
    return 0 if reply.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main())
