#!/usr/bin/env python3
"""Resolve perf-sample.py's output into where each thread spends its time.

perf-report.py SAMPLES.json ROOT [REPLACE=WITH ...]

ROOT is a local copy of the phone's files (ROOT/usr/lib/...), REPLACE=WITH
swaps a library for an unstripped build of the same file. Prints, per thread,
the share of samples in each library, the hottest functions (self time) and
the hottest functions including their callees (from the call chains).
"""
import bisect, collections, json, os, re, subprocess, sys

data = json.load(open(sys.argv[1]))
root = sys.argv[2]
replace = dict(arg.split('=', 1) for arg in sys.argv[3:])
KERNEL_CONTEXT, USER_CONTEXT = 0xffffffffffffff80, 0xfffffffffffffe00

ksyms = sorted((int(a, 16), n) for a, t, n, *_ in (l.split() for l in data['kallsyms'].splitlines())
               if t in 'tTwW' and int(a, 16))
kaddrs = [a for a, _ in ksyms]

maps = {}
for pid, text in data['maps'].items():
    rows = []
    for line in text.splitlines():
        parts = line.split(None, 5)
        if len(parts) < 6 or 'x' not in parts[1]:
            continue
        lo, hi = (int(x, 16) for x in parts[0].split('-'))
        rows.append((lo, hi, int(parts[2], 16), parts[5]))
    maps[int(pid)] = sorted(rows)

loads = {}


def load_segments(path):
    if path not in loads:
        out = subprocess.run(['llvm-readelf', '-lW', path], capture_output=True, text=True).stdout
        loads[path] = [(int(m[1], 16), int(m[2], 16), int(m[3], 16))
                       for m in re.finditer(r'LOAD\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)', out)]
    return loads[path]


def local(path):
    if path in replace:
        return replace[path]
    candidate = root + path
    return candidate if os.path.exists(candidate) else None


def locate(pid, ip):
    """(library, local file, virtual address) or a kernel symbol."""
    if ip >= 0xffff000000000000:
        i = bisect.bisect_right(kaddrs, ip) - 1
        return ('[kernel]', None, ksyms[i][1] if i >= 0 else hex(ip))
    rows = maps.get(pid, [])
    i = bisect.bisect_right(rows, (ip, float('inf'))) - 1
    if i < 0 or not rows[i][0] <= ip < rows[i][1]:
        return ('[unknown]', None, hex(ip))
    lo, hi, off, path = rows[i]
    fileoff = ip - lo + off
    file = local(path)
    if not file:
        return (path, None, hex(fileoff))
    for p_offset, p_vaddr, p_filesz in load_segments(file):
        if p_offset <= fileoff < p_offset + p_filesz:
            return (path, file, fileoff - p_offset + p_vaddr)
    return (path, None, hex(fileoff))


# Resolve every distinct address once through llvm-symbolizer.
wanted = collections.defaultdict(set)
for t, ip, chain in data['samples']:
    pid = data['pids'][str(t)]
    for addr in [ip] + chain:
        if addr in (KERNEL_CONTEXT, USER_CONTEXT):
            continue
        lib, file, vaddr = locate(pid, addr)
        if file:
            wanted[file].add(vaddr)
names = {}
for file, addrs in wanted.items():
    addrs = sorted(addrs)
    out = subprocess.run(['llvm-symbolizer', '--obj=' + file, '--no-inlines', '--demangle'],
                         input='\n'.join(hex(a) for a in addrs), capture_output=True, text=True).stdout
    for addr, block in zip(addrs, out.strip().split('\n\n')):
        names[(file, addr)] = block.split('\n')[0]


def name(pid, addr):
    lib, file, vaddr = locate(pid, addr)
    short = os.path.basename(lib)
    if lib == '[kernel]':
        return short, 'k:' + vaddr
    if file and names.get((file, vaddr), '??') != '??':
        return short, names[(file, vaddr)][:110]
    return short, f'{short}+{vaddr if isinstance(vaddr, str) else hex(vaddr)}'


by_thread = collections.defaultdict(list)
for sample in data['samples']:
    by_thread[sample[0]].append(sample)
for tid, samples in sorted(by_thread.items(), key=lambda kv: -len(kv[1])):
    pid = data['pids'][str(tid)]
    n = len(samples)
    print(f'\n== {data["names"][str(tid)]} ({tid}): {n} samples = {n * data["period_us"] / 1000:.0f} ms CPU')
    libs, selfs, totals = collections.Counter(), collections.Counter(), collections.Counter()
    for _, ip, chain in samples:
        lib, fn = name(pid, ip)
        libs[lib] += 1
        selfs[fn] += 1
        seen = set()
        for addr in chain:
            if addr in (KERNEL_CONTEXT, USER_CONTEXT):
                continue
            fn = name(pid, addr)[1]
            if fn not in seen:
                seen.add(fn)
                totals[fn] += 1
    print('  libraries: ' + ', '.join(f'{lib} {100 * c / n:.0f}%' for lib, c in libs.most_common(8)))
    print('  self:')
    for fn, c in selfs.most_common(18):
        print(f'    {100 * c / n:5.1f}%  {fn}')
    print('  total (with callees):')
    for fn, c in totals.most_common(30):
        print(f'    {100 * c / n:5.1f}%  {fn}')
