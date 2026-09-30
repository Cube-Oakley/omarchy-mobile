#!/usr/bin/env python3
"""Assemble the cpif (modem interface) sources: pinned upstream files plus the Pixel 7 Pro patches.

  assemble.py OUT_DIR [--cache DIR] [--offline]

Each upstream file in sources.json is fetched once into a content-addressed
cache (default out/cpif-src-cache), verified by SHA-256, and laid out under
OUT_DIR; then patches/*.patch are applied in order. The result is the tree
to compile: cpif/ (Kbuild: cpif.ko, shm_ipc.ko, cpif_page.ko) and stubs/.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def fetch(repo, path):
    url = repo['url'].format(commit=repo['commit'], path=path)
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read()
    return base64.b64decode(data) if repo['encoding'] == 'base64' else data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--cache', type=Path, default=ROOT / 'out/cpif-src-cache')
    ap.add_argument('--offline', action='store_true', help='use only cached files')
    a = ap.parse_args()
    manifest = json.loads((HERE / 'sources.json').read_text())
    if a.out.exists():
        ap.error(f'{a.out} exists')
    a.cache.mkdir(parents=True, exist_ok=True)
    for dest, (repo_name, path, digest) in manifest['files'].items():
        cached = a.cache / digest
        if not cached.exists():
            if a.offline:
                sys.exit(f'{dest}: not cached ({repo_name}:{path})')
            data = fetch(manifest['repos'][repo_name], path)
            if hashlib.sha256(data).hexdigest() != digest:
                sys.exit(f'{dest}: checksum mismatch for {repo_name}:{path}')
            cached.write_bytes(data)
        if hashlib.sha256(cached.read_bytes()).hexdigest() != digest:
            sys.exit(f"{dest}: cached source checksum mismatch")
        target = a.out / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(cached, target)
    for patch in sorted((HERE / 'patches').glob('*.patch')):
        subprocess.run(['patch', '-p1', '-s', '-N', '--no-backup-if-mismatch', '-d', str(a.out),
                        '-i', str(patch)], check=True)


if __name__ == '__main__':
    main()
