#!/usr/bin/env python3
"""The shell's display hook on the Pixel: `on` or `off` as the screen lights or
goes dark. It tells the modem service (SIT 0902 screen state and the 0928
indication filter, as stock's RIL does on a display change), so the modem stops
optional signal and dormancy reports while the screen is dark: otherwise it
sends one about every 1.25 s, each waking its PCIe link. Calls, SMS and
registration changes still arrive. Installed as /usr/local/sbin/pixel-display-hook,
called by ~/.config/omarchy-mobile/display-hook."""
import json
import socket
import sys

SOCKET = '/run/omarchy-mobile-telephony/socket'


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ('on', 'off'):
        raise SystemExit('usage: pixel-display-hook on|off')
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
