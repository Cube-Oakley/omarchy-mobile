#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Supervise a prepared RAM-only CBD/RFS laboratory; no module loading or radio-enable commands.

Run manually after preparing /run/modem-lab and loading CPIF. Boots signed CP
firmware with the seed's radio state (verify CFUN before other actions). On
SIGTERM, deadline, or daemon failure, powers CP off before terminating RFS.
The managed boot service uses --continuous; manual runs retain a deadline.
"""
import argparse
import fcntl
import os
from pathlib import Path
import resource
import signal
import stat
import subprocess
import time

ROOT = Path('/run/modem-lab')
BOOT_FD = None


def cp_ioctl(command):
    global BOOT_FD
    if BOOT_FD is None:
        BOOT_FD = os.open('/dev/umts_boot0', os.O_RDWR | os.O_CLOEXEC)
    return fcntl.ioctl(BOOT_FD, command, 0)


def daemon(program, *args):
    def limits():
        resource.setrlimit(resource.RLIMIT_FSIZE, (8 * 1024 * 1024,) * 2)
    log = os.open(ROOT / (program + '-supervised.log'),
                  os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_CLOEXEC, 0o600)
    try:
        return subprocess.Popen([
            'unshare', '--mount', '--net', '--pid', '--fork', '--kill-child=KILL',
            '--mount-proc=' + str(ROOT / 'proc'), str(ROOT / 'enter-runtime'),
            str(ROOT), '/vendor/bin/' + program, *args],
            stdin=subprocess.DEVNULL, stdout=log, stderr=log,
            start_new_session=True, preexec_fn=limits)
    finally:
        os.close(log)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    duration = parser.add_mutually_exclusive_group()
    duration.add_argument('--seconds', type=int, default=3600)
    duration.add_argument('--continuous', action='store_true',
                          help='run until stopped, under the persistent service supervisor')
    args = parser.parse_args()
    if not 30 <= args.seconds <= 14400:
        parser.error('duration must be between 30 and 14400 seconds')
    if subprocess.check_output(['findmnt', '-n', '-o', 'FSTYPE', '--target', str(ROOT)], text=True).strip() != 'tmpfs':
        raise RuntimeError('laboratory must be tmpfs')
    if ROOT.stat().st_mode & 0o077:
        raise RuntimeError('laboratory must be private')
    os.umask(0o077)
    lock = os.open(ROOT / 'runtime.lock', os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    if cp_ioctl(0x6f27) != 0:
        raise RuntimeError('CP must be OFFLINE before starting the supervisor')
    for name in ('data-session.json', 'data-call-response.bin', 'ims-session.json', 'ims-response.bin',
                 'ims-registration.json'):
        (ROOT / name).unlink(missing_ok=True)
    for name in ('umts_boot0', 'umts_rfs0'):
        source = Path('/dev') / name
        info = source.stat()
        if not stat.S_ISCHR(info.st_mode):
            raise RuntimeError('expected modem character device')
        target = ROOT / 'dev' / name
        if target.exists():
            old = target.lstat()
            if not stat.S_ISCHR(old.st_mode) or old.st_rdev != info.st_rdev:
                raise RuntimeError('incorrect sandbox modem channel')
        else:
            os.mknod(target, stat.S_IFCHR | 0o600, info.st_rdev)
    stopping = False
    def stop(signum, frame):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    (ROOT / 'runtime.pid').write_text(str(os.getpid()) + '\n')
    started = []
    try:
        rfs = daemon('rfsd')
        started.append(rfs)
        time.sleep(2)
        if rfs.poll() is not None:
            raise RuntimeError('RFS exited before CP boot')
        cbd = daemon('cbd', '-t', 's5100sit', '-P', 'by-name/modem', '-s', '2', '-o', 'r')
        started.append(cbd)
        boot_deadline = time.monotonic() + 40
        while cbd.poll() is None and not stopping and time.monotonic() < boot_deadline:
            if rfs.poll() is not None:
                raise RuntimeError('RFS exited during CP boot')
            time.sleep(0.2)
        if stopping:
            return
        if cbd.poll() != 0 or cp_ioctl(0x6f27) != 4:
            raise RuntimeError('CBD failed or CP did not become ONLINE')
        print('CP ONLINE; supervised RAM-only RFS active', flush=True)
        deadline = None if args.continuous else time.monotonic() + args.seconds
        while not stopping and (deadline is None or time.monotonic() < deadline):
            if rfs.poll() is not None:
                raise RuntimeError('RFS exited; powering CP off')
            if cp_ioctl(0x6f27) != 4:
                raise RuntimeError('CP left ONLINE; stopping laboratory')
            time.sleep(0.5)
    finally:
        # Keep RFS alive if power-off fails; never deliberately remove it from
        # an ONLINE CP. A manual reboot is then required.
        cp_ioctl(0x6f20)
        if cp_ioctl(0x6f27) != 0:
            raise RuntimeError('CP did not stop; RFS retained, reboot required')
        print('CP OFFLINE; stopping laboratory daemons', flush=True)
        for process in reversed(started):
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
        (ROOT / 'runtime.pid').unlink(missing_ok=True)
        os.close(BOOT_FD)
        os.close(lock)


if __name__ == '__main__':
    main()
