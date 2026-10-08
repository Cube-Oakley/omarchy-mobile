#!/usr/bin/env python3
"""Chromium memory-infra dumps over DevTools (test only): which allocator grows.

memdump.py PORT SECONDS   two detailed dumps SECONDS apart; prints each
                          process's allocators with their growth, largest first
"""
import base64, json, os, socket, struct, sys, time, urllib.request

port, gap = int(sys.argv[1]), float(sys.argv[2])
ws = json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/json/version'))['webSocketDebuggerUrl']
path = ws.split(f':{port}', 1)[1]
s = socket.create_connection(('127.0.0.1', port))
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((f'GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
           f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
buf = b''
while b'\r\n\r\n' not in buf:
    buf += s.recv(4096)
buf = buf.split(b'\r\n\r\n', 1)[1]
ids = iter(range(1, 1 << 30))


def send(method, params=None):
    n = next(ids)
    data = json.dumps({'id': n, 'method': method, 'params': params or {}}).encode()
    mask = os.urandom(4)
    if len(data) < 126:
        head = bytes([0x81, 0x80 | len(data)])
    elif len(data) < 65536:
        head = bytes([0x81, 0x80 | 126]) + struct.pack('>H', len(data))
    else:
        head = bytes([0x81, 0x80 | 127]) + struct.pack('>Q', len(data))
    s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
    return n


def frame():
    """The next text frame's JSON (no fragmentation from DevTools)."""
    global buf
    while True:
        if len(buf) >= 2:
            length, off = buf[1] & 0x7f, 2
            if length == 126:
                length, off = (struct.unpack('>H', buf[2:4])[0], 4) if len(buf) >= 4 else (None, 0)
            elif length == 127:
                length, off = (struct.unpack('>Q', buf[2:10])[0], 10) if len(buf) >= 10 else (None, 0)
            if length is not None and len(buf) >= off + length:
                payload, buf = buf[off:off + length], buf[off + length:]
                return json.loads(payload)
        buf += s.recv(1 << 20)


def wait(n, events):
    while True:
        msg = frame()
        if msg.get('method') == 'Tracing.dataCollected':
            events.extend(msg['params']['value'])
        if msg.get('id') == n:
            return msg


events = []
wait(send('Tracing.start', {'traceConfig': {
    'includedCategories': ['disabled-by-default-memory-infra'],
    'memoryDumpConfig': {'triggers': []}}, 'transferMode': 'ReportEvents'}), events)
time.sleep(1)
wait(send('Tracing.requestMemoryDump', {'levelOfDetail': 'detailed'}), events)
time.sleep(gap)
wait(send('Tracing.requestMemoryDump', {'levelOfDetail': 'detailed'}), events)
n = send('Tracing.end')
while True:
    msg = frame()
    if msg.get('method') == 'Tracing.dataCollected':
        events.extend(msg['params']['value'])
    if msg.get('method') == 'Tracing.tracingComplete':
        break

dumps = {}
for e in events:
    if e.get('ph') != 'v' or 'dumps' not in e.get('args', {}):
        continue
    entry = dumps.setdefault(e['pid'], {}).setdefault(e.get('id'), [e['ts'], {}])
    entry[0] = min(entry[0], e['ts'])
    for name, node in e['args']['dumps'].get('allocators', {}).items():
        size = node.get('attrs', {}).get('size', {}).get('value')
        if size is not None and name.count('/') <= 2:
            entry[1][name] = int(size, 16)
dumps = {pid: sorted(by_id.values(), key=lambda x: x[0]) for pid, by_id in dumps.items()}
for pid, series in dumps.items():
    if len(series) < 2:
        continue
    first, last = series[0][1], series[-1][1]
    growth = sorted(((last.get(k, 0) - first.get(k, 0), k, last.get(k, 0)) for k in set(first) | set(last)), reverse=True)
    print(f'pid {pid}: {sum(v for k, v in last.items() if k.count("/") == 0) >> 20} MB in top-level allocators')
    for d, k, v in growth[:10]:
        print(f'  {d / 2**20:+8.1f} MB  {v / 2**20:8.1f} MB  {k}')
