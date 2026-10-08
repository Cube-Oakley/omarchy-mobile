#!/usr/bin/env python3
"""Keep the kernel's warnings and crashes on disk, across a forced reset.

  pixel-kmsg-keeper.py [FILE]   (default /var/log/pixel-kernel-warnings.log)

On October 6 the phone stopped answering after a few kernel crashes in its
suspend path: the sleep helper died with SIGSEGV four times, the networking
lock (RTNL) stayed taken, SSH and the screen state went with it, and the
reset with the buttons kept nothing (pstore was empty afterwards). This
copies every kernel message at warning level or worse (priority 0-4: oopses,
BUGs, WARNs, errors) from /dev/kmsg to FILE, with the boot's ID at the top,
and the sleeps' entry, exit and pixel-sleep lines. A normal boot writes
about 400 such lines, mostly the modem's at start.

Lines go to the page cache as they come and are flushed to disk at most once
a second, except a crash or warning (Oops, BUG, WARNING, Call trace, panic,
hung task, RCU stall), which is flushed at once. FILE is renamed to FILE.1
past 4 MiB.
"""
import os
import select
import sys
import time

PATH = sys.argv[1] if len(sys.argv) > 1 else '/var/log/pixel-kernel-warnings.log'
LIMIT = 4 << 20
URGENT = (b'Oops', b'BUG', b'WARNING', b'Call trace', b'Kernel panic', b'Internal error',
          b'blocked for more than', b'rcu_preempt detected', b'self-detected stall', b'Unable to handle')
# Informational lines kept too: the sleeps around a hang (October 7: a hang
# after 22 suspend cycles left only their UFS warnings to count them by).
ALSO = (b'PM: suspend entry', b'PM: suspend exit', b'pixel-sleep: ')


def open_log():
    fd = os.open(PATH, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    with open('/proc/sys/kernel/random/boot_id') as f:
        boot = f.read().strip()
    os.write(fd, f'=== boot {boot} at {time.strftime("%Y-%m-%d %H:%M:%S")}\n'.encode())
    os.fsync(fd)
    return fd


def main():
    out = open_log()
    kmsg = os.open('/dev/kmsg', os.O_RDONLY | os.O_NONBLOCK)    # from the start of the buffer
    poll = select.poll()
    poll.register(kmsg, select.POLLIN)
    dirty = False
    last_sync = time.monotonic()
    while True:
        # A second without news flushes what came before it.
        if not poll.poll(1000 if dirty else None):
            os.fsync(out)
            dirty = False
            last_sync = time.monotonic()
            continue
        try:
            record = os.read(kmsg, 8192)
        except BrokenPipeError:     # overwritten before it was read
            continue
        except BlockingIOError:
            continue
        header, _, message = record.partition(b';')
        try:
            level = int(header.split(b',', 1)[0]) & 7
        except ValueError:
            continue
        if level > 4 and not any(word in message for word in ALSO):
            continue
        os.write(out, record if record.endswith(b'\n') else record + b'\n')
        dirty = True
        now = time.monotonic()
        if any(word in message for word in URGENT) or now - last_sync >= 1:
            os.fsync(out)
            dirty = False
            last_sync = now
        if os.fstat(out).st_size > LIMIT:
            os.fsync(out)
            os.close(out)
            os.replace(PATH, PATH + '.1')
            out = open_log()


if __name__ == '__main__':
    main()
