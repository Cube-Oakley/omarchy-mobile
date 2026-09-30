#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixed modem laboratory actions. 'register' requires radio-enable approval.

No arbitrary AT commands, dialing, SMS, PIN entry, APDUs or NV commands.
"""
import argparse
import os
import re
import select
import time

ACTIONS = {
    'status': ['AT+CFUN?', 'AT+CPIN?', 'AT+CEREG?', 'AT+C5GREG?', 'AT+COPS?', 'AT+CSQ', 'AT+CESQ'],
    'register': ['AT+CFUN=1', 'AT+CPIN?', 'AT+CEREG?', 'AT+C5GREG?', 'AT+COPS?', 'AT+CSQ', 'AT+CESQ'],
    'radio-off': ['AT+CFUN=0', 'AT+CFUN?'],
    'tello-data': ['AT+CMEE=2', 'AT+CGDCONT=1,"IPV4V6","wholesale"', 'AT+CGDCONT?', 'AT+CGATT=1', 'AT+CGACT=1,1', 'AT+CGACT?', 'AT+CGCONTRDP=1'],
    'data-activate': ['AT+CGACT=1,1', 'AT+CGACT?', 'AT+CGCONTRDP=1'],
    'tello-attach': ['AT+CGDCONT=1,"IPV4V6","wholesale"', 'AT+CGATT=1', 'AT+CGATT?', 'AT+CEREG?', 'AT+COPS?', 'AT+CSQ'],
    'pdp-status': ['AT+CGATT?', 'AT+CGDCONT?', 'AT+CGACT?'],
    'attach-diagnostics': ['AT+CEER', 'AT+CEMODE?', 'AT+CGREG?', 'AT+CEREG?', 'AT+COPS?'],
}


def exchange(fd, command, timeout):
    data = (command + '\r').encode('ascii')
    if os.write(fd, data) != len(data):
        raise RuntimeError('short command write')
    deadline = time.monotonic() + timeout
    response = bytearray()
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], max(0, deadline - time.monotonic()))
        if not ready:
            break
        data = os.read(fd, 4096)
        if not data:
            break
        response.extend(data)
        if len(response) > 16384:
            raise RuntimeError('response exceeds limit')
        if re.search(rb'(?:^|[\r\n])(OK|ERROR|\+CME ERROR[^\r\n]*)[\r\n]', response):
            break
    return response.decode('ascii', errors='replace')


def report(command, response):
    print(command, flush=True)
    for line in response.splitlines():
        if not line or line == command:
            continue
        if re.search(r'imei|imsi|iccid|serial|\d{14,}', line, re.I):
            print('[private identifier suppressed]', flush=True)
        elif line.startswith(('+CGREG:', '+CEREG:', '+C5GREG:')):
            # Registration mode/status only; omit location-related fields.
            print(','.join(line.split(',')[:2]), flush=True)
        elif line.startswith(('+CGDCONT:', '+CGCONTRDP:')):
            # CID, PDP type and APN only; omit assigned addresses.
            print(','.join(line.split(',')[:3]), flush=True)
        else:
            print(line, flush=True)
    if not response:
        print('[timeout: no response]', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('action', choices=ACTIONS)
    args = ap.parse_args()
    fd = os.open('/dev/umts_router', os.O_RDWR | os.O_NONBLOCK | os.O_CLOEXEC)
    try:
        for command in ACTIONS[args.action]:
            report(command, exchange(fd, command, 45 if command in ['AT+CGATT=1', 'AT+CGACT=1,1'] else 25 if command.startswith('AT+CFUN=') else 4))
    finally:
        os.close(fd)


if __name__ == '__main__':
    main()
