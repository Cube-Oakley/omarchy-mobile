#!/usr/bin/env python3
"""Host-only corruption and preservation checks for Qualcomm GPT success marking."""
import importlib.util
from pathlib import Path
import struct
import unittest
import zlib

spec = importlib.util.spec_from_file_location('boot_success', Path(__file__).parents[1] / 'scripts/mark-boot-successful.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def fixture(a=0x77000000000000, b=0xf3000000000000):
    result = bytearray(m.REGION * 2)
    table = bytearray(96 * 128)
    for index, name, attr in [(11, 'boot_a', a), (47, 'boot_b', b)]:
        e = index * 128
        table[e:e + 32] = bytes(range(32))
        struct.pack_into('<QQQ', table, e + 32, 100 + index * 100, 199 + index * 100, attr)
        raw = name.encode('utf-16le'); table[e + 56:e + 56 + len(raw)] = raw
    for index, disk_offset in enumerate(m.REGION_OFFSETS):
        h = index * m.REGION + (m.SECTOR if index == 0 else m.REGION - m.SECTOR)
        lba = 2 if index == 0 else 634875
        t = index * m.REGION + lba * m.SECTOR - disk_offset
        result[t:t + len(table)] = table
        struct.pack_into('<8sIIIIQQQQ16sQIII', result, h, b'EFI PART', 0x10000, 92, 0, 0,
                         1 if index == 0 else 634879, 634879 if index == 0 else 1,
                         6, 634874, bytes(range(16)), lba, 96, 128, zlib.crc32(table))
        struct.pack_into('<I', result, h + 16, zlib.crc32(result[h:h + 92]))
    return bytes(result)


class BootSuccess(unittest.TestCase):
    def test_only_success_and_crcs_change(self):
        baseline = fixture()
        before = fixture(a=0x37000000000000)
        after, copies = m.prepared(before, baseline)
        self.assertEqual(after, baseline)
        allowed = set()
        for c in copies:
            allowed.add(c['attr'] + 6)
            allowed.update(range(c['header'] + 16, c['header'] + 20))
            allowed.update(range(c['header'] + 88, c['header'] + 92))
        self.assertTrue(all(i in allowed for i, (a, b) in enumerate(zip(before, after)) if a != b))
        self.assertEqual(m.prepared(after, baseline)[0], after)

    def test_retry_count_preserved(self):
        after, _ = m.prepared(fixture(a=0x2f000000000000), fixture())
        self.assertEqual(m.inspect(after)[0]['attributes'], 0x6f000000000000)

    def test_corrupt_header_and_table_rejected(self):
        for pos in (4096 + 16, 8192 + 1500, 2 * m.REGION - 4096 + 88):
            bad = bytearray(fixture()); bad[pos] ^= 1
            with self.assertRaises(ValueError): m.prepared(bad, fixture())

    def test_disagreeing_valid_copies_rejected(self):
        bad = fixture()[:m.REGION] + fixture(a=0x37000000000000)[m.REGION:]
        with self.assertRaises(ValueError): m.prepared(bad, fixture())

    def test_wrong_active_or_unbootable_slot_rejected(self):
        for a, b in [(0x33000000000000, 0xf3000000000000),
                     (0xb7000000000000, 0xf3000000000000),
                     (0x37000000000000, 0xf7000000000000)]:
            with self.assertRaises(ValueError): m.prepared(fixture(a, b), fixture())

    def test_unrelated_bytes_rejected(self):
        bad = bytearray(fixture()); bad[30000] ^= 1
        with self.assertRaises(ValueError): m.prepared(bad, fixture())

    def test_incomplete_capture_rejected(self):
        with self.assertRaises(ValueError): m.prepared(fixture()[:-1], fixture())


if __name__ == '__main__':
    unittest.main()
