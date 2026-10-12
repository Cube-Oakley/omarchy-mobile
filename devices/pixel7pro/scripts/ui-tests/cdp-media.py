#!/usr/bin/env python3
"""Which decoder each <video> of the first page uses (test only).

cdp-media.py PORT SECONDS [URL]   enable DevTools' Media domain, load URL in
the page (or reload it) and print, per player, its decoder name, whether
that is a platform (hardware) decoder, and its errors and warnings, for
SECONDS. A reload briefly has the old page's decoders and the new page's.
"""
import base64, json, os, socket, struct, sys, time, urllib.request

port, seconds = int(sys.argv[1]), float(sys.argv[2])
url = sys.argv[3] if len(sys.argv) > 3 else None
ws = [p for p in json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/json'))
      if p.get('type') == 'page'][0]['webSocketDebuggerUrl']
path = ws.split(f':{port}', 1)[1]
s = socket.create_connection(('127.0.0.1', port))
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((f'GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
           f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
buf = b''
while b'\r\n\r\n' not in buf:
    buf += s.recv(1 << 16)
buf = buf.split(b'\r\n\r\n', 1)[1]
ids = iter(range(1, 1 << 30))


def send(method, params=None):
    data = json.dumps({'id': next(ids), 'method': method, 'params': params or {}}).encode()
    mask = os.urandom(4)
    if len(data) < 126:
        head = bytes([0x81, 0x80 | len(data)])
    else:
        head = bytes([0x81, 0x80 | 126]) + struct.pack('>H', len(data))
    s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


def frame(deadline):
    global buf
    def need(n):
        global buf
        while len(buf) < n:
            s.settimeout(max(0.01, deadline - time.monotonic()))
            data = s.recv(1 << 20)
            if not data:
                sys.exit('cdp-media.py: the browser closed the connection')
            buf += data
    need(2)
    n, off = buf[1] & 0x7f, 2
    if n == 126:
        need(4); n = struct.unpack('>H', buf[2:4])[0]; off = 4
    elif n == 127:
        need(10); n = struct.unpack('>Q', buf[2:10])[0]; off = 10
    need(off + n)
    payload, buf = buf[off:off + n], buf[off + n:]
    return json.loads(payload)


send('Media.enable')
send('Page.navigate', {'url': url}) if url else send('Page.reload')
players = {}
end = time.monotonic() + seconds
while time.monotonic() < end:
    try:
        msg = frame(end)
    except (TimeoutError, socket.timeout):
        break
    method, params = msg.get('method'), msg.get('params', {})
    player = players.setdefault(params.get('playerId'), {'notes': []}) if 'playerId' in params else None
    if method == 'Media.playerPropertiesChanged':
        for prop in params['properties']:
            if prop['name'] in ('kFrameUrl', 'kVideoDecoderName', 'kIsPlatformVideoDecoder', 'kVideoTracks'):
                player[prop['name']] = prop['value']
    elif method == 'Media.playerMessagesLogged':
        player['notes'] += [f"{m['level']}: {m['message']}" for m in params['messages'] if m['level'] in ('error', 'warning')]
    elif method == 'Media.playerErrorsRaised':
        player['notes'] += [f"error: {e.get('errorType')} {e.get('code')}" for e in params['errors']]
for pid, p in players.items():
    if pid is None:
        continue
    print(f"{pid[:8]} decoder={p.get('kVideoDecoderName')} platform={p.get('kIsPlatformVideoDecoder')}")
    for note in dict.fromkeys(p['notes']):
        print('    ' + note[:200])
