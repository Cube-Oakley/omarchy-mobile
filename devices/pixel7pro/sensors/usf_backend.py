"""USF sensor backend for pixel-sensor-proxy: the AoC's sensors over /dev/acd-com.google.usf.

On start it makes sure the AoC's sensor registry is loaded, as Android's sensor
HAL does once per AoC boot (see usf.load_registry): the stock registry scripts,
then this phone's factory calibration from persist:/sensors/registry, read with
debugfs (read-only; persist is never mounted or written). The AoC then probes
its physical sensors, which needs their supplies on (kernel/aoc-power).

Claims start sampling on the AoC; the last release stops it.
"""
import os
import subprocess
import sys
import tempfile
import time

from gi.repository import GLib

import usf

REGISTRY_DIRS = ('/usr/share/omarchy-mobile/sensors/registry',
                 '/usr/share/omarchy-mobile/sensors/registry/append')
PERSIST_REGISTRY = '/sensors/registry'
SENSOR_ORIENTATION = 24      # fused azimuth, pitch, roll in degrees
# Google's lux for automatic brightness, from the TMD3719 under the panel: a
# reading a second, and none of the panel's own light (full brightness and 1%
# read the same). The TMD3719's own light sensor in on-change mode missed a
# fall from 60 to 30 lux; continuous, it sends every conversion (27 a second).
SENSOR_AUTO_BRIGHTNESS = 48
# kind -> (USF sensor type, sampling period, reporting mode, physical)
KINDS = {
    'accel': (usf.SENSOR_ACCEL, 66_666_666, usf.RPT_CONTINUOUS, True),   # 15 Hz, its slowest
    'light': (SENSOR_AUTO_BRIGHTNESS, 1_000_000_000, usf.RPT_CONTINUOUS, False),
    'proximity': (usf.SENSOR_PROX, 200_000_000, usf.RPT_ON_CHANGE, True),
    'compass': (SENSOR_ORIENTATION, 200_000_000, usf.RPT_CONTINUOUS, False),  # 5 Hz
}
CLIENT_ID = 0x50535850   # "PXSP"
# The TMD3719 under the panel times its conversions to the display: without
# the display state Android's sensor HAL normally sends (usf.display_info) it
# never converts, for light or proximity. It is read from the panel's DRM
# connector and backlight, and sent whenever it changes while either samples.
PANEL_DPMS = '/sys/class/drm/card0-DSI-1/dpms'
PANEL_DBV = '/sys/class/backlight/pixel-panel/brightness'
PANEL_120 = '/sys/module/pixel_scanout/parameters/panel120'
DISPLAY_SYNCED = ('light', 'proximity')


def log(msg):
    print(f'pixel-sensor-proxy: {msg}', file=sys.stderr, flush=True)


def read_text(path, default=''):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def display_state():
    """(status, refresh rate, DBV) as the stock HAL reports them: off is 0 Hz."""
    if read_text(PANEL_DPMS, 'On') != 'On':
        return usf.DISP_OFF, 0, 0
    try:
        dbv = int(read_text(PANEL_DBV, '0'))
    except ValueError:
        dbv = 0
    return usf.DISP_ON, 120 if read_text(PANEL_120) == 'Y' else 60, dbv


def persist_device():
    for entry in os.listdir('/sys/class/block'):
        try:
            with open(f'/sys/class/block/{entry}/uevent') as f:
                if 'PARTNAME=persist\n' in f.read():
                    return f'/dev/{entry}'
        except OSError:
            continue
    return None


def factory_calibration(target):
    """Copy persist's sensor calibration scripts into target, read-only."""
    dev = persist_device()
    if not dev:
        return
    listing = subprocess.run(['debugfs', '-R', f'ls {PERSIST_REGISTRY}', dev],
                             capture_output=True, text=True).stdout
    for name in listing.split():
        if not name.endswith('.reg'):
            continue
        data = subprocess.run(['debugfs', '-R', f'cat {PERSIST_REGISTRY}/{name}', dev],
                              capture_output=True).stdout
        if data:
            with open(os.path.join(target, name), 'wb') as f:
                f.write(data)


class UsfBackend:
    def __init__(self, device, report):
        self.report = report
        self.tr = usf.ChannelTransport(device, device + usf.NWU_SUFFIX)
        self.client = usf.UsfClient(self.tr)
        self.mgr = self.client.get_server(usf.UUID_SENSOR_MGR)
        self.max_range = {}
        self._ensure_registry()
        self.handles = self._find_sensors()
        self.has = {kind: kind in self.handles for kind in KINDS}
        self.sampling = {}     # kind -> sampling id
        self.kind_of = {}      # sampling id -> kind
        self.display = None    # last display state sent
        self.display_watch = None
        try:
            self.disp = self.client.get_server(usf.UUID_DISPLAY_INFO)
        except usf.UsfError as e:
            self.disp = None
            log(f'no display info server: {e}')
        self._send_display()
        for fd in self.tr.fds:
            GLib.io_add_watch(fd, GLib.IO_IN, self._readable)
        log('sensors: ' + ', '.join(f'{k} (handle {h})' for k, h in self.handles.items()))

    def _ensure_registry(self):
        reg = self.client.get_server(usf.UUID_REGISTRY)
        if ('loaded', '1') in self.client.registry_get(reg, '/'):
            return
        with tempfile.TemporaryDirectory() as cal:
            factory_calibration(cal)
            dirs = [d for d in REGISTRY_DIRS if os.path.isdir(d)] + [cal]
            usf.load_registry(self.client, dirs, usf.device_cdt(), out=sys.stderr)

    def _find_sensors(self):
        # The AoC probes its sensors asynchronously after the registry loads.
        handles = {}
        deadline = time.monotonic() + 10
        while True:
            infos = {}
            for h in self.client.sensor_list(self.mgr):
                try:
                    infos[h] = self.client.sensor_info(h)
                except usf.UsfError:
                    continue
            handles = {}
            for kind, (stype, _, _, physical) in KINDS.items():
                # A physical sensor has a registry path; the fused ones do not.
                cands = sorted((i['index'], h) for h, i in infos.items()
                               if i['type'] == stype and bool(i['registry_path']) == physical)
                if cands:
                    handles[kind] = cands[0][1]
                    self.max_range[kind] = infos[cands[0][1]]['max_range']
            if len(handles) == len(KINDS) or time.monotonic() > deadline:
                return handles
            time.sleep(0.5)

    def _send_display(self):
        state = display_state()
        if self.disp is None or state == self.display:
            return
        status, rate, dbv = state
        try:
            self.client.display_info(self.disp, brightness=dbv, status=status,
                                     refresh_rate=rate, op_hz=usf.DEFAULT_OP_HZ)
            self.display = state
        except usf.UsfError as e:
            log(f'display info: {e}')
        self._drain(self.client.events)
        self.client.events = []

    def _display_tick(self):
        self._send_display()
        if any(k in self.sampling for k in DISPLAY_SYNCED):
            return True
        self.display_watch = None
        return False

    def start(self, kind):
        stype, period, mode, _ = KINDS[kind]
        if kind in DISPLAY_SYNCED:
            self._send_display()
            if self.display_watch is None:
                self.display_watch = GLib.timeout_add_seconds(1, self._display_tick)
        # These UI sensors must not wake the AP. At 15 Hz the accelerometer
        # otherwise renews a 200 ms AoC wake lock continuously, blocking sleep.
        sid = self.client.start_sampling(self.handles[kind], period, 0, rpt_mode=mode,
                                         client_id=CLIENT_ID, non_wakeup=True)
        self.sampling[kind] = sid
        self.kind_of[sid] = kind
        self._drain(self.client.events)
        self.client.events = []

    def stop(self, kind):
        sid = self.sampling.pop(kind, None)
        if sid is None:
            return
        self.kind_of.pop(sid, None)
        try:
            self.client.stop_sampling(self.handles[kind], sid)
        except usf.UsfError as e:
            log(f'stop {kind}: {e}')
        self._drain(self.client.events)
        self.client.events = []

    def _readable(self, fd, cond):
        self._drain(self.client.poll(0))
        return True

    def _drain(self, items):
        for item in items:
            if item[0] != 'compact':
                continue
            batch = item[1]
            kind = self.kind_of.get(batch['sampling_id'])
            if not kind or not batch['samples']:
                continue
            _ts, _acc, vals = batch['samples'][-1]
            if kind == 'accel':
                self.report('accel', tuple(vals[:3]))
            elif kind == 'light':
                # vals[1] follows the room within seconds; vals[0] is slower and
                # holds its level when the room darkens (Android's debounce).
                self.report('light', vals[1])
            elif kind == 'proximity':
                self.report('proximity', vals[0] < self.max_range['proximity'])
            elif kind == 'compass':
                self.report('compass', vals[0] % 360.0)
