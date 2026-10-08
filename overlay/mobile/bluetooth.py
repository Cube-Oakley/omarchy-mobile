#!/usr/bin/env python3
"""Bluetooth for the mobile shell, through BlueZ (bluetoothd on the system bus).

Usage: bluetooth.py [status] | power on|off | scan [SECONDS]
       | pair|connect|disconnect|forget ADDRESS
Prints one JSON object: the status, plus ok and message for actions. Status
reads BlueZ's whole object tree in one D-Bus call. Actions use bluetoothctl,
except pairing.

Pairing is this process as BlueZ's agent for its own Pair call (with
display, keyboard and yes/no: LE Secure Connections compare a code, keyboards
type one). Before the final status line it prints one JSON line per thing the
user must see or answer, {"event": "request", "kind": KIND, "code": ...}, with
KIND confirm (the six digits must match the device's), authorize, passkey
(the six digits the device shows) or pin; or {"event": "display", "code",
"entered"}: digits to type on the device. Each request is answered with a
line on stdin: yes, no, or the digits. "cancel" (or closing stdin) stops the
pairing. Without PyGObject it pairs as before, with bluetoothctl and no
input or output (headphones, speakers, mice and controllers only).
A board without a running controller can name a start command in device.json
("bluetoothStart"); "power on" runs it first.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ADDRESS = re.compile(r'^[0-9A-F]{2}(:[0-9A-F]{2}){5}$')
DEVICE_CONFIG = Path(os.environ.get('XDG_CONFIG_HOME', Path.home() / '.config')) / 'omarchy-mobile/device.json'
USAGE = 'Usage: bluetooth.py [status] | power on|off | scan [SECONDS] | pair|connect|disconnect|forget ADDRESS'


def value(props, key, default=None):
    """Unwrap busctl's {"type": ..., "data": ...} variant."""
    entry = props.get(key)
    return entry.get('data', default) if isinstance(entry, dict) else default


def start_command():
    try:
        command = json.loads(DEVICE_CONFIG.read_text()).get('bluetoothStart')
    except (OSError, ValueError, AttributeError):
        return None
    return command if isinstance(command, str) and os.access(command, os.X_OK) else None


def unavailable(reason):
    return {'available': False, 'reason': reason, 'startable': start_command() is not None,
            'name': '', 'address': '', 'powered': None, 'discovering': False, 'devices': []}


def snapshot(objects):
    """Status from GetManagedObjects' a{oa{sa{sv}}} data (busctl --json=short)."""
    adapters = sorted(path for path, ifaces in objects.items() if 'org.bluez.Adapter1' in ifaces)
    if not adapters:
        return unavailable('No Bluetooth controller.')
    adapter_path = adapters[0]
    adapter = objects[adapter_path]['org.bluez.Adapter1']
    devices = []
    for path, ifaces in objects.items():
        props = ifaces.get('org.bluez.Device1')
        if props is None or not path.startswith(adapter_path + '/'):
            continue
        address = value(props, 'Address', '')
        name = value(props, 'Name')
        battery = value(ifaces.get('org.bluez.Battery1', {}), 'Percentage')
        devices.append({
            'address': address,
            'name': value(props, 'Alias') or name or address,
            # BlueZ aliases a nameless device to its address; the UI hides those.
            'named': bool(name),
            'paired': bool(value(props, 'Paired', False)),
            'trusted': bool(value(props, 'Trusted', False)),
            'connected': bool(value(props, 'Connected', False)),
            'icon': value(props, 'Icon', ''),
            'rssi': value(props, 'RSSI'),
            'battery': battery,
        })
    devices.sort(key=lambda d: (not d['connected'], not d['paired'],
                                -(d['rssi'] if d['rssi'] is not None else -999), d['name'].lower()))
    return {
        'available': True,
        'reason': '',
        'startable': False,
        'name': value(adapter, 'Alias') or value(adapter, 'Name', ''),
        'address': value(adapter, 'Address', ''),
        'powered': bool(value(adapter, 'Powered', False)),
        'discovering': bool(value(adapter, 'Discovering', False)),
        'devices': devices,
    }


def read_status():
    if not shutil.which('busctl'):
        return unavailable('busctl is not installed.')
    result = subprocess.run(['busctl', '--system', '--json=short', 'call', 'org.bluez', '/',
                             'org.freedesktop.DBus.ObjectManager', 'GetManagedObjects'],
                            capture_output=True, text=True, timeout=5)
    if result.returncode != 0:
        return unavailable('Bluetooth is not running.')
    return snapshot(json.loads(result.stdout)['data'][0])


def ctl(*args, timeout=10):
    """Run bluetoothctl; returns (ok, last meaningful output line)."""
    result = subprocess.run(['bluetoothctl', *args], capture_output=True, text=True, timeout=timeout + 10)
    lines = [re.sub(r'\x1b\[[0-9;]*m', '', line).strip() for line in (result.stdout + result.stderr).splitlines()]
    lines = [line for line in lines if line and not line.startswith(('[CHG]', '[NEW]', '[DEL]', 'Agent'))]
    return result.returncode == 0, lines[-1] if lines else ''


def reason(line, name):
    """A readable failure for bluetoothctl's last line."""
    if not line or line.startswith('Attempting') or re.search(r'page-timeout|Host is down|not available', line, re.I):
        return f"Couldn't reach {name}. Make sure it is on and nearby."
    return line


def device(status, address):
    return next((d for d in status['devices'] if d['address'] == address), None)


def power(on):
    status = read_status()
    if not status['available'] and on:
        command = start_command()
        if not command:
            return status, False, status['reason']
        subprocess.run([command], capture_output=True, text=True, timeout=90)
        for _ in range(40):
            status = read_status()
            if status['available']:
                break
            time.sleep(0.5)
        if not status['available']:
            return status, False, 'Bluetooth did not start.'
    ok, line = ctl('power', 'on' if on else 'off')
    return read_status(), ok, '' if ok else line


AGENT_PATH = '/org/omarchy/mobile/bluetooth/pairing'
AGENT_XML = """<node><interface name="org.bluez.Agent1">
  <method name="Release"/>
  <method name="RequestPinCode"><arg type="o" direction="in"/><arg type="s" direction="out"/></method>
  <method name="DisplayPinCode"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
  <method name="RequestPasskey"><arg type="o" direction="in"/><arg type="u" direction="out"/></method>
  <method name="DisplayPasskey"><arg type="o" direction="in"/><arg type="u" direction="in"/><arg type="q" direction="in"/></method>
  <method name="RequestConfirmation"><arg type="o" direction="in"/><arg type="u" direction="in"/></method>
  <method name="RequestAuthorization"><arg type="o" direction="in"/></method>
  <method name="AuthorizeService"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
  <method name="Cancel"/>
</interface></node>"""
# BlueZ's pairing errors, as the user should read them.
PAIR_ERRORS = {
    'org.bluez.Error.AuthenticationFailed': "The codes didn't match, or {name} turned the pairing down.",
    'org.bluez.Error.AuthenticationRejected': '{name} turned the pairing down.',
    'org.bluez.Error.AuthenticationCanceled': 'Pairing cancelled.',
    'org.bluez.Error.AuthenticationTimeout': "{name} didn't answer in time.",
    'org.bluez.Error.ConnectionAttemptFailed': "Couldn't reach {name}. Make sure it is on and nearby.",
}


def answer(kind, line):
    """The reply to an agent request from the user's line: ('ok', value) or
    ('reject', None). value is None for yes/no requests."""
    line = line.strip()
    if kind in ('confirm', 'authorize'):
        return ('ok', None) if line == 'yes' else ('reject', None)
    if kind == 'passkey' and re.fullmatch(r'[0-9]{1,6}', line):
        return 'ok', int(line)
    if kind == 'pin' and re.fullmatch(r'[0-9A-Za-z]{1,16}', line):
        return 'ok', line
    return 'reject', None


def pair_error(error, name):
    """A readable message for a failed Pair call (GLib.Error)."""
    remote = getattr(error, 'message', str(error))
    match = re.match(r'GDBus\.Error:([\w.]+):', remote)
    template = PAIR_ERRORS.get(match.group(1) if match else '')
    return template.format(name=name) if template else remote.split(': ', 1)[-1] or 'Pairing failed.'


def pair(address):
    try:
        import gi
        gi.require_version('Gio', '2.0')
        from gi.repository import Gio, GLib
    except (ImportError, ValueError):
        return pair_without_agent(address)
    return Pairing(address, Gio, GLib).run()


class Pairing:
    """One Pair call with this process as its agent; requests go to stdout
    and their answers come from stdin, while the main loop keeps serving
    BlueZ."""

    def __init__(self, address, Gio, GLib):
        self.Gio, self.GLib = Gio, GLib
        self.address = address
        self.bus = Gio.bus_get_sync(Gio.BusType.SYSTEM)
        self.loop = GLib.MainLoop()
        self.pending = None
        self.cancelled = False
        self.result = None
        self.name = address
        self.device = None
        self.buffer = ''

    def emit(self, **event):
        print(json.dumps(event), flush=True)

    def call(self, path, interface, method, args=None, reply=None, timeout=25000):
        return self.bus.call_sync('org.bluez', path, interface, method, args,
                                  self.GLib.VariantType(reply) if reply else None,
                                  self.Gio.DBusCallFlags.NONE, timeout, None)

    def objects(self):
        return self.call('/', 'org.freedesktop.DBus.ObjectManager', 'GetManagedObjects',
                         reply='(a{oa{sa{sv}}})').unpack()[0]

    def find(self):
        """The adapter and the device's object path (None while unknown)."""
        objects = self.objects()
        adapters = sorted(path for path, ifaces in objects.items() if 'org.bluez.Adapter1' in ifaces)
        if not adapters:
            raise OSError('No Bluetooth controller.')
        path = adapters[0] + '/dev_' + self.address.replace(':', '_')
        info = objects.get(path, {}).get('org.bluez.Device1')
        if info:
            self.name = info.get('Alias') or info.get('Name') or self.address
        return adapters[0], path if info else None

    def agent_call(self, conn, sender, path, interface, method, params, invocation):
        args = params.unpack()
        if method == 'RequestConfirmation':
            self.ask('confirm', invocation, code='%06d' % args[1])
        elif method == 'RequestAuthorization':
            self.ask('authorize', invocation)
        elif method == 'RequestPasskey':
            self.ask('passkey', invocation)
        elif method == 'RequestPinCode':
            self.ask('pin', invocation)
        elif method in ('DisplayPasskey', 'DisplayPinCode'):
            code = '%06d' % args[1] if method == 'DisplayPasskey' else args[1]
            self.emit(event='display', code=code, entered=args[2] if method == 'DisplayPasskey' else 0)
            invocation.return_value(None)
        elif method == 'Cancel':
            self.pending = None
            self.emit(event='cancelled')
            invocation.return_value(None)
        else:
            # Release, and AuthorizeService for the device being paired.
            invocation.return_value(None)

    def ask(self, kind, invocation, **extra):
        self.pending = (kind, invocation)
        self.emit(event='request', kind=kind, name=self.name, **extra)

    def reply(self, line):
        if line.strip() == 'cancel':
            self.cancel()
            return
        if not self.pending:
            return
        kind, invocation = self.pending
        self.pending = None
        verdict, data = answer(kind, line)
        if verdict == 'reject':
            invocation.return_dbus_error('org.bluez.Error.Rejected', 'Declined')
        elif kind == 'passkey':
            invocation.return_value(self.GLib.Variant('(u)', (data,)))
        elif kind == 'pin':
            invocation.return_value(self.GLib.Variant('(s)', (data,)))
        else:
            invocation.return_value(None)

    def cancel(self):
        self.cancelled = True
        if self.pending:
            self.pending[1].return_dbus_error('org.bluez.Error.Canceled', 'Cancelled')
            self.pending = None
        if self.device:
            self.bus.call('org.bluez', self.device, 'org.bluez.Device1', 'CancelPairing', None, None,
                          self.Gio.DBusCallFlags.NONE, 5000, None, None)

    def stdin(self, fd, condition):
        data = os.read(fd, 4096) if condition & self.GLib.IO_IN else b''
        if not data:
            self.cancel()
            return False
        self.buffer += data.decode(errors='replace')
        while '\n' in self.buffer:
            line, self.buffer = self.buffer.split('\n', 1)
            self.reply(line)
        return True

    def paired(self, bus, result):
        try:
            bus.call_finish(result)
            self.result = None
        except self.GLib.Error as error:
            self.result = error
        self.loop.quit()

    def run(self):
        adapter, self.device = self.find()
        if self.device is None:
            # BlueZ forgets unpaired devices shortly after a scan ends; find it again.
            self.call(adapter, 'org.bluez.Adapter1', 'StartDiscovery')
            try:
                for _ in range(20):
                    time.sleep(0.5)
                    adapter, self.device = self.find()
                    if self.device:
                        break
            finally:
                try:
                    self.call(adapter, 'org.bluez.Adapter1', 'StopDiscovery')
                except self.GLib.Error:
                    pass
            if self.device is None:
                return read_status(), False, "Couldn't find the device. Make sure it is in pairing mode."
        node = self.Gio.DBusNodeInfo.new_for_xml(AGENT_XML)
        register = getattr(self.bus, 'register_object_with_closures2', None) or self.bus.register_object
        registration = register(AGENT_PATH, node.interfaces[0], self.agent_call, None, None)
        self.call('/org/bluez', 'org.bluez.AgentManager1', 'RegisterAgent',
                  self.GLib.Variant('(os)', (AGENT_PATH, 'KeyboardDisplay')))
        watch = self.GLib.io_add_watch(sys.stdin.fileno(), self.GLib.PRIORITY_DEFAULT,
                                       self.GLib.IO_IN | self.GLib.IO_HUP | self.GLib.IO_ERR, self.stdin)
        try:
            self.bus.call('org.bluez', self.device, 'org.bluez.Device1', 'Pair', None, None,
                          self.Gio.DBusCallFlags.NONE, 90000, None, self.paired)
            self.loop.run()
        finally:
            self.GLib.source_remove(watch)
            try:
                self.call('/org/bluez', 'org.bluez.AgentManager1', 'UnregisterAgent',
                          self.GLib.Variant('(o)', (AGENT_PATH,)))
            except self.GLib.Error:
                pass
            self.bus.unregister_object(registration)
        error = self.result
        if error is not None and 'AlreadyExists' not in error.message:
            return read_status(), False, 'Pairing cancelled.' if self.cancelled else pair_error(error, self.name)
        ctl('trust', self.address)
        ok, line = ctl('--timeout', '15', 'connect', self.address, timeout=15)
        status = read_status()
        found = device(status, self.address)
        if found and found['connected']:
            return status, True, f"Connected to {found['name']}."
        return status, bool(found and found['paired']), 'Paired. ' + reason(line, self.name)


def pair_without_agent(address):
    if device(read_status(), address) is None:
        # BlueZ forgets unpaired devices shortly after a scan ends; find it again.
        ctl('--timeout', '8', 'scan', 'on', timeout=8)
    ok, line = ctl('--agent', 'NoInputNoOutput', '--timeout', '25', 'pair', address, timeout=25)
    found = device(read_status(), address)
    if not found or not found['paired']:
        return read_status(), False, line or 'Pairing failed.'
    ctl('trust', address)
    ok, line = ctl('--timeout', '15', 'connect', address, timeout=15)
    status = read_status()
    found = device(status, address)
    if found and found['connected']:
        return status, True, f"Connected to {found['name']}."
    return status, False, 'Paired. ' + reason(line, found['name'] if found else 'the device')


def act(args):
    action = args[0] if args else 'status'
    if action == 'status' and len(args) <= 1:
        return read_status(), True, ''
    if action == 'power' and len(args) == 2 and args[1] in ('on', 'off'):
        return power(args[1] == 'on')
    if action == 'scan' and len(args) <= 2:
        seconds = int(args[1]) if len(args) == 2 and args[1].isdigit() else 12
        seconds = max(3, min(seconds, 30))
        ok, line = ctl('--timeout', str(seconds), 'scan', 'on', timeout=seconds)
        return read_status(), ok, '' if ok else line
    if action in ('pair', 'connect', 'disconnect', 'forget') and len(args) == 2:
        address = args[1].upper()
        if not ADDRESS.match(address):
            raise ValueError('Invalid Bluetooth address')
        if action == 'pair':
            return pair(address)
        command = {'connect': 'connect', 'disconnect': 'disconnect', 'forget': 'remove'}[action]
        ok, line = ctl('--timeout', '15', command, address, timeout=15)
        status = read_status()
        found = device(status, address)
        if action == 'connect':
            ok = bool(found and found['connected'])
        elif action == 'disconnect':
            ok = not (found and found['connected'])
        else:
            ok = found is None or not found['paired']
        if ok:
            return status, True, ''
        return status, False, reason(line, found['name'] if found else 'the device') if action == 'connect' \
            else line or f'Could not {action}.'
    raise ValueError(USAGE)


def main(args):
    try:
        status, ok, message = act(args)
        print(json.dumps({**status, 'ok': ok, 'message': message}))
        return 0 if ok else 1
    except (OSError, ValueError, KeyError, IndexError, subprocess.SubprocessError) as error:
        # A failed action must not make a working controller look unavailable.
        try:
            status = read_status()
        except (OSError, ValueError, KeyError, IndexError, subprocess.SubprocessError):
            status = unavailable(str(error))
        print(json.dumps({**status, 'ok': False, 'message': str(error)}))
        return 1


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
