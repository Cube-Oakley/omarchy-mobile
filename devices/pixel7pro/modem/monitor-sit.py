#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Observe physical-slot registration, IMS/call events and incoming SMS.

Incoming GSM SMS PDUs are privately persisted on the phone before acknowledgment.
An explicit option enables one fixed test SMS to a privately configured approved
destination. Numbers and content never reach stdout. No outgoing call control.
"""
import argparse
from collections import Counter
import os
from pathlib import Path
import signal
import socket
import struct
import time
import uuid
from sit import Sit
from data_call import activate, deactivate, allow_data, provision_default_profile
from radio_settings import usage_setting, nr_mode
from sms_test import send_test_sms
from ims import register as register_ims, unregister as unregister_ims, record_registration

INBOX = Path('/root/.local/state/modem-lab/inbox')
CONTROL = Path('/run/modem-lab/control.sock')
PID = Path('/run/modem-lab/monitor.pid')


def persist_sms(packet):
    if len(packet) < 10 or not 1 <= packet[9] <= 244 or len(packet) < 10 + packet[9]:
        raise ValueError('invalid incoming SMS layout')
    INBOX.mkdir(parents=True, mode=0o700, exist_ok=True)
    INBOX.chmod(0o700)
    fd = os.open(INBOX / (uuid.uuid4().hex + '.sit'),
                 os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
    with os.fdopen(fd, 'wb') as out:
        out.write(packet)
        out.flush()
        os.fsync(out.fileno())
    directory = os.open(INBOX, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    return packet[8]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=900)
    parser.add_argument('--allow-test-sms', action='store_true',
                        help='enable one fixed SMS to the privately configured, user-approved destination')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 7200:
        parser.error('duration must be 1..7200 seconds')
    os.umask(0o077)
    client = Sit(allow_test_sms=args.allow_test_sms)
    CONTROL.unlink(missing_ok=True)
    control = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    control.bind(str(CONTROL))
    control.listen(2)
    control.setblocking(False)
    counts = Counter()
    stopping = False
    def stop(signum, frame):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        PID.write_text(str(os.getpid()) + '\n')
        for ident, label in ((0x091b, 'voice operation'), (0x0957, 'usage setting'),
                             (0x070b, 'preferred network mode'), (0x0750, 'allowed network bitmap'),
                             (0x0930, 'preferred call capability'), (0x0953, 'VoNR capability'),
                             (0x0108, 'SMSC'), (0x0200, 'SIM status')):
            try:
                result = client.request(ident)
                if ident == 0x0108:
                    print('SMSC supplied', bool(result and result[0]), flush=True)
                elif ident == 0x0200 and len(result) >= 3:
                    # Stock SimStatusAdapter: 3-byte prefix, 63-byte apps.
                    # Only type/state; never AIDs, labels or card identifiers.
                    count = result[2]
                    if count > 8 or len(result) < 3 + count * 63:
                        raise RuntimeError('unexpected SIM status layout')
                    print('SIM card state', result[0], 'application count', count, flush=True)
                    for index in range(count):
                        start = 3 + index * 63
                        print('SIM application type', result[start], 'state', result[start+1], flush=True)
                elif len(result) == 4:
                    print(label, struct.unpack('<I', result)[0], flush=True)
            except (RuntimeError, TimeoutError):
                print(label, 'query unavailable', flush=True)
        deadline = time.monotonic() + args.seconds
        next_status = 0
        print('physical-slot SMS/call/IMS listener active; test SMS enabled', args.allow_test_sms, flush=True)
        while not stopping and time.monotonic() < deadline:
            try:
                connection, _ = control.accept()
            except BlockingIOError:
                connection = None
            if connection is not None:
                with connection:
                    connection.settimeout(1)
                    try:
                        command = connection.recv(64).strip()
                        if command == b'connect-data':
                            activate(client)
                        elif command == b'connect-lte-access':
                            activate(client, lte_access=True)
                        elif command == b'provision-profile':
                            provision_default_profile(client)
                        elif command == b'allow-data':
                            allow_data(client)
                            next_status = 0
                        elif command == b'network-auto':
                            mode = client.request(0x0703)
                            print('network selection mode', mode[0] if mode else 'unavailable', flush=True)
                            client.request(0x0704, timeout=90)
                            next_status = 0
                        elif command == b'connect-ims':
                            activate(client, 'ims')
                        elif command == b'register-ims':
                            register_ims(client)
                        elif command == b'unregister-ims':
                            unregister_ims(client)
                        elif command in (b'data-centric', b'restore-usage'):
                            usage_setting(client, restore=command == b'restore-usage')
                            next_status = 0
                        elif command in (b'disable-nr', b'restore-nr'):
                            nr_mode(client, restore=command == b'restore-nr')
                            next_status = 0
                        elif command == b'send-test-sms':
                            send_test_sms(client)
                        elif command == b'send-test-ims-sms':
                            send_test_sms(client, ims=True)
                        elif command in (b'start-ims', b'stop-ims'):
                            # Stock StackControl/RegistrationAdaptor: physical
                            # slot 0 -> stack 1, SIM ready -> 1. No APDU payload.
                            client.request(0x0d39 if command == b'start-ims' else 0x0d3a,
                                           b'\x01\x01', timeout=30)
                        elif command == b'disconnect-data':
                            deactivate(client)
                        elif command == b'disconnect-ims':
                            deactivate(client, cid=2)
                        elif command == b'status':
                            next_status = 0
                        else:
                            raise ValueError('unsupported control command')
                        connection.sendall(b'OK\n')
                    except (ValueError, RuntimeError, TimeoutError, OSError) as error:
                        # These helpers produce status-only errors, no payloads.
                        print('control failed:', type(error).__name__, flush=True)
                        if isinstance(error, (RuntimeError, TimeoutError)):
                            print('control status:', str(error), flush=True)
                        try:
                            connection.sendall(b'ERROR: see private laboratory status log\n')
                        except OSError:
                            pass
            if time.monotonic() >= next_status:
                for ident in (0x0700, 0x0701, 0x0000):
                    try:
                        result = client.request(ident)
                        if ident == 0x0000 and len(result) >= 4:
                            print('current call count', struct.unpack_from('<I', result)[0], flush=True)
                        elif ident in (0x0700, 0x0701) and len(result) >= 4:
                            print('voice' if ident == 0x0700 else 'PS', 'registration', result[0],
                                  'reject', result[1], 'RAT', result[2 if ident == 0x0700 else 3], flush=True)
                            if ident == 0x0701 and len(result) > 0x21:
                                print('IMS VoPS flag', result[0x21], flush=True)
                    except (RuntimeError, TimeoutError):
                        print(f'status 0x{ident:04x} unavailable', flush=True)
                next_status = time.monotonic() + 30
            packet = client.next_indication(timeout=0.5)
            if packet is None:
                continue
            ident = struct.unpack_from('<H', packet, 2)[0]
            counts[ident] += 1
            if ident in (0x010c, 0x010d, 0x0d1e, 0x0d20):
                reference = persist_sms(packet)
                # Stock BuildSmsAck(int success, int reference, int cause),
                # 0x809b0; SmsService DoSmsAck supplies the received reference.
                client.request(0x0d1d if ident in (0x0d1e, 0x0d20) else 0x0102,
                               struct.pack('<IBI', 1, reference, 0))
                print('SMS privately stored and modem acknowledgment accepted', flush=True)
            elif ident == 0x0d05 and len(packet) >= 9:
                print('IMS registration indication raw', packet[8], flush=True)
                if len(packet) >= 20:
                    # Matching RilIndRegistration: omit private URI/reason.
                    _, features, mask, status = record_registration(packet)
                    print('IMS feature', features, 'feature mask', mask,
                          'status code', status, flush=True)
            elif ident in (0x0009, 0x000a, 0x0010, 0x0d01, 0x0d04, 0x0d1e):
                print(f'call/IMS indication 0x{ident:04x}, length {len(packet)}', flush=True)
            elif 0x0d00 <= ident < 0x0e00:
                print(f'IMS indication 0x{ident:04x}, length {len(packet)}', flush=True)
        print('indication counts', ' '.join(f'{k:04x}={v}' for k, v in sorted(counts.items())), flush=True)
    finally:
        control.close()
        CONTROL.unlink(missing_ok=True)
        PID.unlink(missing_ok=True)
        client.close()


if __name__ == '__main__':
    main()
