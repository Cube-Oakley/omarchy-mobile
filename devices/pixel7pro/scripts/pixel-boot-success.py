#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mark the validated installed Pixel slot A successful, changing one flag bit.

The GS201 devinfo v3.11 layout and flag agree with Google's BootControl.cpp
and this device's ABL ab_slot_set_successful implementation. No Trusty/OTP
operation is performed. All other devinfo bytes, including slot B, must match
the original after writing. A private full backup is fsynced before the write.
"""
import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess

SUCCESS_BYTE = 49
SUCCESS_BIT = 2


def prepared(data):
    if len(data) < 8192 or struct.unpack_from('<IHH', data) != (0x49564544, 3, 11):
        raise ValueError('Unrecognized devinfo layout')
    a, b = struct.unpack_from('<II', data, 48)
    # retry_count is the first byte; successful/unbootable/active occupy
    # bits 9/8/10 in the matching ABL. Accept only the active, bootable A.
    if not a & 0x400 or b & 0x400 or a & 0x100 or (a & 0xff) > 3:
        raise ValueError('Slot A is not the expected active bootable slot')
    result = bytearray(data)
    result[SUCCESS_BYTE] |= SUCCESS_BIT
    if any(x != y for i, (x, y) in enumerate(zip(data, result)) if i != SUCCESS_BYTE):
        raise AssertionError('Unexpected boot metadata change')
    return bytes(result)


def partition(name):
    matches = [Path('/dev') / p.name for p in Path('/sys/class/block').iterdir()
               if 'PARTNAME=' + name + '\n' in (p / 'uevent').read_text()]
    if len(matches) != 1:
        raise RuntimeError('Partition lookup is ambiguous')
    return matches[0]


def digest(path):
    with path.open('rb') as src:
        return hashlib.file_digest(src, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--expected-boot-sha256', required=True)
    parser.add_argument('--mark-successful', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch('[0-9a-f]{64}', args.expected_boot_sha256):
        parser.error('Expected a SHA256 digest')
    if b'google,GS201 CHEETAH' not in Path('/sys/firmware/devicetree/base/compatible').read_bytes():
        raise RuntimeError('Not the supported Pixel')
    if Path('/etc/omarchy-mobile-pixel-root').read_text().strip() != 'v1':
        raise RuntimeError('Not the validated Linux root')
    if digest(partition('boot_a')) != args.expected_boot_sha256:
        raise RuntimeError('Installed boot image differs from the validated image')
    os.umask(0o077)
    with open('/run/pixel-boot-success.lock', 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        path = partition('devinfo')
        size = int(subprocess.check_output(['blockdev', '--getsize64', str(path)]))
        if not 8192 <= size <= 64 * 1024 * 1024:
            raise RuntimeError('Unexpected devinfo partition size')
        before = path.read_bytes()
        if len(before) != size:
            raise RuntimeError('Incomplete devinfo read')
        after = prepared(before)
        if after == before:
            print('Validated slot A is already marked successful')
            return
        print('Validated slot A: one success bit at byte 49 will change; slot B preserved')
        if not args.mark_successful:
            print('Dry run; protected partition remains read-only')
            return
        backup = Path('/var/lib/omarchy-mobile/boot-success-backups')
        backup.mkdir(mode=0o700, parents=True, exist_ok=True)
        dest = backup / (hashlib.sha256(before).hexdigest() + '.devinfo')
        if not dest.exists():
            with dest.open('xb') as out:
                out.write(before); out.flush(); os.fsync(out.fileno())
            directory = os.open(backup, os.O_RDONLY | os.O_DIRECTORY)
            try: os.fsync(directory)
            finally: os.close(directory)
        elif dest.read_bytes() != before:
            raise RuntimeError('Boot metadata backup mismatch')
        if subprocess.check_output(['blockdev', '--getro', str(path)]).strip() != b'1':
            raise RuntimeError('devinfo must begin read-only')
        try:
            subprocess.run(['blockdev', '--setrw', str(path)], check=True)
            fd = os.open(path, os.O_RDWR | os.O_DSYNC | os.O_CLOEXEC)
            try:
                if os.pread(fd, size, 0) != before:
                    raise RuntimeError('Boot metadata changed concurrently')
                if os.pwrite(fd, after[SUCCESS_BYTE:SUCCESS_BYTE + 1], SUCCESS_BYTE) != 1:
                    raise RuntimeError('Boot success write incomplete')
                os.fsync(fd)
            finally:
                os.close(fd)
        finally:
            subprocess.run(['blockdev', '--setro', str(path)], check=True)
        if path.read_bytes() != after:
            raise RuntimeError('Boot metadata readback differs; do not reboot')
        print('Slot A success verified; every other byte preserved; devinfo read-only')


if __name__ == '__main__':
    main()
