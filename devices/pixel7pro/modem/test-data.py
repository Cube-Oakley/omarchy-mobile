#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded CID-1 IPv4 packet test; restores its temporary address and routes.

Requires a successful sit-data.py response retained privately in RAM. The
rmnet0 interface must be down and have no addresses. No default route changes.
Only aggregate results are printed, never assigned addresses or DNS replies.
"""
import ipaddress
import json
import os
from pathlib import Path
import socket
import struct
import subprocess


def run(*args):
    result = subprocess.run(args, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError('network command failed: ' + args[0])
    return result.stdout


def main():
    data = Path('/run/modem-lab/data-call-response.bin').read_bytes()
    status, cid, active, pdp = struct.unpack_from('<HBBB', data)
    if status or cid != 1 or active not in (1, 2) or pdp not in (1, 3):
        raise RuntimeError('no successful CID-1 IPv4 bearer')
    address = str(ipaddress.IPv4Address(data[5:9]))
    dns = str(ipaddress.IPv4Address(data[0x1a:0x1e]))
    if ipaddress.ip_address(address).is_unspecified or ipaddress.ip_address(dns).is_unspecified:
        raise RuntimeError('missing IPv4 or DNS configuration')
    info = json.loads(run('ip', '-j', 'addr', 'show', 'dev', 'rmnet0'))[0]
    if 'UP' in info['flags'] or info['addr_info']:
        raise RuntimeError('rmnet0 must be down with no existing addresses')
    routes = []
    addressed = False
    try:
        run('ip', 'link', 'set', 'rmnet0', 'up')
        run('ip', 'addr', 'add', address + '/32', 'dev', 'rmnet0')
        addressed = True
        for target in sorted({dns, '1.1.1.1'}):
            run('ip', 'route', 'add', target + '/32', 'dev', 'rmnet0', 'src', address)
            routes.append(target)
        dns_ok = False
        try:
            # DNS A query for example.com, with a random transaction id.
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, b'rmnet0\0')
                sock.settimeout(10)
                token = struct.unpack('!H', os.urandom(2))[0]
                query = struct.pack('!6H', token, 0x100, 1, 0, 0, 0) + b'\x07example\x03com\0\0\1\0\1'
                sock.connect((dns, 53))
                sock.send(query)
                reply = sock.recv(4096)
                if len(reply) < 12:
                    raise RuntimeError('short DNS response')
                ident, flags, _, answers, _, _ = struct.unpack_from('!6H', reply)
                if ident != token or not flags & 0x8000 or flags & 15 or not answers:
                    raise RuntimeError('DNS query did not succeed')
                print('cellular DNS: successful answer', flush=True)
            dns_ok = True
        except (OSError, RuntimeError) as error:
            print('cellular DNS failed:', type(error).__name__, flush=True)
        result = run('curl', '--silent', '--show-error', '--noproxy', '*',
                     '--interface', 'rmnet0', '--resolve', 'one.one.one.one:443:1.1.1.1',
                     '--connect-timeout', '10', '--max-time', '20', '--output', '/dev/null',
                     '--write-out', '%{http_code} %{size_download}',
                     'https://one.one.one.one/cdn-cgi/trace').decode().split()
        if len(result) != 2 or result[0] != '200':
            raise RuntimeError('HTTPS did not return 200')
        print('cellular HTTPS: certificate verified, HTTP 200, bytes', result[1], flush=True)
        if not dns_ok:
            raise RuntimeError('carrier DNS failed despite completed HTTPS test')
    finally:
        for target in reversed(routes):
            run('ip', 'route', 'del', target + '/32', 'dev', 'rmnet0')
        if addressed:
            run('ip', 'addr', 'del', address + '/32', 'dev', 'rmnet0')
        run('ip', 'link', 'set', 'rmnet0', 'down')
    for name in ('tx_packets', 'rx_packets', 'tx_errors', 'rx_errors', 'tx_dropped', 'rx_dropped'):
        print(name, (Path('/sys/class/net/rmnet0/statistics') / name).read_text().strip())


if __name__ == '__main__':
    main()
