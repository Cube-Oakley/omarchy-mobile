#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixed physical-slot SIT radio controls. Radio enable requires authorization.

BuildRadioPower at 0x74220 uses u32 OFF=1/ON=2 followed by two booleans.
Normal DoRadioPower (0x13bbcc) defaults both optional flags to zero.
GetRadioState returns OFF=0, UNAVAILABLE=1 or an enabled state (2..10).
The stock adapter maps every enabled state to RIL ON=10.
"""
import argparse
import struct
import time
from sit import Sit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['on', 'off', 'status'])
    args = parser.parse_args()
    client = Sit()
    try:
        if args.action != 'status':
            result = client.request(0x0800, struct.pack('<IBB', 2 if args.action == 'on' else 1, 0, 0), timeout=35)
            # Stock ProtocolRadioPowerAdapter checks an optional additional
            # error byte even when the RCM header reports success.
            if result and result[0] in (28, 29):
                raise RuntimeError('radio power rejected with cause ' + str(result[0]))
        deadline = time.monotonic() + 10
        while True:
            result = client.request(0x0801)
            if len(result) != 4:
                raise RuntimeError('unexpected radio-state layout')
            state = struct.unpack('<I', result)[0]
            print('SIT radio state', state, flush=True)
            if args.action == 'status' or (2 <= state <= 10 if args.action == 'on' else state == 0):
                return
            if time.monotonic() >= deadline:
                raise RuntimeError('radio state did not reach requested state')
            time.sleep(1)
    finally:
        client.close()


if __name__ == '__main__':
    main()
