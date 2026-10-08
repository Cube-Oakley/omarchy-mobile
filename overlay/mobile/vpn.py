#!/usr/bin/env python3
"""VPN connections: NetworkManager's (OpenVPN, WireGuard and others), on and off.

  status              {vpns: [{name, kind, state, saved, address}]}
  on NAME | off NAME
  password NAME       keep the password read from stdin with the connection
  forget NAME         drop the kept password

A connection comes from importing its profile as root (nmcli connection
import), so the profile, its keys and the server's name stay out of this
repository and readable by root only; the password is kept the same way
(password-flags 0), given through libnm rather than on a command line.
Prints one JSON object.
"""
import json
import subprocess
import sys
import warnings

# libnm's commit_changes is deprecated in name only; its replacement is async.
warnings.filterwarnings('ignore', category=DeprecationWarning)

KINDS = ('vpn', 'wireguard')


def client():
    import gi
    gi.require_version('NM', '1.0')
    from gi.repository import NM
    return NM, NM.Client.new(None)


def kind_of(connection):
    if connection.get_connection_type() == 'wireguard':
        return 'wireguard'
    setting = connection.get_setting_vpn()
    service = setting.get_service_type() if setting else ''
    return service.rsplit('.', 1)[-1] or 'vpn'


def saved(NM, connection):
    """Whether the connection can start without asking: WireGuard keeps its
    key; a VPN that wants a password needs one kept (flags 0)."""
    setting = connection.get_setting_vpn()
    if setting is None:
        return True
    if setting.get_data_item('connection-type') not in ('password', 'password-tls'):
        return True
    if setting.get_data_item('password-flags') not in (None, '0'):
        return False
    try:
        secrets = connection.get_secrets('vpn', None)
    except Exception:
        return False
    found = secrets.unpack().get('vpn', {}).get('secrets', {}) if secrets is not None else {}
    return bool(found.get('password'))


def state_of(NM, active):
    if active is None:
        return 'off'
    if isinstance(active, NM.VpnConnection):
        state = active.get_vpn_state()
        if state == NM.VpnConnectionState.ACTIVATED:
            return 'on'
        if state in (NM.VpnConnectionState.FAILED, NM.VpnConnectionState.DISCONNECTED):
            return 'off'
        return 'connecting'
    state = active.get_state()
    if state == NM.ActiveConnectionState.ACTIVATED:
        return 'on'
    if state in (NM.ActiveConnectionState.ACTIVATING,):
        return 'connecting'
    return 'off'


def address(active):
    if active is None:
        return ''
    config = active.get_ip4_config()
    addresses = config.get_addresses() if config else []
    return addresses[0].get_address() if addresses else ''


def status():
    NM, nm = client()
    active = {a.get_uuid(): a for a in nm.get_active_connections()}
    vpns = []
    for connection in nm.get_connections():
        if connection.get_connection_type() not in KINDS:
            continue
        running = active.get(connection.get_uuid())
        vpns.append({'name': connection.get_id(), 'kind': kind_of(connection), 'state': state_of(NM, running),
                     'saved': saved(NM, connection), 'address': address(running)})
    vpns.sort(key=lambda v: v['name'].casefold())
    return {'ok': True, 'vpns': vpns}


def find(nm, name):
    for connection in nm.get_connections():
        if connection.get_connection_type() in KINDS and connection.get_id() == name:
            return connection
    raise ValueError('There is no VPN of that name')


def nmcli(*args, wait=45):
    result = subprocess.run(['nmcli', '--wait', str(wait), *args], capture_output=True, text=True, timeout=wait + 15)
    if result.returncode != 0:
        lines = (result.stderr or result.stdout).strip().splitlines()
        raise RuntimeError(lines[-1].removeprefix('Error: ') if lines else 'NetworkManager refused')


def turn(name, on):
    NM, nm = client()
    connection = find(nm, name)
    if on:
        if not saved(NM, connection):
            raise ValueError('Enter the VPN password first')
        nmcli('connection', 'up', 'uuid', connection.get_uuid())
    else:
        nmcli('connection', 'down', 'uuid', connection.get_uuid(), wait=15)
    return status()


def keep_password(name, password):
    NM, nm = client()
    connection = find(nm, name)
    setting = connection.get_setting_vpn()
    if setting is None:
        raise ValueError('This VPN takes no password')
    if password:
        setting.add_secret('password', password)
        setting.add_data_item('password-flags', '0')
    else:
        setting.remove_secret('password')
        setting.add_data_item('password-flags', '2')
    connection.commit_changes(True, None)
    return status()


def main(argv):
    try:
        if argv in ([], ['status']):
            result = status()
        elif len(argv) == 2 and argv[0] in ('on', 'off'):
            result = turn(argv[1], argv[0] == 'on')
        elif len(argv) == 2 and argv[0] == 'password':
            password = sys.stdin.readline().rstrip('\n')
            if not password:
                raise ValueError('The password is empty')
            result = keep_password(argv[1], password)
        elif len(argv) == 2 and argv[0] == 'forget':
            result = keep_password(argv[1], '')
        else:
            print(__doc__.strip(), file=sys.stderr)
            return 2
    except Exception as error:      # libnm's GLib errors included
        result = {'ok': False, 'error': str(error) or 'The VPN did not answer'}
    print(json.dumps(result))
    return 0 if result.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
