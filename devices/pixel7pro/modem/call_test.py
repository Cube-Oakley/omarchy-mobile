# SPDX-License-Identifier: MIT
"""Bounded ordinary IMS call signaling test to the approved private recipient.

Run manually with the listener stopped. --audio opens temporary AoC routes.
If call release cannot be confirmed, stop the supervised CP before RFS.
"""
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import signal
import struct
import time
from data_call import private_write
from ims import REGISTRATION, record_registration
from sit import Sit, ModemError
from sms_test import PRIVATE, approved_destination, submit_pdu

ATTEMPT = PRIVATE / 'call-test-attempt.json'


def dial_payload(destination):
    submit_pdu(destination)  # same approved full-US-number validation
    number = destination.encode('ascii')
    # Matching RilReqDial: voice=1, default CLIR, fixed 513-byte number;
    # no CUG, TTY, emergency, USSD, video, pulled call or geolocation.
    payload = bytearray(679)
    struct.pack_into('<HBH', payload, 0, 1, 0, len(number))
    payload[5:5+len(number)] = number
    return bytes(payload)


def call_status(packet):
    if len(packet) < 19 or struct.unpack_from('<H', packet, 2)[0] != 0x0d04:
        raise ValueError('short or incorrect IMS call indication')
    call_type, call_id, state, multiparty, users = struct.unpack_from('<HBBBB', packet, 8)
    if state > 8 or multiparty or users > 1:
        raise ValueError('unsupported IMS call status')
    # Connected-party numbers and SDP remain private and are not returned.
    return call_type, call_id, state, packet[15]


def approved_ring(packet, destination):
    """Return only the call ID when an ordinary ring matches the private number."""
    if len(packet) < 13 or struct.unpack_from('<H', packet, 2)[0] != 0x0d01:
        raise ValueError('short or incorrect IMS ring indication')
    kind, call_id, length = struct.unpack_from('<HBH', packet, 8)
    if not 1 <= length <= 513 or len(packet) < 13 + length:
        raise ValueError('invalid IMS caller length')
    number = packet[13:13+length].rstrip(b'\0')
    # Accept common national/international caller presentations, never prefixes
    # or substrings. Caller identification is a test filter, not authentication.
    expected = destination.encode('ascii')
    matches = number in (expected, expected[1:], expected[2:])
    return call_id if kind == 1 and matches else None


def reserve_attempt(attempt):
    fd = os.open(attempt, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(b'{"outcome":"pending or unknown"}\n')
        stream.flush()
        os.fsync(stream.fileno())
    directory = os.open(PRIVATE, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def stop_supervised_cp():
    pid = int(Path('/run/modem-lab/runtime.pid').read_text())
    command = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
    if b'/root/modem-lab-tools/runtime.py' not in command:
        raise RuntimeError('cannot identify the owned supervisor')
    os.kill(pid, signal.SIGTERM)
    fd = os.open('/dev/umts_boot0', os.O_RDWR | os.O_CLOEXEC)
    try:
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if fcntl.ioctl(fd, 0x6f27, 0) == 0:
                print('CP OFFLINE after unconfirmed call release', flush=True)
                return
            time.sleep(.25)
        raise RuntimeError('CP shutdown not confirmed; RFS must remain alive')
    finally:
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--audio', action='store_true', help='test AoC voice audio for up to45 seconds after answer')
    parser.add_argument('--incoming', action='store_true', help='wait up to10 minutes for one approved caller, then answer with audio')
    parser.add_argument('--mic-boost-db', type=int, choices=(0, 6), default=None,
                        help='microphone boost above stock (audio default: 6 dB); 0 restores stock gain')
    args = parser.parse_args()
    if args.incoming:
        args.audio = True
    if args.mic_boost_db is not None and not args.audio:
        parser.error('--mic-boost-db requires --audio or --incoming')
    if args.mic_boost_db is None:
        args.mic_boost_db = 6 if args.audio else 0
    os.umask(0o077)
    registration = json.loads(REGISTRATION.read_text())
    if registration.get('state') != 2 or not registration.get('features', 0) & 1:
        raise RuntimeError('ordinary IMS voice registration required')
    destination = approved_destination()
    client = Sit(allow_test_call=True)
    submitted = False
    released = False
    call_id = None
    stopping = False
    audio = None
    answered_at = None
    incoming_at = None
    attempt = ATTEMPT.with_name('voice-call-test-attempt.json') if args.audio else ATTEMPT
    if args.incoming:
        attempt = ATTEMPT.with_name('incoming-call-test-attempt.json')
    if args.mic_boost_db:
        attempt = attempt.with_name(attempt.stem + '-mic-boost.json')
    def stop(signum, frame):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    spec = importlib.util.spec_from_file_location('monitor', Path(__file__).with_name('monitor-sit.py'))
    monitor = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(monitor)
    def event(packet):
        nonlocal call_id, released, stopping, answered_at, submitted, audio, incoming_at
        ident = struct.unpack_from('<H', packet, 2)[0]
        if ident == 0x0d01 and args.incoming and call_id is None:
            candidate = approved_ring(packet, destination)
            if candidate is None:
                print('Incoming call does not match approved voice test; left unanswered', flush=True)
                return
            reserve_attempt(attempt)
            call_id = candidate
            incoming_at = time.monotonic()
            submitted = True  # own cleanup even if audio preparation/answer fails
            from voice_audio import VoiceAudio
            audio = VoiceAudio(mic_boost_db=args.mic_boost_db)
            audio.__enter__()
            # Matching stock answer: ordinary voice, audio send/receive, no video.
            client.request(0x0d02, struct.pack('<HHH', 1, 1, 0), timeout=8)
            print('Approved incoming voice call answered', flush=True)
        elif ident == 0x0d04:
            kind, ident_call, state, cause = call_status(packet)
            print('IMS call type', kind, 'state', state, 'cause', cause, flush=True)
            if not args.incoming and kind == 1 and state == 2 and call_id is None:
                call_id = ident_call
            if call_id is not None and ident_call == call_id:
                if state == 6:
                    released = True
                elif state == 0:
                    if args.audio:
                        if answered_at is None:
                            answered_at = time.monotonic()
                            print('Voice test connected; 45-second conversation window', flush=True)
                    else:
                        stopping = True
        elif ident in (0x010c, 0x010d, 0x0d1e, 0x0d20):
            reference = monitor.persist_sms(packet)
            client.request(0x0d1d if ident >= 0x0d00 else 0x0102,
                           struct.pack('<IBI', 1, reference, 0))
            print('Incoming SMS durably stored and acknowledged', flush=True)
        elif ident == 0x0d05:
            record_registration(packet)
    try:
        calls = client.request(0x0000)
        if len(calls) != 4 or calls != bytes(4):
            raise RuntimeError('call test requires no existing calls')
        if attempt.exists():
            raise RuntimeError('call attempt already recorded; inspect before another test')
        if args.audio and not args.incoming:
            from voice_audio import VoiceAudio
            audio = VoiceAudio(mic_boost_db=args.mic_boost_db)
            audio.__enter__()
        if not args.incoming:
            reserve_attempt(attempt)
            submitted = True
            try:
                client.request(0x0d00, dial_payload(destination), timeout=8)
            except ModemError as error:
                submitted = False
                private_write(attempt, json.dumps({'outcome': 'modem rejected', 'error': error.error}).encode())
                raise
        else:
            print('Ready for one incoming call from the approved test phone; ten-minute window', flush=True)
        deadline = time.monotonic() + (600 if args.incoming else 75 if args.audio else 20)
        while not released and not stopping and time.monotonic() < deadline:
            if incoming_at is not None and time.monotonic() >= incoming_at + 75:
                break
            if answered_at is not None and time.monotonic() >= answered_at + 45:
                break
            packet = client.next_indication(.5)
            if packet:
                event(packet)
    finally:
        try:
            if submitted and not released:
                # A response timeout can still leave a queued dialing event.
                deadline = time.monotonic() + 1
                while time.monotonic() < deadline:
                    packet = client.next_indication(.1)
                    if packet:
                        event(packet)
                if call_id is not None and not released:
                    client.request(0x0d03, bytes([call_id, 1, 0]), timeout=8)
                    deadline = time.monotonic() + 8
                    while not released and time.monotonic() < deadline:
                        packet = client.next_indication(.5)
                        if packet:
                            event(packet)
        finally:
            try:
                client.close()
                if submitted:
                    if not released:
                        stop_supervised_cp()
                    private_write(attempt, json.dumps({'outcome': 'released' if released else 'CP stopped'}).encode())
                    print('Call test ended; release confirmed', released, flush=True)
            finally:
                if audio is not None:
                    audio.__exit__(None, None, None)


if __name__ == '__main__':
    main()
