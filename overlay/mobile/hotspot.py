#!/usr/bin/env python3
"""The phone's Wi-Fi hotspot: mobile data shared over Wi-Fi.

  status              {active, ssid, password, band, clients, ...}
  on | off
  set ssid NAME       2-32 characters
  set password WORDS  8-63 characters (WPA2)
  set band 2.4|5

A NetworkManager connection, omarchy-hotspot: an access point in shared mode
(DHCP and DNS from NetworkManager, 10.42.0.0/24). It is made the first time,
named after the phone with a random password. As on Android, the Wi-Fi radio
leaves the network it was on while it is an access point, and goes back when
the hotspot is off. A device adapter (~/.config/omarchy-mobile/hotspot-iface:
"create" prints the interface to use, "remove" drops it) can give the hotspot
an interface of its own; the Pixel's is a virtual one beside Wi-Fi, which then
stays connected. The way out to mobile data is the device's (on the Pixel,
NetworkManager's dispatcher masquerades the clients). Prints one JSON object.
"""
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys

NAME = 'omarchy-hotspot'
ADAPTER = Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config') / 'omarchy-mobile/hotspot-iface'


def adapter(action):
    """The device's hotspot interface (create) or none: '' without an adapter."""
    if not os.access(ADAPTER, os.X_OK):
        return ''
    result = subprocess.run([str(ADAPTER), action], capture_output=True, text=True, timeout=20)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or 'The hotspot interface could not be made')
    return result.stdout.strip()
ALPHABET = 'abcdefghjkmnpqrstuvwxyz23456789'


def nmcli(*args, wait=10, check=True):
    result = subprocess.run(['nmcli', '--wait', str(wait), '-t', *args], capture_output=True, text=True, timeout=wait + 15)
    if check and result.returncode != 0:
        raise RuntimeError((result.stderr or result.stdout).strip().splitlines()[-1] if (result.stderr or result.stdout).strip()
                           else 'NetworkManager refused')
    return result.stdout


def split(line):
    """nmcli -t fields: ':' separated, with '\\:' for a colon in a value."""
    return [part.replace('\x00', ':') for part in line.replace('\\:', '\x00').split(':')]


def wifi_device():
    for line in nmcli('-f', 'DEVICE,TYPE', 'device').splitlines():
        fields = split(line)
        if len(fields) >= 2 and fields[1] == 'wifi':
            return fields[0]
    return ''


def exists():
    return any(split(line)[0] == NAME for line in nmcli('-f', 'NAME', 'connection', 'show').splitlines())


def active():
    return any(split(line)[0] == NAME for line in nmcli('-f', 'NAME', 'connection', 'show', '--active').splitlines())


def default_ssid():
    # As Android names one: the system's name and four random characters.
    return 'Omarchy-' + ''.join(secrets.choice(ALPHABET.upper()) for _ in range(4))


def create(device):
    password = ''.join(secrets.choice(ALPHABET) for _ in range(12))
    nmcli('connection', 'add', 'type', 'wifi', 'ifname', device, 'con-name', NAME, 'autoconnect', 'no',
          'ssid', default_ssid(), '802-11-wireless.mode', 'ap', '802-11-wireless.band', 'bg',
          'ipv4.method', 'shared', 'ipv6.method', 'ignore',
          'wifi-sec.key-mgmt', 'wpa-psk', 'wifi-sec.proto', 'rsn', 'wifi-sec.pairwise', 'ccmp',
          'wifi-sec.group', 'ccmp', 'wifi-sec.psk', password)


def settings():
    values = {}
    out = nmcli('-s', '-f', '802-11-wireless.ssid,802-11-wireless.band,802-11-wireless-security.psk',
                'connection', 'show', NAME)
    for line in out.splitlines():
        key, _, value = line.partition(':')
        values[key] = value
    band = values.get('802-11-wireless.band', 'bg')
    return {'ssid': values.get('802-11-wireless.ssid', ''), 'password': values.get('802-11-wireless-security.psk', ''),
            'band': '5' if band == 'a' else '2.4'}


def clients(device):
    """Devices on the hotspot now: neighbours in its subnet that answered."""
    try:
        out = subprocess.run(['ip', '-4', 'neigh', 'show', 'dev', device], capture_output=True, text=True,
                             timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return 0
    return sum(1 for line in out.splitlines() if re.search(r'\b(REACHABLE|STALE|DELAY|PROBE)\b', line))


def status():
    device = wifi_device()
    if not device:
        return {'ok': True, 'available': False, 'active': False, 'error': 'No Wi-Fi'}
    result = {'ok': True, 'available': True, 'device': device, 'active': False, 'ssid': '',
              'password': '', 'band': '2.4', 'clients': 0}
    if exists():
        result.update(settings())
        result['active'] = active()
        if result['active']:
            result['clients'] = clients(device)
    return result


def turn(on):
    device = wifi_device()
    if not device:
        raise RuntimeError('No Wi-Fi')
    if on:
        nmcli('radio', 'wifi', 'on')
        device = adapter('create') or device
        if not exists():
            create(device)
        nmcli('connection', 'modify', NAME, 'connection.interface-name', device)
        nmcli('connection', 'up', NAME, 'ifname', device, wait=30)
    # Off, the adapter's interface stays: on the Pixel, deleting the virtual
    # access point drops the Wi-Fi connection beside it for several seconds.
    elif exists() and active():
        nmcli('connection', 'down', NAME)
    return status()


def change(field, value):
    device = wifi_device()
    if not device:
        raise RuntimeError('No Wi-Fi')
    if not exists():
        create(device)
    if field == 'ssid':
        if not 2 <= len(value.encode()) <= 32:
            raise ValueError('The name takes 2 to 32 characters')
        nmcli('connection', 'modify', NAME, '802-11-wireless.ssid', value)
    elif field == 'password':
        if not 8 <= len(value) <= 63 or not value.isprintable():
            raise ValueError('The password takes 8 to 63 characters')
        nmcli('connection', 'modify', NAME, 'wifi-sec.psk', value)
    elif field == 'band':
        if value not in ('2.4', '5'):
            raise ValueError('The band is 2.4 or 5')
        nmcli('connection', 'modify', NAME, '802-11-wireless.band', 'a' if value == '5' else 'bg')
    else:
        raise ValueError('Unknown setting')
    # A running hotspot takes the change when it starts again.
    if active():
        nmcli('connection', 'up', NAME, wait=30)
    return status()


def main(argv):
    try:
        if argv in ([], ['status']):
            result = status()
        elif argv == ['on']:
            result = turn(True)
        elif argv == ['off']:
            result = turn(False)
        elif len(argv) == 3 and argv[0] == 'set':
            result = change(argv[1], argv[2])
        else:
            print(__doc__.strip(), file=sys.stderr)
            return 2
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result = {'ok': False, 'error': str(error)}
    print(json.dumps(result))
    return 0 if result.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
