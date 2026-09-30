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


def log(msg):
    print(f'pixel-sensor-proxy: {msg}', file=sys.stderr, flush=True)


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

    def start(self, kind):
        stype, period, mode, _ = KINDS[kind]
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
