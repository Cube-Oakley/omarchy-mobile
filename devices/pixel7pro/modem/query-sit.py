#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only status through the shared physical-slot IPC0 transport.

Stop the persistent listener first; all SIT helpers use the same owner lock.
Only status fields are printed, never identities, location or IP addresses.
"""
import struct
from sit import Sit


def main():
    client = Sit()
    try:
        for ident in (0x0801, 0x0701, 0x0602):
            result = client.request(ident)
            if ident == 0x0801 and len(result) == 4:
                print('radio_state_raw', struct.unpack('<I', result)[0], flush=True)
            elif ident == 0x0701 and len(result) >= 4:
                print('PS registration', result[0], 'reject cause', result[1],
                      'radio technology raw', result[3], flush=True)
            elif ident == 0x0602 and result:
                print('data call count', result[0], flush=True)
    finally:
        client.close()


if __name__ == '__main__':
    main()
