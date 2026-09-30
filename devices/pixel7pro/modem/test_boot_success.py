# SPDX-License-Identifier: MIT
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('boot_success', Path(__file__).parents[1] / 'scripts/pixel-boot-success.py')
boot_success = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot_success)


class BootSuccessTests(unittest.TestCase):
    def metadata(self):
        data = bytearray(b'x' * 8192)
        struct.pack_into('<IHH', data, 0, 0x49564544, 3, 11)
        struct.pack_into('<II', data, 48, 0x403, 0x3)
        return data

    def test_only_success_bit_changes_and_repeat_is_noop(self):
        before = self.metadata()
        after = boot_success.prepared(before)
        self.assertEqual([i for i, (a, b) in enumerate(zip(before, after)) if a != b], [49])
        self.assertEqual(before[49] ^ after[49], 2)
        self.assertEqual(boot_success.prepared(after), after)

    def test_other_active_slot_and_wrong_format_refused(self):
        for offset, value in ((48, 0x103), (52, 0x403), (0, 0)):
            data = self.metadata()
            struct.pack_into('<I', data, offset, value)
            with self.assertRaises(ValueError):
                boot_success.prepared(data)
