#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bluetooth LE end to end, through BlueZ's D-Bus API: the OS-level check for
phone-as-key accessories (docs/apps-plan-20261002.md, BLE step 1).

  peripheral [SECONDS]   advertise and serve the test GATT service (the phone)
  central [SECONDS]      find it, connect, pair, read, write, subscribe (a PC)

The service (UUID SERVICE) has three characteristics:
- counter: read and notify; a count that rises each second while subscribed;
- inbox: write (with or without response); the peripheral prints what came;
- secret: encrypted, authenticated read only (LE Secure Connections with
  numeric comparison), so reading it proves the pairing.
Both sides register an agent that accepts the numeric comparison and prints
the six digits, which must match. Every step prints one JSON line. central
removes the pairing at the end; peripheral removes the central it paired with
when it exits. As root, both also print the connection's RSSI (the kernel's
management interface, Get Connection Information), which BlueZ's D-Bus API
does not give for a connected device.
"""
import json
import os
import select
import socket
import struct
import sys
import time

import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib  # noqa: E402

BUS = 'org.bluez'
SERVICE = '6a8f0001-6b4f-4e2c-9a4b-6f6d61726368'
COUNTER = '6a8f0002-6b4f-4e2c-9a4b-6f6d61726368'
INBOX = '6a8f0003-6b4f-4e2c-9a4b-6f6d61726368'
SECRET = '6a8f0004-6b4f-4e2c-9a4b-6f6d61726368'
NAME = 'omarchy-ble'
ROOT = '/org/omarchy/blecheck'

INTROSPECTION = """
<node>
  <interface name="org.freedesktop.DBus.ObjectManager">
    <method name="GetManagedObjects"><arg type="a{oa{sa{sv}}}" direction="out"/></method>
  </interface>
  <interface name="org.bluez.GattService1">
    <property name="UUID" type="s" access="read"/>
    <property name="Primary" type="b" access="read"/>
  </interface>
  <interface name="org.bluez.GattCharacteristic1">
    <property name="UUID" type="s" access="read"/>
    <property name="Service" type="o" access="read"/>
    <property name="Flags" type="as" access="read"/>
    <method name="ReadValue"><arg type="a{sv}" direction="in"/><arg type="ay" direction="out"/></method>
    <method name="WriteValue"><arg type="ay" direction="in"/><arg type="a{sv}" direction="in"/></method>
    <method name="StartNotify"/>
    <method name="StopNotify"/>
  </interface>
  <interface name="org.bluez.LEAdvertisement1">
    <method name="Release"/>
    <property name="Type" type="s" access="read"/>
    <property name="ServiceUUIDs" type="as" access="read"/>
    <property name="LocalName" type="s" access="read"/>
    <property name="Discoverable" type="b" access="read"/>
    <property name="Includes" type="as" access="read"/>
  </interface>
  <interface name="org.bluez.Agent1">
    <method name="Release"/>
    <method name="RequestPinCode"><arg type="o" direction="in"/><arg type="s" direction="out"/></method>
    <method name="DisplayPinCode"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
    <method name="RequestPasskey"><arg type="o" direction="in"/><arg type="u" direction="out"/></method>
    <method name="DisplayPasskey"><arg type="o" direction="in"/><arg type="u" direction="in"/><arg type="q" direction="in"/></method>
    <method name="RequestConfirmation"><arg type="o" direction="in"/><arg type="u" direction="in"/></method>
    <method name="RequestAuthorization"><arg type="o" direction="in"/></method>
    <method name="AuthorizeService"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
    <method name="Cancel"/>
  </interface>
</node>
"""
NODE = Gio.DBusNodeInfo.new_for_xml(INTROSPECTION)
# GLib 2.84 renamed the call (the old one mishandles the invocation's reference).
REGISTER = 'register_object_with_closures2' if hasattr(Gio.DBusConnection, 'register_object_with_closures2') \
    else 'register_object'
started = time.monotonic()


def say(event, **data):
    print(json.dumps(dict(t=round(time.monotonic() - started, 3), event=event, **data)), flush=True)


MGMT_GET_CONN_INFO = 0x0031
MGMT_COMMAND_COMPLETE = 0x0001
MGMT_COMMAND_STATUS = 0x0002
HCI_CHANNEL_CONTROL = 3


def connection_rssi(address, kind, index=0):
    """The connection's RSSI in dBm, or None (not root, not connected)."""
    if os.geteuid() != 0:
        return None
    raw = bytes(reversed(bytes.fromhex(address.replace(':', ''))))
    params = raw + bytes([2 if kind == 'random' else 1])  # LE random or public
    try:
        with socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI) as mgmt:
            mgmt.bind((0xffff, HCI_CHANNEL_CONTROL))
            mgmt.send(struct.pack('<HHH', MGMT_GET_CONN_INFO, index, len(params)) + params)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                if not select.select([mgmt], [], [], 0.2)[0]:
                    continue
                reply = mgmt.recv(64)
                event, _, _ = struct.unpack_from('<HHH', reply)
                if event not in (MGMT_COMMAND_COMPLETE, MGMT_COMMAND_STATUS) or \
                        struct.unpack_from('<H', reply, 6)[0] != MGMT_GET_CONN_INFO:
                    continue
                # A refusal (Command Status) carries no RSSI.
                ok = event == MGMT_COMMAND_COMPLETE and reply[8] == 0
                return struct.unpack_from('<b', reply, 16)[0] if ok else None
    except OSError:
        return None
    return None


def call(bus, path, interface, method, args=None, reply=None, timeout=25000):
    return bus.call_sync(BUS, path, interface, method, args, GLib.VariantType(reply) if reply else None,
                         Gio.DBusCallFlags.NONE, timeout, None)


def call_serving(bus, path, interface, method, args=None, timeout=25000):
    """A call during which BlueZ may call back into this process (the agent
    during pairing): the main context keeps running until the reply."""
    done = []
    bus.call(BUS, path, interface, method, args, None, Gio.DBusCallFlags.NONE, timeout, None,
             lambda conn, result: done.append(result))
    context = GLib.MainContext.default()
    while not done:
        context.iteration(True)
    return bus.call_finish(done[0])


def adapter_path(bus):
    objects = call(bus, '/', 'org.freedesktop.DBus.ObjectManager', 'GetManagedObjects',
                   reply='(a{oa{sa{sv}}})').unpack()[0]
    for path, interfaces in objects.items():
        if 'org.bluez.Adapter1' in interfaces:
            return path, objects
    raise SystemExit('No Bluetooth adapter')


def set_property(bus, path, interface, name, value):
    call(bus, path, 'org.freedesktop.DBus.Properties', 'Set', GLib.Variant('(ssv)', (interface, name, value)))


class Agent:
    """Accepts numeric comparison (the six digits printed on both sides)."""

    def __init__(self, bus):
        self.bus = bus
        self.path = ROOT + '/agent'
        getattr(bus, REGISTER)(self.path, NODE.lookup_interface('org.bluez.Agent1'), self.method, None, None)
        call(bus, '/org/bluez', 'org.bluez.AgentManager1', 'RegisterAgent',
             GLib.Variant('(os)', (self.path, 'DisplayYesNo')))
        call(bus, '/org/bluez', 'org.bluez.AgentManager1', 'RequestDefaultAgent', GLib.Variant('(o)', (self.path,)))

    def method(self, conn, sender, path, interface, name, params, invocation):
        args = params.unpack()
        if name == 'RequestConfirmation':
            say('pairing-code', device=args[0], code='%06d' % args[1])
            invocation.return_value(None)
        elif name == 'DisplayPasskey':
            say('pairing-code', device=args[0], code='%06d' % args[1])
            invocation.return_value(None)
        elif name in ('RequestAuthorization', 'AuthorizeService'):
            say('authorize', device=args[0])
            invocation.return_value(None)
        elif name == 'RequestPasskey':
            invocation.return_dbus_error('org.bluez.Error.Rejected', 'No keyboard')
        elif name == 'RequestPinCode':
            invocation.return_dbus_error('org.bluez.Error.Rejected', 'LE only')
        else:
            invocation.return_value(None)


class Peripheral:
    def __init__(self, bus, adapter):
        self.bus = bus
        self.adapter = adapter
        self.count = 0
        self.notifying = False
        self.service = ROOT + '/service0'
        self.chars = {
            ROOT + '/service0/counter': (COUNTER, ['read', 'notify']),
            ROOT + '/service0/inbox': (INBOX, ['write', 'write-without-response']),
            ROOT + '/service0/secret': (SECRET, ['encrypt-authenticated-read']),
        }
        self.advert = ROOT + '/advert0'
        self.peers = set()
        manager = NODE.lookup_interface('org.freedesktop.DBus.ObjectManager')
        getattr(bus, REGISTER)(ROOT, manager, self.manager_call, None, None)
        getattr(bus, REGISTER)(self.service, NODE.lookup_interface('org.bluez.GattService1'), None, self.service_property, None)
        for path in self.chars:
            getattr(bus, REGISTER)(path, NODE.lookup_interface('org.bluez.GattCharacteristic1'),
                                self.char_call, self.char_property, None)
        getattr(bus, REGISTER)(self.advert, NODE.lookup_interface('org.bluez.LEAdvertisement1'),
                            self.advert_call, self.advert_property, None)

    def objects(self):
        out = {self.service: {'org.bluez.GattService1': {
            'UUID': GLib.Variant('s', SERVICE), 'Primary': GLib.Variant('b', True)}}}
        for path, (uuid, flags) in self.chars.items():
            out[path] = {'org.bluez.GattCharacteristic1': {
                'UUID': GLib.Variant('s', uuid), 'Service': GLib.Variant('o', self.service),
                'Flags': GLib.Variant('as', flags)}}
        return out

    def manager_call(self, conn, sender, path, interface, name, params, invocation):
        invocation.return_value(GLib.Variant('(a{oa{sa{sv}}})', (self.objects(),)))

    def service_property(self, conn, sender, path, interface, name):
        return self.objects()[self.service]['org.bluez.GattService1'].get(name)

    def char_property(self, conn, sender, path, interface, name):
        return self.objects()[path]['org.bluez.GattCharacteristic1'].get(name)

    def value(self):
        return list(self.count.to_bytes(4, 'little'))

    def char_call(self, conn, sender, path, interface, name, params, invocation):
        options = params.unpack()[-1] if params.n_children() else {}
        device = options.get('device', '') if isinstance(options, dict) else ''
        if device:
            self.peers.add(device)
        if name == 'ReadValue':
            data = self.value() if path.endswith('counter') else list(b'unlocked')
            say('read', characteristic=path.rsplit('/', 1)[1], device=device)
            invocation.return_value(GLib.Variant('(ay)', (data,)))
        elif name == 'WriteValue':
            data = bytes(params.unpack()[0])
            say('write', characteristic='inbox', device=device, value=data.decode(errors='replace'))
            invocation.return_value(None)
        elif name == 'StartNotify':
            self.notifying = True
            say('subscribed')
            invocation.return_value(None)
        elif name == 'StopNotify':
            self.notifying = False
            say('unsubscribed')
            invocation.return_value(None)

    def tick(self):
        self.count += 1
        if self.notifying:
            self.bus.emit_signal(None, ROOT + '/service0/counter', 'org.freedesktop.DBus.Properties',
                                 'PropertiesChanged', GLib.Variant('(sa{sv}as)', (
                                     'org.bluez.GattCharacteristic1', {'Value': GLib.Variant('ay', self.value())}, [])))
        return True

    def advert_property(self, conn, sender, path, interface, name):
        return {'Type': GLib.Variant('s', 'peripheral'), 'ServiceUUIDs': GLib.Variant('as', [SERVICE]),
                'LocalName': GLib.Variant('s', NAME), 'Discoverable': GLib.Variant('b', True),
                'Includes': GLib.Variant('as', ['tx-power'])}.get(name)

    def advert_call(self, conn, sender, path, interface, name, params, invocation):
        say('advertisement-released')
        invocation.return_value(None)

    def start(self, failed):
        """Registers without blocking: BlueZ asks for the objects meanwhile."""
        def registered(event, data, then=None):
            def done(bus, result):
                try:
                    bus.call_finish(result)
                except GLib.Error as error:
                    say(event + '-failed', error=error.message)
                    failed()
                    return
                say(event, **data)
                if then:
                    then()
            return done
        advertise = lambda: self.bus.call(  # noqa: E731
            BUS, self.adapter, 'org.bluez.LEAdvertisingManager1', 'RegisterAdvertisement',
            GLib.Variant('(oa{sv})', (self.advert, {})), None, Gio.DBusCallFlags.NONE, 25000, None,
            registered('advertising', {'name': NAME}))
        self.bus.call(BUS, self.adapter, 'org.bluez.GattManager1', 'RegisterApplication',
                      GLib.Variant('(oa{sv})', (ROOT, {})), None, Gio.DBusCallFlags.NONE, 25000, None,
                      registered('gatt-registered', {'service': SERVICE}, advertise))

    def stop(self):
        for method, path in (('UnregisterAdvertisement', self.advert), ('UnregisterApplication', ROOT)):
            manager = 'org.bluez.LEAdvertisingManager1' if 'Advert' in method else 'org.bluez.GattManager1'
            try:
                call(self.bus, self.adapter, manager, method, GLib.Variant('(o)', (path,)))
            except GLib.Error:
                pass
        for device in self.peers:
            try:
                call(self.bus, self.adapter, 'org.bluez.Adapter1', 'RemoveDevice', GLib.Variant('(o)', (device,)))
                say('pairing-removed', device=device)
            except GLib.Error:
                pass


def device_changed(conn, sender, path, interface, signal, params):
    """Connections and pairings as BlueZ sees them; while a central is
    connected, its RSSI every 2 s (as root)."""
    changed = {k: v for k, v in params.unpack()[1].items() if k in ('Connected', 'Paired', 'Bonded')}
    if changed:
        say('device', device=path, **changed)
    if changed.get('Connected') and os.geteuid() == 0:
        GLib.timeout_add_seconds(2, report_rssi, conn, path)


def report_rssi(conn, path):
    try:
        props = call(conn, path, 'org.freedesktop.DBus.Properties', 'GetAll',
                     GLib.Variant('(s)', ('org.bluez.Device1',)), '(a{sv})').unpack()[0]
    except GLib.Error:
        return False
    if not props.get('Connected'):
        return False
    say('rssi', device=path, dbm=connection_rssi(props['Address'], props.get('AddressType', 'public')))
    return True


def peripheral(seconds):
    bus = Gio.bus_get_sync(Gio.BusType.SYSTEM)
    adapter, _ = adapter_path(bus)
    for name in ('Powered', 'Pairable'):
        set_property(bus, adapter, 'org.bluez.Adapter1', name, GLib.Variant('b', True))
    Agent(bus)
    server = Peripheral(bus, adapter)
    bus.signal_subscribe(BUS, 'org.freedesktop.DBus.Properties', 'PropertiesChanged', None, 'org.bluez.Device1',
                         Gio.DBusSignalFlags.NONE, device_changed)
    loop = GLib.MainLoop()
    server.start(loop.quit)
    GLib.timeout_add_seconds(1, server.tick)
    GLib.timeout_add_seconds(seconds, loop.quit)
    try:
        loop.run()
    finally:
        server.stop()
    return 0


def central(seconds):
    bus = Gio.bus_get_sync(Gio.BusType.SYSTEM)
    adapter, _ = adapter_path(bus)
    Agent(bus)
    deadline = time.monotonic() + seconds
    # LE only; the service is matched here. A UUID filter crashed BlueZ 5.87's
    # bluetoothd (SIGSEGV in discovery) on the laptop when the phone appeared.
    call(bus, adapter, 'org.bluez.Adapter1', 'SetDiscoveryFilter',
         GLib.Variant('(a{sv})', ({'Transport': GLib.Variant('s', 'le')},)))
    call(bus, adapter, 'org.bluez.Adapter1', 'StartDiscovery')
    say('scanning')
    device = rssi = None
    context = GLib.MainContext.default()
    while device is None and time.monotonic() < deadline:
        context.iteration(False)
        objects = call(bus, '/', 'org.freedesktop.DBus.ObjectManager', 'GetManagedObjects',
                       reply='(a{oa{sa{sv}}})').unpack()[0]
        for path, interfaces in objects.items():
            info = interfaces.get('org.bluez.Device1')
            if info and (SERVICE in info.get('UUIDs', []) or info.get('Name') == NAME) and 'RSSI' in info:
                device, rssi = path, info['RSSI']
        time.sleep(0.2)
    call(bus, adapter, 'org.bluez.Adapter1', 'StopDiscovery')
    if device is None:
        say('not-found')
        return 1
    say('found', device=device, rssi=rssi)
    t = time.monotonic()
    call_serving(bus, device, 'org.bluez.Device1', 'Connect')
    while not call(bus, device, 'org.freedesktop.DBus.Properties', 'Get',
                   GLib.Variant('(ss)', ('org.bluez.Device1', 'ServicesResolved')), '(v)').unpack()[0]:
        time.sleep(0.1)
        if time.monotonic() > deadline:
            say('services-not-resolved')
            return 1
    say('connected', ms=round((time.monotonic() - t) * 1000))
    props = call(bus, device, 'org.freedesktop.DBus.Properties', 'GetAll',
                 GLib.Variant('(s)', ('org.bluez.Device1',)), '(a{sv})').unpack()[0]
    link = connection_rssi(props['Address'], props.get('AddressType', 'public'))
    if link is not None:
        say('rssi', dbm=link)
    objects = call(bus, '/', 'org.freedesktop.DBus.ObjectManager', 'GetManagedObjects',
                   reply='(a{oa{sa{sv}}})').unpack()[0]
    chars = {info['org.bluez.GattCharacteristic1']['UUID']: path for path, info in objects.items()
             if path.startswith(device + '/') and 'org.bluez.GattCharacteristic1' in info}
    result = {'found': True, 'rssi': rssi, 'link_rssi': link}
    # The other side may ask for pairing at any request: the agent must be
    # served meanwhile.
    value = bytes(call_serving(bus, chars[COUNTER], 'org.bluez.GattCharacteristic1', 'ReadValue',
                               GLib.Variant('(a{sv})', ({},))).unpack()[0])
    say('read', characteristic='counter', value=int.from_bytes(value, 'little'))
    call_serving(bus, chars[INBOX], 'org.bluez.GattCharacteristic1', 'WriteValue',
                 GLib.Variant('(aya{sv})', (list(b'hello from the central'), {})))
    say('wrote', characteristic='inbox')
    notes = []
    sub = bus.signal_subscribe(BUS, 'org.freedesktop.DBus.Properties', 'PropertiesChanged', chars[COUNTER], None,
                               Gio.DBusSignalFlags.NONE,
                               lambda *a: notes.append(a[5].unpack()[1]['Value']) if 'Value' in a[5].unpack()[1] else None)
    call_serving(bus, chars[COUNTER], 'org.bluez.GattCharacteristic1', 'StartNotify')
    end = time.monotonic() + 4
    while time.monotonic() < end:
        context.iteration(False)
        time.sleep(0.05)
    call(bus, chars[COUNTER], 'org.bluez.GattCharacteristic1', 'StopNotify')
    bus.signal_unsubscribe(sub)
    say('notified', count=len(notes))
    result['notifications'] = len(notes)
    t = time.monotonic()
    try:
        try:
            call_serving(bus, device, 'org.bluez.Device1', 'Pair', timeout=60000)
        except GLib.Error as error:
            # The other side may have asked for pairing first (the laptop's
            # BlueZ does), and the agent has already accepted it.
            if 'AlreadyExists' not in error.message:
                raise
        say('paired', ms=round((time.monotonic() - t) * 1000))
        secret = bytes(call_serving(bus, chars[SECRET], 'org.bluez.GattCharacteristic1', 'ReadValue',
                                    GLib.Variant('(a{sv})', ({},))).unpack()[0])
        say('read', characteristic='secret', value=secret.decode(errors='replace'))
        result['secure_read'] = secret == b'unlocked'
    except GLib.Error as error:
        say('pairing-failed', error=error.message)
        result['secure_read'] = False
    call(bus, device, 'org.bluez.Device1', 'Disconnect')
    call(bus, adapter, 'org.bluez.Adapter1', 'RemoveDevice', GLib.Variant('(o)', (device,)))
    say('done', **result)
    return 0 if result.get('secure_read') and result['notifications'] >= 2 else 1


def main(args):
    if len(args) in (1, 2) and args[0] in ('peripheral', 'central'):
        seconds = int(args[1]) if len(args) == 2 else 120
        return (peripheral if args[0] == 'peripheral' else central)(seconds)
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
