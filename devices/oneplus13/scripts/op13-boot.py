#!/usr/bin/env python3
"""Hands-off OnePlus 13 boot cycles: native Linux <-> fastboot, RAM boot, flash.

  op13-boot.py fastboot              native Linux -> fastboot (orderly native-reboot)
  op13-boot.py ram IMAGE SHA256      (Linux or fastboot) -> RAM boot IMAGE, wait for SSH
  op13-boot.py flash IMAGE SHA256    (Linux or fastboot) -> flash boot_a only, boot it
  op13-boot.py linux                 fastboot -> normal boot from boot_a, wait for SSH
  op13-boot.py wait                  no restart: wait for SSH (re-activates host USB)
  op13-boot.py status                fastboot identity and slot A state

Every fastboot action first checks the private serial, product `sun`, unlocked
state and active slot A. Flashing writes only `boot_a`, refuses images that are
not 96 MiB Android v4 boot images or whose hash differs, and never touches
VBMeta, slots or the slot-success flag (see mark-boot-successful.py).
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
SSH = ROOT / 'out/native-probe17/arch-session/ssh'
NM_PROFILE = 'd83e85f0-b43f-4a26-a597-26efa606b885'
HOST_IFACE = 'enp198s0f0u2'
BOOT_SIZE = 100663296


def serial():
    value = os.environ.get('PHONE_SERIAL', '')
    path = ROOT / 'out/device.serial'
    if not value and path.is_file():
        value = (path.read_text().split() or [''])[0]
    if not re.fullmatch(r'[A-Za-z0-9_-]+', value):
        raise SystemExit('Set PHONE_SERIAL or put the serial in ignored out/device.serial')
    return value


def fastboot(*args, timeout=60):
    r = subprocess.run(['fastboot', '-s', serial(), *args], text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    if r.returncode:
        raise SystemExit(f'fastboot {" ".join(args)} failed:\n{r.stdout.strip()}')
    return r.stdout


def getvar(name):
    out = fastboot('getvar', name)
    m = re.search(rf'^{re.escape(name)}:\s*(.*)$', out, re.M)
    return m.group(1).strip() if m else ''


def in_fastboot():
    r = subprocess.run(['fastboot', 'devices'], text=True, capture_output=True, timeout=15)
    return any(line.split()[:1] == [serial()] for line in r.stdout.splitlines())


def check_identity():
    state = {v: getvar(v) for v in ('product', 'unlocked', 'current-slot',
                                    'slot-successful:a', 'slot-unbootable:a', 'slot-retry-count:a')}
    if state['product'] != 'sun' or state['unlocked'] != 'yes' or state['current-slot'] != 'a':
        raise SystemExit(f'Unexpected fastboot identity, phone untouched: {state}')
    return state


def ssh(command, timeout=30):
    return subprocess.run([str(SSH), command], text=True, capture_output=True, timeout=timeout)


def wait_fastboot(seconds=90):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if in_fastboot():
            return
        time.sleep(1)
    raise SystemExit('Fastboot did not appear')


def to_fastboot():
    if in_fastboot():
        return
    log = ROOT / 'out/op13-boot-reboot.log'
    subprocess.run([sys.executable, str(ROOT / 'scripts/native-shell.py'), '--wait', '5',
                    '--timeout', '15', '--log', str(log),
                    '--command', '/bin/native-reboot --bootloader'], timeout=60)
    wait_fastboot()


def wait_linux(seconds=180):
    """Re-activate the host profile once the gadget re-enumerates, then wait for SSH."""
    end = time.monotonic() + seconds
    activated = False
    while time.monotonic() < end:
        if Path(f'/sys/class/net/{HOST_IFACE}').exists() and not activated:
            r = subprocess.run(['nmcli', 'connection', 'up', 'uuid', NM_PROFILE, 'ifname', HOST_IFACE],
                               capture_output=True, text=True, timeout=30)
            activated = r.returncode == 0
        if activated:
            r = ssh('cat /proc/sys/kernel/random/boot_id; uname -r', timeout=15)
            if r.returncode == 0:
                boot_id, release = r.stdout.split()[:2]
                print(f'linux up: {release} boot {boot_id}')
                return boot_id
        time.sleep(2)
    raise SystemExit('Native Linux SSH did not come up')


def checked_image(path, digest):
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise SystemExit('Image hash mismatch; phone untouched')
    if len(data) != BOOT_SIZE or data[:8] != b'ANDROID!' or struct.unpack_from('<I', data, 40)[0] != 4:
        raise SystemExit('Expected a 96 MiB Android v4 boot image')
    return Path(path).resolve()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('action', choices=['fastboot', 'ram', 'flash', 'linux', 'wait', 'status'])
    ap.add_argument('image', nargs='?')
    ap.add_argument('sha256', nargs='?')
    a = ap.parse_args()
    if a.action in ('ram', 'flash'):
        if not (a.image and a.sha256):
            ap.error(f'{a.action} needs IMAGE and SHA256')
        image = checked_image(a.image, a.sha256)
    if a.action == 'wait':
        wait_linux()
        return
    to_fastboot()
    state = check_identity()
    print('fastboot:', state)
    if a.action in ('fastboot', 'status'):
        return
    if a.action == 'ram':
        fastboot('boot', str(image), timeout=180)
    elif a.action == 'flash':
        fastboot('flash', 'boot_a', str(image), timeout=180)
        print('flashed boot_a; slot-successful:a is now', getvar('slot-successful:a'))
        fastboot('reboot')
    else:
        fastboot('reboot')
    wait_linux()


if __name__ == '__main__':
    main()
