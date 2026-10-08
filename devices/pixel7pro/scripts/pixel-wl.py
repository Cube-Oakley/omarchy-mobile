#!/usr/bin/env python3
"""BCM4389 firmware ioctls and iovars through brcmfmac's nl80211 vendor command.

brcmfmac passes a raw firmware command (a "dcmd") through vendor command 1 of
Broadcom's OUI: a 20-byte header (command, length, payload offset, set, magic)
and the payload. Iovars go through GET_VAR (262) and SET_VAR (263) as
"name\\0" plus a 32-bit value; the PM mode is ioctl 86 (85 reads it). Commands
as in brcmfmac's fwil.h. Installed as /usr/local/sbin/pixel-wl.

  pixel-wl [-i IFACE] get NAME | set NAME VALUE
  pixel-wl [-i IFACE] get-int CMD | set-int CMD VALUE
"""
import argparse
import struct
import subprocess
import sys

GET_VAR, SET_VAR = 262, 263


def dcmd(iface, cmd, length, payload=b'', set_=False):
    header = struct.pack('<IiIII', cmd, length, 20, int(set_), 0)
    args = ['iw', 'dev', iface, 'vendor', 'send' if set_ else 'recv', '0x001018', '0x1']
    args += ['0x%02x' % b for b in header + payload]
    out = subprocess.run(args, capture_output=True, text=True)
    if out.returncode:
        sys.exit(f'pixel-wl: command {cmd}: {(out.stdout + out.stderr).strip()}')
    if set_:
        return b''
    # iw prints 16 bytes per "vendor response:" line. The reply is netlink
    # attributes (u16 length, u16 type, padded to 4); type 2 holds the data.
    data = bytes(int(x, 16) for x in out.stdout.replace('vendor response:', ' ').split())
    pos, reply = 0, b''
    while pos + 4 <= len(data):
        size, kind = struct.unpack_from('<HH', data, pos)
        if kind == 2:
            reply += data[pos + 4:pos + size]
        pos += (size + 3) & ~3
    return reply


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('-i', '--iface', default='wlp1s0')
    ap.add_argument('op', choices=['get', 'set', 'get-int', 'set-int'])
    ap.add_argument('what')
    ap.add_argument('value', nargs='?', type=int)
    args = ap.parse_args()
    if args.op.startswith('set') and args.value is None:
        ap.error('set needs a value')
    if args.op == 'get-int':
        print(struct.unpack('<i', dcmd(args.iface, int(args.what), 4)[:4])[0])
    elif args.op == 'set-int':
        dcmd(args.iface, int(args.what), 4, struct.pack('<i', args.value), True)
    elif args.op == 'get':
        name = args.what.encode() + b'\0'
        print(struct.unpack('<i', dcmd(args.iface, GET_VAR, max(len(name), 4), name)[:4])[0])
    else:
        payload = args.what.encode() + b'\0' + struct.pack('<i', args.value)
        dcmd(args.iface, SET_VAR, len(payload), payload, True)


if __name__ == '__main__':
    main()
