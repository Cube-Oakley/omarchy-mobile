#!/usr/bin/env python3
"""Host-only checks for boot-image delta reconstruction; no phone required."""
import bz2
import importlib.util
from pathlib import Path
import shutil
import subprocess
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/reconstruct-stock.py"
SPEC = importlib.util.spec_from_file_location("reconstruct_stock", SCRIPT)
stock = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stock)


def number(value):
    return (abs(value) | (1 << 63 if value < 0 else 0)).to_bytes(8, "little")


def patch(triples, diff, extra, size, brotli=False):
    def compress(data):
        if brotli:
            return subprocess.run(["brotli", "-c"], input=data, capture_output=True,
                                  check=True).stdout
        return bz2.compress(data)
    control = compress(b"".join(number(n) for triple in triples for n in triple))
    delta, literal = compress(diff), compress(extra)
    magic = b"BSDF2\x02\x02\x02" if brotli else b"BSDIFF40"
    return magic + number(len(control)) + number(len(delta)) + number(size) + control + delta + literal


class ReconstructionTests(unittest.TestCase):
    def test_wraparound_and_literal_bytes(self):
        data = patch([(3, 1, 0)], bytes([1, 255, 2]), b"!", 4)
        self.assertEqual(stock.bspatch(b"abc", data, 4), b"bae!")

    @unittest.skipUnless(shutil.which("brotli"), "brotli CLI unavailable")
    def test_android_brotli_format(self):
        data = patch([(3, 1, 0)], bytes([1, 255, 2]), b"!", 4, brotli=True)
        self.assertEqual(stock.bspatch(b"abc", data, 4), b"bae!")

    def test_signed_seek_and_zero_outside_source(self):
        data = patch([(0, 0, -1), (2, 0, 0)], bytes([7, 1]), b"", 2)
        self.assertEqual(stock.bspatch(b"a", data, 2), bytes([7, 98]))

    def test_reject_truncated_header(self):
        with self.assertRaises(ValueError):
            stock.bspatch(b"", b"BSDIFF40", 1)

    def test_reject_destination_overrun(self):
        with self.assertRaises(ValueError):
            stock.bspatch(b"abc", patch([(5, 0, 0)], b"12345", b"", 3), 3)

    def test_reject_short_diff(self):
        with self.assertRaises(ValueError):
            stock.bspatch(b"a", patch([(1, 0, 0)], b"", b"", 1), 1)

    def test_reject_wrong_output_size(self):
        with self.assertRaises(ValueError):
            stock.bspatch(b"a", patch([(1, 0, 0)], b"\0", b"", 1), 2)

    def test_reject_unused_stream(self):
        with self.assertRaises(ValueError):
            stock.bspatch(b"a", patch([(1, 0, 0)], b"\0\0", b"", 1), 1)

    def test_extent_partition_boundary(self):
        self.assertEqual(stock.extents([b"\x08\x01\x10\x01"], 4096, 8192), [(4096, 4096)])
        with self.assertRaises(ValueError):
            stock.extents([b"\x08\x02\x10\x01"], 4096, 8192)


if __name__ == "__main__":
    unittest.main()
