#!/usr/bin/env python3
"""Sample where threads spend CPU time, through perf_event_open (test only).

perf-sample.py SECONDS OUT TID...

Each thread gets a task-clock event that samples every 250 us of CPU time it
runs, with its user and kernel call chain (frame pointers). OUT is JSON:
the samples, each process's /proc maps and the kernel's symbols, so the
addresses can be resolved later on a machine with unstripped libraries
(perf-report.py). Root only.
"""
import ctypes, json, mmap, os, struct, sys, time

PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK = 1, 1
SAMPLE_IP, SAMPLE_TID, SAMPLE_CALLCHAIN = 1 << 0, 1 << 1, 1 << 5
RECORD_SAMPLE = 9
PAGES = 64                      # ring buffer data pages per thread
NR_PERF_EVENT_OPEN = 241        # arm64
IOC_ENABLE, IOC_DISABLE = 0x2400, 0x2401

seconds, out = float(sys.argv[1]), sys.argv[2]
tids = [int(t) for t in sys.argv[3:]]
libc = ctypes.CDLL(None, use_errno=True)
page = mmap.PAGESIZE


def attr():
    a = bytearray(128)
    flags = 1 << 0                                  # disabled until all are open
    struct.pack_into('IIQQQQQ', a, 0, PERF_TYPE_SOFTWARE, 128, PERF_COUNT_SW_TASK_CLOCK,
                     250000, SAMPLE_IP | SAMPLE_TID | SAMPLE_CALLCHAIN, 0, flags)
    struct.pack_into('H', a, 108, 48)               # sample_max_stack
    return bytes(a)


events = []
for tid in tids:
    buf = ctypes.create_string_buffer(attr(), 128)
    fd = libc.syscall(NR_PERF_EVENT_OPEN, buf, tid, -1, -1, 0)
    if fd < 0:
        sys.exit(f'perf_event_open {tid}: errno {ctypes.get_errno()}')
    ring = mmap.mmap(fd, (PAGES + 1) * page)
    events.append((tid, fd, ring))
for _, fd, _ in events:
    libc.ioctl(fd, IOC_ENABLE, 0)

samples = []
tails = {tid: 0 for tid in tids}
lost = 0


def drain(tid, ring):
    """Read the samples the kernel has written since the last call."""
    head = struct.unpack_from('Q', ring, 1024)[0]
    tail = tails[tid]
    size = PAGES * page
    while tail < head:
        at = page + tail % size
        kind, misc, length = struct.unpack_from('IHH', ring, at)
        record = bytes(ring[at:at + length]) if at + length <= page + size else \
            bytes(ring[at:page + size]) + bytes(ring[page:page + (at + length - page - size)])
        if kind == RECORD_SAMPLE:
            ip, pid, t, nr = struct.unpack_from('QIIQ', record, 8)
            chain = list(struct.unpack_from(f'{nr}Q', record, 32))
            samples.append([t, ip, chain])
        tail += length
    tails[tid] = tail
    struct.pack_into('Q', ring, 1032, tail)       # data_tail: the space is free again


end = time.monotonic() + seconds
while time.monotonic() < end:
    time.sleep(0.05)
    for tid, _, ring in events:
        drain(tid, ring)
for tid, fd, ring in events:
    libc.ioctl(fd, IOC_DISABLE, 0)
    drain(tid, ring)

pids = {}
for tid in tids:
    pid = int(open(f'/proc/{tid}/status').read().split('Tgid:')[1].split()[0])
    pids[tid] = pid
maps = {pid: open(f'/proc/{pid}/maps').read() for pid in set(pids.values())}
names = {tid: open(f'/proc/{tid}/comm').read().strip() for tid in tids}
kallsyms = open('/proc/kallsyms').read()
json.dump({'samples': samples, 'pids': pids, 'names': names, 'maps': maps, 'kallsyms': kallsyms,
           'period_us': 250}, open(out, 'w'))
print(f'{len(samples)} samples from {len(tids)} threads -> {out}')
