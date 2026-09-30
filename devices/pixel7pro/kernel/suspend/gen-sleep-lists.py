#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate pixel-sleep-lists.h from Google's GS201 PMUCAL tables.

Input (GPL-2.0-only, Copyright (c) 2021 Samsung Electronics Co., Ltd.), from
LineageOS android_kernel_google_gs201 @ 40ff934 (see STOCK_COMMIT):
  drivers/soc/google/cal-if/gs201/flexpmu_cal_system_gs201.h  (the lists)
  drivers/soc/google/cal-if/gs201/flexpmu_cal_local_gs201.h   (PD STATUS)
  drivers/soc/google/cal-if/gs201/flexpmu_cal_cpu_gs201.h     (CPU STATUS)

Output: the SYS_SLEEP enter/save/exit/early lists and pmucal_lpm_init, entry
for entry, with the stock access types (enum pmucal_seq_acctype values,
pmucal_common.h:19-38), masks, values and condition registers unchanged, each
tagged with its source line. Nothing is reordered, merged or dropped.

Every register base is resolved against BLOCKS below, which records the PMU
power domain that must be on before the block may be touched and how stock
writes it (pmucal_rae.c:140-148: PMU_ALIVE 0x1806xxxx through the secure SMC,
everything else with writel). An unknown base is an error, so a new stock
table can never add an unreviewed register block silently.

Usage:
  gen-sleep-lists.py --cal-dir <.../cal-if/gs201> [-o pixel-sleep-lists.h]
  gen-sleep-lists.py --fetch [-o pixel-sleep-lists.h]
"""
import argparse
import ast
import hashlib
import os
import re
import sys
import tempfile
import urllib.request

STOCK_COMMIT = '40ff93424549ffebfbba32e9435dfc58c40decb2'
STOCK_URL = ('https://raw.githubusercontent.com/LineageOS/android_kernel_google_gs201/'
             + STOCK_COMMIT + '/drivers/soc/google/cal-if/gs201/')
SYSTEM = 'flexpmu_cal_system_gs201.h'
LOCAL = 'flexpmu_cal_local_gs201.h'
CPU = 'flexpmu_cal_cpu_gs201.h'

# pmucal_common.h:19-38, same order and values.
TYPES = ['PMUCAL_READ', 'PMUCAL_WRITE', 'PMUCAL_COND_READ', 'PMUCAL_COND_WRITE',
         'PMUCAL_SAVE_RESTORE', 'PMUCAL_COND_SAVE_RESTORE', 'PMUCAL_WAIT',
         'PMUCAL_WAIT_TWO', 'PMUCAL_CHECK_SKIP', 'PMUCAL_COND_CHECK_SKIP',
         'PMUCAL_WRITE_WAIT', 'PMUCAL_WRITE_RETRY', 'PMUCAL_WRITE_RETRY_INV',
         'PMUCAL_WRITE_RETURN', 'PMUCAL_SET_BIT_ATOMIC', 'PMUCAL_CLR_BIT_ATOMIC',
         'PMUCAL_DELAY', 'PMUCAL_CLEAR_PEND']
# Types the runtime implements (the only ones in these five lists). A WAIT,
# DELAY or retry type would need polling; refuse to generate rather than
# silently emitting an entry the bounded runtime cannot execute exactly.
SUPPORTED = {'PMUCAL_READ', 'PMUCAL_WRITE', 'PMUCAL_COND_READ', 'PMUCAL_COND_WRITE',
             'PMUCAL_SAVE_RESTORE', 'PMUCAL_COND_SAVE_RESTORE',
             'PMUCAL_SET_BIT_ATOMIC', 'PMUCAL_CLEAR_PEND'}

LISTS = [  # (stock array, C array, expected entries)
    ('pmucal_lpm_init', 'ps_lpm_init', 102),
    ('enter_sleep', 'ps_enter', 3),
    ('save_sleep', 'ps_save', 545),
    ('exit_sleep', 'ps_exit', 37),
    ('early_sleep', 'ps_early', 9),
]

PMU, INTR, OTHER = 'PS_CLASS_PMU', 'PS_CLASS_INTR_GEN', 'PS_CLASS_OTHER'

# base: (name, PMU STATUS offset gating it or 0, write class, cluster, note)
# cluster: 0 = CPUCL0 block (read on a cluster-0 CPU), 1/2 = CPUCL1/2 block
# (read only while that cluster's NONCPU STATUS is on), -1 = not a CPU block.
# Status offsets: *_STATUS reads in flexpmu_cal_local_gs201.h (emitted in
# ps_pd[] with their lines); HSI1 0x2104 is the stock condition register in
# flexpmu_cal_system_gs201.h:176. CMU bases per power domain: the cmu_id of
# each exynos-pd node in arch/arm64/boot/dts/google/gs201-pm-domains.dtsi;
# the sysreg is cmu + 0x20000 in every stock pairing (p2vmap). Blocks with
# status 0 are PMU_ALIVE or TOP: powered whenever any AP CPU runs (stock has
# no PD for them; the SoC-down sequencer is the only thing that removes them).
BLOCKS = {
    0x18060000: ('PMU_ALIVE', 0, PMU, -1, 'SMC 0x82000504 (pmucal_rae.c:144)'),
    0x18070000: ('PMU_INTR_GEN', 0, INTR, -1, 'writel (pmucal_rae.c:147)'),
    0x18000000: ('CMU_APM', 0, OTHER, -1, 'ALIVE'),
    0x18020000: ('SYSREG_APM', 0, OTHER, -1, 'ALIVE'),
    0x10010000: ('CMU_MISC', 0, OTHER, -1, 'TOP'),
    0x10030000: ('SYSREG_MISC', 0, OTHER, -1, 'TOP'),
    0x10800000: ('CMU_PERIC0', 0, OTHER, -1, 'TOP'),
    0x10820000: ('SYSREG_PERIC0', 0, OTHER, -1, 'TOP'),
    0x10c00000: ('CMU_PERIC1', 0, OTHER, -1, 'TOP'),
    0x10c20000: ('SYSREG_PERIC1', 0, OTHER, -1, 'TOP'),
    0x1e080000: ('CMU_TOP', 0, OTHER, -1, 'TOP'),
    0x20800000: ('CMU_MIF0', 0, OTHER, -1, 'MIF, TOP while awake'),
    0x20900000: ('CMU_MIF1', 0, OTHER, -1, 'MIF, TOP while awake'),
    0x20a00000: ('CMU_MIF2', 0, OTHER, -1, 'MIF, TOP while awake'),
    0x20b00000: ('CMU_MIF3', 0, OTHER, -1, 'MIF, TOP while awake'),
    0x20820000: ('SYSREG_MIF0', 0, OTHER, -1, 'MIF'),
    0x20920000: ('SYSREG_MIF1', 0, OTHER, -1, 'MIF'),
    0x20a20000: ('SYSREG_MIF2', 0, OTHER, -1, 'MIF'),
    0x20b20000: ('SYSREG_MIF3', 0, OTHER, -1, 'MIF'),
    0x20840000: ('DMC_MIF0', 0, OTHER, -1, 'MIF PWRMGMT_BUNDLE'),
    0x20940000: ('DMC_MIF1', 0, OTHER, -1, 'MIF PWRMGMT_BUNDLE'),
    0x20a40000: ('DMC_MIF2', 0, OTHER, -1, 'MIF PWRMGMT_BUNDLE'),
    0x20b40000: ('DMC_MIF3', 0, OTHER, -1, 'MIF PWRMGMT_BUNDLE'),
    0x20c00000: ('CMU_CPUCL0', 0, OTHER, 0, 'cluster 0'),
    0x20c40000: ('SYSREG_CPUCL0', 0, OTHER, 0, 'cluster 0'),
    0x20c10000: ('CMU_CPUCL1', 0x1404, OTHER, 1, 'CLUSTER1_NONCPU_STATUS'),
    0x20c20000: ('CMU_CPUCL2', 0x1604, OTHER, 2, 'CLUSTER2_NONCPU_STATUS'),
    0x1e000000: ('CMU_NOCL0', 0x1b84, OTHER, -1, 'NOCL0_STATUS'),
    0x1e020000: ('SYSREG_NOCL0', 0x1b84, OTHER, -1, 'NOCL0_STATUS'),
    0x1e800000: ('CMU_NOCL1B', 0x1a04, OTHER, -1, 'NOCL1B_STATUS'),
    0x1e820000: ('SYSREG_NOCL1B', 0x1a04, OTHER, -1, 'NOCL1B_STATUS (stock COND, exit)'),
    0x1f000000: ('CMU_NOCL2A', 0x1a84, OTHER, -1, 'NOCL2A_STATUS'),
    0x1f020000: ('SYSREG_NOCL2A', 0x1a84, OTHER, -1, 'NOCL2A_STATUS (stock COND, exit)'),
    0x20000000: ('CMU_NOCL1A', 0x1b04, OTHER, -1, 'NOCL1A_STATUS'),
    0x20020000: ('SYSREG_NOCL1A', 0x1b04, OTHER, -1, 'NOCL1A_STATUS (stock COND, exit)'),
    0x20510000: ('TREX_D_NOCL1A', 0x1b04, OTHER, -1, 'NOCL1A_STATUS (INF: NOCL1A range)'),
    0x11020000: ('SYSREG_HSI0', 0x2084, OTHER, -1, 'HSI0_STATUS, pd_hsi0 cmu 0x11000000'),
    0x11800000: ('CMU_HSI1', 0x2104, OTHER, -1, 'HSI1_STATUS (stock COND)'),
    0x11820000: ('SYSREG_HSI1', 0x2104, OTHER, -1, 'HSI1_STATUS'),
    0x14420000: ('SYSREG_HSI2', 0x2184, OTHER, -1, 'HSI2_STATUS, pd_hsi2 cmu 0x14400000'),
    0x14700000: ('UFS_HSI2', 0x2184, OTHER, -1, 'HSI2_STATUS (UFS HCI, ufs-exynos)'),
    0x17020000: ('SYSREG_EH', 0x1c04, OTHER, -1, 'EH_STATUS, pd_eh cmu 0x17000000'),
    0x1a020000: ('SYSREG_AOC', 0x1884, OTHER, -1, 'AOC_STATUS, pd_aoc cmu 0x1A000000'),
    0x1a420000: ('SYSREG_CSIS', 0x2404, OTHER, -1, 'CSIS_STATUS'),
    0x1a820000: ('SYSREG_G3AA', 0x2584, OTHER, -1, 'G3AA_STATUS'),
    0x1aa20000: ('SYSREG_PDP', 0x2484, OTHER, -1, 'PDP_STATUS'),
    0x1ac20000: ('SYSREG_IPP', 0x2604, OTHER, -1, 'IPP_STATUS'),
    0x1b020000: ('SYSREG_DNS', 0x2504, OTHER, -1, 'DNS_STATUS'),
    0x1b420000: ('SYSREG_ITP', 0x2684, OTHER, -1, 'ITP_STATUS'),
    0x1b720000: ('SYSREG_MCSC', 0x2704, OTHER, -1, 'MCSC_STATUS'),
    0x1bc20000: ('SYSREG_TNR', 0x2804, OTHER, -1, 'TNR_STATUS'),
    0x1c020000: ('SYSREG_DPU', 0x2204, OTHER, -1, 'DPU_STATUS'),
    0x1c0b0000: ('DPUF_DMA', 0x2204, OTHER, -1, 'DPU_STATUS (INF: DPU range)'),
    0x1c220000: ('SYSREG_DISP', 0x2284, OTHER, -1, 'DISP_STATUS'),
    0x1c620000: ('SYSREG_G2D', 0x2304, OTHER, -1, 'G2D_STATUS'),
    0x1c820000: ('SYSREG_MFC', 0x2384, OTHER, -1, 'MFC_STATUS'),
    0x1ca20000: ('SYSREG_BO', 0x2884, OTHER, -1, 'BO_STATUS'),
    0x1cc00000: ('CMU_TPU', 0x2904, OTHER, -1, 'TPU_STATUS (stock COND)'),
    0x1cc20000: ('SYSREG_TPU', 0x2904, OTHER, -1, 'TPU_STATUS'),
    0x1d020000: ('SYSREG_GDC', 0x2784, OTHER, -1, 'GDC_STATUS'),
    0x25a00000: ('CMU_AUR', 0x2984, OTHER, -1, 'AUR_STATUS (stock COND)'),
    0x25a40000: ('SYSREG_AUR', 0x2984, OTHER, -1, 'AUR_STATUS'),
    0x27f00000: ('CMU_G3D', 0x1e04, OTHER, -1, 'G3D_STATUS (stock COND)'),
    0x27f20000: ('SYSREG_G3D', 0x1e04, OTHER, -1, 'G3D_STATUS (stock COND, exit)'),
}

ENTRY = re.compile(r'PMUCAL_SEQ_DESC\((.*)\)\s*,?\s*$', re.S)


def value(text):
    """Evaluate a stock integer expression: literals, <<, |, parentheses."""
    tree = ast.parse(text.strip(), mode='eval')

    def ev(node):
        if isinstance(node, ast.Expression):
            return ev(node.body)
        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return node.value
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.LShift):
            return ev(node.left) << ev(node.right)
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.BitOr):
            return ev(node.left) | ev(node.right)
        raise ValueError('unsupported expression: ' + text)
    v = ev(tree)
    if v < 0 or v > 0xffffffff:
        raise ValueError('value out of 32 bits: ' + text)
    return v


def split_args(s):
    out, depth, cur, quote = [], 0, '', False
    for ch in s:
        if ch == '"':
            quote = not quote
        if not quote and ch == '(':
            depth += 1
        if not quote and ch == ')':
            depth -= 1
        if not quote and ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
            continue
        cur += ch
    out.append(cur.strip())
    return out


def parse_arrays(text):
    """{array: [(line, [10 fields])]} for every struct pmucal_seq array."""
    arrays, cur, buf, start = {}, None, '', 0
    for no, line in enumerate(text.split('\n'), 1):
        m = re.match(r'struct pmucal_seq (\w+)\[\] = \{', line)
        if m:
            cur = m.group(1)
            arrays[cur] = []
            buf = ''
            continue
        if cur is None:
            continue
        if line.strip() == '};':
            if buf.strip():
                raise ValueError('%s: unterminated entry before line %d' % (cur, no))
            cur = None
            continue
        if not buf:
            start = no
        buf += line + '\n'
        if buf.rstrip().endswith('),'):
            m = ENTRY.match(buf.strip())
            if not m:
                raise ValueError('line %d: not a PMUCAL_SEQ_DESC entry' % start)
            f = split_args(m.group(1))
            if len(f) != 10:
                raise ValueError('line %d: %d fields' % (start, len(f)))
            arrays[cur].append((start, f))
            buf = ''
    return arrays


def check_lpm_list(text):
    """The SYS_SLEEP slot of pmucal_lpm_list must name the four sleep arrays."""
    m = re.search(r'\[SYS_SLEEP\]\s*=\s*\{(.*?)\}', text, re.S)
    if not m:
        raise ValueError('no [SYS_SLEEP] in pmucal_lpm_list')
    body = m.group(1)
    for field, arr in (('enter', 'enter_sleep'), ('save', 'save_sleep'),
                       ('exit', 'exit_sleep'), ('early_wakeup', 'early_sleep')):
        if not re.search(r'\.%s\s*=\s*%s\s*,' % (field, arr), body):
            raise ValueError('[SYS_SLEEP].%s is not %s' % (field, arr))


def status_regs(local, cpu, system_lines):
    """PMU STATUS registers with their stock lines, for ps_pd[]."""
    out = {}
    for fname, text in ((CPU, cpu), (LOCAL, local)):
        for no, line in enumerate(text.split('\n'), 1):
            m = re.search(r'PMUCAL_READ, "(\w+_STATUS)", 0x18060000, (0x[0-9a-f]+),', line)
            if m and int(m.group(2), 16) not in out:
                out[int(m.group(2), 16)] = (m.group(1), fname, no)
    # HSI1 has no cal-if local PD; stock uses its STATUS as the save condition.
    no = next(n for n, l in system_lines if '0x18060000, 0x2104,' in l)
    out.setdefault(0x2104, ('HSI1_STATUS', SYSTEM, no))
    return out


def c_str(s):
    if not re.fullmatch(r'[A-Za-z0-9_]+', s):
        raise ValueError('unexpected register name ' + s)
    return '"%s"' % s


def generate(srcdir):
    texts, sums = {}, {}
    for f in (SYSTEM, LOCAL, CPU):
        raw = open(os.path.join(srcdir, f), 'rb').read()
        sums[f] = hashlib.sha256(raw).hexdigest()
        texts[f] = raw.decode()
    system = texts[SYSTEM]
    # The lists live in the kernel half; the ACPM_FRAMEWORK half is firmware.
    kernel_half = system.split('#else', 1)[0]
    arrays = parse_arrays(kernel_half)
    check_lpm_list(kernel_half)
    pds = status_regs(texts[LOCAL], texts[CPU], list(enumerate(system.split('\n'), 1)))

    lists, used = {}, {}
    for stock, cname, want in LISTS:
        ents = arrays.get(stock)
        if ents is None or len(ents) != want:
            raise ValueError('%s: %s entries, expected %d'
                             % (stock, None if ents is None else len(ents), want))
        rows = []
        for line, f in ents:
            typ = f[0]
            if typ not in TYPES:
                raise ValueError('line %d: unknown type %s' % (line, typ))
            if typ not in SUPPORTED:
                raise ValueError('line %d: %s needs a runtime this module lacks' % (line, typ))
            name = f[1].strip('"')
            base, off, mask, val = (value(x) for x in f[2:6])
            cbase, coff, cmask, cval = (value(x) for x in f[6:10])
            for b in (base,) + ((cbase,) if cbase else ()):
                if b not in BLOCKS:
                    raise ValueError('line %d: base %#x has no BLOCKS entry' % (line, b))
            if cbase and cbase not in (0x18060000, 0x18070000):
                raise ValueError('line %d: condition outside PMU/INTR_GEN' % line)
            if typ == 'PMUCAL_SET_BIT_ATOMIC' and (base != 0x18060000 or off > 0x3fff
                                                   or val > 31):
                raise ValueError('line %d: atomic set outside PMU_ALIVE' % line)
            if typ in ('PMUCAL_COND_READ', 'PMUCAL_COND_WRITE', 'PMUCAL_COND_SAVE_RESTORE',
                       'PMUCAL_CLEAR_PEND') and not cbase:
                raise ValueError('line %d: %s without a condition register' % (line, typ))
            ext = off + 4
            if typ == 'PMUCAL_SET_BIT_ATOMIC':
                ext = off + 4  # the 0xc000 alias is written by SMC, never mapped
            used[base] = max(used.get(base, 0), ext)
            if cbase:
                used[cbase] = max(used.get(cbase, 0), coff + 4)
            rows.append((line, typ, name, base, off, mask, val, cbase, coff, cmask, cval))
        lists[cname] = (stock, rows)

    # Mapping for the PD STATUS reads and the fixed ALIVE registers.
    used[0x18060000] = max(used.get(0x18060000, 0), 0x4000)
    used[0x18070000] = max(used.get(0x18070000, 0), 0x2000)
    bases = sorted(used)
    index = {b: i for i, b in enumerate(bases)}
    if len(bases) > 250:
        raise ValueError('too many blocks for a u8 index')

    o = []
    w = o.append
    w('/* SPDX-License-Identifier: GPL-2.0-only */')
    w('/*')
    w(' * GENERATED by gen-sleep-lists.py; do not edit.')
    w(' *')
    w(' * Register tables transcribed entry for entry from Google/Samsung GS201')
    w(' * PMUCAL sources, Copyright (c) 2021 Samsung Electronics Co., Ltd.,')
    w(' * GPL-2.0-only, LineageOS android_kernel_google_gs201 @ %s:' % STOCK_COMMIT[:12])
    for f in (SYSTEM, LOCAL, CPU):
        w(' *   drivers/soc/google/cal-if/gs201/%s' % f)
        w(' *     sha256 %s' % sums[f])
    w(' * Each entry keeps its stock access type (enum pmucal_seq_acctype),')
    w(' * mask, value and condition; "line" is its line in %s.' % SYSTEM)
    w(' */')
    w('#ifndef PIXEL_SLEEP_LISTS_H')
    w('#define PIXEL_SLEEP_LISTS_H')
    w('')
    w('#define PS_STOCK_COMMIT "%s"' % STOCK_COMMIT)
    w('')
    w('/* Register blocks: base, mapped size, gating PMU STATUS (0 = ALIVE/TOP),')
    w(' * write class, cluster (-1 none), name, note. */')
    w('static const struct ps_block ps_blocks[] __maybe_unused = {')
    for b in bases:
        name, st, cls, cl, note = BLOCKS[b]
        size = (used[b] + 0xfff) & ~0xfff
        w('\t{ %#010x, %#07x, %#06x, %s, %d, "%s", "%s" },'
          % (b, size, st, cls, cl, name, note))
    w('};')
    w('#define PS_NR_BLOCKS %d' % len(bases))
    for b in bases:
        w('#define PS_BLK_%s %d' % (BLOCKS[b][0], index[b]))
    w('')
    w('/* Every PMU power STATUS register named in the stock cal-if tables. */')
    w('static const struct ps_pd ps_pd[] __maybe_unused = {')
    for off in sorted(pds):
        name, fname, no = pds[off]
        w('\t{ %#06x, %4d, "%s", "%s" },' % (off, no, name, fname))
    w('};')
    w('')
    for cname, (stock, rows) in lists.items():
        w('/* %s: %s, lines %d-%d (%d entries). */'
          % (cname, stock, rows[0][0], rows[-1][0], len(rows)))
        w('static const struct ps_seq %s[] __maybe_unused = {' % cname)
        for (line, typ, name, base, off, mask, val, cbase, coff, cmask, cval) in rows:
            cb = index[cbase] if cbase else 'PS_NO_BLOCK'
            w('\tPS_SEQ(%s, %s, %d, %#06x, %#010x, %#010x, %s, %#06x, %#010x, %#010x, %d),'
              % (typ.replace('PMUCAL_', 'PS_'), c_str(name), index[base], off, mask, val,
                 cb, coff, cmask, cval, line))
        w('};')
        w('#define %s_N %d' % (cname.upper(), len(rows)))
        w('')
    w('#endif')
    return '\n'.join(o) + '\n', {k: len(v[1]) for k, v in lists.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cal-dir', help='directory holding the three stock headers')
    ap.add_argument('--fetch', action='store_true', help='download them at STOCK_COMMIT')
    ap.add_argument('-o', '--output', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), 'pixel-sleep-lists.h'))
    args = ap.parse_args()
    if bool(args.cal_dir) == bool(args.fetch):
        ap.error('give exactly one of --cal-dir or --fetch')
    if args.fetch:
        with tempfile.TemporaryDirectory(prefix='gs201-cal-') as tmp:
            for f in (SYSTEM, LOCAL, CPU):
                with urllib.request.urlopen(STOCK_URL + f) as r, \
                        open(os.path.join(tmp, f), 'wb') as o:
                    o.write(r.read())
            text, counts = generate(tmp)
    else:
        text, counts = generate(args.cal_dir)
    with open(args.output, 'w') as f:
        f.write(text)
    print('wrote %s: %s' % (args.output, ', '.join('%s %d' % kv for kv in counts.items())))


if __name__ == '__main__':
    sys.exit(main())
