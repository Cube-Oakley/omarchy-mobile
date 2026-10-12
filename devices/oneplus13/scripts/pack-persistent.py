#!/usr/bin/env python3
"""Pack this unlocked CPH2655's tested kernel within its exact stock boot layout.

Can retain stock AVB metadata or regenerate an unsigned, correct payload hash.
This is only for an unlocked development phone, not an OEM-signed kernel.
Neither mode changes the separate VBMeta partition or its verification flags.
"""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile

STOCK_SHA256 = 'bbb4980f9efc63371268b8f7fbf78d0516e19c78a2c6a5f254170a409ebea983'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel', type=Path, required=True)
    ap.add_argument('--stock', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--compression', choices=['none', 'lz4'], default='none',
                    help='Default raw Image matches stock; LZ4 is an unvalidated experiment')
    ap.add_argument('--metadata', choices=['stock', 'unsigned'], default='stock',
                    help='Retain stock metadata or regenerate the boot payload hash')
    a = ap.parse_args()
    subprocess.run(['git', 'check-ignore', '-q', str(a.output.resolve())],
                   cwd=Path(__file__).resolve().parents[3], check=True)
    stock = a.stock.read_bytes()
    if hashlib.sha256(stock).hexdigest() != STOCK_SHA256:
        ap.error('Exact verified CPH2655 stock boot image required')
    original, offset, size = struct.unpack_from('>QQQ', stock, len(stock) - 52)
    if stock[-64:-60] != b'AVBf' or (original, offset, size) != (36753408, 36753408, 2368):
        ap.error('Unexpected stock AVB layout')
    kernel = a.kernel.read_bytes()
    if kernel[56:60] != b'ARM\x64':
        ap.error('Expected raw ARM64 Image')
    a.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=a.output.parent) as tmp:
        tmp = Path(tmp)
        kernel_path = a.kernel
        if a.compression == 'lz4':
            kernel_path = tmp / 'Image.lz4'
            subprocess.run(['lz4', '-f', '-l', '-9', str(a.kernel), str(kernel_path)], check=True)
            restored = subprocess.check_output(['lz4', '-d', '-c', str(kernel_path)])
            if restored != kernel:
                ap.error('Kernel compression round trip failed')
        subprocess.run(['mkbootimg', '--header_version', '4', '--kernel', str(kernel_path),
                        '--output', str(tmp / 'boot.img')], check=True)
        payload = (tmp / 'boot.img').read_bytes()
    if len(payload) > original:
        ap.error('Payload exceeds stock AVB boundary; refusing to truncate')
    result = payload.ljust(original, b'\0') + stock[original:]
    if a.metadata == 'unsigned':
        with tempfile.TemporaryDirectory(dir=a.output.parent) as tmp:
            # avbtool verify_image resolves the descriptor's partition name.
            boot = Path(tmp) / 'boot.img'
            boot.write_bytes(payload.ljust(original, b'\0'))
            subprocess.run([
                'avbtool', 'add_hash_footer', '--image', str(boot),
                '--partition_size', str(len(stock)), '--partition_name', 'boot',
                '--algorithm', 'NONE', '--salt', '0d5a1a48ceeaddd3a2b9031aaf76267f',
                '--prop', 'com.android.build.boot.os_version:15',
                '--prop', 'com.android.build.boot.fingerprint:'
                'qti/sun/sun:15/AP3A.240617.008/1785729736175:user/release-keys',
                '--prop', 'com.android.build.boot.security_patch:2026-08-01',
            ], check=True)
            subprocess.run(['avbtool', 'verify_image', '--image', str(boot)], check=True)
            result = boot.read_bytes()
    with a.output.open('xb') as stream:
        stream.write(result)
    print(f'Payload {len(payload)} bytes; stock boundary {original}; image {len(result)}')
    print(hashlib.sha256(result).hexdigest(), a.output)


if __name__ == '__main__':
    main()
