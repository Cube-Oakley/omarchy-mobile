#!/usr/bin/env python3
"""Portable NetworkManager status and explicit user-requested Wi-Fi actions."""
import ipaddress
import json
import os
import re
import subprocess
import sys


def nmcli(*args, password=None, wait=3):
    return subprocess.run(['nmcli', '--wait', str(wait), '-t', *args],
                          input=None if password is None else password + '\n',
                          check=True, capture_output=True, text=True, timeout=wait + 5,
                          env=dict(os.environ, LC_ALL='C')).stdout


def split_fields(line):
    fields, word, escaped = [], '', False
    for ch in line:
        if escaped:
            word += ch
            escaped = False
        elif ch == '\\': escaped = True
        elif ch == ':': fields.append(word); word = ''
        else: word += ch
    fields.append(word)
    return fields


def interfaces():
    return [r[0] for r in (split_fields(l) for l in nmcli('-f', 'DEVICE,TYPE', 'device').splitlines())
            if len(r) == 2 and r[1] == 'wifi']


def detail(interface):
    text = nmcli('-f', 'GENERAL,IP4,IP6', 'device', 'show', interface)
    result = {}
    for line in text.splitlines():
        row = split_fields(line)
        if len(row) >= 2: result[row[0]] = ':'.join(row[1:])
    return result


def networks(interface):
    rows = []
    for line in nmcli('-f', 'IN-USE,BSSID,SSID,SIGNAL,SECURITY', 'device', 'wifi', 'list',
                      'ifname', interface, '--rescan', 'no').splitlines():
        r = split_fields(line)
        if len(r) != 5 or not r[2]: continue
        rows.append(dict(active=r[0] == '*', bssid=r[1], ssid=r[2],
                         signal=int(r[3]), security=r[4]))
    # Multiple APs for one SSID remain NetworkManager's choice after connection.
    unique = {}
    for row in sorted(rows, key=lambda r: (r['active'], r['signal']), reverse=True):
        unique.setdefault((row['ssid'], row['security']), row)
    return list(unique.values())


def radio_enabled(text=None):
    value = (nmcli('radio', 'wifi') if text is None else text).strip().split(':')[-1].strip().lower()
    if value not in ('enabled', 'disabled'):
        raise ValueError('Wi-Fi radio state unavailable')
    return value == 'enabled'


def empty_status(radio):
    return dict(available=True, radio=radio, interface='', connected=False, connection='', uuid='',
                ssid='', signal=None, ipv4=[], ipv6=[], gateway='', dns=[], dns_static=[],
                dns_auto=True, ipv4_method='', ipv4_static={})


def parse_dns(servers):
    if servers is None:
        return []
    if isinstance(servers, str):
        servers = servers.replace(',', ' ').split()
    if not isinstance(servers, list):
        raise ValueError('DNS servers must be a list')
    cleaned = []
    for value in servers:
        if not isinstance(value, str):
            raise ValueError('DNS server is not an address')
        text = value.strip()
        if not text:
            continue
        try:
            ipaddress.ip_address(text)
        except ValueError as error:
            raise ValueError('DNS server is not an address') from error
        cleaned.append(text)
        if len(cleaned) > 4:
            raise ValueError('At most four DNS servers')
    return cleaned


def static_ipv4(address, gateway):
    """A fixed address (a.b.c.d or a.b.c.d/prefix, /24 if none) and its gateway."""
    if not isinstance(address, str) or not isinstance(gateway, str):
        raise ValueError('Enter an address and a gateway')
    text = address.strip()
    try:
        interface = ipaddress.ip_interface(text if '/' in text else text + '/24')
        router = ipaddress.ip_address(gateway.strip())
    except ValueError as error:
        raise ValueError('That is not an IPv4 address') from error
    if interface.version != 4 or router.version != 4:
        raise ValueError('Static addresses are IPv4 here')
    network = interface.network
    if not 8 <= network.prefixlen <= 30:
        raise ValueError('The prefix must be between /8 and /30')
    if interface.ip in (network.network_address, network.broadcast_address):
        raise ValueError('That address is reserved in its network')
    if router not in network or router == interface.ip:
        raise ValueError('The gateway must be another address in the same network')
    return str(interface), str(router)


def connection_profile(name):
    if not name:
        return {}
    text = nmcli('-f', 'connection.uuid,ipv4.method,ipv4.addresses,ipv4.gateway,ipv4.dns,'
                 'ipv4.ignore-auto-dns,ipv6.method', 'connection', 'show', name)
    result = {}
    for line in text.splitlines():
        row = split_fields(line)
        if len(row) >= 2:
            result[row[0]] = ':'.join(row[1:])
    dns = [part for part in result.get('ipv4.dns', '').replace(',', ' ').split() if part]
    ignore = result.get('ipv4.ignore-auto-dns', '').lower() in ('yes', 'true', '1')
    addresses = [part for part in result.get('ipv4.addresses', '').replace(',', ' ').split() if part]
    return {
        'uuid': result.get('connection.uuid', ''),
        'ipv4_method': result.get('ipv4.method', ''),
        'ipv4_static': {'address': addresses[0] if addresses else '',
                        'gateway': result.get('ipv4.gateway', '').strip(), 'dns': dns},
        'dns_static': dns,
        'dns_auto': not ignore,
    }


def wifi_uuids():
    rows = []
    for line in nmcli('-f', 'UUID,TYPE,NAME', 'connection', 'show').splitlines():
        row = split_fields(line)
        if len(row) >= 2 and row[1] == '802-11-wireless':
            rows.append(row[0])
    return rows


def status():
    try:
        radio = radio_enabled()
    except (OSError, ValueError, subprocess.SubprocessError):
        radio = True
    devices = interfaces()
    if not devices:
        status = empty_status(radio)
        if not radio:
            return status
        status['available'] = False
        return status
    states = [(i, detail(i)) for i in devices]
    interface, info = next(((i, d) for i, d in states if d.get('GENERAL.STATE', '').startswith('100')),
                           states[0])
    connected = info.get('GENERAL.STATE', '').startswith('100')
    active = next((r for r in networks(interface) if r['active']), {}) if connected else {}
    values = lambda prefix: [v for k,v in info.items() if k.startswith(prefix) and v]
    connection = info.get('GENERAL.CONNECTION', '')
    profile = connection_profile(connection) if connected else {}
    return dict(available=True, radio=radio, interface=interface, connected=connected,
                connection=connection, uuid=profile.get('uuid', ''),
                ssid=active.get('ssid', ''), signal=active.get('signal'),
                ipv4=values('IP4.ADDRESS'), ipv6=values('IP6.ADDRESS'),
                gateway=info.get('IP4.GATEWAY', ''), dns=values('IP4.DNS') + values('IP6.DNS'),
                dns_static=profile.get('dns_static', []), dns_auto=profile.get('dns_auto', True),
                ipv4_method=profile.get('ipv4_method', ''), ipv4_static=profile.get('ipv4_static', {}))


def action(name, request):
    if name == 'radio':
        if request.get('enabled') is True:
            nmcli('radio', 'wifi', 'on', wait=10)
        elif request.get('enabled') is False:
            nmcli('radio', 'wifi', 'off', wait=10)
        else:
            raise ValueError('Wi-Fi radio request invalid')
        return {'ok': True}
    if name in ('dns', 'forget', 'ipv4'):
        uuid = request.get('uuid', '')
        if uuid not in wifi_uuids():
            raise ValueError('Wi-Fi connection unavailable')
        if name == 'ipv4':
            # Automatic is DHCP; Static fixes the address, gateway and DNS.
            if request.get('auto') is True:
                nmcli('connection', 'modify', uuid, 'ipv4.method', 'auto', 'ipv4.addresses', '',
                      'ipv4.gateway', '', wait=10)
            elif request.get('auto') is False:
                address, gateway = static_ipv4(request.get('address'), request.get('gateway'))
                servers = parse_dns(request.get('dns'))
                if not servers:
                    raise ValueError('Enter at least one DNS server')
                nmcli('connection', 'modify', uuid, 'ipv4.method', 'manual', 'ipv4.addresses', address,
                      'ipv4.gateway', gateway, 'ipv4.dns', ' '.join(servers),
                      'ipv4.ignore-auto-dns', 'yes', wait=10)
            else:
                raise ValueError('IP settings request invalid')
            nmcli('connection', 'up', uuid, wait=30)
            return {'ok': True}
        if name == 'forget':
            nmcli('connection', 'delete', uuid, wait=10)
            return {'ok': True}
        auto = request.get('auto')
        if auto is True:
            nmcli('connection', 'modify', uuid, 'ipv4.dns', '', 'ipv4.ignore-auto-dns', 'no', wait=10)
        elif auto is False:
            servers = parse_dns(request.get('servers'))
            if not servers:
                raise ValueError('Enter at least one DNS server')
            nmcli('connection', 'modify', uuid, 'ipv4.dns', ' '.join(servers),
                  'ipv4.ignore-auto-dns', 'yes', wait=10)
        else:
            raise ValueError('DNS request invalid')
        nmcli('connection', 'up', uuid, wait=20)
        return {'ok': True}
    interface = request.get('interface', '')
    if interface not in interfaces(): raise ValueError('Wi-Fi interface unavailable')
    if name in ('scan', 'list'):
        if name == 'scan': nmcli('device', 'wifi', 'rescan', 'ifname', interface, wait=10)
        return dict(ok=True, networks=networks(interface))
    if name == 'disconnect':
        nmcli('device', 'disconnect', interface, wait=10)
        return {'ok': True}
    if name == 'connect':
        bssid = request.get('bssid', '')
        if not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', bssid):
            raise ValueError('Choose a nearby network again')
        password = request.get('password', '')
        if not isinstance(password, str) or any(c in password for c in '\r\n\0'):
            raise ValueError('Password must be a single line')
        # --ask reads stdin: credentials never appear in argv, shell text or logs.
        nmcli('--ask', 'device', 'wifi', 'connect', bssid, 'ifname', interface,
              password=password, wait=35)
        return {'ok': True}
    raise ValueError('Unknown network action')


if __name__ == '__main__':
    try:
        result = status() if len(sys.argv) == 1 else action(sys.argv[1], json.load(sys.stdin))
    except ValueError as error:
        # Our own checks explain themselves; a failed connect stays generic.
        result = {'available': False} if len(sys.argv) == 1 else {'ok': False, 'error': (
            str(error) if sys.argv[1] in ('ipv4', 'dns') else
            'Network request failed. Check the password or try scanning again.')}
    except (OSError, subprocess.SubprocessError):
        result = {'available': False} if len(sys.argv) == 1 else {
            'ok': False, 'error': 'Network request failed. Check the password or try scanning again.'}
    print(json.dumps(result))
