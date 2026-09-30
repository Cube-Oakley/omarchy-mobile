# SPDX-License-Identifier: MIT
"""Fixed Tello data-call layouts and an owned bearer lifecycle."""
import json
import os
from pathlib import Path
import struct

STATE = Path('/run/modem-lab/data-session.json')
RESPONSE = Path('/run/modem-lab/data-call-response.bin')


def paths(cid):
    if cid == 1:
        return STATE, RESPONSE
    if cid == 2:
        return STATE.with_name('ims-session.json'), RESPONSE.with_name('ims-response.bin')
    raise ValueError('unsupported bearer CID')


def call_ids(payload):
    # Stock DataCallUtil::DetermineDCTypeBySize (0x55da0). Reject variable
    # traffic-descriptor records until their full framing is implemented.
    if not payload or payload[0] > 15:
        raise RuntimeError('invalid data-call list')
    count = payload[0]
    if count == 0:
        if len(payload) != 1:
            raise RuntimeError('unexpected empty data-call list')
        return set()
    size, remainder = divmod(len(payload) - 1, count)
    if remainder or size not in (132, 234, 239, 246, 280, 292):
        raise RuntimeError('unsupported data-call list layout')
    cids = [payload[1 + index * size + 2] for index in range(count)]
    if len(set(cids)) != count or any(not 1 <= cid <= 15 for cid in cids):
        raise RuntimeError('invalid or duplicate data-call CID')
    return set(cids)


def owned_ids():
    result = set()
    for cid in (1, 2):
        state, _ = paths(cid)
        if state.exists():
            owned = json.loads(state.read_text())
            if owned.get('cid') != cid or not 1 <= owned.get('session', 0) <= 15:
                raise RuntimeError('invalid bearer ownership record')
            result.add(cid)
    return result


def private_write(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_CLOEXEC, 0o600)
    with os.fdopen(fd, 'wb') as out:
        out.write(data)


def setup_payload(cid, radio_technology, pdu_session, apn):
    # Absolute request offsets below include the 12-byte RCM header.
    full = bytearray(983)
    full[12] = cid
    full[13] = radio_technology
    full[14] = 0  # stock converter 0x7b1e0: only tethering maps to 1
    full[15] = 1 if apn == 'ims' else 0  # IMS/default APN type (0x7b030)
    encoded = apn.encode('ascii')
    if apn not in {'wholesale', 'fast.t-mobile.com', 'ims'}:
        raise ValueError('APN outside verified Tello settings')
    full[16:16 + len(encoded)] = encoded
    full[0xd9] = 0  # no authentication (0x7b1d0)
    full[0xda] = 2 if apn == 'ims' else 3  # IPV6/IPV4V6 (0x7b130)
    full[0xdb] = 2 if apn == 'ims' else 0  # IPv6 P-CSCF request (0x7b290)
    # No handover addresses, roaming override or slice.
    # MTU at 0xf1 stays zero (network/default choice).
    full[0xf5] = 2  # stock caller selects v3/983-byte form
    struct.pack_into('<i', full, 0xf6, pdu_session)
    # No requested network slice: SST 0, SD -1, mapped SST 0, mapped SD -1.
    struct.pack_into('<i', full, 0xfc, -1)
    struct.pack_into('<i', full, 0x101, -1)
    full[0x105] = 1  # match-all rule allowed, no traffic descriptor supplied
    return bytes(full[12:])


def initial_attach(client, apn):
    if apn == 'ims':
        raise ValueError('IMS is not the initial attach profile')
    attach = bytearray(251)
    attach[12:245] = setup_payload(1, 14, 1, apn)[:233]
    attach[0xf8] = 3
    client.request(0x0603, bytes(attach[12:]), timeout=30)


def allow_data(client):
    previous = client.request(0x0711)
    print('PS service permission before', previous[0] if len(previous) == 1 else 'unexpected', flush=True)
    client.request(0x0625, b'\x01\x00')
    client.request(0x0710, b'\x01')
    if client.request(0x0711) != b'\x01':
        raise RuntimeError('PS service permission is not enabled')
    print('PS service permission enabled', flush=True)


def provision_default_profile(client):
    # Stock BuildSetDataProfile (0x7a720), 246-byte request. This configures
    # the network APN profile, not a SIM subscription or UICC operation.
    full = bytearray(246)
    struct.pack_into('<I', full, 12, 0)  # Android default profile ID
    full[16:25] = b'wholesale'
    full[0x75] = full[0x76] = 3  # home/roaming IPV4V6
    full[0xe9] = 1  # enabled
    struct.pack_into('<I', full, 0xea, 1)  # default APN-type bitmap
    client.request(0x0613, bytes(full[12:]), timeout=30)
    print('default Tello data profile accepted', flush=True)


def activate(client, apn='wholesale', *, lte_access=False):
    requested_cid = 2 if apn == 'ims' else 1
    state_path, response_path = paths(requested_cid)
    state = client.request(0x0701)
    if len(state) < 4 or state[0] not in (1, 5):
        raise RuntimeError('PS network is not registered')
    calls = call_ids(client.request(0x0602))
    owned = owned_ids()
    if state_path.exists() or requested_cid in calls or calls != owned:
        raise RuntimeError('existing target or unresolved bearer ownership')
    allow_data(client)
    allocated = client.request(0x061a)
    if len(allocated) != 4:
        raise RuntimeError('unexpected PDU-session response')
    session = struct.unpack('<i', allocated)[0]
    if not 1 <= session <= 15:
        raise RuntimeError('invalid PDU session')
    if any(json.loads(paths(cid)[0].read_text())['session'] == session for cid in owned):
        # Never release a duplicate allocation belonging to the other bearer.
        raise RuntimeError('modem reused an owned PDU session')
    # Persist ownership before setup. A timeout has an unknown outcome; do
    # not recycle its PDU session while the modem may still be activating it.
    try:
        private_write(state_path, json.dumps({'cid': requested_cid, 'session': session}).encode())
    except OSError:
        client.request(0x061b, struct.pack('<i', session))
        raise
    released = False
    try:
        rat = 14 if lte_access else state[3]
        print('data setup access RAT', rat, 'registered RAT', state[3], flush=True)
        result = client.request(0x0600, setup_payload(requested_cid, rat, session, apn), timeout=45)
        if len(result) < 132:
            raise RuntimeError('unexpected data-call layout')
        status, cid, raw_active, pdp = struct.unpack_from('<HBBB', result)
        print('data-call status', status, 'CID', cid, 'active raw', raw_active, 'PDP type', pdp, flush=True)
        if cid == requested_cid and raw_active == 0:
            client.request(0x061b, struct.pack('<i', session))
            released = True
        if status or cid != requested_cid or raw_active not in (1, 2):
            raise RuntimeError('data call did not become active')
        private_write(response_path, result)
        print('owned data bearer active; private IP configuration retained in RAM', flush=True)
        if apn == 'ims':
            # Base response parser at 0x52a04: family then 3 v4 and 3 v6
            # P-CSCF addresses. Report presence only, never their values.
            print('IMS IPv6 present', any(result[9:25]),
                  'P-CSCF family', result[0x42],
                  'base IPv6 P-CSCF count', sum(bool(any(result[i:i+16])) for i in (0x4f, 0x5f, 0x6f)), flush=True)
    finally:
        if released:
            state_path.unlink(missing_ok=True)
            response_path.unlink(missing_ok=True)


def deactivate(client, cid=1):
    state_path, response_path = paths(cid)
    if cid == 2 and state_path.with_name('ims-registration.json').exists():
        raise RuntimeError('unregister IMS before releasing its bearer')
    owned = json.loads(state_path.read_text())
    if owned.get('cid') != cid or not 1 <= owned.get('session', 0) <= 15:
        raise RuntimeError('invalid bearer ownership record')
    flags = int(Path(f"/sys/class/net/rmnet{owned['cid'] - 1}/flags").read_text(), 16)
    if flags & 1:
        raise RuntimeError('remove host data configuration first')
    # Stock BuildDeactDataCall (0x7a8d0): CID and normal-release reason.
    client.request(0x0601, bytes([owned['cid'], 0]), timeout=30)
    calls = call_ids(client.request(0x0602))
    if cid in calls:
        raise RuntimeError('deactivated bearer still listed; retaining reservation')
    client.request(0x061b, struct.pack('<i', owned['session']))
    state_path.unlink()
    response_path.unlink(missing_ok=True)
    if calls != owned_ids():
        raise RuntimeError('unexpected remaining data-call ownership')
    print('owned data bearer deactivated; PDU session released', flush=True)
