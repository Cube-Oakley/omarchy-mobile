#!/usr/bin/env python3
"""Mark the validated CPH2655 boot_a successful in both Qualcomm GPT copies.

Dry-run by default. Requires the private original GPT capture and the exact
installed boot hash. Changes only boot_a's success bit and dependent CRCs;
partition addresses, other attributes, other slots and payloads are preserved.
Run only after independently verifying a normal boot, never to bless a RAM test.
"""
import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import zlib

SECTOR = 4096
REGION = 1024 * 1024
DISK_BYTES = 634880 * SECTOR
REGION_OFFSETS = (0, DISK_BYTES - REGION)
SUCCESS = 1 << 54
ACTIVE = 1 << 50
UNBOOTABLE = 1 << 55


def require(condition, message):
    if not condition:
        raise ValueError(message)


def inspect(data):
    require(len(data) == REGION * 2, 'Incomplete GPT regions')
    copies = []
    for index, disk_offset in enumerate(REGION_OFFSETS):
        start = index * REGION
        h = start + (SECTOR if index == 0 else REGION - SECTOR)
        header = data[h:h + SECTOR]
        fields = struct.unpack_from('<8sIIIIQQQQ16sQIII', header)
        sig, rev, size, crc, reserved, here, other, first, last, guid, lba, count, stride, tcrc = fields
        require((sig, rev, size, reserved) == (b'EFI PART', 0x10000, 92, 0), 'Unexpected GPT header')
        require((here, other) == ((1, 634879) if index == 0 else (634879, 1)), 'Unexpected GPT header locations')
        require((first, last, count, stride) == (6, 634874, 96, 128), 'Unexpected GPT geometry')
        require(lba == (2 if index == 0 else 634875), 'Unexpected GPT table location')
        check = bytearray(header[:size]); check[16:20] = bytes(4)
        require(zlib.crc32(check) == crc, 'GPT header CRC mismatch')
        t = start + lba * SECTOR - disk_offset
        table = data[t:t + count * stride]
        require(zlib.crc32(table) == tcrc, 'GPT table CRC mismatch')
        names = {}
        for i in range(count):
            e = table[i * stride:(i + 1) * stride]
            if e[:16] == bytes(16):
                continue
            name = e[56:128].decode('utf-16le').rstrip('\0')
            require(name not in names, 'Duplicate GPT partition name')
            names[name] = i
        require(names.get('boot_a') == 11 and names.get('boot_b') == 47, 'Unexpected boot partition positions')
        a = struct.unpack_from('<Q', table, 11 * stride + 48)[0]
        b = struct.unpack_from('<Q', table, 47 * stride + 48)[0]
        require(a & ACTIVE and not a & UNBOOTABLE and not b & ACTIVE, 'Slot A must be active and bootable')
        copies.append(dict(header=h, table=t, table_bytes=table, guid=guid,
                           attr=t + 11 * stride + 48, attributes=a))
    require(copies[0]['table_bytes'] == copies[1]['table_bytes'], 'GPT tables disagree')
    require(copies[0]['guid'] == copies[1]['guid'], 'GPT disk GUIDs disagree')
    return copies


def prepared(before, baseline):
    copies = inspect(before)
    reference = inspect(baseline)
    # Only boot_a retry/success byte and CRCs may differ from the exact original
    # capture. All partition layout and adjacent firmware bytes must match.
    normalized = bytearray(before)
    for item, original in zip(copies, reference):
        off = item['attr'] + 6
        require((before[off] ^ baseline[off]) & ~0x78 == 0,
                'Unexpected boot_a attribute change')
        normalized[off] = baseline[off]
        for delta in (16, 88):
            pos = item['header'] + delta
            normalized[pos:pos + 4] = baseline[pos:pos + 4]
    require(normalized == baseline, 'GPT differs from original beyond boot_a retries/success and CRCs')
    result = bytearray(before)
    for item in copies:
        result[item['attr'] + 6] |= 0x40
        h, t = item['header'], item['table']
        struct.pack_into('<I', result, h + 88, zlib.crc32(result[t:t + 96 * 128]))
        result[h + 16:h + 20] = bytes(4)
        struct.pack_into('<I', result, h + 16, zlib.crc32(result[h:h + 92]))
    inspect(result)
    return bytes(result), copies


def read_regions(fd):
    return b''.join(os.pread(fd, REGION, off) for off in REGION_OFFSETS)


def digest(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, required=True, help='Original primary + backup 1 MiB captures')
    ap.add_argument('--expected-baseline-sha256', required=True)
    ap.add_argument('--expected-boot-sha256', required=True)
    ap.add_argument('--mark-successful', action='store_true')
    a = ap.parse_args()
    for value in (a.expected_baseline_sha256, a.expected_boot_sha256):
        require(re.fullmatch('[0-9a-f]{64}', value), 'Expected SHA256')
    require(Path('/sys/firmware/devicetree/base/model').read_bytes().rstrip(b'\0') ==
            b'OnePlus 13 CPH2655 Omarchy USB probe', 'Wrong board')
    require(Path('/etc/omarchy-mobile-oneplus13-root').read_text().strip() == 'oneplus13-cph2655-v1', 'Wrong root')
    require('PARTNAME=boot_a\n' in Path('/sys/class/block/sde12/uevent').read_text(), 'Wrong boot partition')
    require(digest('/dev/sde12') == a.expected_boot_sha256, 'Installed boot image differs')
    require(int(Path('/sys/class/block/sde/size').read_text()) * 512 == DISK_BYTES, 'Wrong disk size')
    require(int(Path('/sys/class/block/sde/queue/logical_block_size').read_text()) == SECTOR, 'Wrong sector size')
    subprocess.run(['pgrep', '-x', 'Hyprland'], check=True, stdout=subprocess.DEVNULL)
    require(digest(a.baseline) == a.expected_baseline_sha256, 'Original GPT capture digest mismatch')
    baseline = a.baseline.read_bytes()
    os.umask(0o077)
    with open('/run/oneplus13-boot-success.lock', 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fd = os.open('/dev/sde', os.O_RDONLY | os.O_CLOEXEC)
        try:
            before = read_regions(fd)
        finally:
            os.close(fd)
        after, copies = prepared(before, baseline)
        print('Validated matching GPT copies; boot_a attributes:', hex(copies[0]['attributes']))
        if before == after:
            print('Slot A already successful; no write')
            return
        print('Prepared one boot_a success bit per GPT copy and their CRCs; all other bytes preserved')
        if not a.mark_successful:
            print('Dry run; storage remains read-only')
            return
        directory = Path('/var/lib/omarchy-mobile/boot-success-backups')
        directory.mkdir(parents=True, mode=0o700, exist_ok=True)
        backup = directory / (hashlib.sha256(before).hexdigest() + '.gpt-regions')
        if backup.exists():
            require(backup.read_bytes() == before, 'Existing backup differs')
        else:
            with backup.open('xb') as stream:
                stream.write(before); stream.flush(); os.fsync(stream.fileno())
        dfd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)
        require(subprocess.check_output(['blockdev', '--getro', '/dev/sde']).strip() == b'1', 'Disk must start read-only')
        try:
            subprocess.run(['blockdev', '--setrw', '/dev/sde'], check=True)
            fd = os.open('/dev/sde', os.O_RDWR | os.O_DSYNC | os.O_CLOEXEC)
            try:
                require(read_regions(fd) == before, 'GPT changed concurrently')
                # Complete and verify backup first, then primary. One valid GPT
                # copy remains intact across each table/header update.
                for index in (1, 0):
                    item = copies[index]
                    for pos in (item['attr'] // SECTOR * SECTOR, item['header']):
                        disk_pos = REGION_OFFSETS[index] + pos - index * REGION
                        block = after[pos:pos + SECTOR]
                        require(os.pwrite(fd, block, disk_pos) == SECTOR, 'Incomplete GPT sector write')
                        os.fsync(fd)
                        require(os.pread(fd, SECTOR, disk_pos) == block, 'GPT sector readback mismatch')
                require(read_regions(fd) == after, 'Full GPT region readback mismatch')
            finally:
                os.close(fd)
        finally:
            subprocess.run(['blockdev', '--setro', '/dev/sde'], check=True)
        print('Slot A success verified; GPT copies agree; disk read-only')


if __name__ == '__main__':
    main()
