#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stand-in for Android's GNSS HAL (gps.default.so / GpsiClient) talking to
Broadcom's stock gpsd over its named-pipe IPC, so gpsd runs a position session
without Android. Prints fixes, and can serve NMEA 0183 built from them.

Transport (gpsd GpsiServer::Init 0x38700c, IpcNamedPipe::Init 0x35a7c4,
IpcNamedPipeThread::OpenNamedPipes 0x35b8dc): two FIFOs in gpsd's log dir,
  .gps.interface.pipe.to_gpsd  client writes, gpsd reads (gpsd opens it first)
  .gps.interface.pipe.to_jni   gpsd writes, client reads
The client opens to_gpsd O_WRONLY, then to_jni O_RDONLY, like
gps.default.so GpsiClient::Init (IpcNamedPipe::Init(..., server=false)).

Message (IpcOutgoingMessage 0x359a44/Send 0x35a4f8, IpcIncomingMessage::Receive
0x359458), little endian:
  u32 total_length (includes itself)
  tagged int interface  (0 = GPS call, 1 = callback, 2 = extension to gpsd,
                         3 = extension from gpsd)
  tagged int message id
  payload items:
    tagged int   : u32 0x5245ff02, i32      tagged pointer: u32 0x5245ff06, u64
    tagged double: u32 0x5245ff07, f64      tagged float  : u32 0x5245ff08, f32
    bytes        : u64 length, data padded to 4 (no tag; length 0 = no data)

gpsd only forwards NMEA and satellite status once told to, as the AIDL HAL
does from IGnss startNmea/startSvStatus: extension message 46 carrying
"REPORT_CTRL=NMEA_CB_ON" / "REPORT_CTRL=SVSTATUS_CB_ON" (gps.default.so
LibGpsSetGnssConfiguration 0x1f0b0; gpsd GlGpsdInterface::SetConfig 0x373ad0
-> GpsInterfaceImpl::ReportControl 0x3855dc).

Closing the pipes makes gpsd log "IPC Connection closed by libgps. Restarting
GPS daemon..." and _exit(0) (GpsiServerIpcNamedPipe::OnConnectionClosedByPeer
0x38de68 -> GpsdExit 0x374100): restart gpsd after every client run.
"""
import argparse
import datetime
import json
import os
import pty
import select
import signal
import socket
import struct
import sys
import time

TAG_INT, TAG_PTR, TAG_DOUBLE, TAG_FLOAT = 0x5245FF02, 0x5245FF06, 0x5245FF07, 0x5245FF08

# Interface 0 (client -> gpsd), GpsiServer::HandleGpsInterfaceMessage jump
# table at 0x90090.
GPS_INIT, GPS_START, GPS_STOP, GPS_CLEANUP = 0, 1, 2, 3
GPS_INJECT_TIME, GPS_INJECT_LOCATION, GPS_DELETE_AIDING = 4, 5, 7
GPS_SET_POSITION_MODE = 8
EXT_GNSS_CONFIGURATION = 46   # interface 2; text "KEY=VALUE"

# Interface 1 (gpsd -> client), GpsiClient::HandleGpsInterfaceMessage table.
CB_NAMES = {
    0x100: 'location', 0x101: 'status', 0x102: 'sv_status', 0x103: 'nmea',
    0x104: 'capabilities', 0x107: 'request_utc_time',
    0x108: 'xtra_download_request', 0x109: 'agps_status', 0x10a: 'ni_notify',
    0x10b: 'agps_request_setid', 0x10c: 'agps_request_refloc',
    0x113: 'set_config', 0x114: 'measurement', 0x115: 'navigation_message',
    0x117: 'gnss_sv_status', 0x118: 'system_info',
    0x119: 'gnss_measurement', 0x11a: 'gnss_navigation_message',
    0x11d: 'request_location', 0x11e: 'dbg_internal_state',
    0x120: 'nfw_notify', 0x122: 'antenna_info',
    0x126: 'psds_download_request', 0x127: 'power_stats',
}
STATUS = {0: 'NONE', 1: 'SESSION_BEGIN', 2: 'SESSION_END', 3: 'ENGINE_ON',
          4: 'ENGINE_OFF'}
CONSTELLATION = {0: '?', 1: 'GPS', 2: 'SBAS', 3: 'GLONASS', 4: 'QZSS',
                 5: 'BEIDOU', 6: 'GALILEO', 7: 'IRNSS'}
TALKER = {1: 'GP', 2: 'GP', 3: 'GL', 4: 'GQ', 5: 'GB', 6: 'GA', 7: 'GI'}
SYSTEM_ID = {'GP': 1, 'GL': 2, 'GA': 3, 'GB': 4, 'GQ': 5, 'GI': 6}
SV_USED_IN_FIX = 0x4


def log(*args):
    print(*args, file=sys.stderr, flush=True)


# ---------------------------------------------------------------- encoding
class Msg:
    def __init__(self, iface, msg_id):
        self.body = bytearray()
        self.int(iface)
        self.int(msg_id)

    def int(self, v):
        self.body += struct.pack('<Ii', TAG_INT, v)
        return self

    def ptr(self, v):
        self.body += struct.pack('<IQ', TAG_PTR, v)
        return self

    def double(self, v):
        self.body += struct.pack('<Id', TAG_DOUBLE, v)
        return self

    def float(self, v):
        self.body += struct.pack('<If', TAG_FLOAT, v)
        return self

    def bytes(self, data):
        self.body += struct.pack('<Q', len(data)) + data
        self.body += b'\0' * (-len(data) % 4)
        return self

    def encode(self):
        return struct.pack('<I', 4 + len(self.body)) + bytes(self.body)


class Reader:
    def __init__(self, payload):
        self.b, self.p = payload, 0

    def int(self):
        tag, v = struct.unpack_from('<Ii', self.b, self.p)
        if tag != TAG_INT:
            raise ValueError('expected int tag, got %#x' % tag)
        self.p += 8
        return v

    def bytes(self):
        n, = struct.unpack_from('<Q', self.b, self.p)
        self.p += 8
        data = self.b[self.p:self.p + n]
        self.p += (n + 3) & ~3
        return data


# ---------------------------------------------------------------- decoding
def parse_location(d):
    # broadcom::GpsLocation, 112 bytes. 0x00..0x3f: gps.default.so
    # convert_location 0x1eddc; 0x40..0x57 (doubles): gpsd log format of
    # GpsLocationMessage::OnMessage 0x38fc3c (inferred).
    flags, = struct.unpack_from('<H', d, 8)
    lat, lon, alt = struct.unpack_from('<3d', d, 0x10)
    speed, bearing, acc = struct.unpack_from('<3f', d, 0x28)
    ts, = struct.unpack_from('<q', d, 0x38)
    loc = dict(flags=flags, lat=lat, lon=lon, time_ms=ts)
    if flags & 0x02: loc['alt'] = alt
    if flags & 0x04: loc['speed'] = speed
    if flags & 0x08: loc['bearing'] = bearing
    if flags & 0x10: loc['acc'] = acc
    if len(d) >= 0x58:
        vacc, sacc, bacc = struct.unpack_from('<3d', d, 0x40)
        if flags & 0x20: loc['vacc'] = vacc
        if flags & 0x40: loc['speed_acc'] = sacc
        if flags & 0x80: loc['bearing_acc'] = bacc
    return loc


def parse_gnss_sv_status(d):
    # GnssSvStatus, 2576 bytes: u64 size, i32 num_svs, then 64 entries of
    # 0x28 bytes from 0x10 (gpsd log_gnss_sv_status_cb 0x391100).
    n, = struct.unpack_from('<i', d, 8)
    svs = []
    for i in range(max(0, min(n, 64))):
        o = 0x10 + i * 0x28
        svid, const = struct.unpack_from('<hB', d, o + 8)
        cn0, elev, az = struct.unpack_from('<3f', d, o + 0xc)
        flags, = struct.unpack_from('<B', d, o + 0x18)
        freq, = struct.unpack_from('<f', d, o + 0x1c)
        svs.append(dict(svid=svid, constellation=const, cn0=cn0, elev=elev,
                        az=az, flags=flags, freq_mhz=freq / 1e6))
    return svs


def parse_gps_sv_status(d):
    # Legacy GpsSvStatus, 800 bytes (layout of AOSP gps.h, assumed).
    n, = struct.unpack_from('<i', d, 8)
    used, = struct.unpack_from('<I', d, 16 + 32 * 24 + 8)
    svs = []
    for i in range(max(0, min(n, 32))):
        o = 16 + i * 24
        prn, = struct.unpack_from('<i', d, o + 8)
        cn0, elev, az = struct.unpack_from('<3f', d, o + 12)
        svs.append(dict(svid=prn, constellation=1, cn0=cn0, elev=elev, az=az,
                        flags=SV_USED_IN_FIX if 0 < prn <= 32 and used >> (prn - 1) & 1 else 0,
                        freq_mhz=0.0))
    return svs


# ---------------------------------------------------------------- NMEA
def nmea(body):
    cs = 0
    for c in body.encode():
        cs ^= c
    return '$%s*%02X\r\n' % (body, cs)


def nmea_latlon(v, pos, neg, width):
    h = pos if v >= 0 else neg
    v = abs(v)
    deg = int(v)
    minutes = (v - deg) * 60
    return '%0*d%08.5f' % (width, deg, minutes), h


def nmea_svid(sv):
    s, c = sv['svid'], sv['constellation']
    if c == 2 and 120 <= s <= 158:
        return s - 87
    if c == 3:
        return 64 + s if 1 <= s <= 24 else None  # frequency-channel ids skipped
    if c == 4 and s >= 193:
        return s - 192
    return s


def nmea_fix(loc, svs_used):
    t = datetime.datetime.fromtimestamp(loc['time_ms'] / 1000, datetime.timezone.utc)
    hms = t.strftime('%H%M%S') + '.%02d' % (t.microsecond // 10000)
    lat, ns = nmea_latlon(loc['lat'], 'N', 'S', 2)
    lon, ew = nmea_latlon(loc['lon'], 'E', 'W', 3)
    # Android altitude is above the WGS84 ellipsoid: report it with a zero
    # geoid separation so gpsd's altHAE is right (altMSL then equals it).
    alt = '%.1f,M,0.0,M' % loc['alt'] if 'alt' in loc else ',M,,M'
    out = [nmea('GNGGA,%s,%s,%s,%s,%s,1,%02d,,%s,,' % (hms, lat, ns, lon, ew, svs_used, alt))]
    knots = '%.2f' % (loc['speed'] * 1.943844) if 'speed' in loc else ''
    course = '%.1f' % loc['bearing'] if 'bearing' in loc else ''
    out.append(nmea('GNRMC,%s,A,%s,%s,%s,%s,%s,%s,%s,,,A' % (
        hms, lat, ns, lon, ew, knots, course, t.strftime('%d%m%y'))))
    if 'acc' in loc:  # horizontal accuracy as a GST 1-sigma lat/lon error
        e = '%.1f' % (loc['acc'] / 1.4142)
        v = '%.1f' % loc['vacc'] if 'vacc' in loc else ''
        out.append(nmea('GNGST,%s,,,,,%s,%s,%s' % (hms, e, e, v)))
    return out


def nmea_sky(svs, three_d):
    best = {}
    for sv in svs:  # one entry per satellite: its strongest signal (L1 or L5)
        talker = TALKER.get(sv['constellation'])
        prn = nmea_svid(sv)
        if not talker or prn is None:
            continue
        key = (talker, prn)
        old = best.get(key)
        used = sv['flags'] & SV_USED_IN_FIX | (old['flags'] & SV_USED_IN_FIX if old else 0)
        if old is None or sv['cn0'] > old['cn0']:
            best[key] = dict(sv, prn=prn)
        best[key]['flags'] |= used
    out = []
    for talker in sorted({k[0] for k in best}):
        sats = sorted((v for k, v in best.items() if k[0] == talker), key=lambda s: s['prn'])
        used = [s['prn'] for s in sats if s['flags'] & SV_USED_IN_FIX][:12]
        if used:
            out.append(nmea('%sGSA,A,%d,%s,,,,%d' % (
                talker, 3 if three_d else 2,
                ','.join(['%02d' % p for p in used] + [''] * (12 - len(used))), SYSTEM_ID[talker])))
        pages = (len(sats) + 3) // 4
        for page in range(pages):
            fields = []
            for s in sats[page * 4:page * 4 + 4]:
                fields.append('%02d,%s,%s,%s' % (
                    s['prn'], '%02d' % round(s['elev']) if s['elev'] or s['az'] else '',
                    '%03d' % (round(s['az']) % 360) if s['elev'] or s['az'] else '',
                    '%02d' % round(s['cn0']) if s['cn0'] > 0 else ''))
            out.append(nmea('%sGSV,%d,%d,%02d,%s' % (talker, pages, page + 1, len(sats), ','.join(fields))))
    return out


class NmeaSinks:
    def __init__(self, stdout, pty_link, tcp_port):
        self.stdout = stdout
        self.pty_master = None
        self.pty_link = pty_link
        self.server = None
        self.clients = []
        if pty_link:
            self.pty_master, slave = pty.openpty()
            name = os.ttyname(slave)
            try:
                os.unlink(pty_link)
            except FileNotFoundError:
                pass
            os.symlink(name, pty_link)
            os.set_blocking(self.pty_master, False)
            log('NMEA pty %s -> %s' % (pty_link, name))
        if tcp_port:
            self.server = socket.socket(socket.AF_INET6 if ':' in tcp_port[0] else socket.AF_INET)
            self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.server.bind(tcp_port)
            self.server.listen(4)
            self.server.setblocking(False)
            log('NMEA TCP server on %s:%d' % tcp_port)

    def fds(self):
        return [self.server] if self.server else []

    def accept(self):
        c, addr = self.server.accept()
        c.setblocking(False)
        self.clients.append(c)
        log('NMEA client %s' % (addr,))

    def write(self, lines):
        if not lines:
            return
        data = ''.join(lines)
        if self.stdout:
            sys.stdout.write(data)
            sys.stdout.flush()
        raw = data.encode()
        if self.pty_master is not None:
            try:
                os.write(self.pty_master, raw)
            except OSError:  # nobody reading, or buffer full: drop
                pass
        for c in list(self.clients):
            try:
                c.send(raw)
            except (BlockingIOError, InterruptedError):
                pass
            except OSError:
                self.clients.remove(c)
                c.close()

    def close(self):
        if self.pty_link:
            try:
                os.unlink(self.pty_link)
            except OSError:
                pass


# ---------------------------------------------------------------- IPC
def open_fifo(path, flags, deadline, what):
    """Open a FIFO end, waiting for the peer until the deadline."""
    if not os.path.exists(path):
        os.mkfifo(path, 0o660)
    while True:
        try:
            if flags == os.O_WRONLY:  # fails with ENXIO until gpsd reads
                fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
                os.set_blocking(fd, True)
                return fd
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            return fd
        except OSError as e:
            if e.errno != 6 or time.monotonic() > deadline:  # ENXIO
                raise SystemExit('cannot open %s (%s): %s; is gpsd running and idle?' % (path, what, e))
            time.sleep(0.1)


class Client:
    def __init__(self, args):
        self.args = args
        self.buf = b''
        self.svs = []
        self.last_fix = None
        self.fixes = 0
        self.gpsd_nmea = args.nmea_source == 'gpsd'  # auto switches on first gpsd GGA/RMC
        self.sinks = NmeaSinks(args.nmea, args.pty, args.tcp)
        d = args.dir
        deadline = time.monotonic() + args.timeout
        self.wfd = open_fifo(os.path.join(d, '.gps.interface.pipe.to_gpsd'), os.O_WRONLY, deadline, 'to gpsd')
        # gpsd opens its write end right after its read end connects.
        self.rfd = open_fifo(os.path.join(d, '.gps.interface.pipe.to_jni'), os.O_RDONLY, deadline, 'from gpsd')
        self.writer_seen = False
        log('connected to gpsd via %s' % d)

    def send(self, m, what):
        os.write(self.wfd, m.encode())
        if self.args.verbose:
            log('-> %s' % what)

    def extension(self, msg_id, data, what):
        # GpsiClient::SendGpsExtensionMessage (gps.default.so 0x272e0):
        # int -1, int -1, pointer 0, bytes(u32 length), bytes(data)
        self.send(Msg(2, msg_id).int(-1).int(-1).ptr(0).bytes(struct.pack('<I', len(data)))
                  .bytes(data), what)

    def start(self):
        a = self.args
        self.send(Msg(0, GPS_INIT), 'gps_init')
        for ctl in ('NMEA_CB_ON', 'SVSTATUS_CB_ON'):
            self.extension(EXT_GNSS_CONFIGURATION, b'REPORT_CTRL=' + ctl.encode(), 'REPORT_CTRL=' + ctl)
        if a.cold:
            self.send(Msg(0, GPS_DELETE_AIDING).bytes(struct.pack('<H', 0xFFFF)), 'delete_aiding_data(ALL)')
        if a.inject_time:
            self.inject_time()
        if a.inject_location:
            lat, lon, acc = (float(x) for x in a.inject_location.split(','))
            self.send(Msg(0, GPS_INJECT_LOCATION).double(lat).double(lon).float(acc), 'inject_location')
        # mode 0 = standalone: gpsd sets SuplEnable=false (0x37e294); recurrence
        # 0 = periodic; min interval ms; preferred accuracy m; preferred time ms;
        # low-power mode byte.
        pm = Msg(0, GPS_SET_POSITION_MODE)
        for v in (a.mode, 0, a.interval, a.accuracy, 0):
            pm.bytes(struct.pack('<I', v))
        pm.bytes(b'\x01' if a.low_power else b'\x00')
        self.send(pm, 'set_position_mode(mode=%d, periodic, %d ms)' % (a.mode, a.interval))
        self.send(Msg(0, GPS_START), 'gps_start')

    def inject_time(self):
        now = int(time.time() * 1000)
        ref = int(time.clock_gettime(time.CLOCK_BOOTTIME) * 1000)  # android::elapsedRealtime
        self.send(Msg(0, GPS_INJECT_TIME).bytes(struct.pack('<q', now)).bytes(struct.pack('<q', ref))
                  .int(self.args.time_uncertainty), 'inject_time')

    def stop(self):
        try:
            self.send(Msg(0, GPS_STOP), 'gps_stop')
            end = time.monotonic() + 2
            while time.monotonic() < end:  # let SESSION_END/ENGINE_OFF arrive
                if not self.pump(0.2):
                    break
            self.send(Msg(0, GPS_CLEANUP), 'gps_cleanup')
        except OSError:
            pass
        self.sinks.close()

    def pump(self, timeout):
        fds = [self.rfd] + self.sinks.fds()
        r, _, _ = select.select(fds, [], [], timeout)
        for f in r:
            if f is not self.rfd:
                self.sinks.accept()
                continue
            try:
                data = os.read(self.rfd, 65536)
            except BlockingIOError:
                continue
            if not data:
                if self.writer_seen:
                    log('gpsd closed the pipe')
                    return False
                time.sleep(0.05)  # gpsd has not opened its write end yet
                continue
            self.writer_seen = True
            self.buf += data
            while len(self.buf) >= 4:
                n, = struct.unpack_from('<I', self.buf)
                if n < 4 or n > 4 << 20:
                    raise SystemExit('bad frame length %d' % n)
                if len(self.buf) < n:
                    break
                frame, self.buf = self.buf[4:n], self.buf[n:]
                self.dispatch(frame)
        return True

    def emit(self, kind, obj):
        if self.args.json:
            obj = dict(obj, type=kind)
            print(json.dumps(obj), flush=True)

    def dispatch(self, frame):
        r = Reader(frame)
        iface, mid = r.int(), r.int()
        name = CB_NAMES.get(mid, '%#x' % mid) if iface == 1 else 'extension%d/%d' % (iface, mid)
        payload = frame[r.p:]
        try:
            self.handle(mid if iface == 1 else None, r)
        except (ValueError, struct.error) as e:
            log('%s: cannot parse (%s): %s' % (name, e, payload.hex()))
            return
        if self.args.verbose and (iface != 1 or mid not in (0x100, 0x103, 0x117)):
            log('<- %s %s' % (name, payload.hex()))

    def handle(self, mid, r):
        a = self.args
        if mid == 0x100:
            loc = parse_location(r.bytes())
            self.fixes += 1
            used = sum(1 for s in self.svs if s['flags'] & SV_USED_IN_FIX)
            self.last_fix = loc
            self.emit('fix', loc)
            if not a.json:
                t = datetime.datetime.fromtimestamp(loc['time_ms'] / 1000, datetime.timezone.utc)
                log('FIX %s %.7f %.7f alt=%s acc=%s spd=%s used=%d' % (
                    t.strftime('%H:%M:%S.%f')[:-3], loc['lat'], loc['lon'],
                    '%.1f' % loc['alt'] if 'alt' in loc else '-',
                    '%.1f' % loc['acc'] if 'acc' in loc else '-',
                    '%.2f' % loc['speed'] if 'speed' in loc else '-', used))
            if not self.gpsd_nmea:
                self.sinks.write(nmea_fix(loc, used))
            if a.count and self.fixes >= a.count:
                raise KeyboardInterrupt
        elif mid in (0x117, 0x102):
            self.svs = parse_gnss_sv_status(r.bytes()) if mid == 0x117 else parse_gps_sv_status(r.bytes())
            self.emit('sky', dict(svs=self.svs))
            if not a.json and a.sky:
                seen = sorted(self.svs, key=lambda s: -s['cn0'])
                log('SKY %d sv, %d used: %s' % (len(seen), sum(1 for s in seen if s['flags'] & SV_USED_IN_FIX),
                    ' '.join('%s%d:%.0f%s' % (CONSTELLATION[s['constellation']][:3], s['svid'], s['cn0'],
                             '*' if s['flags'] & SV_USED_IN_FIX else '') for s in seen[:16])))
            if not self.gpsd_nmea:
                self.sinks.write(nmea_sky(self.svs, bool(self.last_fix and 'alt' in self.last_fix)))
        elif mid == 0x103:
            ts = struct.unpack('<q', r.bytes())[0]
            n = r.int()
            text = r.bytes().decode('ascii', 'replace') if n > 0 else ''
            self.emit('nmea', dict(time_ms=ts, sentence=text.strip()))
            if a.verbose:
                log('NMEA %s' % text.strip())
            standard = text.startswith('$G') and text[3:6] in ('GGA', 'RMC', 'GSV', 'GSA', 'GNS', 'VTG', 'GST')
            if standard and a.nmea_source == 'auto' and not self.gpsd_nmea:
                log("gpsd sends its own NMEA: serving that instead of synthesized sentences")
                self.gpsd_nmea = True
            if text.startswith('$') and ((standard and self.gpsd_nmea) or (a.pglor and not standard)):
                self.sinks.write([text.rstrip('\r\n') + '\r\n'])
        elif mid == 0x101:
            st, = struct.unpack_from('<H', r.bytes(), 8)
            self.emit('status', dict(status=STATUS.get(st, st)))
            log('status %s' % STATUS.get(st, st))
        elif mid == 0x104:
            caps, = struct.unpack('<I', r.bytes())
            log('capabilities %#x' % caps)
        elif mid == 0x118:
            d = r.bytes()
            log('year of hardware %d' % struct.unpack_from('<H', d, 8))
        elif mid == 0x107:
            if a.inject_time:
                self.inject_time()
        elif mid is not None and mid not in CB_NAMES:
            log('unknown callback %#x' % mid)

    def close(self):
        os.close(self.wfd)
        os.close(self.rfd)


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--dir', default='/run/gnss-lab/data/vendor/gps',
                   help="gpsd's pipe directory, as seen from here (default: %(default)s)")
    p.add_argument('--interval', type=int, default=1000, help='fix interval in ms')
    p.add_argument('--accuracy', type=int, default=0, help='preferred accuracy in m (0: none)')
    p.add_argument('--mode', type=int, default=0, help='0 standalone (default; forces SUPL off), 1 MS-based')
    p.add_argument('--low-power', action='store_true', help='request low-power mode')
    p.add_argument('--cold', action='store_true', help='delete all aiding data first (cold start)')
    p.add_argument('--inject-time', action='store_true',
                   help='inject system time at start and on request (gpsd ignores it while '
                        'IgnoreInjectedSystemTime=true)')
    p.add_argument('--time-uncertainty', type=int, default=1000, help='ms, with --inject-time')
    p.add_argument('--inject-location', metavar='LAT,LON,ACC', help='seed position (accuracy in m)')
    p.add_argument('--nmea', action='store_true', help='write NMEA to stdout')
    p.add_argument('--pty', metavar='LINK', help='serve NMEA on a pty, symlinked at LINK (for gpsd -n LINK)')
    p.add_argument('--tcp', metavar='[HOST:]PORT', help='serve NMEA over TCP (e.g. 127.0.0.1:10110)')
    p.add_argument('--nmea-source', choices=('auto', 'gpsd', 'synth'), default='auto',
                   help="NMEA to serve: gpsd's own sentences, sentences built from fixes and "
                        "satellite status, or auto (built until gpsd's own arrive; default)")
    p.add_argument('--pglor', action='store_true', help="also serve gpsd's proprietary $PGLOR sentences")
    p.add_argument('--json', action='store_true', help='JSON lines on stdout instead of text on stderr')
    p.add_argument('--sky', action='store_true', help='print satellite lines')
    p.add_argument('--count', type=int, default=0, help='stop after N fixes')
    p.add_argument('--duration', type=float, default=0, help='stop after N seconds')
    p.add_argument('--timeout', type=float, default=20, help='seconds to wait for gpsd pipes')
    p.add_argument('-v', '--verbose', action='store_true')
    a = p.parse_args()
    if a.tcp:
        host, _, port = a.tcp.rpartition(':')
        a.tcp = (host or '127.0.0.1', int(port))
    if a.json and a.nmea:
        p.error('--json and --nmea both use stdout')

    def terminate(signum, frame):
        raise KeyboardInterrupt

    c = Client(a)
    signal.signal(signal.SIGTERM, terminate)
    end = time.monotonic() + a.duration if a.duration else None
    try:
        c.start()
        while c.pump(1.0):
            if end and time.monotonic() > end:
                break
    except KeyboardInterrupt:
        pass
    finally:
        c.stop()
        c.close()
        log('stopped after %d fixes; gpsd exits now and must be restarted' % c.fixes)


if __name__ == '__main__':
    main()
