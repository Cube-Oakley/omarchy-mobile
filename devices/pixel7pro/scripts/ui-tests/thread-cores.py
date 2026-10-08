#!/usr/bin/env python3
"""Which CPU cluster each thread of some processes runs on, and how busy it
is, while a command runs (test only).

thread-cores.py PID[,PID...] COMMAND...
Samples /proc/PID/task/*/stat every 2 ms. Cores 0-3 are LITTLE (A55),
4-5 MID (A78), 6-7 BIG (X1).
"""
import os, subprocess, sys, threading, time, collections

pids = [int(p) for p in sys.argv[1].split(',')]
cluster = lambda c: 'LITTLE' if c < 4 else ('MID' if c < 6 else 'BIG')
seen = collections.defaultdict(collections.Counter)  # (pid, comm) -> cluster -> samples while running
ticks = {}
stop = False

def stat(pid, tid):
    with open(f'/proc/{pid}/task/{tid}/stat') as f:
        text = f.read()
    comm = text[text.index('(') + 1:text.rindex(')')]
    rest = text[text.rindex(')') + 2:].split()
    return comm, rest[0], int(rest[11]) + int(rest[12]), int(rest[36])  # comm, state, utime+stime, processor

def sampler():
    while not stop:
        for pid in pids:
            try:
                tids = os.listdir(f'/proc/{pid}/task')
            except FileNotFoundError:
                continue
            for tid in tids:
                try:
                    comm, state, t, cpu = stat(pid, tid)
                except (FileNotFoundError, ValueError, ProcessLookupError):
                    continue
                key = (pid, tid, comm)
                ticks.setdefault(key, [t, t])[1] = t
                if state == 'R':
                    seen[key][cluster(cpu)] += 1
        time.sleep(0.002)

thread = threading.Thread(target=sampler, daemon=True)
thread.start()
start = time.monotonic()
subprocess.run(sys.argv[2:])
span = time.monotonic() - start
stop = True
thread.join()
hz = os.sysconf('SC_CLK_TCK')
rows = sorted(ticks.items(), key=lambda kv: -(kv[1][1] - kv[1][0]))[:14]
for key, (a, b) in rows:
    cpu = 100 * (b - a) / hz / span
    where = ', '.join(f'{c} {n}' for c, n in seen[key].most_common())
    print(f'{cpu:5.1f}% cpu  {key[2]:28s} running on: {where}')
