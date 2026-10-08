#!/usr/bin/env python3
"""Android's media session as an MPRIS player, for the media card.

  run    publish org.mpris.MediaPlayer2.android while Android has a track

Waydroid's container watcher (devices/pixel7pro/waydroid) keeps what Android
plays in $XDG_RUNTIME_DIR/omarchy-mobile/android-media.json: the keyboard
bridge's MediaBridge reports the media session that plays, else the most
recent one (the app, title, artist, album, duration and position in ms, the
state, which actions it takes, and an artwork version whose PNG is
android-media-art.png beside it). This publishes it on the session bus while
there is a track, so the shade's and the lock screen's media card show it,
and sends the card's buttons back through the watcher's FIFO (android-power:
"media CMD [POSITION_MS]").
"""
import json
import os
from pathlib import Path
import sys
import time
import zlib

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import Gio, GLib

BUS_NAME = 'org.mpris.MediaPlayer2.android'
OBJECT = '/org/mpris/MediaPlayer2'
ROOT = 'org.mpris.MediaPlayer2'
PLAYER = 'org.mpris.MediaPlayer2.Player'
PROPERTIES = 'org.freedesktop.DBus.Properties'
STATUS = {'playing': 'Playing', 'paused': 'Paused'}


def runtime():
    return Path(os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}')) / 'omarchy-mobile'


def send(command):
    """A line for the watcher; nothing happens while Android is not running
    (no reader on the FIFO)."""
    try:
        fd = os.open(runtime() / 'android-power', os.O_WRONLY | os.O_NONBLOCK)
    except OSError:
        return False
    try:
        os.write(fd, (command + '\n').encode())
    finally:
        os.close(fd)
    return True


def has_track(state):
    return state.get('state') not in (None, 'none') and bool(state.get('title'))


class Player(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, OBJECT)
        self.bus = bus
        self.state = {}
        self.received = time.monotonic()
        self.owned = False

    # What the session reported, from the watcher's file.
    def update(self, state):
        before = self.state
        if state == before:
            return
        expected = self.position()
        self.state, self.received = state, time.monotonic()
        if has_track(state) and not self.owned:
            self.bus.request_name(BUS_NAME)
            self.owned = True
        elif not has_track(state) and self.owned:
            self.bus.release_name(BUS_NAME)
            self.owned = False
            return
        if not self.owned:
            return
        self.PropertiesChanged(ROOT, {key: value for key, value in self.properties(ROOT).items()
                                      if key in ('Identity', 'DesktopEntry')}, [])
        changed = self.properties(PLAYER)
        del changed['Position']
        self.PropertiesChanged(PLAYER, changed, [])
        # A jump the clock does not explain: a seek.
        if before.get('title') == state.get('title') and abs(self.position() - expected) > 2_000_000:
            self.Seeked(dbus.Int64(self.position()))

    def position(self):
        state = self.state
        position = state.get('position', 0) * 1000
        if state.get('state') == 'playing':
            position += (time.monotonic() - self.received) * 1e6 * state.get('speed', 1)
        length = state.get('duration', 0) * 1000
        return int(min(position, length) if length > 0 else position)

    def metadata(self):
        state = self.state
        key = f"{state.get('package')}/{state.get('title')}/{state.get('album')}".encode()
        data = {'mpris:trackid': dbus.ObjectPath(f'/org/omarchy/android/{zlib.crc32(key):08x}')}
        if state.get('title'):
            data['xesam:title'] = state['title']
        if state.get('artist'):
            data['xesam:artist'] = dbus.Array([state['artist']], signature='s')
        if state.get('album'):
            data['xesam:album'] = state['album']
        if state.get('duration'):
            data['mpris:length'] = dbus.Int64(state['duration'] * 1000)
        if state.get('art'):
            data['mpris:artUrl'] = f"file://{runtime() / 'android-media-art.png'}?v={state['art']}"
        return dbus.Dictionary(data, signature='sv')

    def properties(self, interface):
        state = self.state
        if interface == ROOT:
            return {'CanQuit': False, 'CanRaise': False, 'HasTrackList': False,
                    'Identity': state.get('app') or 'Android',
                    'DesktopEntry': 'waydroid.' + state['package'] if state.get('package') else '',
                    'SupportedUriSchemes': dbus.Array([], signature='s'),
                    'SupportedMimeTypes': dbus.Array([], signature='s')}
        if interface == PLAYER:
            return {'PlaybackStatus': STATUS.get(state.get('state'), 'Stopped'),
                    'LoopStatus': 'None', 'Rate': 1.0, 'Shuffle': False, 'Volume': 1.0,
                    'Metadata': self.metadata(), 'Position': dbus.Int64(self.position()),
                    'MinimumRate': 1.0, 'MaximumRate': 1.0,
                    'CanGoNext': bool(state.get('next')), 'CanGoPrevious': bool(state.get('previous')),
                    'CanPlay': True, 'CanPause': True, 'CanSeek': bool(state.get('seek')),
                    'CanControl': True}
        raise dbus.exceptions.DBusException(f'No interface {interface}',
                                            name='org.freedesktop.DBus.Error.UnknownInterface')

    @dbus.service.method(PROPERTIES, in_signature='ss', out_signature='v')
    def Get(self, interface, name):
        return self.properties(interface)[name]

    @dbus.service.method(PROPERTIES, in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface):
        return self.properties(interface)

    @dbus.service.method(PROPERTIES, in_signature='ssv')
    def Set(self, interface, name, value):
        pass  # Volume, rate, loop and shuffle stay Android's.

    @dbus.service.signal(PROPERTIES, signature='sa{sv}as')
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    @dbus.service.method(ROOT)
    def Raise(self):
        pass

    @dbus.service.method(ROOT)
    def Quit(self):
        pass

    @dbus.service.method(PLAYER)
    def Next(self):
        send('media next')

    @dbus.service.method(PLAYER)
    def Previous(self):
        send('media previous')

    @dbus.service.method(PLAYER)
    def Pause(self):
        send('media pause')

    @dbus.service.method(PLAYER)
    def Stop(self):
        send('media pause')

    @dbus.service.method(PLAYER)
    def Play(self):
        send('media play')

    @dbus.service.method(PLAYER)
    def PlayPause(self):
        send('media toggle')

    @dbus.service.method(PLAYER, in_signature='x')
    def Seek(self, offset):
        send(f'media seek {max(0, (self.position() + offset) // 1000)}')

    @dbus.service.method(PLAYER, in_signature='ox')
    def SetPosition(self, track, position):
        if position >= 0:
            send(f'media seek {position // 1000}')

    @dbus.service.method(PLAYER, in_signature='s')
    def OpenUri(self, uri):
        pass

    @dbus.service.signal(PLAYER, signature='x')
    def Seeked(self, position):
        pass


def read_state(path):
    try:
        text = path.read_text().strip()
        return json.loads(text) if text else None
    except (OSError, ValueError):
        return None  # Missing, or caught mid-write: the next change has it.


def run():
    DBusGMainLoop(set_as_default=True)
    player = Player(dbus.SessionBus())
    path = runtime() / 'android-media.json'
    path.parent.mkdir(parents=True, exist_ok=True)
    state = read_state(path)
    if state is not None:
        player.update(state)

    def changed(*_):
        state = read_state(path)
        if state is not None:
            player.update(state)
    # The watcher writes the file in place: a change, then CHANGES_DONE.
    monitor = Gio.File.new_for_path(str(path)).monitor_file(Gio.FileMonitorFlags.NONE, None)
    monitor.connect('changed', lambda _m, _f, _o, event: changed() if event in (
        Gio.FileMonitorEvent.CHANGES_DONE_HINT, Gio.FileMonitorEvent.CREATED) else None)
    GLib.MainLoop().run()


def main(args):
    if args == ['run']:
        run()
        return 0
    print(__doc__.strip().splitlines()[2], file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
