#!/usr/bin/env python3
"""One DevTools call on a local endpoint (test only; timing data, not pages).

cdp.py PORT page|browser METHOD [JSON_PARAMS]   print the result as JSON
cdp.py PORT browser trace SECONDS CATEGORIES OUT  record a trace to OUT

`page` is the first page target, `browser` the browser target (histograms,
tracing). The trace is written as returned (JSON with traceEvents).
"""
import base64, json, os, socket, struct, sys, time, urllib.request

port, target, method = int(sys.argv[1]), sys.argv[2], sys.argv[3]
if target == 'browser':
    ws = json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/json/version'))['webSocketDebuggerUrl']
else:
    ws = [p for p in json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/json'))
          if p.get('type') == 'page'][0]['webSocketDebuggerUrl']
path = ws.split(f':{port}', 1)[1]
s = socket.create_connection(('127.0.0.1', port))
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((f'GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
           f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
def more():
    """More bytes; a closed browser ends the script (it spun at full CPU)."""
    data = s.recv(1 << 20)
    if not data:
        sys.exit('cdp.py: the browser closed the connection')
    return data

buf = b''
while b'\r\n\r\n' not in buf:
    buf += more()
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
    global buf
    while len(buf) < 2:
        buf += more()
    n = buf[1] & 0x7f
    off = 2
    if n == 126:
        while len(buf) < 4: buf += more()
        n = struct.unpack('>H', buf[2:4])[0]; off = 4
    elif n == 127:
        while len(buf) < 10: buf += more()
        n = struct.unpack('>Q', buf[2:10])[0]; off = 10
    while len(buf) < off + n:
        buf += more()
    payload, buf = buf[off:off + n], buf[off + n:]
    return json.loads(payload)

def call(method, params=None, events=None):
    n = send(method, params)
    while True:
        msg = frame()
        if msg.get('id') == n:
            return msg
        if events is not None and 'method' in msg:
            events.append(msg)

if method == 'trace':
    seconds, categories, out = float(sys.argv[4]), sys.argv[5], sys.argv[6]
    call('Tracing.start', {'transferMode': 'ReturnAsStream',
                           'traceConfig': {'includedCategories': categories.split(','), 'recordMode': 'recordAsMuchAsPossible'}})
    print('tracing', flush=True)
    time.sleep(seconds)
    events = []
    call('Tracing.end', events=events)
    while not any(e['method'] == 'Tracing.tracingComplete' for e in events):
        events.append(frame())
    stream = [e for e in events if e['method'] == 'Tracing.tracingComplete'][0]['params']['stream']
    with open(out, 'wb') as f:
        while True:
            r = call('IO.read', {'handle': stream, 'size': 1 << 20})['result']
            chunk = r['data']
            f.write(base64.b64decode(chunk) if r.get('base64Encoded') else chunk.encode())
            if r.get('eof'):
                break
    call('IO.close', {'handle': stream})
    print('trace written', out)
else:
    params = json.loads(sys.argv[4]) if len(sys.argv) > 4 else {}
    print(json.dumps(call(method, params).get('result', {})))
