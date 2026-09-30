#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Temporary LTE-only diagnosis; restore the exact saved modem mode afterward.

Radio must be SIT OFF and listener stopped. Stock builder 0x74680 maps
LTE_ONLY to protocol 11. Getter 0x48650 reads a u32 at response offset 12.
Other modes are opaque: only restore bytes previously obtained from this CP.
All modem NV remains in the RAM-only laboratory.
"""
import argparse
from pathlib import Path
import struct
from data_call import private_write
from sit import Sit

SAVED = Path('/run/modem-lab/original-radio-mode.bin')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['lte-only', 'restore'])
    args = parser.parse_args()
    client = Sit()
    try:
        if client.request(0x0801) != bytes(4):
            raise RuntimeError('SIT radio must be off')
        calls = client.request(0x0602)
        if not calls or calls[0]:
            raise RuntimeError('release data bearers first')
        current = client.request(0x070b)
        if len(current) != 4:
            raise RuntimeError('unexpected preferred-mode response')
        if args.action == 'lte-only':
            if SAVED.exists():
                raise RuntimeError('restore previous experiment first')
            private_write(SAVED, current)
            target = struct.pack('<I', 11)
        else:
            target = SAVED.read_bytes()
            if len(target) != 4:
                raise RuntimeError('invalid saved mode')
        client.request(0x070a, target, timeout=30)
        if client.request(0x070b) != target:
            raise RuntimeError('preferred-mode verification failed')
        if args.action == 'restore':
            SAVED.unlink()
        print('preferred protocol mode', struct.unpack('<I', target)[0], flush=True)
    finally:
        client.close()


if __name__ == '__main__':
    main()
