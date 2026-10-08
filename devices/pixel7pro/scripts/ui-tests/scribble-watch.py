#!/usr/bin/env python3
"""Watch zeroed RAM for writes nobody made (diagnostic, run as root).

scribble-watch.py MIB HOURS [LOG]

Fills MIB MiB of anonymous memory with zeros, locks it in place, then scans
it every 20 seconds. A page that is no longer all zeros was written by
something other than this process: a device's DMA into memory the kernel
had given out, for example. Each one is logged with its physical address
(from /proc/self/pagemap) and the first bytes found, then zeroed again.
"""
import ctypes
import mmap
import struct
import sys
import time

PAGE = 4096
CHUNK = 1 << 20

size = int(sys.argv[1]) << 20
hours = float(sys.argv[2])
log = open(sys.argv[3] if len(sys.argv) > 3 else '/root/scribble.log', 'a', buffering=1)

buf = mmap.mmap(-1, size, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
zero_chunk = bytes(CHUNK)
for off in range(0, size, CHUNK):
    buf[off:off + CHUNK] = zero_chunk          # touch: every page is backed
base = ctypes.addressof(ctypes.c_char.from_buffer(buf))
libc = ctypes.CDLL(None, use_errno=True)
locked = libc.mlock(ctypes.c_void_p(base), ctypes.c_size_t(size)) == 0


def physical(off):
    with open('/proc/self/pagemap', 'rb') as pagemap:
        pagemap.seek((base + off) // PAGE * 8)
        entry = struct.unpack('<Q', pagemap.read(8))[0]
    return (entry & ((1 << 55) - 1)) * PAGE if entry >> 63 else None


def stamp():
    return time.strftime('%F %T')


log.write(f'{stamp()} watching {size >> 20} MiB for {hours} h, locked={locked}\n')
end = time.time() + hours * 3600
passes = found = 0
zero_page = bytes(PAGE)
while time.time() < end:
    for off in range(0, size, CHUNK):
        if buf[off:off + CHUNK] == zero_chunk:
            continue
        for page in range(off, off + CHUNK, PAGE):
            data = buf[page:page + PAGE]
            if data == zero_page:
                continue
            nonzero = [i for i, b in enumerate(data) if b]
            pa = physical(page)
            where = f'phys {pa:#x}' if pa is not None else 'phys ?'
            log.write(f'{stamp()} page +{page:#x} {where}: {len(nonzero)} bytes changed from {nonzero[0]:#x}: '
                      f'{data[nonzero[0]:nonzero[0] + 32].hex()}\n')
            found += 1
            buf[page:page + PAGE] = zero_page
    passes += 1
    time.sleep(20)
log.write(f'{stamp()} done: {passes} passes, {found} pages changed\n')
