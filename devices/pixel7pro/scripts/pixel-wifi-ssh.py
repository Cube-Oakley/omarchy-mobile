#!/usr/bin/env python3
"""Opt-in, key-only SSH recovery on one configured Wi-Fi IPv4 address.

Run `pixel-wifi-ssh enable ADDRESS` once. The boot helper and NetworkManager
dispatcher reconcile the listener when that address appears. USB SSH stays
independent; no listener is created on the cellular address.
"""
import argparse
import fcntl
import ipaddress
import json
import os
from pathlib import Path
import signal
import subprocess
import time

POLICY = Path('/etc/ssh/pixel-wifi-address')
CONFIG = Path('/run/sshd_config.pixel-wifi')
PID = Path('/run/sshd-pixel-wifi.pid')
SSHD = '/usr/bin/sshd'


def wifi_has_address(address):
    links = json.loads(subprocess.check_output(['ip', '-j', '-4', 'addr'], text=True))
    return any(Path('/sys/class/net', link['ifname'], 'wireless').is_dir()
               and any(a.get('local') == address for a in link.get('addr_info', []))
               for link in links)


def stop():
    try:
        pid = int(PID.read_text())
        # Never signal a reused PID belonging to another service.
        cmd = Path(f'/proc/{pid}/cmdline').read_bytes()
        if str(CONFIG).encode() in cmd and 'sshd' in os.readlink(f'/proc/{pid}/exe'):
            os.kill(pid, signal.SIGTERM)
    except (FileNotFoundError, ProcessLookupError):
        pass
    PID.unlink(missing_ok=True)


def reconcile():
    with open('/run/pixel-wifi-ssh.lock', 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return reconcile_locked()


def reconcile_locked():
    if not POLICY.exists():
        return False
    address = str(ipaddress.IPv4Address(POLICY.read_text().strip()))
    if not wifi_has_address(address):
        stop()
        return False
    try:
        pid = int(PID.read_text())
        if (str(CONFIG).encode() in Path(f'/proc/{pid}/cmdline').read_bytes()
                and 'sshd' in os.readlink(f'/proc/{pid}/exe')):
            return True
    except (FileNotFoundError, ProcessLookupError):
        pass
    CONFIG.write_text(f'''Port 22
ListenAddress {address}
HostKey /etc/ssh/ssh_host_ed25519_key.pixel
PidFile {PID}
PermitRootLogin prohibit-password
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
AuthenticationMethods publickey
UsePAM no
AllowUsers root
AllowTcpForwarding no
AllowAgentForwarding no
X11Forwarding no
PermitTunnel no
Subsystem sftp internal-sftp
''')
    CONFIG.chmod(0o600)
    subprocess.run([SSHD, '-t', '-f', str(CONFIG)], check=True)
    subprocess.run([SSHD, '-f', str(CONFIG), '-E', '/run/sshd-pixel-wifi.log'], check=True)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('enable', 'disable', 'refresh', 'boot'))
    parser.add_argument('address', nargs='?')
    args = parser.parse_args()
    if args.action == 'enable':
        if not args.address:
            parser.error('enable requires a Wi-Fi IPv4 address')
        address = str(ipaddress.IPv4Address(args.address))
        if not wifi_has_address(address):
            parser.error('Address must currently belong to a Wi-Fi interface')
        stop()
        POLICY.write_text(address + '\n')
        POLICY.chmod(0o600)
    elif args.action == 'disable':
        POLICY.unlink(missing_ok=True)
        stop()
        return
    # Only boot waits for DHCP. No permanent polling or idle battery wakeups.
    deadline = time.monotonic() + (120 if args.action == 'boot' else 0)
    while POLICY.exists():
        if reconcile() or time.monotonic() >= deadline:
            break
        time.sleep(2)


if __name__ == '__main__':
    main()
