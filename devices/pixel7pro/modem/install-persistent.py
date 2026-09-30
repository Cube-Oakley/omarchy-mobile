#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Install an opt-in modem bundle from this phone's previously validated lab.

Run on the Pixel from a staged source tree containing modem/ and scripts/.
Copies private assets locally; never extracts identity/NV to the host or writes
protected partitions. Requires a fresh boot and the known laboratory modules.
The new PID1 shutdown hook must be RAM-tested before installing the boot image.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--enable', action='store_true', help='enable the installed bundle on the next boot')
    args = parser.parse_args()
    os.umask(0o077)
    source = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location('pixel_manager', source / 'manager.py')
    manager = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manager)
    if Path('/sys/module/cpif').exists() or Path('/run/modem-lab').exists():
        raise RuntimeError('Install from a fresh boot with the modem stopped')
    manager.protected_readonly()
    assets = manager.ASSETS
    if assets.exists():
        raise RuntimeError('Modem installation already exists; inspect before replacing it')
    destination = Path('/usr/local/lib/omarchy-mobile/modem')
    config = Path('/etc/NetworkManager/conf.d/20-pixel-modem.conf')
    if destination.exists() or config.exists():
        raise RuntimeError('Existing modem helpers or network configuration; inspect before replacing them')
    scripts = source.parent / 'scripts'
    for name in ('pixel-persistent-session.sh', 'pixel-reboot.sh'):
        subprocess.run(['bash', '-n', str(scripts / name)], check=True)
    inputs = {
        'pixel-gpio.ko': '/root/tools/modem-stage4/pixel-gpio.ko',
        'pixel-pcie-cp.ko': '/root/tools/suspend/pixel-pcie-cp.ko',
        'shm_ipc.ko': '/root/tools/modem-stage4/shm_ipc.ko',
        'cpif.ko': '/root/tools/suspend/cpif.ko',
        'pixel-wdt-pm.ko': '/root/tools/suspend/pixel-wdt-pm.ko',
        'brcmfmac.ko': '/root/tools/suspend/brcmfmac-rptr.ko',
        'wdtkick': '/root/tools/wdtkick',
        'runtime.tar': '/root/modem-runtime.tar',
    }
    for path in inputs.values():
        if not Path(path).is_file():
            raise RuntimeError('Missing validated laboratory asset')
    assets.mkdir(mode=0o700, parents=True)
    backup = assets / 'before-install'
    backup.mkdir(mode=0o700)
    targets = {
        '/usr/local/lib/omarchy-mobile/pixel-boot.sh': scripts / 'pixel-persistent-session.sh',
        '/usr/local/sbin/pixel-reboot': scripts / 'pixel-reboot.sh',
        '/usr/local/sbin/pixel-suspend': scripts / 'pixel-suspend.py',
    }
    for target in targets:
        if Path(target).exists():
            shutil.copy2(target, backup / Path(target).name)
    manifest = {'kernel': os.uname().release, 'sha256': {}}
    for name, path in inputs.items():
        target = assets / name
        shutil.copyfile(path, target)
        target.chmod(0o700 if name == 'wdtkick' else 0o600)
        with target.open('rb') as src:
            manifest['sha256'][name] = hashlib.file_digest(src, 'sha256').hexdigest()
    manager.durable_json(assets / 'manifest.json', manifest)
    destination.mkdir(mode=0o700, exist_ok=False)
    for path in source.glob('*.py'):
        if not path.name.startswith('test_'):
            shutil.copyfile(path, destination / path.name)
            (destination / path.name).chmod(0o600)
    for target, path in targets.items():
        tmp = Path(target + '.modem-new')
        shutil.copyfile(path, tmp)
        tmp.chmod(0o700)
        tmp.replace(target)
    config.write_text('[keyfile]\nunmanaged-devices=interface-name:usb0;interface-name:rmnet*\n')
    if args.enable:
        manager.ENABLED.touch(mode=0o600)
        try:
            manager.verify_bundle()
        except BaseException:
            manager.ENABLED.unlink()
            raise
    os.sync()
    print('Persistent modem bundle installed; enabled for next boot:', args.enable)


if __name__ == '__main__':
    main()
