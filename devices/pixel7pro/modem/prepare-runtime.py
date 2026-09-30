#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prepare the phone's RAM-only modem laboratory; never starts a daemon.

Run as root on the phone: prepare-runtime.py PRIVATE_RUNTIME.tar
The archive must contain regular files/directories only: stock Bionic runtime,
stock cbd/rfsd, modem.bin, verified NV seed, local properties and MIT helpers.
No archive, firmware, identity, or NV contents belong in git.
"""
import hashlib
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tarfile

ROOT = Path('/run/modem-lab')
PROTECTED = set('persist efs efs_backup modem_userdata devinfo mfg_data fips modem_a modem_b'.split())


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    seen = set()
    for p in Path('/sys/class/block').iterdir():
        props = dict(line.split('=', 1) for line in (p / 'uevent').read_text().splitlines() if '=' in line)
        name = props.get('PARTNAME')
        if name in PROTECTED:
            if subprocess.check_output(['blockdev', '--getro', '/dev/' + p.name], text=True).strip() != '1':
                raise SystemExit('Protected partition is not read-only: ' + name)
            seen.add(name)
    if seen != PROTECTED:
        raise SystemExit('Missing protected partition metadata')
    if ROOT.exists():
        raise SystemExit('RAM sandbox already exists; reboot before preparing a new one')
    ROOT.mkdir(mode=0o700)
    subprocess.run(['mount', '-t', 'tmpfs', '-o', 'size=512m,mode=0700,nosuid', 'tmpfs', str(ROOT)], check=True)
    with tarfile.open(sys.argv[1]) as archive:
        for member in archive.getmembers():
            if not (member.isfile() or member.isdir()):
                raise SystemExit('Archive contains a link or special file')
        archive.extractall(ROOT, filter='data')
    # Capability-free daemons run as uid 0; archive ownership is irrelevant.
    for p in ROOT.rglob('*'):
        os.chown(p, 0, 0)
    for part, name in [('efs', 'nv_normal.bin'), ('efs', 'nv_protected.bin'), ('efs_backup', 'nv_protected.bak')]:
        p = ROOT / 'mnt/vendor' / part / name
        if hashlib.md5(p.read_bytes() + b'Samsung_SIT_RIL').hexdigest().encode() != p.with_name(name + '.md5').read_bytes():
            raise SystemExit('Stock NV checksum mismatch: ' + name)
    for directory in ['dev/socket', 'dev/block/by-name', 'proc', 'sys/power', 'properties',
                      'data/vendor/log/cbd', 'data/vendor/log/rfsd', 'data/vendor/ssrdump',
                      'mnt/vendor/modem_img/images/default']:
        (ROOT / directory).mkdir(parents=True, exist_ok=True)
    for name, minor in [('null', 3), ('zero', 5), ('urandom', 9)]:
        os.mknod(ROOT / 'dev' / name, stat.S_IFCHR | 0o666, os.makedev(1, minor))
    for name in ['wake_lock', 'wake_unlock']:
        (ROOT / 'sys/power' / name).touch()
    for name in ['tar', 'timeout', 'gunzip', 'mkdir', 'chmod', 'rm', 'cp', 'mv']:
        (ROOT / 'system/bin' / name).symlink_to('toybox')
    (ROOT / 'vendor/bin/tar').symlink_to('/system/bin/toybox')
    firmware = '/data/vendor/radio/image/modem.bin'
    (ROOT / firmware.lstrip('/')).chmod(0o400)
    for name in ['dev/block/by-name/modem', 'mnt/vendor/modem_img/images/default/modem.bin']:
        (ROOT / name).symlink_to(firmware)
    live = Path('/sys/firmware/devicetree/base/chosen')
    copied = ROOT / 'sys/firmware/devicetree/base/chosen'
    shutil.copytree(live / 'plat', copied / 'plat')
    (copied / 'config').mkdir()
    for name in ['imei1', 'imei2']:
        p = copied / 'config' / name
        shutil.copyfile(live / 'config' / name, p)
        p.chmod(0o400)
    print('RAM sandbox prepared; three stock NV checksums verified; no modem channels exposed')


if __name__ == '__main__':
    main()
