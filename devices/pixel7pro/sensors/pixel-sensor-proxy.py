#!/usr/bin/env python3
"""iio-sensor-proxy's D-Bus service (net.hadess.SensorProxy) for the Pixel's sensors.

The Pixel 7 Pro's sensors hang off the AoC, not Linux buses, so there is no
IIO device for iio-sensor-proxy to open. This daemon serves the same system
bus API from a sensor backend, so its clients (monitor-sensor, the shell's
rotation, light and proximity helpers) work unchanged:

  /net/hadess/SensorProxy          net.hadess.SensorProxy
      HasAccelerometer, AccelerometerOrientation, AccelerometerTilt,
      HasAmbientLight, LightLevelUnit, LightLevel, HasProximity, ProximityNear;
      Claim/Release{Accelerometer,Light,Proximity}
  /net/hadess/SensorProxy/Compass  net.hadess.SensorProxy.Compass
      HasCompass, CompassHeading (degrees from magnetic north); Claim/ReleaseCompass

As in iio-sensor-proxy, a sensor runs only while some client holds a claim,
and a client's claims end when it leaves the bus.

  /net/hadess/SensorProxy          org.omarchy.Mobile.SensorPower
      Pause, Resume (root only)

Pause stops every sensor and keeps the claims; Resume starts the claimed ones
again. pixel-suspend pauses for each system sleep: a running sensor, even a
non-wakeup one, has the AoC write its samples to DRAM, which takes the memory
interface out of its sleep state about 28 times a second (+17 mA in SYS_SLEEP).

  /org/omarchy/Mobile/Steps        org.omarchy.Mobile.Steps
      Enabled, Today; SetEnabled(b) (root or the session user)

Step counting, when enabled: the AoC's step counter counts by itself, with no
wake-ups, and keeps running through Pause (it reports only on change, at most
once a second, so a phone lying still writes nothing). Its cumulative
readings become rows of steps per minute in /var/lib/omarchy-mobile/steps/
steps.db, an append-only SQLite log that apps and syncs read (see
overlay/mobile/steps.py).

  pixel-sensor-proxy.py [--backend usf|test] [--device PATH]
"""
import argparse
import math
import os
import pwd
import signal
import sqlite3
import sys
import time
import uuid

import gi
gi.require_version('Gio', '2.0')
gi.require_version('GLibUnix', '2.0')
from gi.repository import Gio, GLib, GLibUnix  # noqa: E402

NAME = 'net.hadess.SensorProxy'
PATH = '/net/hadess/SensorProxy'
XML = '''<node>
  <interface name="net.hadess.SensorProxy">
    <method name="ClaimAccelerometer"/>
    <method name="ReleaseAccelerometer"/>
    <method name="ClaimLight"/>
    <method name="ReleaseLight"/>
    <method name="ClaimProximity"/>
    <method name="ReleaseProximity"/>
    <property name="HasAccelerometer" type="b" access="read"/>
    <property name="AccelerometerOrientation" type="s" access="read"/>
    <property name="AccelerometerTilt" type="s" access="read"/>
    <property name="HasAmbientLight" type="b" access="read"/>
    <property name="LightLevelUnit" type="s" access="read"/>
    <property name="LightLevel" type="d" access="read"/>
    <property name="HasProximity" type="b" access="read"/>
    <property name="ProximityNear" type="b" access="read"/>
  </interface>
</node>'''
POWER_XML = '''<node>
  <interface name="org.omarchy.Mobile.SensorPower">
    <method name="Pause"/>
    <method name="Resume"/>
  </interface>
</node>'''
COMPASS_XML = '''<node>
  <interface name="net.hadess.SensorProxy.Compass">
    <method name="ClaimCompass"/>
    <method name="ReleaseCompass"/>
    <property name="HasCompass" type="b" access="read"/>
    <property name="CompassHeading" type="d" access="read"/>
  </interface>
</node>'''

# iio-sensor-proxy's orientation thresholds (src/orientation.c).
THRESHOLD = 35
SAME_AXIS_LIMIT = 5


def orientation(prev, x, y, z):
    """iio-sensor-proxy's orientation_calc(), for a gravity vector in its frame:
    upright portrait reads y < 0, left edge up reads x > 0."""
    portrait = round(math.degrees(math.atan2(x, math.sqrt(y * y + z * z))))
    landscape = round(math.degrees(math.atan2(y, math.sqrt(x * x + z * z))))
    if abs(portrait) > THRESHOLD and abs(landscape) > THRESHOLD:
        return prev
    if abs(portrait) > THRESHOLD:
        ret = 'left-up' if portrait > 0 else 'right-up'
        if prev in ('left-up', 'right-up') and abs(portrait) < SAME_AXIS_LIMIT:
            return prev
        return ret
    if abs(landscape) > THRESHOLD:
        ret = 'bottom-up' if landscape > 0 else 'normal'
        if prev in ('bottom-up', 'normal') and abs(landscape) < SAME_AXIS_LIMIT:
            return prev
        return ret
    return prev


def tilt(x, y, z):
    """The screen's tilt, as iio-sensor-proxy names it, for a gravity vector in
    its frame (lying face up reads z < 0)."""
    g = math.sqrt(x * x + y * y + z * z) or 1.0
    angle = math.degrees(math.acos(max(-1.0, min(1.0, z / g))))
    if angle < 10:
        return 'face-down'
    if angle > 170:
        return 'face-up'
    if angle < 80:
        return 'tilted-down'
    if angle > 100:
        return 'tilted-up'
    return 'vertical'


class TestBackend:
    """Scripted readings, to exercise the D-Bus side without sensors."""
    has = {'accel': True, 'light': True, 'proximity': True, 'compass': True, 'steps': True}

    def __init__(self, report):
        self.report = report
        self.timers = {}
        self.step = 0

    def start(self, kind):
        self.timers[kind] = GLib.timeout_add(1000, self._tick, kind)

    def stop(self, kind):
        GLib.source_remove(self.timers.pop(kind))

    def _tick(self, kind):
        self.step += 1
        if kind == 'accel':
            # Android frame (reaction to gravity): upright, then on the left edge.
            ax, ay, az = [(0, 9.8, 0.5), (-9.8, 0, 0.5)][(self.step // 3) % 2]
            self.report('accel', (ax, ay, az))
        elif kind == 'light':
            self.report('light', 100.0 + self.step)
        elif kind == 'compass':
            self.report('compass', (10.0 * self.step) % 360)
        elif kind == 'steps':
            self.report('steps', [(int(time.time() * 1000), self.step * 2)])
        else:
            self.report('proximity', self.step % 4 < 2)
        return True


STEPS_DIR = '/var/lib/omarchy-mobile/steps'
STEPS_PATH = '/org/omarchy/Mobile/Steps'
STEPS_XML = '''<node>
  <interface name="org.omarchy.Mobile.Steps">
    <method name="SetEnabled"><arg name="enabled" type="b" direction="in"/></method>
    <property name="Enabled" type="b" access="read"/>
    <property name="Today" type="u" access="read"/>
  </interface>
</node>'''
STEPS_SCHEMA = '''
CREATE TABLE IF NOT EXISTS steps (
    id TEXT PRIMARY KEY,          -- random UUID, the same on every copy
    start_ms INTEGER NOT NULL,    -- the minute, ms since 1970 UTC
    end_ms INTEGER NOT NULL,
    count INTEGER NOT NULL,
    source TEXT NOT NULL,
    recorded_ms INTEGER NOT NULL  -- when this row was written: sync after it
);
CREATE INDEX IF NOT EXISTS steps_start ON steps(start_ms);
CREATE INDEX IF NOT EXISTS steps_recorded ON steps(recorded_ms);
CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, value TEXT NOT NULL);
'''


def session_uid():
    """The desktop user's uid (/etc/omarchy-mobile/session user=NAME), or None."""
    try:
        with open('/etc/omarchy-mobile/session') as f:
            for line in f:
                key, _, value = line.strip().partition('=')
                if key == 'user':
                    return pwd.getpwnam(value).pw_uid
    except (OSError, KeyError):
        pass
    return None


class Steps:
    """The step counter's readings, per minute, in steps.db; on while
    STEPS_DIR/enabled exists."""
    SOURCE = 'pixel7pro.aoc.step-counter'

    def __init__(self, bus, backend):
        self.bus = bus
        self.backend = backend
        self.bucket = None          # [minute start ms, steps]
        self.flush_timer = None
        os.makedirs(STEPS_DIR, mode=0o755, exist_ok=True)
        old = os.umask(0o022)       # readable by apps and syncs
        try:
            self.db = sqlite3.connect(os.path.join(STEPS_DIR, 'steps.db'))
            self.db.executescript(STEPS_SCHEMA)
            self.db.commit()
        finally:
            os.umask(old)
        row = self.db.execute("SELECT value FROM state WHERE key = 'last_raw'").fetchone()
        self.last_raw = int(row[0]) if row else None
        self.enabled = os.path.exists(os.path.join(STEPS_DIR, 'enabled')) and backend.has.get('steps', False)
        info = Gio.DBusNodeInfo.new_for_xml(STEPS_XML).interfaces[0]
        bus.register_object_with_closures2(STEPS_PATH, info, self._call, self._get, None)
        if self.enabled:
            GLib.idle_add(self._start)   # after the proxy is in place to take readings

    def _start(self):
        if self.enabled:
            self.backend.start('steps')
        return False

    def _get(self, conn, sender, path, iface, prop):
        return {'Enabled': GLib.Variant('b', self.enabled),
                'Today': GLib.Variant('u', self.today())}.get(prop)

    def _call(self, conn, sender, path, iface, method, params, invocation):
        try:
            uid = conn.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                                 'GetConnectionUnixUser', GLib.Variant('(s)', (sender,)),
                                 GLib.VariantType.new('(u)'), Gio.DBusCallFlags.NONE, -1, None).unpack()[0]
        except GLib.Error:
            uid = None
        if uid is None or uid not in (0, session_uid()):
            invocation.return_dbus_error('org.freedesktop.DBus.Error.AccessDenied', 'Root or the session user only')
            return
        if not self.backend.has.get('steps', False):
            invocation.return_dbus_error('net.hadess.SensorProxy.Error.NoSensor', 'No step counter')
            return
        self.set_enabled(params.unpack()[0])
        invocation.return_value(None)

    def set_enabled(self, on):
        if on == self.enabled:
            return
        flag = os.path.join(STEPS_DIR, 'enabled')
        if on:
            open(flag, 'w').close()
            self.backend.start('steps')
        else:
            self.backend.stop('steps')
            self.flush()
            os.unlink(flag)
        self.enabled = on
        self.bus.emit_signal(None, STEPS_PATH, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
                             GLib.Variant('(sa{sv}as)', ('org.omarchy.Mobile.Steps',
                                                         {'Enabled': GLib.Variant('b', on)}, [])))

    def feed(self, samples):
        """Cumulative readings (wall ms, steps since the counter started)."""
        for at_ms, raw in samples:
            if self.last_raw is None:
                self.last_raw = raw     # the first reading: a starting point
                continue
            # A lower reading: the counter started again (an AoC restart).
            delta = raw - self.last_raw if raw >= self.last_raw else raw
            self.last_raw = raw
            if delta <= 0:
                continue
            minute = at_ms // 60_000 * 60_000
            if self.bucket and self.bucket[0] != minute:
                self.flush()
            if not self.bucket:
                self.bucket = [minute, 0]
            self.bucket[1] += delta
        if self.bucket and self.flush_timer is None:
            self.flush_timer = GLib.timeout_add_seconds(60, self._flush_later)

    def _flush_later(self):
        self.flush_timer = None
        self.flush()
        return False

    def flush(self):
        if self.bucket and self.bucket[1] > 0:
            start = self.bucket[0]
            self.db.execute('INSERT INTO steps VALUES (?, ?, ?, ?, ?, ?)',
                            (str(uuid.uuid4()), start, start + 60_000, self.bucket[1], self.SOURCE,
                             int(time.time() * 1000)))
        self.bucket = None
        if self.last_raw is not None:
            self.db.execute("INSERT OR REPLACE INTO state VALUES ('last_raw', ?)", (str(self.last_raw),))
        self.db.commit()

    def today(self):
        lt = time.localtime()
        midnight = int(time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday, 0, 0, 0, 0, 0, -1)) * 1000)
        total = self.db.execute('SELECT COALESCE(SUM(count), 0) FROM steps WHERE start_ms >= ?',
                                (midnight,)).fetchone()[0]
        return total + (self.bucket[1] if self.bucket and self.bucket[0] >= midnight else 0)


class Proxy:
    def __init__(self, bus, backend):
        self.bus = bus
        self.backend = backend
        self.claims = {'accel': set(), 'light': set(), 'proximity': set(), 'compass': set()}
        self.paused = False
        self.state = {'AccelerometerOrientation': 'undefined', 'AccelerometerTilt': 'undefined',
                      'LightLevel': 0.0, 'ProximityNear': False, 'CompassHeading': -1.0}
        info = Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0]
        compass = Gio.DBusNodeInfo.new_for_xml(COMPASS_XML).interfaces[0]
        bus.register_object_with_closures2(PATH, info, self._call, self._get, None)
        bus.register_object_with_closures2(PATH + '/Compass', compass, self._compass_call,
                                           self._compass_get, None)
        power = Gio.DBusNodeInfo.new_for_xml(POWER_XML).interfaces[0]
        bus.register_object_with_closures2(PATH, power, self._power_call, None, None)
        self.steps = Steps(bus, backend)
        bus.signal_subscribe('org.freedesktop.DBus', 'org.freedesktop.DBus', 'NameOwnerChanged',
                             '/org/freedesktop/DBus', None, Gio.DBusSignalFlags.NONE,
                             self._owner_changed)

    def _get(self, conn, sender, path, iface, prop):
        has = self.backend.has
        values = {
            'HasAccelerometer': GLib.Variant('b', has['accel']),
            'AccelerometerOrientation': GLib.Variant('s', self.state['AccelerometerOrientation']),
            'AccelerometerTilt': GLib.Variant('s', self.state['AccelerometerTilt']),
            'HasAmbientLight': GLib.Variant('b', has['light']),
            'LightLevelUnit': GLib.Variant('s', 'lux'),
            'LightLevel': GLib.Variant('d', self.state['LightLevel']),
            'HasProximity': GLib.Variant('b', has['proximity']),
            'ProximityNear': GLib.Variant('b', self.state['ProximityNear']),
        }
        return values.get(prop)

    def _call(self, conn, sender, path, iface, method, params, invocation):
        action, _, sensor = method.partition('Claim') if method.startswith('Claim') \
            else method.partition('Release')
        kind = {'Accelerometer': 'accel', 'Light': 'light', 'Proximity': 'proximity'}[sensor]
        if not self.backend.has[kind]:
            invocation.return_dbus_error('net.hadess.SensorProxy.Error.NoSensor', f'No {sensor.lower()}')
            return
        if method.startswith('Claim'):
            self._claim(kind, sender)
        else:
            self._release(kind, sender)
        invocation.return_value(None)

    def _compass_get(self, conn, sender, path, iface, prop):
        return {'HasCompass': GLib.Variant('b', self.backend.has['compass']),
                'CompassHeading': GLib.Variant('d', self.state['CompassHeading'])}.get(prop)

    def _compass_call(self, conn, sender, path, iface, method, params, invocation):
        if not self.backend.has['compass']:
            invocation.return_dbus_error('net.hadess.SensorProxy.Error.NoSensor', 'No compass')
            return
        if method == 'ClaimCompass':
            self._claim('compass', sender)
        else:
            self._release('compass', sender)
        invocation.return_value(None)

    def _power_call(self, conn, sender, path, iface, method, params, invocation):
        try:
            uid = conn.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                                 'GetConnectionUnixUser', GLib.Variant('(s)', (sender,)),
                                 GLib.VariantType.new('(u)'), Gio.DBusCallFlags.NONE, -1, None).unpack()[0]
        except GLib.Error:
            uid = None
        if uid != 0:
            invocation.return_dbus_error('org.freedesktop.DBus.Error.AccessDenied', 'Root only')
            return
        if method == 'Pause':
            self.pause()
        else:
            self.resume()
        invocation.return_value(None)

    def pause(self):
        if self.paused:
            return
        self.paused = True
        for kind, holders in self.claims.items():
            if holders:
                self.backend.stop(kind)

    def resume(self):
        if not self.paused:
            return
        self.paused = False
        for kind, holders in self.claims.items():
            if holders:
                self.backend.start(kind)

    def _claim(self, kind, sender):
        first = not self.claims[kind]
        self.claims[kind].add(sender)
        if first and not self.paused:
            self.backend.start(kind)

    def _release(self, kind, sender):
        if sender in self.claims[kind]:
            self.claims[kind].discard(sender)
            if not self.claims[kind] and not self.paused:
                self.backend.stop(kind)

    def _owner_changed(self, conn, sender, path, iface, signal_name, params):
        name, _old, new = params.unpack()
        if new == '' and name.startswith(':'):
            for kind in self.claims:
                self._release(kind, name)

    def report(self, kind, value):
        if kind == 'steps':
            self.steps.feed(value)
            return
        changed = {}
        if kind == 'accel':
            ax, ay, az = value
            # Android frame -> iio-sensor-proxy's: the gravity vector itself.
            x, y, z = -ax, -ay, -az
            o = orientation(self.state['AccelerometerOrientation'], x, y, z)
            t = tilt(x, y, z)
            if o != self.state['AccelerometerOrientation']:
                changed['AccelerometerOrientation'] = GLib.Variant('s', o)
            if t != self.state['AccelerometerTilt']:
                changed['AccelerometerTilt'] = GLib.Variant('s', t)
        elif kind == 'light':
            if value != self.state['LightLevel']:
                changed['LightLevel'] = GLib.Variant('d', float(value))
        elif kind == 'proximity':
            if bool(value) != self.state['ProximityNear']:
                changed['ProximityNear'] = GLib.Variant('b', bool(value))
        elif kind == 'compass':
            # A tenth of a degree is below the fusion's accuracy.
            if abs(value - self.state['CompassHeading']) >= 0.1:
                changed['CompassHeading'] = GLib.Variant('d', float(value))
        if not changed:
            return
        for prop, variant in changed.items():
            self.state[prop] = variant.unpack()
        path, iface = (PATH + '/Compass', NAME + '.Compass') if kind == 'compass' else (PATH, NAME)
        self.bus.emit_signal(None, path, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
                             GLib.Variant('(sa{sv}as)', (iface, changed, [])))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--backend', choices=('usf', 'test'), default='usf')
    ap.add_argument('--device', default='/dev/acd-com.google.usf')
    a = ap.parse_args()
    bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    proxy = None

    def report(kind, value):
        proxy.report(kind, value)

    if a.backend == 'test':
        backend = TestBackend(report)
    else:
        from usf_backend import UsfBackend  # noqa: PLC0415
        backend = UsfBackend(a.device, report)
    proxy = Proxy(bus, backend)
    loop = GLib.MainLoop()

    def lost(conn, name):
        print(f'{NAME} is owned by another process', file=sys.stderr)
        loop.quit()

    Gio.bus_own_name_on_connection(bus, NAME, Gio.BusNameOwnerFlags.NONE, None, lost)
    GLibUnix.signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, loop.quit)
    GLibUnix.signal_add(GLib.PRIORITY_DEFAULT, signal.SIGINT, loop.quit)
    loop.run()
    proxy.steps.flush()


if __name__ == '__main__':
    main()
