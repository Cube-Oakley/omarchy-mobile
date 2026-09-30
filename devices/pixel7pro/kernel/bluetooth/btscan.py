"""Count nearby Bluetooth devices through a raw HCI socket (addresses are not printed)."""
import fcntl, os, select, socket, struct, time
HCIDEVUP = 0x400448c9
s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
try:
    fcntl.ioctl(s.fileno(), HCIDEVUP, 0)
except OSError as e:
    print('HCIDEVUP:', e)
s.bind((0,))
s.setsockopt(0, 2, struct.pack('<IIIHxx', 0xffffffff, 0xffffffff, 0xffffffff, 0))  # all events
def send(opcode, params=b''):
    s.send(bytes([1, opcode & 0xff, opcode >> 8, len(params)]) + params)
def collect(seconds, handler):
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            handler(s.recv(1024))
seen_le, seen_br, status = set(), set(), {}
def le(pkt):
    if pkt[0] == 4 and pkt[1] == 0x0e:
        status[pkt[4] | pkt[5] << 8] = pkt[6]
    if pkt[0] == 4 and pkt[1] == 0x3e and pkt[3] == 0x0d:      # LE extended advertising report
        n, off = pkt[4], 5
        for _ in range(n):
            addr = pkt[off + 3:off + 9]; dlen = pkt[off + 23]
            seen_le.add(hash(addr)); off += 24 + dlen
def br(pkt):
    if pkt[0] == 4 and pkt[1] == 0x0f:
        status['inquiry'] = pkt[3]
    if pkt[0] == 4 and pkt[1] in (0x02, 0x22, 0x2f):
        seen_br.add(hash(pkt[4:10]))
send(0x2042, b'\x00\x00\x00\x00\x00\x00')              # extended scan off
collect(0.3, le)
send(0x2041, b'\x00\x00\x01\x00\x10\x00\x10\x00')          # passive, 1M PHY, 10 ms
collect(0.3, le)
send(0x2042, b'\x01\x00\x00\x00\x00\x00')              # on
collect(6, le)
send(0x2042, b'\x00\x00\x00\x00\x00\x00')
collect(0.3, le)
print(f'LE scan: {len(seen_le)} devices (command status {status})')
send(0x0401, b'\x33\x8b\x9e\x05\x00')                      # inquiry 6.4 s
collect(8, br)
print(f'BR/EDR inquiry: {len(seen_br)} devices (status {status.get("inquiry")})')
