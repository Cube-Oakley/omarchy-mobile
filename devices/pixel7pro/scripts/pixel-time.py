#!/usr/bin/env python3
"""The Pixel's wall clock: network time over a read-only, drifting RTC.

  pixel-time restore        at boot, after the RTC module: the clock from the
                            RTC plus the offset the last sync measured
  pixel-time sync [--due]   ask NTP servers, set the clock, keep the offset;
                            with --due only when the last sync is older than
                            SYNC_EVERY (the Wi-Fi dispatcher's way)
  pixel-time status         the offset kept, and when

The S2MPG12 RTC (kernel/rtc/pixel-rtc.c) is read-only: nothing writes PMIC
registers it does not have to. Its crystal drifts about two seconds a day,
and nothing set the clock otherwise: on October 7 the phone ran 51 s ahead,
enough to break authenticator codes (30 s steps). The sleep adapter's wake
alarms count in the RTC's own seconds, so a clock that differs from the RTC
moves none of them. Installed as /usr/local/sbin/pixel-time.

A sync asks three servers (SERVERS) by SNTP and uses the answer with the
shortest round trip, once two of them agree within a second: one wrong or
spoofed server sets nothing. Stdlib only; no daemon, no timers.
"""
import json
import os
import socket
import struct
import sys
import time
from pathlib import Path

SERVERS = ('0.arch.pool.ntp.org', '1.arch.pool.ntp.org', '2.arch.pool.ntp.org')
STATE = Path('/var/lib/omarchy-mobile/rtc-offset.json')
RTC = Path('/sys/class/rtc/rtc0/since_epoch')
SYNC_EVERY = 6 * 3600
AGREE = 1.0
# Offsets below this are left alone (the RTC's own second is the floor).
STEP = 0.05
# An RTC whose corrected time falls this far before the last sync was reset
# (a flat battery restarts it near 2000).
RESET_GAP = 86400
NTP_EPOCH = 2208988800


def to_ntp(t):
    seconds = int(t)
    return seconds + NTP_EPOCH, int((t - seconds) * 2 ** 32)


def from_ntp(data):
    seconds, fraction = struct.unpack('!II', data)
    return seconds - NTP_EPOCH + fraction / 2 ** 32


def query(server, timeout=3.0):
    """(offset, round trip) from one server, or OSError/ValueError."""
    family, kind, proto, _, address = socket.getaddrinfo(server, 123, type=socket.SOCK_DGRAM)[0]
    request = bytearray(48)
    request[0] = 0x23                   # leap 0, version 4, client
    with socket.socket(family, kind, proto) as s:
        s.settimeout(timeout)
        t1 = time.time()
        struct.pack_into('!II', request, 40, *to_ntp(t1))
        s.sendto(request, address)
        reply, _ = s.recvfrom(512)
        t4 = time.time()
    if len(reply) < 48 or reply[0] & 7 != 4 or not 1 <= reply[1] <= 15 or reply[0] >> 6 == 3:
        raise ValueError(f'{server}: not a usable answer')
    if reply[24:32] != request[40:48]:
        raise ValueError(f'{server}: answer to another request')
    t2, t3 = from_ntp(reply[32:40]), from_ntp(reply[40:48])
    return ((t2 - t1) + (t3 - t4)) / 2, (t4 - t1) - (t3 - t2)


def rtc_edge():
    """The RTC's count just after it ticks, with the clock's time then: the
    offset between them to a few milliseconds instead of a second."""
    start = int(RTC.read_text())
    deadline = time.monotonic() + 1.5
    while time.monotonic() < deadline:
        now = int(RTC.read_text())
        if now != start:
            return now, time.time()
        time.sleep(0.005)
    raise OSError('RTC did not tick')


def save(offset, server):
    STATE.parent.mkdir(parents=True, exist_ok=True)
    temporary = STATE.with_suffix('.new')
    temporary.write_text(json.dumps({'offset': round(offset, 3), 'synced': int(time.time()),
                                     'server': server}) + '\n')
    os.replace(temporary, STATE)


def load():
    try:
        return json.loads(STATE.read_text())
    except (OSError, ValueError):
        return {}


def sync(due_only):
    if due_only and time.time() - load().get('synced', 0) < SYNC_EVERY:
        return 0
    answers = []
    for server in SERVERS:
        try:
            answers.append((*query(server), server))
        except (OSError, ValueError) as error:
            print(f'pixel-time: {server}: {error}', file=sys.stderr)
    answers.sort(key=lambda a: a[1])
    best = next((a for a in answers if any(b is not a and abs(b[0] - a[0]) < AGREE for b in answers)), None)
    if best is None:
        print(f'pixel-time: no two servers agree ({len(answers)} answered)', file=sys.stderr)
        return 1
    offset, delay, server = best
    if abs(offset) >= STEP:
        time.clock_settime(time.CLOCK_REALTIME, time.time() + offset)
    moved = f'clock {"moved" if abs(offset) >= STEP else "kept"} by {offset:+.3f} s ({server}, {delay * 1000:.0f} ms)'
    try:
        rtc, now = rtc_edge()
    except OSError as error:
        # No RTC (its module did not load): the clock is right, nothing to
        # keep for the next boot. On October 11 this failed after the step.
        print(f'pixel-time: {moved}, no RTC offset kept ({error})')
        return 0
    save(now - rtc, server)
    print(f'pixel-time: {moved}, RTC {now - rtc:+.3f} s')
    return 0


def restore():
    state = load()
    if 'offset' not in state:
        return 0
    rtc, now = rtc_edge()
    target = rtc + state['offset']
    if target < state.get('synced', 0) - RESET_GAP:
        # The RTC counts from its reset (a flat battery): the offset no longer
        # applies, and the clock floor stands until the next network sync,
        # which is due now that nothing is kept.
        STATE.rename(STATE.with_name(f'{STATE.stem}.before-rtc-reset.json'))
        print(f'pixel-time: RTC reads {time.strftime("%F", time.gmtime(rtc))}, before the last sync: '
              f'reset, offset dropped')
        return 0
    if abs(target - now) >= STEP:
        time.clock_settime(time.CLOCK_REALTIME, target + (time.time() - now))
    print(f'pixel-time: RTC {state["offset"]:+.3f} s, from a sync at '
          f'{time.strftime("%F %T", time.localtime(state.get("synced", 0)))}')
    return 0


def main(args):
    if args == ['restore']:
        return restore()
    if args in (['sync'], ['sync', '--due']):
        return sync('--due' in args)
    if args == ['status']:
        print(json.dumps(load()))
        return 0
    print(__doc__.split('\n\n')[1], file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
