#!/usr/bin/env python3
"""The shell's display hook on the Pixel: `on` or `off` as the screen lights or
goes dark. It tells the modem service (SIT 0902 screen state and the 0928
indication filter, as stock's RIL does on a display change), so the modem stops
optional signal and dormancy reports while the screen is dark: otherwise it
sends one about every 1.25 s, each waking its PCIe link. Calls, SMS and
registration changes still arrive. It also holds the touch controller in reset
while the screen is dark (pixel_touch_input's sleep parameter; about 5 mA in
system sleep). With deep sleep, which unloads Wi-Fi and Bluetooth, it loads
them again in the background when the screen lights. Installed as
/usr/local/sbin/pixel-display-hook, called by
~/.config/omarchy-mobile/display-hook."""
import json
from pathlib import Path
import socket
import subprocess
import sys

SOCKET = '/run/omarchy-mobile-telephony/socket'
TOUCH_SLEEP = '/sys/module/pixel_touch_input/parameters/sleep'
WIFI_RESTART = '/usr/local/sbin/pixel-wifi-restart'
BT_RESTART = '/usr/local/sbin/pixel-bt-restart'
BT_SLEEPING = Path('/run/pixel-bt-sleeping')


def background(command):
    subprocess.Popen(['setsid', '-f', *command], stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ('on', 'off'):
        raise SystemExit('usage: pixel-display-hook on|off')
    try:
        with open(TOUCH_SLEEP, 'w') as touch:
            touch.write('0' if sys.argv[1] == 'on' else '1')
    except OSError:
        pass  # no touch driver loaded
    if (sys.argv[1] == 'on' and Path('/sys/module/pixel_sleep').exists()
            and not Path('/sys/module/brcmfmac').exists()):
        background([WIFI_RESTART, '--start'])
    if sys.argv[1] == 'on' and BT_SLEEPING.exists():
        background([BT_RESTART])
    try:
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(8)
            client.connect(SOCKET)
            client.sendall(json.dumps(dict(action='screen-state', on=sys.argv[1] == 'on')).encode() + b'\n')
            with client.makefile() as stream:
                reply = json.loads(stream.readline(65536))
    except (OSError, ValueError) as error:
        raise SystemExit(f'modem service unavailable: {error}')
    # During a call the service keeps reports on and says so; that is fine.
    return 0 if reply.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main())
