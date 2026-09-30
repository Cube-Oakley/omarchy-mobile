# SPDX-License-Identifier: MIT
"""Saved, reversible radio settings in the RAM-only laboratory."""
from pathlib import Path
import struct
from data_call import private_write

SAVED_USAGE = Path('/run/modem-lab/original-usage-setting.bin')
SAVED_NR = Path('/run/modem-lab/original-nr-mode.bin')


def nr_mode(client, restore=False):
    # Matching BuildSetNrMode 0x761f0: one byte; getter 0x4b200.
    # On this phone, changing 3 to 0 returns NR21 to LTE14 and recovers
    # actual DNS/HTTPS traffic. Preserve the opaque original byte.
    current = client.request(0x0749)
    if len(current) != 1:
        raise RuntimeError('unexpected NR mode response')
    if restore:
        target = SAVED_NR.read_bytes()
        if len(target) != 1:
            raise RuntimeError('invalid saved NR mode')
    else:
        if SAVED_NR.exists():
            raise RuntimeError('NR experiment already active')
        private_write(SAVED_NR, current)
        target = b'\0'
    client.request(0x0748, target, timeout=30)
    if client.request(0x0749) != target:
        raise RuntimeError('NR mode did not reach requested value')
    if restore:
        SAVED_NR.unlink()
    print('verified NR mode', target[0], flush=True)


def usage_setting(client, restore=False):
    # Stock SetUsageSettingHandler accepts 1/2 unchanged; builder 0x768c0
    # sends u32 at offset 12. AOSP UsageSetting defines voice=1, data=2.
    current = client.request(0x0957)
    valid = (struct.pack('<I', 1), struct.pack('<I', 2))
    if current not in valid:
        raise RuntimeError('unexpected usage setting')
    if restore:
        target = SAVED_USAGE.read_bytes()
        if target not in valid:
            raise RuntimeError('invalid saved usage setting')
    else:
        if SAVED_USAGE.exists():
            raise RuntimeError('usage experiment already active')
        private_write(SAVED_USAGE, current)
        target = valid[1]
    client.request(0x0956, target)
    if client.request(0x0957) != target:
        raise RuntimeError('usage setting did not reach requested value')
    if restore:
        SAVED_USAGE.unlink()
    print('verified usage setting', struct.unpack('<I', target)[0], flush=True)
