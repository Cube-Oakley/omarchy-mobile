#!/usr/bin/env python3
"""Build a local sparse ext4 Pixel userdata image from a prepared Arch RAM snapshot.

Never contacts or flashes a phone. The archive and output contain private SSH
material and must stay in ignored local output directories.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import tarfile
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent
SIZE = 245977141248


def sha256(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def sparse_geometry(path):
    counts = {0xcac1: 0, 0xcac2: 0, 0xcac3: 0, 0xcac4: 0}
    with path.open('rb') as source:
        header = source.read(28)
        magic, major, minor, file_header, chunk_header, block, blocks, chunks, crc = struct.unpack('<I4H4I', header)
        if magic != 0xed26ff3a or major != 1 or file_header != 28 or chunk_header != 12 or block != 4096:
            raise RuntimeError('Unexpected Android sparse header')
        total = 0
        for _ in range(chunks):
            kind, reserved, nblocks, length = struct.unpack('<2H2I', source.read(12))
            if kind not in counts or length < 12:
                raise RuntimeError('Invalid sparse chunk')
            if kind == 0xcac1 and length != 12 + nblocks * block:
                raise RuntimeError('Invalid raw chunk length')
            if kind == 0xcac2 and length != 16:
                raise RuntimeError('Invalid fill chunk length')
            if kind == 0xcac3 and length != 12:
                raise RuntimeError('Invalid skipped chunk length')
            counts[kind] += nblocks * block
            total += nblocks
            source.seek(length - 12, 1)
        if total != blocks or source.tell() != path.stat().st_size or blocks * block != SIZE:
            raise RuntimeError('Sparse image geometry mismatch')
    if counts[0xcac3] < SIZE * 0.9:
        raise RuntimeError('Free space is not skipped; refusing an unnecessarily huge write')
    return {hex(kind): count for kind, count in counts.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    overrides = {
        'root/pixel-gpu-session-start.sh': ('scripts/pixel-gpu-session-start.sh', 0o700),
        'root/pixel-desktop-prepare.sh': ('adapter/desktop-prepare.sh', 0o700),
        'usr/local/lib/omarchy-mobile/pixel-boot.sh': ('scripts/pixel-persistent-session.sh', 0o700),
        'root/install-pixel-root.sh': ('scripts/install-pixel-root.sh', 0o700),
        'usr/local/sbin/pixel-usb-diag': ('scripts/pixel-usb-diag.py', 0o755),
        'usr/local/sbin/pixel-wifi-ssh': ('scripts/pixel-wifi-ssh.py', 0o755),
        'etc/NetworkManager/dispatcher.d/50-pixel-wifi-ssh': ('scripts/pixel-wifi-ssh-dispatcher.sh', 0o755),
        'usr/local/sbin/pixel-reboot': ('scripts/pixel-reboot.sh', 0o755),
        'etc/NetworkManager/conf.d/10-pixel.conf': ('adapter/network/NetworkManager-pixel.conf', 0o644),
        'usr/share/alsa/ucm2/conf.d/aoc-snd-card/aoc-snd-card.conf':
            ('audio/ucm2/conf.d/aoc-snd-card/aoc-snd-card.conf', 0o644),
        'usr/share/alsa/ucm2/Google/aoc/HiFi.conf': ('audio/ucm2/Google/aoc/HiFi.conf', 0o644),
        'etc/wireplumber/wireplumber.conf.d/51-pixel-aoc.conf': ('audio/wireplumber/51-pixel-aoc.conf', 0o644),
        'usr/local/lib/omarchy-mobile/sensors/pixel-sensor-proxy.py': ('sensors/pixel-sensor-proxy.py', 0o755),
        'usr/local/lib/omarchy-mobile/sensors/usf.py': ('sensors/usf.py', 0o644),
        'usr/local/lib/omarchy-mobile/sensors/usf_backend.py': ('sensors/usf_backend.py', 0o644),
    }
    directories = {'mnt': 0o755, 'run': 0o755, 'dev': 0o755, 'proc': 0o755, 'sys': 0o755, 'tmp': 0o1777}
    proof = 'pixel-persistent-' + uuid.uuid4().hex + '\n'
    extras = {'etc/omarchy-mobile-pixel-root': b'v1\n', 'root/persistence-proof.txt': proof.encode()}
    # The AoC (audio DSP) firmware, 12255112-polygon from the stock vendor
    # image. Proprietary: it stays in the ignored out/ and in this local image.
    # The boot script points the kernel's firmware path at this directory.
    aoc_firmware = (ROOT / 'out/stock-vendor/audio/firmware/aoc.bin').read_bytes()
    if not hashlib.sha256(aoc_firmware).hexdigest().startswith('3d4dc885e95163bb'):
        raise RuntimeError('Stock AoC firmware checksum mismatch')
    extras['usr/lib/firmware/omarchy-mobile/aoc.bin'] = aoc_firmware
    # The stock sensor registry scripts (/vendor/etc/sensors/registry), which
    # pixel-sensor-proxy loads into the AoC. Vendor data: ignored out/ only.
    registry = ROOT / 'out/stock-vendor/sensors/registry'
    for path in sorted(registry.rglob('*.reg')):
        extras['usr/share/omarchy-mobile/sensors/registry/' + str(path.relative_to(registry))] = path.read_bytes()
    required = {'usr/bin/bash', 'usr/bin/Hyprland', 'usr/bin/quickshell', 'etc/ssh/sshd_config.pixel'}
    seen = set()
    prepared = out / 'root.tar'
    with tarfile.open(args.archive, 'r|*') as source, tarfile.open(prepared, 'w') as target:
        for member in source:
            name = member.name.removeprefix('./').rstrip('/')
            if member.name.startswith('/') or '..' in Path(name).parts:
                raise RuntimeError('Unsafe archive member')
            seen.add(name)
            if name in overrides or name in directories or name in extras:
                continue
            target.addfile(member, source.extractfile(member) if member.isfile() else None)
        if not required <= seen:
            raise RuntimeError('Prepared Arch archive is missing desktop/SSH files')
        for name, mode in directories.items():
            member = tarfile.TarInfo(name)
            member.type = tarfile.DIRTYPE
            member.mode = mode
            target.addfile(member)
        files = {name: ((ROOT / relative).read_bytes(), mode) for name, (relative, mode) in overrides.items()}
        files.update({name: (data, 0o644) for name, data in extras.items()})
        # Parents of added files that the archive lacks, shallowest first.
        parents = {str(parent) for name in files for parent in Path(name).parents if str(parent) != '.'}
        for name in sorted(parents - seen - set(directories), key=lambda n: n.count('/')):
            member = tarfile.TarInfo(name)
            member.type = tarfile.DIRTYPE
            member.mode = 0o755
            target.addfile(member)
        for name, (data, mode) in files.items():
            member = tarfile.TarInfo(name)
            member.size, member.mode, member.mtime = len(data), mode, int(time.time())
            target.addfile(member, io.BytesIO(data))
    raw = out / 'root.raw'
    with raw.open('xb') as image:
        image.truncate(SIZE)
    sparse = out / 'root.sparse.img'
    with (out / 'build.log').open('w') as log:
        subprocess.run(['mke2fs', '-t', 'ext4', '-F', '-b', '4096', '-L', 'omarchy-root',
                        '-m', '1', '-N', '1048576', '-J', 'size=128', '-E',
                        'nodiscard,lazy_itable_init=1,lazy_journal_init=1', '-d', str(prepared), str(raw)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run(['e2fsck', '-fn', str(raw)], stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run(['img2simg', '-s', str(raw), str(sparse)], stdout=log, stderr=subprocess.STDOUT, check=True)
    chunks = sparse_geometry(sparse)
    metadata = {'archive_sha256': sha256(args.archive), 'sparse_sha256': sha256(sparse),
                'expanded_bytes': SIZE, 'sparse_bytes': sparse.stat().st_size,
                'chunk_bytes': chunks, 'persistence_proof': proof.strip()}
    (out / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata, indent=2))
    print('Local image checks pass. Nothing was flashed.')


if __name__ == '__main__':
    main()
