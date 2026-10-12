#!/usr/bin/env python3
"""Configure this port's private Bluetooth test identity through Linux MGMT.

The factory address path is not implemented yet. This never writes a factory
partition and waits for the controller to finish reconfiguration before use.
"""
import os
from pathlib import Path
import re
import secrets
import socket
import struct
import time


def main():
    assert Path('/etc/omarchy-mobile-oneplus13-root').read_text().strip() == 'oneplus13-cph2655-v1'
    path = Path('/var/lib/omarchy-mobile/oneplus13/bluetooth-test-address')
    if not path.exists():
        address = bytes([2]) + secrets.token_bytes(5)
        with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as f:
            f.write(':'.join(f'{b:02X}' for b in address) + '\n')
    address = path.read_text().strip()
    if not re.fullmatch(r'02(?::[0-9A-F]{2}){5}', address):
        raise ValueError('Invalid private Bluetooth test address')
    wire_address = bytes.fromhex(address.replace(':', ''))[::-1]
    with socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI) as sock:
        sock.bind((0xffff, 3))  # HCI_DEV_NONE, HCI_CHANNEL_CONTROL

        def command(opcode, index=0xffff, payload=b''):
            sock.send(struct.pack('<HHH', opcode, index, len(payload)) + payload)
            deadline = time.monotonic() + 10
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f'MGMT command {opcode:#x}')
                sock.settimeout(remaining)
                packet = sock.recv(65535)
                if len(packet) < 9:
                    continue
                event, device, length = struct.unpack_from('<HHH', packet)
                if len(packet) != length + 6 or device != index or event not in (1, 2):
                    continue
                completed, status = struct.unpack_from('<HB', packet, 6)
                if completed != opcode:
                    continue
                if status:
                    raise RuntimeError(f'MGMT command {opcode:#x} failed: {status:#x}')
                return packet[9:]

        def has_index(opcode):
            data = command(opcode)
            if len(data) < 2:
                raise ValueError('Truncated MGMT index list')
            count, = struct.unpack_from('<H', data)
            if len(data) != 2 + 2 * count:
                raise ValueError('Invalid MGMT index list')
            return 0 in struct.unpack_from('<' + 'H' * count, data, 2)

        for _ in range(60):
            if has_index(0x36):  # Read Unconfigured Index List
                break
            time.sleep(0.5)
        else:
            raise TimeoutError('Bluetooth did not reach identity configuration')
        command(0x39, 0, wire_address)  # Set Public Address (volatile controller state)
        for _ in range(60):
            if has_index(0x03):  # Read Index List; reconfiguration has finished
                if command(0x04, 0)[:6] != wire_address:
                    raise RuntimeError('Bluetooth address readback mismatch')
                print('Bluetooth private test identity configured and verified')
                return
            time.sleep(0.5)
        raise TimeoutError('Bluetooth did not finish identity configuration')


if __name__ == '__main__':
    main()
