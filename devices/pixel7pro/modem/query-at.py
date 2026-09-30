#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Radio-off AT smoke test. Fixed read-only allow-list; no arbitrary commands."""
import os
import re
import select
import time

fd = os.open('/dev/umts_router', os.O_RDWR | os.O_NONBLOCK | os.O_CLOEXEC)
try:
    for command in ['AT', 'AT+CGMR', 'AT+CFUN?']:
        os.write(fd, (command + '\r').encode('ascii'))
        deadline = time.monotonic() + 3
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
                raise RuntimeError('response exceeds smoke-test limit')
            if re.search(rb'(?:^|[\r\n])(OK|ERROR|\+CME ERROR[^\r\n]*)[\r\n]', response):
                break
        print(command, flush=True)
        for line in response.decode('ascii', errors='replace').splitlines():
            if re.search(r'imei|imsi|iccid|serial|\d{14,}', line, re.I):
                print('[private identifier suppressed]')
            else:
                print(line)
        if not response:
            print('[timeout: no response]')
finally:
    os.close(fd)
