#!/usr/bin/env python3
"""Where trusty_apploader waits: every 2 s for 20 s, its kernel stack, wait
channel and system call, read from outside the lab's PID namespace. Reads
/proc only."""
import os, time

def loaders():
    for pid in os.listdir('/proc'):
        if not pid.isdigit() or int(pid) == os.getpid():
            continue
        try:
            argv = open(f'/proc/{pid}/cmdline', 'rb').read().split(b'\0')
        except OSError:
            continue
        if any(a.endswith(b'/trusty_apploader') for a in argv[:2]):
            yield pid

for i in range(10):
    time.sleep(2)
    for pid in loaders():
        def read(name):
            try:
                return open(f'/proc/{pid}/{name}').read().strip()
            except OSError as e:
                return f'({e.strerror})'
        print(f'--- {2 * (i + 1)} s, pid {pid}: state {read("stat").split()[2]}, '
              f'wchan {read("wchan")}, syscall {read("syscall").split()[0]}')
        print(read('stack'))
