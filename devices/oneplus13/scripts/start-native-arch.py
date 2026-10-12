#!/usr/bin/env python3
"""Provision shared Arch Linux ARM in OnePlus 13 RAM, then enable USB-only key SSH.

Run after RAM-booting the network image. Requires desktop NetworkManager rights,
native serial access, ssh-keygen and the pinned cached OnePlus base archive.
Never mounts a phone block device or writes Android storage.
"""
# Adapted from the Pixel USB bring-up helpers; device identity/link differ.
import argparse
import base64
import gzip
import hashlib
import json
from pathlib import Path
import re
import shlex
import socket
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent
ARCH_SHA = '42a4eeaa038994ffd31fa173256ef2f0ef511358eeb41b9ea1f8626391b9b319'
PHONE = '10.77.13.1'
HOST_MAC = '02:70:13:00:00:02'


def run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, **kwargs).stdout.strip()


def find_interface():
    for p in Path('/sys/class/net').iterdir():
        if (p / 'address').read_text().strip() != HOST_MAC:
            continue
        for parent in (p / 'device').resolve().parents:
            try:
                if ((parent / 'idVendor').read_text().strip() == '0525' and
                    (parent / 'idProduct').read_text().strip() == 'a4aa' and
                    'Omarchy-OnePlus13' in (parent / 'manufacturer').read_text()):
                    return p.name
            except OSError:
                pass
    raise RuntimeError('OnePlus 13 network gadget not found; RAM-boot the network image first')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive', type=Path, default=ROOT.parent / 'oneplus7pro/.work/arch/ArchLinuxARM-aarch64-latest.tar.gz')
    ap.add_argument('--archive-sha256', default=ARCH_SHA, help='expected SHA256 of the selected gzip rootfs archive')
    ap.add_argument('--output', type=Path, required=True, help='new local directory for logs and private temporary SSH credentials')
    a = ap.parse_args()
    if not re.fullmatch(r'[0-9a-f]{64}', a.archive_sha256):
        ap.error('--archive-sha256 must be 64 lowercase hexadecimal characters')
    archive_sha = a.archive_sha256
    out = a.output.resolve()
    subprocess.run(['git', 'check-ignore', '-q', str(out / 'id_ed25519')], cwd=ROOT, check=True)
    print('Checking shared Arch archive...', flush=True)
    with a.archive.open('rb') as f:
        if hashlib.file_digest(f, 'sha256').hexdigest() != archive_sha:
            raise RuntimeError('Archive checksum differs from the selected pinned input')
    # fastboot returns before the native composite gadget finishes enumerating.
    deadline = time.monotonic() + 30
    while True:
        try:
            interface = find_interface()
            break
        except RuntimeError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.5)
    out.mkdir(parents=True, exist_ok=False, mode=0o700)

    def serial(command, name, timeout=30):
        return run([sys.executable, str(ROOT / 'scripts/native-shell.py'),
                    '--command', command, '--timeout', str(timeout), '--log', str(out / name)],
                   timeout=timeout + 60)

    def put(data, path, name):
        encoded = base64.b64encode(gzip.compress(data)).decode()
        if len(encoded) > 2800:
            raise RuntimeError('Serial upload too large for canonical terminal line')
        serial(f'echo {encoded} | base64 -d | gzip -d >{shlex.quote(path)}', name)

    # Refuse to overwrite a live chroot. /run must be tmpfs before any provisioning.
    preflight = serial("set -e; grep -q ' /run tmpfs ' /proc/mounts; test ! -e /run/arch; "
                       "uname -r; cat /proc/cmdline; cat /proc/uptime", 'preflight.txt')
    limits = re.findall(r'\boneplus13_test_seconds=(\d+)\b', preflight)
    uptime = re.search(r'^(\d+\.\d+) \d+\.\d+\r?$', preflight, re.M)
    if not limits or not uptime:
        raise RuntimeError('Cannot determine the native image runtime limit')
    limit = int(limits[-1])
    if limit and limit - float(uptime[1]) < 360:
        raise RuntimeError('Less than six minutes remain; reboot a fresh network image first')
    # The initial powersave governor deliberately boots at minimum frequency;
    # extracting the shared rootfs there can exceed the provisioning deadline.
    serial('mkdir -p /run/transfer', 'transfer-directory.txt')
    put((ROOT / 'scripts/set-probe-cpu-policy.sh').read_bytes(),
        '/run/transfer/cpu-policy.sh', 'cpu-policy-upload.txt')
    serial('sh /run/transfer/cpu-policy.sh', 'cpu-policy.txt')
    profile_uuid = str(uuid.uuid4())
    profile_name = 'oneplus13-linux-usb-' + profile_uuid[:8]
    run(['nmcli', 'connection', 'add', 'save', 'no', 'type', 'ethernet',
         'con-name', profile_name, 'ifname', interface, 'connection.uuid', profile_uuid,
         '802-3-ethernet.mac-address', HOST_MAC, 'connection.autoconnect', 'no',
         'connection.permissions', 'user:' + run(['id', '-un']),
         'ipv4.method', 'manual', 'ipv4.addresses', '10.77.13.2/30',
         'ipv4.never-default', 'yes', 'ipv6.method', 'disabled'])
    metadata = {'archive': str(a.archive.resolve()), 'archive_sha256': archive_sha,
                'interface': interface, 'network_profile_uuid': profile_uuid,
                'network_profile_name': profile_name, 'phone': PHONE}
    (out / 'session.json').write_text(json.dumps(metadata, indent=2) + '\n')
    run(['nmcli', '--wait', '15', 'connection', 'up', 'uuid', profile_uuid, 'ifname', interface])
    serial('mkdir -p /run/transfer; nc -l -s 10.77.13.1 -p 8022 '
           '>/run/transfer/archlinuxarm.tar.gz 2>/run/transfer/receive.log </dev/null &', 'receiver.txt')
    print('Sending archive over USB...', flush=True)
    started = time.monotonic()
    # Listener starts asynchronously; only retry connection before sending bytes.
    for attempt in range(20):
        try:
            conn = socket.create_connection((PHONE, 8022), timeout=5)
            break
        except ConnectionRefusedError:
            if attempt == 19:
                raise
            time.sleep(0.2)
    with conn, a.archive.open('rb') as f:
        conn.settimeout(60)
        count = conn.sendfile(f)
        conn.shutdown(socket.SHUT_WR)
    if count != a.archive.stat().st_size:
        raise RuntimeError('Incomplete archive transfer')
    metadata.update(transfer_bytes=count, transfer_seconds=round(time.monotonic() - started, 2))
    # Check checksum before extracting; no generic archive kernel is ever booted.
    print('Verifying and unpacking in RAM...', flush=True)
    serial("set -e; echo '" + archive_sha + "  /run/transfer/archlinuxarm.tar.gz' | sha256sum -c -; "
           'mkdir /run/arch; tar -xzf /run/transfer/archlinuxarm.tar.gz -C /run/arch '
           '>/run/transfer/extract.log 2>&1; rm /run/transfer/archlinuxarm.tar.gz; '
           'echo ONEPLUS13_ARCH_EXTRACTED; df -h /run',
           'extract.txt', timeout=240)
    run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-C', 'oneplus13-ram-bringup', '-f', str(out / 'id_ed25519')])
    put((out / 'id_ed25519.pub').read_bytes(), '/run/transfer/authorized_keys', 'authorize.txt')
    put((ROOT / 'scripts/native-arch-services.sh').read_bytes(), '/run/transfer/arch-services.sh', 'services-upload.txt')
    proof = serial(f'date -s @{int(time.time())}; sh /run/transfer/arch-services.sh', 'services-start.txt', timeout=90)
    host_key = re.search(r'^ssh-ed25519 (\S+) root@oneplus13-linux\r?$', proof, re.M)
    if not host_key:
        raise RuntimeError('No SSH host key received over serial')
    known_hosts = out / 'known_hosts'
    known_hosts.write_text(PHONE + ' ssh-ed25519 ' + host_key[1] + '\n')
    # All paths are local artifacts. No user SSH configuration is edited.
    options = ['-F', '/dev/null', '-i', str(out / 'id_ed25519'),
               '-o', 'IdentitiesOnly=yes', '-o', 'UserKnownHostsFile=' + str(known_hosts),
               '-o', 'StrictHostKeyChecking=yes', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10']
    cmd = ['ssh', *options, 'root@' + PHONE]
    (out / 'ssh').write_text('#!/bin/sh\nexec ' + shlex.join(cmd) + ' "$@"\n')
    (out / 'ssh').chmod(0o700)
    evidence = run([*cmd, 'set -e; id; uname -a; cat /etc/os-release; '
                    '/usr/lib/systemd/systemd --version; pacman -Q bash glibc openssh systemd; '
                    'stat -f -c "root filesystem: %T" /; '
                    'printf "PID1: "; cat /proc/1/comm; cat /proc/uptime; date -u'])
    (out / 'ssh-proof.txt').write_text(evidence + '\n')
    metadata['ssh_verified'] = True
    (out / 'session.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(evidence)
    print(f'Connect: {out}/ssh')
    print(f'Network cleanup after reboot: nmcli connection delete uuid {profile_uuid}')
    print('Everything on the phone is in RAM and disappears at reboot or the image timeout.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'oneplus13-arch: {error}', file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout or '', file=sys.stderr)
            print(error.stderr or '', file=sys.stderr)
        sys.exit(1)
