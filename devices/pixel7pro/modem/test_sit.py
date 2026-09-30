#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-only transport tests; no modem or radio access."""
from collections import deque
import os
import socket
import struct
import tempfile
import threading
import unittest
from sit import Sit


class TransportTests(unittest.TestCase):
    def setUp(self):
        self.local, self.peer = socket.socketpair()
        self.local.setblocking(False)
        self.client = Sit.__new__(Sit)
        self.client.fd = self.local.fileno()
        self.client.token = 100
        self.client.buffer = bytearray()
        self.client.indications = deque()

    def tearDown(self):
        self.local.close()
        self.peer.close()

    def test_fragmented_reply_preserves_coalesced_indications(self):
        indication = struct.pack('<BBHHH', 2, 0, 0x0009, 8, 0)
        def reply():
            request = self.peer.recv(4096)
            token = struct.unpack_from('<I', request, 6)[0]
            response = struct.pack('<BBHHIBB', 1, 0, 0x0701, 16, token, 0, 0) + b'\x01\0\0\x15'
            self.peer.sendall(indication[:3])
            self.peer.sendall(indication[3:] + response + indication)
        worker = threading.Thread(target=reply)
        worker.start()
        self.assertEqual(self.client.request(0x0701), b'\x01\0\0\x15')
        worker.join(2)
        self.assertEqual(self.client.next_indication(), indication)
        self.assertEqual(self.client.next_indication(), indication)

    def test_telemetry_burst_preserves_sms_and_request_response(self):
        sms = struct.pack('<BBHHHBB', 2, 0, 0x0d1e, 11, 0, 7, 1)+b'x'
        def reply():
            request = self.peer.recv(4096)
            token = struct.unpack_from('<I', request, 6)[0]
            self.peer.sendall(sms)
            for index in range(400):
                self.peer.sendall(struct.pack('<BBHHHI', 2, 0, 0x0742, 12, 0, index))
            self.peer.sendall(struct.pack('<BBHHIBB', 1, 0, 0x0701, 16, token, 0, 0)+b'\1\0\0\x0e')
        worker = threading.Thread(target=reply)
        worker.start()
        self.assertEqual(self.client.request(0x0701), b'\1\0\0\x0e')
        worker.join(2)
        self.assertEqual(self.client.next_indication(), sms)
        self.assertEqual(struct.unpack_from('<I', self.client.next_indication(), 8)[0], 399)
        self.assertFalse(self.client.indications)

    def test_disallowed_commands_never_write(self):
        for command in (0x0001, 0x0100, 0x0d00, 0x0d02, 0x0d03, 0x0d0a, 0x0d1b, 0x0201, 0x0208, 0x020c, 0x0249):
            with self.assertRaises(ValueError):
                self.client.request(command)
        self.peer.setblocking(False)
        with self.assertRaises(BlockingIOError):
            self.peer.recv(1)

    def test_invalid_length_is_rejected(self):
        self.peer.sendall(struct.pack('<BBHH', 1, 0, 0x0701, 8))
        with self.assertRaises(RuntimeError):
            self.client.read_packet(0.1)

    def test_timeout_and_eof_are_distinct(self):
        self.assertIsNone(self.client.read_packet(0.01))
        self.peer.shutdown(socket.SHUT_WR)
        with self.assertRaises(EOFError):
            self.client.read_packet(0.1)

    def test_sms_persist_is_private_and_rejects_truncation(self):
        import importlib.util
        from pathlib import Path
        spec = importlib.util.spec_from_file_location('monitor', Path(__file__).with_name('monitor-sit.py'))
        monitor = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(monitor)
        with tempfile.TemporaryDirectory() as directory:
            monitor.INBOX = Path(directory) / 'inbox'
            packet = struct.pack('<BBHHHBB', 2, 0, 0x010c, 13, 0, 7, 3) + b'\0\0\0'
            self.assertEqual(monitor.persist_sms(packet), 7)
            stored, = monitor.INBOX.iterdir()
            self.assertEqual(stored.read_bytes(), packet)
            self.assertEqual(stored.stat().st_mode & 0o777, 0o600)
            self.assertEqual(monitor.INBOX.stat().st_mode & 0o777, 0o700)
            with self.assertRaises(ValueError):
                monitor.persist_sms(packet[:-1])


if __name__ == '__main__':
    unittest.main()
