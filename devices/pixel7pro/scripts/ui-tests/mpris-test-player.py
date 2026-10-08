#!/usr/bin/env python3
"""A fake MPRIS player for testing the shell's media card (test only).

Publishes org.mpris.MediaPlayer2.omarchytest on the session bus with one
track, a running position and the usual controls; every control call is
printed. Run inside the session (as-cube.sh) for SECONDS (default 120).

    mpris-test-player.py [SECONDS]
"""
import sys, time
import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

ROOT, PLAYER = 'org.mpris.MediaPlayer2', 'org.mpris.MediaPlayer2.Player'
PROPS = 'org.freedesktop.DBus.Properties'
LENGTH = 215 * 1_000_000


class Player(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, '/org/mpris/MediaPlayer2')
        self.playing = True
        self.track = 1
        self.offset = 0
        self.since = time.monotonic()

    def position(self):
        if not self.playing:
            return self.offset
        return min(LENGTH, self.offset + int((time.monotonic() - self.since) * 1_000_000))

    def metadata(self):
        return dbus.Dictionary({
            'mpris:trackid': dbus.ObjectPath(f'/org/omarchy/track/{self.track}'),
            'mpris:length': dbus.Int64(LENGTH),
            'xesam:title': f'Test track {self.track}',
            'xesam:artist': dbus.Array(['Omarchy test player'], signature='s'),
            'xesam:album': 'Media card test',
        }, signature='sv')

    def player_props(self):
        return {
            'PlaybackStatus': 'Playing' if self.playing else 'Paused',
            'Metadata': self.metadata(),
            'Position': dbus.Int64(self.position()),
            'CanGoNext': True, 'CanGoPrevious': True, 'CanPlay': True, 'CanPause': True,
            'CanSeek': True, 'CanControl': True, 'Rate': 1.0, 'Volume': 1.0,
            'MinimumRate': 1.0, 'MaximumRate': 1.0, 'LoopStatus': 'None', 'Shuffle': False,
        }

    def changed(self, *names):
        props = self.player_props()
        self.PropertiesChanged(PLAYER, {n: props[n] for n in names}, [])

    @dbus.service.method(PROPS, in_signature='ss', out_signature='v')
    def Get(self, interface, name):
        return self.GetAll(interface)[name]

    @dbus.service.method(PROPS, in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface):
        if interface == ROOT:
            return {'Identity': 'Test player', 'DesktopEntry': 'omarchy-test-player', 'CanQuit': False,
                    'CanRaise': False, 'HasTrackList': False,
                    'SupportedUriSchemes': dbus.Array([], signature='s'),
                    'SupportedMimeTypes': dbus.Array([], signature='s')}
        return self.player_props()

    @dbus.service.method(PROPS, in_signature='ssv')
    def Set(self, interface, name, value):
        print('Set', name, value, flush=True)

    @dbus.service.signal(PROPS, signature='sa{sv}as')
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    @dbus.service.method(PLAYER)
    def PlayPause(self):
        print('PlayPause', flush=True)
        self.offset, self.since, self.playing = self.position(), time.monotonic(), not self.playing
        self.changed('PlaybackStatus')

    @dbus.service.method(PLAYER)
    def Play(self):
        print('Play', flush=True)
        if not self.playing:
            self.PlayPause()

    @dbus.service.method(PLAYER)
    def Pause(self):
        print('Pause', flush=True)
        if self.playing:
            self.PlayPause()

    @dbus.service.method(PLAYER)
    def Next(self):
        print('Next', flush=True)
        self.track, self.offset, self.since = self.track + 1, 0, time.monotonic()
        self.changed('Metadata')

    @dbus.service.method(PLAYER)
    def Previous(self):
        print('Previous', flush=True)
        self.track, self.offset, self.since = max(1, self.track - 1), 0, time.monotonic()
        self.changed('Metadata')

    @dbus.service.method(PLAYER, in_signature='x')
    def Seek(self, offset):
        print('Seek', offset, flush=True)

    @dbus.service.method(PLAYER, in_signature='ox')
    def SetPosition(self, track, position):
        print('SetPosition', int(position) // 1_000_000, 's', flush=True)
        self.offset, self.since = int(position), time.monotonic()


DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
name = dbus.service.BusName('org.mpris.MediaPlayer2.omarchytest', bus)
player = Player(bus)
loop = GLib.MainLoop()
GLib.timeout_add_seconds(int(sys.argv[1]) if len(sys.argv) > 1 else 120, loop.quit)
print('playing', flush=True)
loop.run()
