#!/usr/bin/env python3
"""Restart the native Pixel into fastboot or Linux without pressing buttons.

  pixel-reboot.py fastboot   Linux -> bootloader (fastboot)
  pixel-reboot.py linux      Linux or fastboot -> Linux from boot_a
  pixel-reboot.py wait       no restart: wait for Linux (e.g. after a RAM boot)

From Linux it runs the phone's `pixel-reboot` over USB SSH, so PID 1 performs
its orderly stop, sync and read-only remount. From fastboot it sends
`fastboot reboot`. It then waits for the target; for Linux it restores the
host side of the USB network and waits for SSH and the desktop.
Never flashes, erases or switches slots.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent
HOST_MAC = '02:70:07:00:00:02'
PROFILE_PREFIX = 'pixel-linux-usb-'
DEFAULT_SSH = ROOT / 'out/checkpoints/20260925-persistence-power/arch-session-f/ssh'


def phone_serial():
    """Target identity is private local state, never a checked-in handset serial."""
    serial = os.environ.get('PHONE_SERIAL', '')
    path = ROOT / 'out/device.serial'
    if not serial and path.is_file():
        serial = (path.read_text().split() or [''])[0]
    if not re.fullmatch(r'[A-Za-z0-9_-]+', serial):
        raise SystemExit('Set PHONE_SERIAL or put the target serial in ignored out/device.serial.')
    return serial


def run(args, timeout=30):
    return subprocess.run(args, text=True, capture_output=True, timeout=timeout)


def linux_interface():
    """Host interface of the native Linux USB gadget, or None."""
    for p in Path('/sys/class/net').iterdir():
        try:
            if (p / 'address').read_text().strip() != HOST_MAC:
                continue
            for parent in (p / 'device').resolve().parents:
                if ((parent / 'idVendor').is_file() and
                        (parent / 'idVendor').read_text().strip() == '0525' and
                        'pixel-' in (parent / 'manufacturer').read_text()):
                    return p.name
        except OSError:
            pass
    return None


def in_fastboot(serial):
    try:
        out = run(['fastboot', 'devices'], timeout=10).stdout
    except subprocess.TimeoutExpired:
        return False
    return any(line.split()[:2] == [serial, 'fastboot'] for line in out.splitlines())


def state(serial):
    if in_fastboot(serial):
        return 'fastboot'
    return 'linux' if linux_interface() else None


def wait_for(predicate, seconds, what):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(1)
    raise SystemExit(f'Timed out after {seconds} s waiting for {what}')


def network_ready(interface):
    out = run(['ip', '-4', '-o', 'addr', 'show', 'dev', interface]).stdout
    return ' 10.77.7.2/30 ' in out


def ensure_profile(interface):
    names = run(['nmcli', '-g', 'NAME', 'connection', 'show']).stdout.split('\n')
    profile = next((n for n in names if n.startswith(PROFILE_PREFIX)), None)
    if profile:
        return profile
    profile = PROFILE_PREFIX + uuid.uuid4().hex[:8]
    user = run(['id', '-un']).stdout.strip()
    r = run(['nmcli', 'connection', 'add', 'save', 'no', 'type', 'ethernet',
             'con-name', profile, 'ifname', interface,
             '802-3-ethernet.mac-address', HOST_MAC, 'connection.autoconnect', 'no',
             'connection.permissions', 'user:' + user,
             'ipv4.method', 'manual', 'ipv4.addresses', '10.77.7.2/30',
             'ipv4.never-default', 'yes', 'ipv6.method', 'disabled'])
    if r.returncode:
        raise SystemExit('Cannot create the host USB network profile: ' + r.stderr.strip())
    return profile


def network_up(seconds=40):
    """Activate the Pixel's host profile; another profile may claim the port first.
    The gadget occasionally re-enumerates early in boot, so look the interface
    up again and retry until the deadline.
    """
    deadline = time.monotonic() + seconds
    while True:
        interface = linux_interface()
        if interface and network_ready(interface):
            return
        if interface:
            profile = ensure_profile(interface)
            r = run(['nmcli', '--wait', '15', 'connection', 'up', 'id', profile,
                     'ifname', interface], timeout=30)
            if r.returncode == 0:
                return
            error = r.stderr.strip()
        else:
            error = 'the Linux USB gadget is not present'
        if time.monotonic() >= deadline:
            raise SystemExit('Cannot bring up the Pixel USB network: ' + error)
        time.sleep(2)


def ssh(wrapper, command, timeout=30):
    # The wrapper passes its arguments as the remote command, so no ssh options here.
    try:
        return run([str(wrapper), command], timeout=timeout)
    except subprocess.TimeoutExpired:
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('target', choices=['fastboot', 'linux', 'wait'])
    ap.add_argument('--ssh-wrapper', type=Path, default=DEFAULT_SSH)
    ap.add_argument('--timeout', type=int, default=180, help='seconds to wait for the target')
    a = ap.parse_args()
    serial = phone_serial()
    start = time.monotonic()
    current = state(serial)
    previous_boot = None
    print(f'Pixel is in {current or "no recognisable USB state"}', flush=True)
    if current is None and a.target != 'wait':
        raise SystemExit('Pixel not found on USB as fastboot or the Linux gadget')
    if a.target == 'fastboot' and current == 'fastboot':
        return 0

    if a.target == 'wait':
        pass
    elif current == 'fastboot':
        r = run(['fastboot', '-s', serial, 'reboot'])
        if r.returncode:
            raise SystemExit('fastboot reboot failed: ' + r.stderr.strip())
        wait_for(lambda: not in_fastboot(serial), 30, 'fastboot to go away')
    else:
        network_up()
        before = ssh(a.ssh_wrapper, 'cat /proc/sys/kernel/random/boot_id', timeout=15)
        if before is not None and before.returncode == 0:
            previous_boot = before.stdout.strip()
        r = ssh(a.ssh_wrapper, 'pixel-reboot ' + ('bootloader' if a.target == 'fastboot' else 'linux'))
        # PID 1 ends the SSH session while stopping, so only a refusal counts.
        if r is not None and 'pixel-reboot: restarting' not in r.stdout:
            raise SystemExit('Phone refused to restart: ' + (r.stderr or r.stdout).strip())
        # SSH can take longer to close than the phone takes to restart, so
        # the USB disconnect may already be over. A new boot ID establishes
        # a completed Linux restart even when that short interval was missed.
        if a.target == 'fastboot':
            wait_for(lambda: linux_interface() is None, 30, 'Linux to stop')

    if a.target == 'fastboot':
        wait_for(lambda: in_fastboot(serial), a.timeout, 'fastboot')
        print(f'Fastboot after {time.monotonic() - start:.0f} s', flush=True)
        return 0

    wait_for(lambda: linux_interface() is not None, a.timeout, 'the Linux USB gadget')
    boot = None
    deadline = time.monotonic() + 60
    while boot is None and time.monotonic() < deadline:
        network_up()
        r = ssh(a.ssh_wrapper, 'cat /proc/sys/kernel/random/boot_id', timeout=15)
        if r is not None and r.returncode == 0 and r.stdout.strip() != previous_boot:
            boot = r.stdout.strip()
        else:
            time.sleep(2)
    if boot is None:
        raise SystemExit('Linux gadget is up but SSH does not answer')
    print(f'Linux SSH after {time.monotonic() - start:.0f} s (boot {boot})', flush=True)
    wait_for(lambda: (ssh(a.ssh_wrapper, 'grep -q "persistent desktop startup complete" '
                                         '/proc/1/root/run/pixel-persistent.log') or
                      subprocess.CompletedProcess([], 1)).returncode == 0,
             a.timeout, 'the desktop startup')
    print(f'Desktop started after {time.monotonic() - start:.0f} s', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
