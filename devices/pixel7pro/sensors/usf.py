#!/usr/bin/env python3
"""Minimal, dependency-free host client for Google's USF (Unified Sensor
Framework) as spoken by the Pixel 7 Pro (GS201) AoC over
/dev/acd-com.google.usf.

Everything here was recovered by static analysis of the stock vendor
libraries (libusf.so, sensors.usf.so); see USF-PROTOCOL.md next to this file
for the derivation and the confidence of each claim.

Wire format in one paragraph: every message on the AoC channel is one bare
FlatBuffer (no length/magic/crc header) whose root table is `UsfMsg`
{0: u32 type, 1: [ubyte] data}.  Requests (type 1) and responses (type 2)
carry a nested FlatBuffer "envelope" {0: u32 msg id, 1: u32 request id,
2: u32 server handle (request) / UsfErr status (response), 3: [ubyte] payload},
and the payload is again a nested FlatBuffer specific to the message id.
Sensor samples arrive as UsfMsg type 9 whose data is a packed binary
"compact sample batch", not a FlatBuffer.

This module only ever sends: GetServer, Echo, SensorList, SensorInfo,
StartSampling and StopSampling requests (plus, only when explicitly asked for
with --check-registry / --get-time, the read-only RegistryGet and GetTime
requests).  It never sends registry writes, I2C/SPI/DMA, register writes,
injection, self-test or production-script requests.
"""

import argparse
import errno
import os
import select
import struct
import sys
import time

# ---------------------------------------------------------------------------
# Protocol constants
# ---------------------------------------------------------------------------

DEFAULT_DEVICE = '/dev/acd-com.google.usf'
NWU_SUFFIX = '.non_wake_up'
MAX_MSG_SIZE = 1012          # EfwTransportClient max message size (0x3f4)
READ_SIZE = 4096             # libusf reads with a 1024-byte buffer

# UsfMsg.type (outer message type, usf::UsfMsgType)
MSG_REQ = 1
MSG_RESP = 2
MSG_FRAG = 3
MSG_BATCH = 4
MSG_EVENT = 5
MSG_TEST = 7                 # transport test traffic (UsfTestTransport*)
MSG_COMPACT = 9              # raw (non-FlatBuffer) sample batch / event
MSG_TYPE_NAMES = {MSG_REQ: 'REQ', MSG_RESP: 'RESP', MSG_FRAG: 'FRAG', MSG_BATCH: 'BATCH',
                  MSG_EVENT: 'EVENT', MSG_TEST: 'TEST', MSG_COMPACT: 'COMPACT'}

# Envelope slot 0: request/response message id (responses echo the request id)
REQ_ECHO = 1
REQ_STAT_GET_ROWS = 2
REQ_GET_SERVER = 3
REQ_SENSOR_INFO = 4
REQ_SENSOR_LIST = 5
REQ_SPI_TRANSFER = 6
REQ_I2C_TRANSFER = 7
REQ_START_SAMPLING = 8
REQ_STOP_SAMPLING = 9
REQ_RECONFIG_SAMPLING = 10
REQ_SYNC_TIME = 11
REQ_FLUSH_SAMPLES = 14
REQ_REG_SAMPLE_CHANNEL = 15
REQ_UNREG_SAMPLE_CHANNEL = 16
REQ_SET_SAMPLE_TRANSFORM = 17
REQ_CLR_SAMPLE_TRANSFORM = 18
REQ_SELF_TEST = 19
REQ_USB_CHARGING_CURRENT = 20
REQ_REGISTRY_GET = 21
REQ_REGISTRY_SET = 22
REQ_REGISTRY_REMOVE = 23
REQ_REGISTRY_LOAD_SCRIPT = 24
REQ_GET_TIME = 25
REQ_REGISTRY_PROD_SCRIPT = 27
REQ_GET_SAMPLE_TRANSFORM = 28
REQ_STAT_LIST = 29
REQ_STAT_RESET = 30
REQ_DMA_WRITE = 31
REQ_SUEZ = 32
REQ_SHMEM_TRANSPORT_INIT = 33
REQ_SHMEM_TRANSPORT_DEINIT = 34
REQ_DEBUG_GET_BUFFER = 35
REQ_REGISTRY_PROD_SCRIPT_NODE = 40
REQ_CONTEXT_EVENT = 300
REQ_START_DATA_INJECTION = 800
REQ_STOP_DATA_INJECTION = 801
REQ_DATA_INJECTION = 802
REQ_DATA_INJECTION_STATUS = 803
REQ_DATA_INJECTION_REPEATED = 804
REQ_DISPLAY_INFO = 1001
REQ_GET_SENSOR_LIST_STATUS = 1007

# The only request ids this tool is allowed to put on the wire.
ALLOWED_REQUESTS = {REQ_GET_SERVER, REQ_ECHO, REQ_SENSOR_LIST, REQ_SENSOR_INFO,
                    REQ_START_SAMPLING, REQ_STOP_SAMPLING,
                    REQ_REGISTRY_GET, REQ_GET_TIME,   # read-only, opt-in
                    # The registry load Android's sensor HAL does at every AoC
                    # boot: into the AoC's RAM only (opt-in, --load-registry).
                    REQ_REGISTRY_LOAD_SCRIPT, REQ_REGISTRY_SET}

SERVER_MGR_HANDLE = 1        # fixed handle of the server-manager server

# Server UUIDs (16 raw bytes, sent as GetServerReq.uuid)
UUID_SENSOR_MGR = bytes.fromhex('6b31cbf0744a14a1d4e8ae8a3e09fc4d')
UUID_TIME = bytes.fromhex('8bb81b18254c299f0a45deb4d8cbc541')
UUID_REGISTRY = bytes.fromhex('d76a14cc264deea1c6406f8b461682c0')
UUID_STAT = bytes.fromhex('cf79f8eaee434740535220b6b973febf')

# usf::UsfSensorType (subset; full table in USF-PROTOCOL.md)
SENSOR_TYPE_NAMES = {
    1: 'accelerometer', 2: 'gyroscope', 3: 'proximity', 4: 'pressure',
    5: 'pressure temperature', 6: 'magnetometer', 7: 'magnetometer temperature',
    8: 'IMU temperature', 9: 'ambient light', 11: 'camera vsync', 12: 'spectral',
    13: 'flicker', 14: 'binned brightness', 15: 'camera lift', 16: 'device orientation',
    17: 'device pickup', 18: 'double twist', 19: 'front camera light',
    20: 'game rotation vector', 21: 'geomagnetic rotation vector', 22: 'gravity',
    23: 'linear acceleration', 24: 'orientation', 25: 'proximity gated single tap',
    26: 'proximity gated double tap', 27: 'rotation vector', 28: 'significant motion',
    29: 'step detector', 30: 'step counter', 31: 'tilt detector', 32: 'motion detect',
    33: 'stationary detect', 34: 'touch gesture', 37: 'ppg', 38: 'ppg controller',
    39: 'ecg', 40: 'ecg lead detector', 41: 'heart rate', 42: 'heart beat',
    43: 'wrist tilt gesture', 45: 'hall effect', 46: 'rear light', 48: 'auto brightness',
    50: 'low latency off body detect', 51: 'proximity gated long press', 52: 'quick pickup',
    53: 'hinge angle', 56: 'fir temperature', 58: 'step cadence',
    59: 'galvanic skin response', 63: 'smart band', 67: 'fir extended range temperature',
    68: 'proximity for BTS', 69: 'proximity for voice-calls',
    73: 'misaligned charger detection', 80: 'one handed gestures',
    82: 'mag accessory detector',
}
SENSOR_ACCEL = 1
SENSOR_GYRO = 2
SENSOR_PROX = 3
SENSOR_PRESSURE = 4
SENSOR_MAG = 6
SENSOR_LIGHT = 9

# StartSamplingReq.rpt_mode (UsfSensorReportingMode)
RPT_CONTINUOUS = 1
RPT_ON_CHANGE = 2
RPT_ONE_SHOT = 3

# UsfErr values observed in the binaries (meanings inferred from call sites)
USF_ERR_NAMES = {0: 'OK', 2: 'FAILED', 3: 'SIZE/OVERFLOW', 5: 'INVALID/MALFORMED',
                 6: 'NO_MEMORY', 7: 'NOT_FOUND', 8: 'UNSUPPORTED', 11: 'NOT_CONNECTED',
                 12: 'NOT_READY', 14: 'IO_ERROR', 17: 'BUSY/EAGAIN', 19: 'ALREADY_ACTIVE',
                 20: 'OUT_OF_RANGE'}

# UsfSampleAccuracy -> meaning (via Sensor::ConvertUsfAccuracyToAndroid)
ACCURACY_NAMES = {0: 'default', 1: 'unreliable', 2: 'low', 3: 'medium', 4: 'high'}


class UsfError(Exception):
    pass


# ---------------------------------------------------------------------------
# Minimal FlatBuffers builder (back-to-front, like the reference builder)
# ---------------------------------------------------------------------------

class Builder:
    """Just enough of flatbuffers::FlatBufferBuilder for the USF tables.

    Offsets returned by the create_*/end_table methods are measured from the
    end of the buffer (like the reference implementation)."""

    def __init__(self):
        self.data = bytearray()     # finished tail of the buffer
        self.minalign = 1
        self.fields = None          # slot -> offset while a table is open
        self.table_start = 0

    def _size(self):
        return len(self.data)

    def _pad(self, n):
        if n:
            self.data[0:0] = b'\0' * n

    def prep(self, size, additional):
        if size > self.minalign:
            self.minalign = size
        pad = (-(len(self.data) + additional)) % size
        self._pad(pad)

    def _prepend(self, fmt, value):
        self.data[0:0] = struct.pack('<' + fmt, value)

    # -- scalars -----------------------------------------------------------
    _FMT = {'u8': ('B', 1), 'i8': ('b', 1), 'u16': ('H', 2), 'u32': ('I', 4), 'i32': ('i', 4),
            'u64': ('Q', 8), 'i64': ('q', 8), 'f32': ('f', 4), 'bool': ('B', 1)}

    def add_scalar(self, slot, kind, value):
        """Always writes the field (force_defaults semantics)."""
        if self.fields is None:
            raise ValueError('no open table')
        fmt, size = self._FMT[kind]
        if kind == 'bool':
            value = 1 if value else 0
        self.prep(size, 0)
        self._prepend(fmt, value)
        self.fields[slot] = self._size()

    def add_offset(self, slot, off):
        if self.fields is None:
            raise ValueError('no open table')
        self.prep(4, 0)
        rel = self._size() + 4 - off
        self._prepend('I', rel)
        self.fields[slot] = self._size()

    # -- vectors -----------------------------------------------------------
    def create_bytes(self, data, align=8):
        """[ubyte]/[byte] vector.  Nested FlatBuffers are placed 8-aligned."""
        data = bytes(data)
        self.prep(4, len(data))
        self.prep(align, len(data))
        self.data[0:0] = data
        self.prep(4, 0)
        self._prepend('I', len(data))
        return self._size()

    def create_u32_vector(self, values):
        n = len(values)
        self.prep(4, 4 * n)
        self.data[0:0] = struct.pack('<%dI' % n, *values)
        self._prepend('I', n)
        return self._size()

    def create_offset_vector(self, offsets):
        n = len(offsets)
        self.prep(4, 4 * n)
        for off in reversed(offsets):
            self.prep(4, 0)
            self._prepend('I', self._size() + 4 - off)
        self._prepend('I', n)
        return self._size()

    # -- tables ------------------------------------------------------------
    def start_table(self):
        if self.fields is not None:
            raise ValueError('nested table')
        self.fields = {}
        self.table_start = self._size()

    def end_table(self):
        self.prep(4, 0)
        self._prepend('i', 0)                  # soffset placeholder
        obj = self._size()
        nslots = (max(self.fields) + 1) if self.fields else 0
        vt = [0] * nslots
        for slot, off in self.fields.items():
            vt[slot] = obj - off
        vt_bytes = struct.pack('<HH', 4 + 2 * nslots, obj - self.table_start) + \
            struct.pack('<%dH' % nslots, *vt)
        self.data[0:0] = vt_bytes              # vtable directly before table
        vt_off = self._size()
        pos = len(self.data) - obj             # absolute index of the table now
        struct.pack_into('<i', self.data, pos, vt_off - obj)
        self.fields = None
        return obj

    def finish(self, root):
        self.prep(self.minalign, 4)
        self.prep(4, 0)
        self._prepend('I', self._size() + 4 - root)
        return bytes(self.data)


def build_table(fields):
    """fields: list of (slot, kind, value).  kind in scalar kinds or
    'bytes' (nested/raw byte vector), 'u32vec', 'tables' (list of field-lists).
    Returns the finished FlatBuffer bytes."""
    b = Builder()
    root = _emit_table(b, fields)
    return b.finish(root)


def _emit_table(b, fields):
    # Children (vectors / sub tables) must be serialized before the table.
    offs = {}
    for slot, kind, value in fields:
        if value is None:
            continue
        if kind == 'bytes':
            offs[slot] = b.create_bytes(value)
        elif kind == 'u32vec':
            offs[slot] = b.create_u32_vector(list(value))
        elif kind == 'tables':
            subs = [_emit_table(b, sub) for sub in value]
            offs[slot] = b.create_offset_vector(subs)
    b.start_table()
    # Largest scalars first gives the most compact packing (like flatc).
    order = sorted((f for f in fields if f[2] is not None),
                   key=lambda f: -Builder._FMT.get(f[1], ('', 4))[1])
    for slot, kind, value in order:
        if slot in offs:
            b.add_offset(slot, offs[slot])
        else:
            b.add_scalar(slot, kind, value)
    return b.end_table()


# ---------------------------------------------------------------------------
# Minimal FlatBuffers reader with bounds checks
# ---------------------------------------------------------------------------

class Table:
    def __init__(self, buf, pos):
        self.buf = buf
        self.pos = pos
        n = len(buf)
        if pos < 0 or pos + 4 > n or pos % 4:
            raise UsfError('bad table offset %d (len %d)' % (pos, n))
        vt = pos - struct.unpack_from('<i', buf, pos)[0]
        if vt < 0 or vt + 4 > n or vt % 2:
            raise UsfError('bad vtable offset %d' % vt)
        self.vt = vt
        self.vt_size, self.obj_size = struct.unpack_from('<HH', buf, vt)
        if self.vt_size < 4 or vt + self.vt_size > n or self.vt_size % 2:
            raise UsfError('bad vtable size %d' % self.vt_size)

    @classmethod
    def root(cls, buf):
        buf = bytes(buf)
        if len(buf) < 8:
            raise UsfError('buffer too short (%d)' % len(buf))
        off = struct.unpack_from('<I', buf, 0)[0]
        if off < 1 or off > len(buf) - 4:
            raise UsfError('bad root offset %d' % off)
        return cls(buf, off)

    def _field(self, slot):
        vo = 4 + 2 * slot
        if vo >= self.vt_size:
            return 0
        return struct.unpack_from('<H', self.buf, self.vt + vo)[0]

    def has(self, slot):
        return self._field(slot) != 0

    def scalar(self, slot, kind, default=0):
        fo = self._field(slot)
        if not fo:
            return default
        fmt, size = Builder._FMT[kind]
        p = self.pos + fo
        if p + size > len(self.buf):
            raise UsfError('scalar field %d out of range' % slot)
        v = struct.unpack_from('<' + fmt, self.buf, p)[0]
        return bool(v) if kind == 'bool' else v

    def _vector(self, slot):
        fo = self._field(slot)
        if not fo:
            return None
        p = self.pos + fo
        if p + 4 > len(self.buf):
            raise UsfError('offset field %d out of range' % slot)
        vec = p + struct.unpack_from('<I', self.buf, p)[0]
        if vec + 4 > len(self.buf):
            raise UsfError('vector %d out of range' % slot)
        n = struct.unpack_from('<I', self.buf, vec)[0]
        return vec + 4, n

    def bytes(self, slot):
        v = self._vector(slot)
        if v is None:
            return None
        start, n = v
        if start + n > len(self.buf):
            raise UsfError('byte vector %d overruns buffer' % slot)
        return self.buf[start:start + n]

    def string(self, slot):
        b = self.bytes(slot)
        if b is None:
            return None
        return b.split(b'\0', 1)[0].decode('utf-8', 'replace')

    def u32_vector(self, slot):
        v = self._vector(slot)
        if v is None:
            return []
        start, n = v
        if start + 4 * n > len(self.buf):
            raise UsfError('u32 vector %d overruns buffer' % slot)
        return list(struct.unpack_from('<%dI' % n, self.buf, start))

    def tables(self, slot):
        v = self._vector(slot)
        if v is None:
            return []
        start, n = v
        out = []
        for i in range(n):
            p = start + 4 * i
            out.append(Table(self.buf, p + struct.unpack_from('<I', self.buf, p)[0]))
        return out


# ---------------------------------------------------------------------------
# Outer message, envelope, fragments, batches, events
# ---------------------------------------------------------------------------

def encode_msg(msg_type, data):
    """UsfMsg {0: u32 type, 1: [ubyte] data}"""
    return build_table([(0, 'u32', msg_type), (1, 'bytes', data)])


def decode_msg(buf):
    t = Table.root(buf)
    return t.scalar(0, 'u32'), t.bytes(1)


def encode_envelope(msg_id, req_id, handle_or_status, payload=None):
    """Request/response envelope nested in UsfMsg.data for types 1 and 2."""
    return build_table([(0, 'u32', msg_id), (1, 'u32', req_id),
                        (2, 'u32', handle_or_status), (3, 'bytes', payload)])


def decode_envelope(data):
    t = Table.root(data)
    return {'id': t.scalar(0, 'u32'), 'req_id': t.scalar(1, 'u32'),
            'handle_or_status': t.scalar(2, 'u32'), 'payload': t.bytes(3)}


def encode_request(msg_id, req_id, server_handle, payload=None):
    if msg_id not in ALLOWED_REQUESTS:
        raise UsfError('refusing to encode request id %d (not in the allowed set)' % msg_id)
    if req_id == 0:
        raise UsfError('request id must be non-zero')
    return encode_msg(MSG_REQ, encode_envelope(msg_id, req_id, server_handle, payload))


def encode_response(msg_id, req_id, status, payload=None):
    """Only used by the self-tests / simulator (the AoC builds responses)."""
    return encode_msg(MSG_RESP, encode_envelope(msg_id, req_id, status, payload))


def encode_frag(total_size, offset, chunk, first_field=1):
    """UsfMsgFrag {0: u32 (always 1 in SendMsgFrag), 1: u32 total, 2: u32 offset, 3: [ubyte]}"""
    return encode_msg(MSG_FRAG, build_table([(0, 'u32', first_field), (1, 'u32', total_size),
                                             (2, 'u32', offset), (3, 'bytes', chunk)]))


def fragment(msg, max_size=MAX_MSG_SIZE, chunk=None):
    """Split an encoded UsfMsg into FRAG messages (self-test helper; the
    host never needs to send messages larger than MAX_MSG_SIZE)."""
    if chunk is None:
        chunk = max_size - 96   # 72 bytes of FlatBuffer overhead + alignment
    out = []
    for off in range(0, len(msg), chunk):
        out.append(encode_frag(len(msg), off, msg[off:off + chunk]))
    return out


def decode_frag(data):
    t = Table.root(data)
    return {'field0': t.scalar(0, 'u32'), 'total': t.scalar(1, 'u32'),
            'offset': t.scalar(2, 'u32'), 'chunk': t.bytes(3) or b''}


class Reassembler:
    """Mirror of usf::UsfMsgReassembler (in-order fragments only)."""

    def __init__(self):
        self.buf = None
        self.total = 0

    def add(self, frag):
        if frag['offset'] == 0:
            self.buf = bytearray()
            self.total = frag['total']
        if self.buf is None:
            raise UsfError('fragment at offset %d without a start fragment' % frag['offset'])
        if frag['offset'] != len(self.buf):
            expected, self.buf = len(self.buf), None
            raise UsfError('out-of-order fragment (expected %d, got %d)' % (expected, frag['offset']))
        if len(self.buf) + len(frag['chunk']) > self.total:
            self.buf = None
            raise UsfError('fragment overruns message')
        self.buf += frag['chunk']
        if len(self.buf) == self.total:
            msg, self.buf = bytes(self.buf), None
            return msg
        return None


def encode_batch(msgs):
    """UsfMsgBatch {0: [UsfMsgBatchEntry {0: [ubyte] msg}]} (self-test helper)"""
    return encode_msg(MSG_BATCH, build_table([(0, 'tables', [[(0, 'bytes', m)] for m in msgs])]))


def decode_batch(data):
    return [e.bytes(0) or b'' for e in Table.root(data).tables(0)]


def decode_event(data):
    """UsfMsgEvent {0: u32 event type, 1: [ubyte] nested payload}"""
    t = Table.root(data)
    return t.scalar(0, 'u32'), t.bytes(1)


# ---------------------------------------------------------------------------
# Request payload encoders
# ---------------------------------------------------------------------------

def encode_get_server_req(uuid):
    if len(uuid) != 16:
        raise ValueError('UUID must be 16 bytes')
    return build_table([(0, 'bytes', uuid)])


def encode_echo_req(data):
    return build_table([(0, 'bytes', data)])


def encode_start_sampling_req(period_ns, max_latency_ns=0, rpt_mode=RPT_CONTINUOUS,
                              client_id=0x4C494E58, non_wakeup=False, transform_level=3,
                              android_timestamps=False, include_bias=False,
                              reconfig_sampling_id=0, channel_handle=0):
    """UsfMsgStartSamplingReq with the same defaults as UsfApiImpl::StartSampling."""
    return build_table([
        (0, 'u32', rpt_mode),
        (1, 'u64', period_ns),
        (2, 'u64', max_latency_ns),
        (3, 'bool', False),              # src (sample-rate conversion)
        (4, 'bool', False),              # passive
        (5, 'bool', non_wakeup),         # deliver on the non_wake_up channel
        (6, 'u32', channel_handle),      # 0 = send samples as messages on this transport
        (7, 'u32', 0),                   # unknown, 0 in all callers
        (8, 'u32', 0),                   # unknown, 0 in all callers
        (9, 'u32', transform_level),     # 3 = default/fully transformed
        (10, 'u64', client_id),          # echoed (low 32 bits) in every sample batch
        (11, 'bool', android_timestamps),  # convert timestamps to Android boottime
        (12, 'bool', False),             # lp_memory
        (13, 'bool', include_bias),
        (14, 'bool', False),             # skip min-delay clamp
        (15, 'u32', 1),                  # unknown, 1 in all callers
        (16, 'u32', reconfig_sampling_id),  # non-zero: reconfigure existing sampling id
    ])


def encode_stop_sampling_req(sampling_id):
    return build_table([(0, 'u32', sampling_id)])


def encode_registry_get_req(path):
    return build_table([(0, 'bytes', path.encode() + b'\0')])


def encode_registry_load_script_req(text, cdt, mlb=0):
    """UsfMsgRegistryLoadScriptReq {0: [byte] script (C string), 1: u32 CDT, 2: u32 MLB}"""
    return build_table([(0, 'bytes', text + b'\0'), (1, 'u32', cdt), (2, 'u32', mlb)])


def encode_registry_set_req(path, props):
    """UsfMsgRegistrySetReq {0: [byte] node path, 1: [Prop {0: [byte] name, 1: [byte] value}]}"""
    return build_table([(0, 'bytes', path.encode() + b'\0'),
                        (1, 'tables', [[(0, 'bytes', k.encode() + b'\0'), (1, 'bytes', v.encode() + b'\0')]
                                       for k, v in props])])


# ---------------------------------------------------------------------------
# Response payload decoders (and encoders for the self-tests / simulator)
# ---------------------------------------------------------------------------

def decode_get_server_resp(p):
    return Table.root(p).scalar(0, 'u32')


def encode_get_server_resp(handle):
    return build_table([(0, 'u32', handle)])


def decode_echo_resp(p):
    return Table.root(p).bytes(0) or b''


def encode_echo_resp(data):
    return build_table([(0, 'bytes', data)])


def decode_sensor_list_resp(p):
    return Table.root(p).u32_vector(0)


def encode_sensor_list_resp(handles):
    return build_table([(0, 'u32vec', handles)])


SENSOR_INFO_FIELDS = [
    # slot, kind, name
    (0, 'str', 'name'),
    (1, 'u32', 'type'),
    (2, 'f32', 'max_range'),
    (3, 'u64', 'min_period_ns'),
    (4, 'u32', 'fifo_reserved'),
    (5, 'u32', 'fifo_max'),
    (6, 'u64', 'max_period_ns'),
    (7, 'str', 'vendor'),
    (8, 'bool', 'flag8'),
    (9, 'f32', 'resolution'),
    (10, 'str', 'registry_path'),
    (11, 'u32', 'index'),
    (12, 'u64', 'raw_min_period_ns'),
    (13, 'bool', 'flag13'),
]


def decode_sensor_info_resp(p):
    t = Table.root(p)
    out = {}
    for slot, kind, name in SENSOR_INFO_FIELDS:
        out[name] = t.string(slot) if kind == 'str' else t.scalar(slot, kind)
    return out


def encode_sensor_info_resp(info):
    fields = []
    for slot, kind, name in SENSOR_INFO_FIELDS:
        v = info.get(name)
        if v is None:
            continue
        if kind == 'str':
            # USF C strings are [byte] vectors whose last byte is NUL
            # (UsfFlatBuffersGetCString, libusf 0x72974)
            fields.append((slot, 'bytes', v.encode() + b'\0'))
        else:
            fields.append((slot, kind, v))
    return build_table(fields)


def decode_start_sampling_resp(p):
    return Table.root(p).scalar(0, 'u32')


def encode_start_sampling_resp(sampling_id):
    return build_table([(0, 'u32', sampling_id)])


def decode_get_time_resp(p):
    return Table.root(p).scalar(0, 'u64')


def decode_registry_get_resp(p):
    t = Table.root(p)
    props = [(e.string(0), e.string(1)) for e in t.tables(0)]
    return props


def decode_start_sampling_req(p):
    """Used by the self-tests and the simulator."""
    t = Table.root(p)
    return {'rpt_mode': t.scalar(0, 'u32'), 'period_ns': t.scalar(1, 'u64'),
            'max_latency_ns': t.scalar(2, 'u64'), 'non_wakeup': t.scalar(5, 'bool'),
            'channel_handle': t.scalar(6, 'u32'), 'transform_level': t.scalar(9, 'u32', 3),
            'client_id': t.scalar(10, 'u64'), 'android_timestamps': t.scalar(11, 'bool'),
            'include_bias': t.scalar(13, 'bool'), 'field15': t.scalar(15, 'u32'),
            'reconfig_sampling_id': t.scalar(16, 'u32')}


# ---------------------------------------------------------------------------
# Compact sample batches (UsfMsg type 9 data)
# ---------------------------------------------------------------------------

TS_MASK = (1 << 60) - 1


def decode_compact_batch(data):
    """Decode UsfCompactSampleBatchHeader (format 1) / UsfCompactLargeSampleBatchHeader
    (format 2) batches.  Returns a dict with a 'samples' list of
    (timestamp_ns, accuracy, [floats])."""
    if len(data) < 16:
        raise UsfError('compact batch too short (%d)' % len(data))
    fmt, client_id, sampling_id = struct.unpack_from('<III', data, 0)
    if fmt == 1:
        w = struct.unpack_from('<I', data, 12)[0]
        stype = w & 0xffff
        count = (w >> 16) & 0x3ff
        nvals = (w >> 26) & 0x3f
        base = 16
    elif fmt == 2:
        if len(data) < 17:
            raise UsfError('large compact batch too short')
        w = struct.unpack_from('<I', data, 12)[0] | (data[16] << 32)
        stype = w & 0xffff
        count = (w >> 16) & 0x3ff
        nvals = (w >> 26) & 0x3ff
        base = 17
    else:
        raise UsfError('unknown compact batch format %d' % fmt)
    stride = 8 + 4 * nvals
    if base + count * stride > len(data):
        raise UsfError('compact batch truncated: need %d bytes, have %d' % (base + count * stride, len(data)))
    samples = []
    for i in range(count):
        p = base + i * stride
        ts = struct.unpack_from('<Q', data, p)[0]
        vals = list(struct.unpack_from('<%df' % nvals, data, p + 8))
        samples.append((ts & TS_MASK, ts >> 60, vals))
    return {'format': fmt, 'client_id': client_id, 'sampling_id': sampling_id,
            'sensor_type': stype, 'count': count, 'nvals': nvals, 'samples': samples}


def encode_compact_batch(client_id, sampling_id, sensor_type, samples, large=False):
    """Self-test/simulator helper mirroring UsfMsgSampleEventChannel::SendCompact*."""
    nvals = len(samples[0][2]) if samples else 0
    count = len(samples)
    if large:
        w = (sensor_type & 0xffff) | ((count & 0x3ff) << 16) | ((nvals & 0x3ff) << 26)
        hdr = struct.pack('<IIII', 2, client_id, sampling_id, w & 0xffffffff) + bytes([w >> 32])
    else:
        w = (sensor_type & 0xffff) | ((count & 0x3ff) << 16) | ((nvals & 0x3f) << 26)
        hdr = struct.pack('<IIII', 1, client_id, sampling_id, w)
    body = b''.join(struct.pack('<Q', (ts & TS_MASK) | ((acc & 0xf) << 60)) +
                    struct.pack('<%df' % nvals, *vals) for ts, acc, vals in samples)
    return encode_msg(MSG_COMPACT, hdr + body)


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------

def hexdump(b, width=16):
    return '\n'.join('  %04x  %s' % (i, b[i:i + width].hex(' ')) for i in range(0, len(b), width))


class ChannelTransport:
    """Plain read()/write() of whole messages on the AoC channel device(s)."""

    def __init__(self, device, nwu_device=None, verbose=False):
        self.verbose = verbose
        self.fd = os.open(device, os.O_RDWR | os.O_NONBLOCK)
        self.fds = [self.fd]
        self.nwu_fd = None
        if nwu_device and os.path.exists(nwu_device):
            # opened like libusf does; only ever read from
            self.nwu_fd = os.open(nwu_device, os.O_RDWR | os.O_NONBLOCK)
            self.fds.append(self.nwu_fd)

    def send(self, msg):
        if len(msg) > MAX_MSG_SIZE:
            # As UsfTransport::SendMsg does: FRAG messages, in order.
            for frag in fragment(msg):
                self.send(frag)
            return
        if self.verbose:
            print('>> %d bytes\n%s' % (len(msg), hexdump(msg)))
        deadline = time.monotonic() + 1.0
        while True:
            try:
                n = os.write(self.fd, msg)
            except OSError as e:
                if e.errno == errno.EAGAIN and time.monotonic() < deadline:
                    time.sleep(0.005)
                    continue
                raise
            # aoc_channel_dev counts the 4-byte channel index it prepends.
            if n not in (len(msg), len(msg) + 4):
                raise UsfError('short write %d/%d' % (n, len(msg)))
            return

    def recv(self, timeout):
        """Return a list of (raw message bytes, is_nwu) read within timeout."""
        r, _, _ = select.select(self.fds, [], [], max(0.0, timeout))
        out = []
        for fd in r:
            while True:
                try:
                    b = os.read(fd, READ_SIZE)
                except OSError as e:
                    if e.errno == errno.EAGAIN:
                        break
                    raise
                if not b:
                    break
                if self.verbose:
                    print('<< %d bytes%s\n%s' % (len(b), ' (nwu)' if fd == self.nwu_fd else '', hexdump(b)))
                out.append((b, fd == self.nwu_fd))
        return out

    def close(self):
        for fd in self.fds:
            os.close(fd)


class DryRunTransport:
    """Prints what would be sent; answers with a simulated AoC so that the
    whole flow can be exercised offline."""

    def __init__(self, simulate=True, quiet=False):
        self.sim = FakeAoc() if simulate else None
        self.pending = []
        self.quiet = quiet
        self.sent = []

    def send(self, msg):
        if len(msg) > MAX_MSG_SIZE:
            raise UsfError('message too large')
        self.sent.append(msg)
        if not self.quiet:
            t, d = decode_msg(msg)
            env = decode_envelope(d)
            print('would send %d bytes: UsfMsg(type=%s) envelope(id=%d, req_id=%d, handle=%d, payload=%d bytes)'
                  % (len(msg), MSG_TYPE_NAMES.get(t, t), env['id'], env['req_id'], env['handle_or_status'],
                     len(env['payload'] or b'')))
            print(hexdump(msg))
        if self.sim:
            self.pending.extend(self.sim.handle(msg))

    def recv(self, timeout):
        if self.pending:
            out, self.pending = [(m, False) for m in self.pending], []
            return out
        if self.sim:
            more = self.sim.tick()
            if more:
                return [(m, False) for m in more]
        time.sleep(min(timeout, 0.01))
        return []

    def close(self):
        pass


class FakeAoc:
    """A tiny AoC stand-in built only from this module's encoders."""

    def __init__(self):
        self.handles = {UUID_SENSOR_MGR: 2, UUID_TIME: 3, UUID_REGISTRY: 4}
        self.sensors = {10: {'name': 'LSM6DSV Accelerometer', 'type': 1, 'vendor': 'STMicro',
                             'max_range': 78.4532, 'resolution': 0.0023928226, 'min_period_ns': 2404000,
                             'max_period_ns': 1000000000, 'fifo_reserved': 3000, 'fifo_max': 3000,
                             'index': 0, 'registry_path': '/dev/lsm6dsv/0/accel'},
                        11: {'name': 'LSM6DSV Gyroscope', 'type': 2, 'vendor': 'STMicro', 'index': 0}}
        self.active = {}
        self.next_sampling = 100
        self.t0 = time.monotonic()

    def handle(self, raw):
        t, data = decode_msg(raw)
        if t != MSG_REQ:
            return []
        env = decode_envelope(data)
        mid, rid, h, p = env['id'], env['req_id'], env['handle_or_status'], env['payload']

        def resp(status, payload=None):
            return [encode_response(mid, rid, status, payload)]

        if mid == REQ_GET_SERVER and h == SERVER_MGR_HANDLE:
            uuid = Table.root(p).bytes(0)
            if uuid in self.handles:
                return resp(0, encode_get_server_resp(self.handles[uuid]))
            return resp(7)
        if mid == REQ_ECHO:
            return resp(0, encode_echo_resp(Table.root(p).bytes(0)))
        if mid == REQ_SENSOR_LIST and h == self.handles[UUID_SENSOR_MGR]:
            return resp(0, encode_sensor_list_resp(sorted(self.sensors)))
        if mid == REQ_SENSOR_INFO and h in self.sensors:
            return resp(0, encode_sensor_info_resp(self.sensors[h]))
        if mid == REQ_START_SAMPLING and h in self.sensors:
            req = decode_start_sampling_req(p)
            sid = self.next_sampling
            self.next_sampling += 1
            self.active[sid] = [h, req, int((time.monotonic() - self.t0) * 1e9) + 10 ** 9]
            return resp(0, encode_start_sampling_resp(sid))
        if mid == REQ_REGISTRY_GET and h == self.handles[UUID_REGISTRY]:
            props = [[(0, 'bytes', b'loaded\0'), (1, 'bytes', b'1\0')]]
            return resp(0, build_table([(0, 'tables', props)]))
        if mid == REQ_GET_TIME and h == self.handles[UUID_TIME]:
            now = int((time.monotonic() - self.t0) * 1e9) + 10 ** 9
            return resp(0, build_table([(0, 'u64', now)]))
        if mid == REQ_STOP_SAMPLING and h in self.sensors:
            sid = Table.root(p).scalar(0, 'u32')
            self.active.pop(sid, None)
            return resp(0 if sid else 7)
        return resp(7)

    def tick(self):
        """Emit samples in (simulated) real time, as the AoC would."""
        out = []
        now = int((time.monotonic() - self.t0) * 1e9) + 10 ** 9
        for sid, st in self.active.items():
            h, req, last = st
            period = max(req['period_ns'], 1000000)
            samples = []
            while last + period <= now and len(samples) < 16:
                last += period
                samples.append((last, 4, [0.01, -0.02, 9.81]))
            st[2] = last
            if samples:
                out.append(encode_compact_batch(req['client_id'] & 0xffffffff, sid,
                                                self.sensors[h]['type'], samples))
        return out


# ---------------------------------------------------------------------------
# Client
# ---------------------------------------------------------------------------

class UsfClient:
    def __init__(self, transport, verbose=False):
        self.tr = transport
        self.verbose = verbose
        self.next_req_id = 1
        self.reasm = Reassembler()
        self.events = []          # decoded non-response messages

    def _alloc_req_id(self):
        rid = self.next_req_id
        self.next_req_id = (self.next_req_id + 1) & 0xffffffff or 1
        return rid

    def _dispatch(self, raw, nwu, out):
        """Decode one raw channel message; append ('resp', env) / ('compact', dict) /
        ('event', ...) / ('req', env) tuples to out."""
        t, data = decode_msg(raw)
        if t == MSG_RESP:
            out.append(('resp', decode_envelope(data)))
        elif t == MSG_COMPACT:
            out.append(('compact', decode_compact_batch(data)))
        elif t == MSG_FRAG:
            full = self.reasm.add(decode_frag(data))
            if full is not None:
                self._dispatch(full, nwu, out)
        elif t == MSG_BATCH:
            for m in decode_batch(data):
                self._dispatch(m, nwu, out)
        elif t == MSG_EVENT:
            et, payload = decode_event(data)
            out.append(('event', et, payload))
        elif t == MSG_REQ:
            out.append(('req', decode_envelope(data)))
        else:
            out.append(('unknown', t, data))

    def poll(self, timeout):
        out = []
        for raw, nwu in self.tr.recv(timeout):
            try:
                self._dispatch(raw, nwu, out)
            except UsfError as e:
                print('warning: undecodable message (%s): %s' % (e, raw[:64].hex()), file=sys.stderr)
        return out

    def request(self, msg_id, handle, payload=None, timeout=5.0):
        rid = self._alloc_req_id()
        self.tr.send(encode_request(msg_id, rid, handle, payload))
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise UsfError('timeout waiting for response to request id %d (msg %d)' % (rid, msg_id))
            for item in self.poll(left):
                if item[0] == 'resp' and item[1]['req_id'] == rid:
                    env = item[1]
                    if env['id'] not in (0, msg_id):
                        print('note: response id %d for request %d' % (env['id'], msg_id), file=sys.stderr)
                    return env['handle_or_status'], env['payload']
                if item[0] == 'req':
                    print('note: ignoring request from AoC: %r' % (item[1],), file=sys.stderr)
                else:
                    self.events.append(item)

    def checked(self, msg_id, handle, payload=None, timeout=5.0):
        status, p = self.request(msg_id, handle, payload, timeout)
        if status != 0:
            raise UsfError('request %d to handle %d failed: UsfErr %d (%s)'
                           % (msg_id, handle, status, USF_ERR_NAMES.get(status, '?')))
        return p

    # -- high level --------------------------------------------------------
    def get_server(self, uuid):
        return decode_get_server_resp(self.checked(REQ_GET_SERVER, SERVER_MGR_HANDLE,
                                                   encode_get_server_req(uuid)))

    def echo(self, data, handle=SERVER_MGR_HANDLE):
        return decode_echo_resp(self.checked(REQ_ECHO, handle, encode_echo_req(data)))

    def sensor_list(self, mgr_handle):
        return decode_sensor_list_resp(self.checked(REQ_SENSOR_LIST, mgr_handle))

    def sensor_info(self, handle):
        return decode_sensor_info_resp(self.checked(REQ_SENSOR_INFO, handle))

    def start_sampling(self, handle, period_ns, max_latency_ns=0, **kw):
        return decode_start_sampling_resp(self.checked(
            REQ_START_SAMPLING, handle, encode_start_sampling_req(period_ns, max_latency_ns, **kw)))

    def stop_sampling(self, handle, sampling_id):
        self.checked(REQ_STOP_SAMPLING, handle, encode_stop_sampling_req(sampling_id))

    def get_time(self, time_handle):
        return decode_get_time_resp(self.checked(REQ_GET_TIME, time_handle))

    def registry_get(self, reg_handle, path):
        return decode_registry_get_resp(self.checked(REQ_REGISTRY_GET, reg_handle,
                                                     encode_registry_get_req(path)))

    def registry_load_script(self, reg_handle, text, cdt, mlb=0):
        self.checked(REQ_REGISTRY_LOAD_SCRIPT, reg_handle,
                     encode_registry_load_script_req(text, cdt, mlb), timeout=10.0)

    def registry_set(self, reg_handle, path, props):
        self.checked(REQ_REGISTRY_SET, reg_handle, encode_registry_set_req(path, props))


# ---------------------------------------------------------------------------
# Self tests
# ---------------------------------------------------------------------------

def verify_flatbuffer(buf):
    """Structural checks similar to flatbuffers::Verifier (alignment, bounds)."""
    t = Table.root(buf)
    if t.pos % 4:
        raise UsfError('root table misaligned')
    if len(buf) % 4:
        raise UsfError('buffer length not 4-aligned')
    return t


def self_test(verbose=False):
    ok = 0

    def check(cond, what):
        nonlocal ok
        if not cond:
            raise AssertionError(what)
        ok += 1

    # envelope + outer round trip for every allowed request
    payloads = {
        REQ_GET_SERVER: encode_get_server_req(UUID_SENSOR_MGR),
        REQ_ECHO: encode_echo_req(b'hello usf'),
        REQ_SENSOR_LIST: None,
        REQ_SENSOR_INFO: None,
        REQ_START_SAMPLING: encode_start_sampling_req(20000000),
        REQ_STOP_SAMPLING: encode_stop_sampling_req(7),
        REQ_GET_TIME: None,
        REQ_REGISTRY_GET: encode_registry_get_req('/'),
    }
    for mid, p in payloads.items():
        msg = encode_request(mid, 0x1234 + mid, 42, p)
        verify_flatbuffer(msg)
        check(len(msg) <= MAX_MSG_SIZE, 'size')
        t, d = decode_msg(msg)
        check(t == MSG_REQ, 'outer type')
        verify_flatbuffer(d)
        env = decode_envelope(d)
        check(env['id'] == mid and env['req_id'] == 0x1234 + mid and env['handle_or_status'] == 42,
              'envelope fields %d' % mid)
        check(env['payload'] == p, 'payload bytes %d' % mid)
        if p is not None:
            verify_flatbuffer(p)
    try:
        encode_request(REQ_REGISTRY_PROD_SCRIPT, 1, 4, b'')
        check(False, 'registry prod script must be refused')
    except UsfError:
        ok += 1

    # payload round trips
    check(Table.root(encode_get_server_req(UUID_TIME)).bytes(0) == UUID_TIME, 'uuid')
    check(decode_echo_resp(encode_echo_resp(b'\x00\x01xyz')) == b'\x00\x01xyz', 'echo')
    check(decode_get_server_resp(encode_get_server_resp(17)) == 17, 'get server')
    check(decode_sensor_list_resp(encode_sensor_list_resp([5, 6, 70000])) == [5, 6, 70000], 'list')
    info = {'name': 'LSM6DSV Accelerometer', 'type': 1, 'max_range': 78.5, 'min_period_ns': 2404000,
            'fifo_reserved': 3000, 'fifo_max': 3000, 'max_period_ns': 10 ** 9, 'vendor': 'STM',
            'flag8': True, 'resolution': 0.5, 'registry_path': '/dev/lsm6dsv/0/accel', 'index': 2,
            'raw_min_period_ns': 1250000, 'flag13': False}
    back = decode_sensor_info_resp(encode_sensor_info_resp(info))
    check(back == info, 'sensor info %r' % back)
    ss = decode_start_sampling_req(encode_start_sampling_req(20000000, 5000000, client_id=99,
                                                             non_wakeup=True, include_bias=True))
    check(ss['period_ns'] == 20000000 and ss['max_latency_ns'] == 5000000 and ss['client_id'] == 99
          and ss['non_wakeup'] and ss['include_bias'] and ss['rpt_mode'] == 1
          and ss['transform_level'] == 3 and ss['field15'] == 1, 'start sampling %r' % ss)
    check(decode_start_sampling_resp(encode_start_sampling_resp(123)) == 123, 'start resp')
    check(Table.root(encode_stop_sampling_req(123)).scalar(0, 'u32') == 123, 'stop req')

    # u64 field alignment inside the start-sampling table, both relative to
    # the payload and absolute within the complete channel message (the AoC
    # side is 32-bit ARM, where 64-bit loads want natural alignment)
    p = encode_start_sampling_req(0x1122334455667788, 0x0102030405060708, client_id=0xabcdef)
    t = Table.root(p)
    m = encode_request(REQ_START_SAMPLING, 77, 12, p)
    base = m.find(p)
    check(base % 8 == 0 and len(m) % 8 == 0, 'payload placement')
    for slot in (1, 2, 10):
        check((t.pos + t._field(slot)) % 8 == 0, 'u64 alignment slot %d' % slot)
        check((base + t.pos + t._field(slot)) % 8 == 0, 'absolute u64 alignment slot %d' % slot)

    # response path through the client dispatcher, incl. fragments and batch
    samples = [(123456789 + i * 20000000, 4, [0.1 * i, -0.2, 9.8]) for i in range(5)]
    compact = encode_compact_batch(0x4C494E58, 100, 1, samples)
    dec = decode_compact_batch(decode_msg(compact)[1])
    check(dec['count'] == 5 and dec['nvals'] == 3 and dec['sensor_type'] == 1 and dec['sampling_id'] == 100
          and dec['client_id'] == 0x4C494E58, 'compact header %r' % dec)
    check(all(a[0] == b[0] and a[1] == b[1] and all(abs(x - y) < 1e-6 for x, y in zip(a[2], b[2]))
              for a, b in zip(samples, dec['samples'])), 'compact samples')
    large = decode_compact_batch(decode_msg(encode_compact_batch(1, 2, 12, [(5, 1, [1.0] * 70)], large=True))[1])
    check(large['format'] == 2 and large['nvals'] == 70 and large['samples'][0][2] == [1.0] * 70, 'large')

    big = encode_response(REQ_SENSOR_INFO, 9, 0, encode_echo_resp(bytes(range(256)) * 12))
    frags = fragment(big)
    check(len(frags) > 1 and all(len(f) <= MAX_MSG_SIZE for f in frags), 'fragment sizes')
    batch = encode_batch([encode_response(REQ_ECHO, 3, 0, encode_echo_resp(b'x')), compact])

    class ListTransport:
        def __init__(self, msgs):
            self.msgs = msgs

        def recv(self, timeout):
            m, self.msgs = self.msgs, []
            return [(x, False) for x in m]

    c = UsfClient(ListTransport(frags + [batch]))
    items = c.poll(0)
    check(items[0][0] == 'resp' and items[0][1]['req_id'] == 9, 'reassembled response')
    check(decode_echo_resp(items[0][1]['payload']) == bytes(range(256)) * 12, 'reassembled payload')
    check(items[1][0] == 'resp' and items[1][1]['req_id'] == 3, 'batch resp')
    check(items[2][0] == 'compact' and items[2][1]['count'] == 5, 'batch compact')

    # full main flow against the simulated AoC
    tr = DryRunTransport(simulate=True, quiet=True)
    rc = run(tr, rate_hz=50, duration=0.05, sensor_type=SENSOR_ACCEL, index=0, out=open(os.devnull, 'w'))
    check(rc == 0, 'simulated run')
    sent_ids = [decode_envelope(decode_msg(m)[1])['id'] for m in tr.sent]
    check(sent_ids[:4] == [REQ_GET_SERVER, REQ_ECHO, REQ_SENSOR_LIST, REQ_SENSOR_INFO], 'order %r' % sent_ids)
    check(sent_ids[-2:] == [REQ_START_SAMPLING, REQ_STOP_SAMPLING], 'order tail %r' % sent_ids)
    check(set(sent_ids) <= ALLOWED_REQUESTS, 'only allowed requests')
    check(REQ_REGISTRY_GET not in sent_ids and REQ_GET_TIME not in sent_ids, 'opt-in reads off by default')
    tr = DryRunTransport(simulate=True, quiet=True)
    rc = run(tr, rate_hz=100, duration=0.03, check_registry=True, get_time=True, out=open(os.devnull, 'w'))
    sent_ids = [decode_envelope(decode_msg(m)[1])['id'] for m in tr.sent]
    check(rc == 0 and REQ_REGISTRY_GET in sent_ids and REQ_GET_TIME in sent_ids, 'opt-in reads')
    check(decode_registry_get_resp(build_table([(0, 'tables', [[(0, 'bytes', b'loaded\0'), (1, 'bytes', b'1\0')]])]))
          == [('loaded', '1')], 'registry get resp')
    print('self-test: %d checks passed' % ok)
    return 0


# ---------------------------------------------------------------------------
# Main flow
# ---------------------------------------------------------------------------

CDT_PATH = '/sys/firmware/devicetree/base/chosen/plat'


def device_cdt():
    """The board's CDT, 0xPPPPSJIV, as libusf reads it (UsfRegistryStoreFile::
    GetDevCdtInfo): big-endian u32 cells product, stage, major, minor, variant."""
    cells = []
    for name in ('product', 'stage', 'major', 'minor', 'variant'):
        with open(os.path.join(CDT_PATH, name), 'rb') as f:
            cells.append(int.from_bytes(f.read(4), 'big'))
    product, stage, major, minor, variant = cells
    return (product << 16) | (stage << 12) | (major << 8) | (minor << 4) | variant


def load_registry(c, dirs, cdt, out=sys.stdout):
    """What Android's sensor HAL does once per AoC boot (SensorHal::LoadRegistry):
    every *.reg script of each directory, in order, as RegistryLoadScript, then
    "/" loaded=1. It writes only the AoC's registry, in its RAM."""
    reg = c.get_server(UUID_REGISTRY)
    for d in dirs:
        for name in sorted(os.listdir(d)):
            path = os.path.join(d, name)
            if not name.endswith('.reg') or not os.path.isfile(path):
                continue
            with open(path, 'rb') as f:
                text = f.read()
            c.registry_load_script(reg, text, cdt)
            print('loaded %s (%d bytes)' % (path, len(text)), file=out)
    c.registry_set(reg, '/', [('loaded', '1')])
    print('registry "/" loaded=1 (CDT %#x)' % cdt, file=out)


def run(tr, rate_hz=50.0, duration=3.0, sensor_type=SENSOR_ACCEL, index=0, check_registry=False,
        get_time=False, out=sys.stdout, client_id=0x4C494E58, registry_dirs=None):
    c = UsfClient(tr)
    # 1. "handshake": there is no transport-level hello; the first request is
    #    a GetServer for the sensor manager on the fixed server-manager handle 1.
    mgr = c.get_server(UUID_SENSOR_MGR)
    print('sensor manager handle: %d' % mgr, file=out)
    # 2. echo round trip against the server manager
    pong = c.echo(b'omarchy-usf-ping')
    print('echo: %r' % pong, file=out)
    if registry_dirs:
        load_registry(c, registry_dirs, device_cdt(), out)
    if check_registry:
        reg = c.get_server(UUID_REGISTRY)
        props = c.registry_get(reg, '/')
        print('registry "/" properties: %r' % (props,), file=out)
    if get_time:
        th = c.get_server(UUID_TIME)
        t0 = time.clock_gettime_ns(time.CLOCK_BOOTTIME) if hasattr(time, 'CLOCK_BOOTTIME') else 0
        core = c.get_time(th)
        t1 = time.clock_gettime_ns(time.CLOCK_BOOTTIME) if hasattr(time, 'CLOCK_BOOTTIME') else 0
        print('sensor-core time %d ns, host boottime midpoint %d ns (offset %d)'
              % (core, (t0 + t1) // 2, (t0 + t1) // 2 - core), file=out)
    # 3. sensor list + info
    handles = c.sensor_list(mgr)
    print('%d sensors' % len(handles), file=out)
    infos = {}
    for h in handles:
        try:
            info = c.sensor_info(h)
        except UsfError as e:
            print('  handle %d: %s' % (h, e), file=out)
            continue
        infos[h] = info
        print('  handle %3d type %3d (%s) idx %d name %r vendor %r range %g res %g min %d ns max %d ns path %r'
              % (h, info['type'], SENSOR_TYPE_NAMES.get(info['type'], '?'), info['index'], info['name'],
                 info['vendor'], info['max_range'], info['resolution'], info['min_period_ns'],
                 info['max_period_ns'], info['registry_path']), file=out)
    cands = sorted((i['index'], h) for h, i in infos.items() if i['type'] == sensor_type)
    if not handles:
        print('empty sensor list: the AoC registry is probably not loaded '
              '(Android\'s HAL sends RegistryLoadScript + RegistrySet "/" loaded=1 at boot)', file=out)
        return 2
    if not cands:
        print('no sensor of type %d' % sensor_type, file=out)
        return 2
    pick = [h for i, h in cands if i == index] or [cands[0][1]]
    handle = pick[0]
    # 4. start sampling
    period = int(1e9 / rate_hz)
    sid = c.start_sampling(handle, period, 0, client_id=client_id)
    print('started handle %d at %.1f Hz: sampling id %d' % (handle, rate_hz, sid), file=out)
    n = 0
    end = time.monotonic() + duration
    try:
        pending, c.events = c.events, []
        while True:
            for item in pending:
                if item[0] == 'compact':
                    b = item[1]
                    if b['sampling_id'] != sid:
                        continue
                    for ts, acc, vals in b['samples']:
                        n += 1
                        print('%16d ns acc=%d %s' % (ts, acc, ' '.join('%+.4f' % v for v in vals)), file=out)
                elif item[0] == 'event':
                    print('event type %d (%d bytes)' % (item[1], len(item[2] or b'')), file=out)
            left = end - time.monotonic()
            if left <= 0:
                break
            pending = c.poll(min(left, 0.2))
    finally:
        # 5. stop sampling (always, even on Ctrl-C)
        c.stop_sampling(handle, sid)
        print('stopped sampling id %d after %d samples' % (sid, n), file=out)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description='Minimal USF host client for the Pixel 7 Pro AoC')
    ap.add_argument('device', nargs='?', default=DEFAULT_DEVICE)
    ap.add_argument('--no-nwu', action='store_true', help='do not open the .non_wake_up channel')
    ap.add_argument('--dry-run', action='store_true',
                    help='print the hex of every message instead of opening the device '
                         '(responses come from a built-in simulator)')
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--rate', type=float, default=50.0)
    ap.add_argument('--duration', type=float, default=3.0)
    ap.add_argument('--type', type=int, default=SENSOR_ACCEL, help='UsfSensorType (1=accel)')
    ap.add_argument('--index', type=int, default=0, help='sensor index among sensors of that type')
    ap.add_argument('--check-registry', action='store_true', help='read-only RegistryGet of "/"')
    ap.add_argument('--get-time', action='store_true', help='read-only GetTime from the time server')
    ap.add_argument('--load-registry', nargs='+', metavar='DIR',
                    help='load the *.reg scripts of each DIR into the AoC registry, then set "/" loaded=1')
    ap.add_argument('-v', '--verbose', action='store_true', help='hexdump all channel traffic')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test(a.verbose)
    if a.dry_run:
        tr = DryRunTransport(simulate=True)
    else:
        tr = ChannelTransport(a.device, None if a.no_nwu else a.device + NWU_SUFFIX, verbose=a.verbose)
    try:
        return run(tr, a.rate, a.duration, a.type, a.index, a.check_registry, a.get_time,
                   registry_dirs=a.load_registry)
    finally:
        tr.close()


if __name__ == '__main__':
    sys.exit(main())
