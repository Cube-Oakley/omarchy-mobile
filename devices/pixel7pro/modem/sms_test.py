# SPDX-License-Identifier: MIT
"""One fixed, explicitly authorized SMS test; never retries automatically.

Destination lives in a private file on the phone, never source or output.
SIT layout: stock BuildSendSms 0x80790, response adapter 0x68830/0x68860.
GSM SMS-SUBMIT: ordinary text, no UDH, no special PID, no delivery report.
"""
import json
import os
from pathlib import Path
import re
import stat
import struct
from data_call import private_write
from sit import ModemError

PRIVATE = Path('/root/.local/state/modem-lab')
DESTINATION = PRIVATE / 'test-recipient.json'
ATTEMPT = PRIVATE / 'sms-test-attempt.json'
TEXT = 'Pixel modem SMS test.'


def pack_text(text):
    # This test intentionally uses only the ASCII-compatible GSM alphabet.
    if not re.fullmatch(r'[A-Za-z0-9 .!?-]{1,160}', text):
        raise ValueError('text outside the fixed GSM test alphabet or length')
    bits = sum(ord(char) << (7 * index) for index, char in enumerate(text))
    return bits.to_bytes((7 * len(text) + 7) // 8, 'little')


def submit_pdu(destination, text=TEXT):
    if not re.fullmatch(r'\+1[2-9][0-9]{9}', destination):
        raise ValueError('expected an approved full US number')
    digits = destination[1:]
    padded = digits + ('F' if len(digits) % 2 else '')
    address = bytes(int(padded[i+1] + padded[i], 16) for i in range(0, len(padded), 2))
    return bytes([0x01, 0, len(digits), 0x91]) + address + bytes([0, 0, len(text)]) + pack_text(text)


def approved_destination():
    fd = os.open(DESTINATION, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    with os.fdopen(fd) as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077 or info.st_uid != os.geteuid():
            raise RuntimeError('test destination file must be owned and private')
        destination = json.load(stream)['destination']
    # Validate without printing it or embedding it in an exception.
    submit_pdu(destination)
    return destination


def send_test_sms(client, *, ims=False):
    if not getattr(client, 'allow_test_sms', False):
        raise RuntimeError('listener was not explicitly enabled for test SMS')
    destination = approved_destination()
    if ims:
        from ims import require_sms_registration
        require_sms_registration()
    packet_state = client.request(0x0701)
    voice_state = client.request(0x0700)
    if not any(state and state[0] in (1, 5) for state in (packet_state, voice_state)):
        raise RuntimeError('network is not registered for ordinary service')
    sim = client.request(0x0200)
    if len(sim) < 3 or sim[0] != 1 or not 1 <= sim[2] <= 8 or len(sim) < 3 + 63 * sim[2]:
        raise RuntimeError('physical-slot SIM is not ready')
    if not any(sim[3+63*i:5+63*i] == b'\x02\x05' for i in range(sim[2])):
        raise RuntimeError('USIM application is not ready')
    pdu = submit_pdu(destination)
    if ims:
        # Matching RilReqSendSms: SMSC length + 12 bytes, TPDU length +
        # 244 bytes, ordinary SMS type=0, no emergency geolocation.
        smsc = client.request(0x0108)
        if len(smsc) != 13 or not 1 <= smsc[0] <= 12:
            raise RuntimeError('unexpected modem SMSC layout')
        payload = bytearray(260)
        payload[:13] = smsc
        payload[13] = len(pdu)
        payload[14:14+len(pdu)] = pdu
    else:
        payload = bytearray(259)
        payload[0] = 0  # stock vendor.radio.smsdomain default
        payload[1] = 1  # one zero octet: use modem/SIM default SMSC
        payload[14] = len(pdu)
        payload[15:15+len(pdu)] = pdu
    attempt = ATTEMPT.with_name('ims-sms-test-attempt.json') if ims else ATTEMPT
    PRIVATE.mkdir(mode=0o700, parents=True, exist_ok=True)
    PRIVATE.chmod(0o700)
    # O_EXCL is the duplicate-send guard, including after an uncertain timeout.
    fd = os.open(attempt, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(b'{"outcome":"pending or unknown"}\n')
        stream.flush()
        os.fsync(stream.fileno())
    directory = os.open(PRIVATE, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    try:
        result = client.request(0x0d1b if ims else 0x0100, bytes(payload), timeout=90)
    except ModemError as error:
        private_write(attempt, json.dumps({'outcome': 'modem rejected', 'header_error': error.error}).encode())
        raise
    if len(result) < 253:
        raise RuntimeError('unexpected SMS response; outcome unknown, do not retry')
    cause = struct.unpack_from('<I', result, 249)[0]
    private_write(attempt, json.dumps({'outcome': 'response received', 'cause': cause}).encode())
    reference = struct.unpack_from('<I', result)[0] if ims else result[0]
    print('IMS SMS' if ims else 'SMS', 'submission response: reference', reference, 'cause', cause, flush=True)
    if cause:
        raise RuntimeError('modem reported SMS failure')
    print('modem accepted the test SMS; recipient confirmation still needed', flush=True)
