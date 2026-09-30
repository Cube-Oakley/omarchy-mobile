#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Persistent Pixel modem supervisor, enabled only with a private local bundle.

prepare runs before audio so the shared GPIO provider is loaded once. run
starts after audio and NetworkManager. Failed starts are latched across reboot;
there is no automatic warm CP restart or replay of application requests.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ASSETS = Path('/var/lib/omarchy-mobile/modem')
RUN = Path('/run/pixel-modem')
LAB = Path('/run/modem-lab')
GUARD = ASSETS / 'starting.json'
ENABLED = ASSETS / 'enabled'
FILES = ('pixel-gpio.ko', 'pixel-pcie-cp.ko', 'shm_ipc.ko', 'cpif.ko', 'pixel-wdt-pm.ko',
         'brcmfmac.ko', 'wdtkick', 'runtime.tar')


def boot_id():
    return Path('/proc/sys/kernel/random/boot_id').read_text().strip()


def durable_json(path, data):
    tmp = path.with_suffix('.tmp')
    with tmp.open('w') as out:
        json.dump(data, out)
        out.flush()
        os.fsync(out.fileno())
    tmp.replace(path)
    fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def command(*args, timeout=60):
    result = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, timeout=timeout)
    if result.returncode:
        raise RuntimeError('Modem operation failed: ' + Path(args[0]).name)


def protected_readonly():
    if b'google,GS201 CHEETAH' not in Path('/sys/firmware/devicetree/base/compatible').read_bytes():
        raise RuntimeError('Not the supported Pixel')
    if Path('/etc/omarchy-mobile-pixel-root').read_text().strip() != 'v1':
        raise RuntimeError('Not the validated persistent Linux root')
    expected = set('persist efs efs_backup modem_userdata devinfo mfg_data fips modem_a modem_b'.split())
    seen = set()
    for entry in Path('/sys/class/block').iterdir():
        props = dict(line.split('=', 1) for line in (entry / 'uevent').read_text().splitlines() if '=' in line)
        name = props.get('PARTNAME')
        if name in expected:
            ro = subprocess.check_output(['blockdev', '--getro', '/dev/' + entry.name], text=True).strip()
            if ro != '1':
                raise RuntimeError('Protected partition is not read-only')
            seen.add(name)
    if seen != expected:
        raise RuntimeError('Protected partition inventory incomplete')


def verify_bundle():
    if not ENABLED.is_file():
        raise RuntimeError('Persistent modem service is not enabled')
    if ASSETS.stat().st_uid != 0 or ASSETS.stat().st_mode & 0o077:
        raise RuntimeError('Modem bundle must be root-owned and private')
    manifest = json.loads((ASSETS / 'manifest.json').read_text())
    if manifest['kernel'] != os.uname().release:
        raise RuntimeError('Modem modules do not match this kernel')
    for name in FILES:
        path = ASSETS / name
        if path.is_symlink() or not path.is_file() or path.stat().st_uid != 0 or path.stat().st_mode & 0o022:
            raise RuntimeError('Invalid private modem bundle file')
        with path.open('rb') as source:
            digest = hashlib.file_digest(source, 'sha256').hexdigest()
        if digest != manifest['sha256'].get(name):
            raise RuntimeError('Modem bundle checksum mismatch')
    protected_readonly()


def launch(name, *args):
    with (RUN / (name + '.log')).open('wb') as out:
        child = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=out, stderr=out,
                                 start_new_session=True)
    durable_json(RUN / (name + '.json'), {'pid': child.pid, 'boot': boot_id(),
                 'start': Path(f'/proc/{child.pid}/stat').read_text().split(') ', 1)[1].split()[19]})
    return child


def recorded_pid(name):
    try:
        data = json.loads((RUN / (name + '.json')).read_text())
        fields = Path(f"/proc/{data['pid']}/stat").read_text().split(') ', 1)[1].split()
        if fields[0] == 'Z':
            return None
        start = fields[19]
        if data['boot'] == boot_id() and data['start'] == start:
            return data['pid']
    except (FileNotFoundError, ProcessLookupError):
        pass
    return None


def cp_state():
    try:
        fd = os.open('/dev/umts_boot0', os.O_RDWR | os.O_CLOEXEC)
    except FileNotFoundError:
        return 0
    try:
        return fcntl.ioctl(fd, 0x6f27, 0)
    finally:
        os.close(fd)


def terminate(child, seconds):
    if child is None or child.poll() is not None:
        return
    child.terminate()
    child.wait(timeout=seconds)


def prepare():
    verify_bundle()
    if GUARD.exists():
        raise RuntimeError('An earlier modem session did not stop cleanly; automatic start skipped')
    if Path('/sys/module/pixel_gpio').exists() or Path('/sys/module/cpif').exists():
        raise RuntimeError('GPIO preparation requires a fresh boot before audio')
    durable_json(GUARD, {'boot': boot_id(), 'stage': 'preparing'})
    watchdog = launch('watchdog', str(ASSETS / 'wdtkick'))
    time.sleep(.3)
    if watchdog.poll() is not None:
        raise RuntimeError('Modem watchdog failed to start')
    command('insmod', str(ASSETS / 'pixel-wdt-pm.ko'))
    command('insmod', str(ASSETS / 'pixel-gpio.ko'), 'modem=1')
    durable_json(GUARD, {'boot': boot_id(), 'stage': 'gpio-ready'})
    print('Modem GPIO and watchdog ready; audio may start', flush=True)


def shutdown_children(app, network, runtime):
    # The app gets time to release a call and restore PCM/mixer state. Network
    # rollback precedes CP shutdown. Never kill the RFS namespace from here.
    try:
        terminate(app, 120)
    finally:
        try:
            terminate(network, 30)
        finally:
            terminate(runtime, 60)
    if cp_state() != 0:
        raise RuntimeError('CP remains online; RFS and watchdog retained')


def run():
    verify_bundle()
    if json.loads(GUARD.read_text()) != {'boot': boot_id(), 'stage': 'gpio-ready'}:
        raise RuntimeError('Modem GPIO preparation was not completed in this boot')
    if not recorded_pid('watchdog'):
        raise RuntimeError('Modem watchdog is not running')
    if LAB.exists():
        raise RuntimeError('Existing modem runtime; refusing to replace it')
    stopping = False
    def stop(*_):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    durable_json(RUN / 'manager.json', {'pid': os.getpid(), 'boot': boot_id(),
                 'start': Path('/proc/self/stat').read_text().split(') ', 1)[1].split()[19]})
    runtime = app = network = None
    try:
        durable_json(GUARD, {'boot': boot_id(), 'stage': 'starting'})
        for module in ('pixel-pcie-cp.ko', 'shm_ipc.ko', 'cpif.ko'):
            if stopping:
                return
            command('insmod', str(ASSETS / module))
        command(sys.executable, str(HERE / 'prepare-runtime.py'), str(ASSETS / 'runtime.tar'), timeout=120)
        runtime = launch('runtime', sys.executable, '-u', str(HERE / 'runtime.py'), '--continuous')
        deadline = time.monotonic() + 50
        while cp_state() != 4:
            if stopping:
                return
            if runtime.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError('Modem firmware did not start')
            time.sleep(.25)
        app = launch('apps', sys.executable, '-u', str(HERE / 'app_service.py'), '--bringup')
        deadline = time.monotonic() + 900
        while not Path('/run/omarchy-mobile-telephony/socket').exists():
            if stopping:
                return
            if app.poll() is not None or runtime.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError('Modem registration did not complete')
            time.sleep(.5)
        network = launch('network', sys.executable, '-u', str(HERE / 'data_network.py'))
        durable_json(GUARD, {'boot': boot_id(), 'stage': 'running'})
        print('Persistent modem service running', flush=True)
        while not stopping:
            if any(child.poll() is not None for child in (runtime, app, network)):
                raise RuntimeError('A modem service exited; stopping the session')
            if not recorded_pid('watchdog'):
                raise RuntimeError('Modem watchdog exited')
            time.sleep(.5)
    finally:
        shutdown_children(app, network, runtime)
        # Even on a handled failure leave the marker for diagnosis. Only an
        # explicit orderly shutdown enables another automatic boot attempt.
        if stopping:
            GUARD.unlink(missing_ok=True)
            os.sync()
        (RUN / 'manager.json').unlink(missing_ok=True)


def stop():
    pid = recorded_pid('manager')
    if pid:
        os.kill(pid, signal.SIGTERM)
        deadline = time.monotonic() + 220
        while recorded_pid('manager'):
            if time.monotonic() >= deadline:
                raise RuntimeError('Modem shutdown incomplete; reboot not requested')
            time.sleep(.25)
    if cp_state() != 0 or recorded_pid('runtime'):
        raise RuntimeError('Modem runtime still active; retaining watchdog')
    pid = recorded_pid('watchdog')
    if pid:
        os.kill(pid, signal.SIGTERM)
        time.sleep(2)
        if recorded_pid('watchdog'):
            raise RuntimeError('Watchdog did not disarm')
    # prepare may have succeeded before the normal boot reached run.
    if GUARD.exists() and json.loads(GUARD.read_text()).get('stage') == 'gpio-ready':
        GUARD.unlink()
        os.sync()
    if ASSETS.is_dir():
        durable_json(ASSETS / 'last-stop.json', {
            'boot': boot_id(), 'cp_offline': True,
            'apps_stopped': recorded_pid('apps') is None,
            'network_stopped': recorded_pid('network') is None,
            'runtime_stopped': True, 'watchdog_stopped': True,
            'guard_cleared': not GUARD.exists()})
    print('Modem stopped; watchdog disarmed', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('prepare', 'run', 'stop', 'verify'))
    args = parser.parse_args()
    os.umask(0o077)
    RUN.mkdir(mode=0o700, exist_ok=True)
    if args.action in ('stop', 'verify'):
        {'stop': stop, 'verify': verify_bundle}[args.action]()
        return
    lock = os.open(RUN / 'manager.lock', os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    try:
        {'prepare': prepare, 'run': run}[args.action]()
    finally:
        os.close(lock)


if __name__ == '__main__':
    main()
