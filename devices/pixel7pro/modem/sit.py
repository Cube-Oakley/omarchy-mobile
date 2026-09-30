#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Single-owner SIT IPC0 transport for the manual physical-SIM laboratory."""
from collections import deque
import fcntl
import os
import select
import struct
import time

# Data/radio/IMS lifecycle, display state, read-only status, and incoming SMS ACK.
# No dial, outgoing SMS, SIM I/O, PIN, subscription, APDU or vendor NV requests.
ALLOWED = {0x0000, 0x0102, 0x0108, 0x0200, 0x0600, 0x0601, 0x0602, 0x0603, 0x0613,
           0x061a, 0x061b, 0x0625, 0x0700, 0x0701, 0x0703, 0x0704, 0x070a, 0x070b, 0x0710, 0x0711, 0x0800, 0x0801,
           0x0748, 0x0749, 0x0750, 0x0902, 0x091b, 0x0928, 0x0930, 0x0953, 0x0956, 0x0957, 0x0d07, 0x0d08, 0x0d1d, 0x0d38, 0x0d39, 0x0d3a}


class ModemError(RuntimeError):
    def __init__(self, error, payload):
        super().__init__('modem rejected request with error ' + str(error))
        self.error = error
        self.payload = payload  # private; never include payload in str/repr


class Sit:
    def __init__(self, *, allow_test_sms=False, allow_test_call=False):
        self.allow_test_sms = allow_test_sms
        self.allow_test_call = allow_test_call
        self.lock = os.open('/run/modem-lab/ipc0.lock', os.O_CREAT | os.O_RDWR | os.O_CLOEXEC, 0o600)
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.fd = os.open('/dev/umts_ipc0', os.O_RDWR | os.O_NONBLOCK | os.O_CLOEXEC)
        except BaseException:
            os.close(self.lock)
            raise
        self.token = int.from_bytes(os.urandom(4), 'little') & 0x7fffffff
        self.buffer = bytearray()
        self.indications = deque()

    def close(self):
        os.close(self.fd)
        os.close(self.lock)

    def read_packet(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            if len(self.buffer) >= 6:
                kind, _, _, length = struct.unpack_from('<BBHH', self.buffer)
                if kind not in (1, 2) or not (8 if kind == 2 else 12) <= length <= 32768:
                    raise RuntimeError('invalid SIT framing')
                if len(self.buffer) >= length:
                    packet = bytes(self.buffer[:length])
                    del self.buffer[:length]
                    return packet
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            ready, _, _ = select.select([self.fd], [], [], remaining)
            if not ready:
                return None
            chunk = os.read(self.fd, 65536)
            if not chunk:
                raise EOFError('SIT channel closed')
            self.buffer.extend(chunk)
            if len(self.buffer) > 65536:
                raise RuntimeError('SIT receive limit exceeded')

    def request(self, ident, payload=b'', timeout=8):
        if (ident not in ALLOWED
                and not (ident in (0x0100, 0x0d1b) and getattr(self, 'allow_test_sms', False))
                and not (ident in (0x0d00, 0x0d02, 0x0d03, 0x0d0a) and getattr(self, 'allow_test_call', False))):
            raise ValueError('request outside laboratory allow-list')
        self.token += 1
        data = struct.pack('<BBHHIBB', 0, 0, ident, 12 + len(payload), self.token, 0, 0) + payload
        if os.write(self.fd, data) != len(data):
            raise RuntimeError('short SIT write')
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            packet = self.read_packet(deadline - time.monotonic())
            if packet is None:
                break
            kind, _, message, length = struct.unpack_from('<BBHH', packet)
            if kind == 2:
                # Physical-channel and signal-strength reports are snapshots.
                # Retain the latest of each during long requests so frequent
                # telemetry cannot exhaust the queue ahead of an SMS reply.
                # SMS, calls, registration and all other events keep FIFO order.
                if message in (0x0742, 0x0906):
                    for old in self.indications:
                        if struct.unpack_from('<H', old, 2)[0] == message:
                            self.indications.remove(old)
                            break
                if len(self.indications) >= 256:
                    raise RuntimeError('unprocessed indication limit exceeded')
                self.indications.append(packet)
            elif message == ident and struct.unpack_from('<I', packet, 6)[0] == self.token:
                print(f'SIT 0x{ident:04x}: error={packet[10]} length={length}', flush=True)
                if packet[10]:
                    raise ModemError(packet[10], packet[12:])
                return packet[12:]
        raise TimeoutError(f'SIT 0x{ident:04x}')

    def next_indication(self, timeout=1):
        if self.indications:
            return self.indications.popleft()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            packet = self.read_packet(deadline - time.monotonic())
            if packet is None or packet[0] == 2:
                return packet
        return None
