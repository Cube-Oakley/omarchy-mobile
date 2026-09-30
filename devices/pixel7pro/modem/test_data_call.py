#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-only lifecycle failure tests; no radio/device access."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import data_call


class Modem:
    def __init__(self, status=0, fail_release=False):
        self.sent = []
        self.status = status
        self.fail_release = fail_release

    def request(self, ident, payload=b'', **kwargs):
        self.sent.append((ident, payload))
        if ident == 0x0701:
            return b'\1\0\0\x15'
        if ident == 0x0602:
            return b'\0'
        if ident == 0x0711:
            return b'\1'
        if ident == 0x061a:
            return struct.pack('<i', 3)
        if ident == 0x0600:
            response = bytearray(292)
            struct.pack_into('<HBBB', response, 0, self.status, payload[0], 0 if self.status else 1, 2)
            return response
        if ident == 0x0601 and self.fail_release:
            raise TimeoutError('synthetic release failure')
        return b''


class LifecycleTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        root = Path(self.directory.name)
        self.state = root / 'owned.json'
        self.response = root / 'response.bin'
        self.patches = [patch.object(data_call, 'STATE', self.state),
                        patch.object(data_call, 'RESPONSE', self.response)]
        for item in self.patches:
            item.start()

    def tearDown(self):
        for item in self.patches:
            item.stop()
        self.directory.cleanup()

    def test_rejected_setup_releases_pdu_without_creating_ownership(self):
        modem = Modem(status=36)
        with self.assertRaises(RuntimeError):
            data_call.activate(modem)
        self.assertEqual(modem.sent[-1], (0x061b, struct.pack('<i', 3)))
        self.assertFalse(self.state.exists())
        self.assertFalse(self.response.exists())

    def test_existing_ownership_prevents_setup(self):
        self.state.write_text('{}')
        modem = Modem()
        with self.assertRaises(RuntimeError):
            data_call.activate(modem)
        self.assertNotIn(0x0600, [i for i, _ in modem.sent])

    def test_setup_timeout_keeps_pdu_reserved_for_recovery(self):
        modem = Modem()
        request = modem.request
        def timeout(ident, payload=b'', **kwargs):
            if ident == 0x0600:
                raise TimeoutError('unknown setup outcome')
            return request(ident, payload, **kwargs)
        modem.request = timeout
        with self.assertRaises(TimeoutError):
            data_call.activate(modem)
        self.assertEqual(json.loads(self.state.read_text()), {'cid': 1, 'session': 3})
        self.assertNotIn(0x061b, [i for i, _ in modem.sent])

    def test_failed_deactivation_retains_recovery_record(self):
        self.state.write_text(json.dumps({'cid': 1, 'session': 3}))
        self.response.write_bytes(b'private')
        original = Path.read_text
        def read(path, *args, **kwargs):
            if str(path) == '/sys/class/net/rmnet0/flags':
                return '0x0'
            return original(path, *args, **kwargs)
        modem = Modem(fail_release=True)
        with patch.object(Path, 'read_text', read), self.assertRaises(TimeoutError):
            data_call.deactivate(modem)
        self.assertEqual(modem.sent, [(0x0601, b'\1\0')])
        self.assertTrue(self.state.exists())
        self.assertTrue(self.response.exists())

    def test_ims_failure_preserves_active_internet_bearer(self):
        self.state.write_text(json.dumps({'cid': 1, 'session': 2}))
        self.response.write_bytes(b'existing private internet configuration')
        modem = Modem(status=36)
        request = modem.request
        def existing(ident, payload=b'', **kwargs):
            if ident == 0x0602:
                record = bytearray(292)
                record[2:4] = bytes([1, 1])
                return bytes([1]) + record
            return request(ident, payload, **kwargs)
        modem.request = existing
        with self.assertRaises(RuntimeError):
            data_call.activate(modem, 'ims')
        self.assertEqual(json.loads(self.state.read_text()), {'cid': 1, 'session': 2})
        self.assertEqual(self.response.read_bytes(), b'existing private internet configuration')
        self.assertFalse(data_call.paths(2)[0].exists())
        self.assertEqual(modem.sent[-1], (0x061b, struct.pack('<i', 3)))

    def test_duplicate_pdu_allocation_does_not_release_existing_bearer(self):
        self.state.write_text(json.dumps({'cid': 1, 'session': 3}))
        modem = Modem()
        request = modem.request
        def existing(ident, payload=b'', **kwargs):
            if ident == 0x0602:
                record = bytearray(292)
                record[2:4] = bytes([1, 1])
                return bytes([1]) + record
            return request(ident, payload, **kwargs)
        modem.request = existing
        with self.assertRaises(RuntimeError):
            data_call.activate(modem, 'ims')
        self.assertNotIn(0x061b, [i for i, _ in modem.sent])
        self.assertFalse(data_call.paths(2)[0].exists())
        self.assertEqual(json.loads(self.state.read_text()), {'cid': 1, 'session': 3})

    def test_unowned_bearer_blocks_additional_setup(self):
        modem = Modem()
        request = modem.request
        def unowned(ident, payload=b'', **kwargs):
            if ident == 0x0602:
                record = bytearray(292)
                record[2:4] = bytes([1, 1])
                return bytes([1]) + record
            return request(ident, payload, **kwargs)
        modem.request = unowned
        with self.assertRaises(RuntimeError):
            data_call.activate(modem, 'ims')
        self.assertNotIn(0x061a, [i for i, _ in modem.sent])

    def test_call_list_rejects_truncation_and_duplicate_cids(self):
        with self.assertRaises(RuntimeError):
            data_call.call_ids(bytes([1]) + bytes(291))
        record = bytearray(292)
        record[2] = 1
        with self.assertRaises(RuntimeError):
            data_call.call_ids(bytes([2]) + record + record)


if __name__ == '__main__':
    unittest.main()
