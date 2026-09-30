#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compare a pixel-sleep-audit report with the stock GS201 tables.

    sleep-audit-diff.py report.txt [--dt live.dts] [--all]

Checks, printing each mismatch:
- pmucal_lpm_init: every target read against the stock value under its mask
  (the report already carries both; flexpmu_cal_system_gs201.h:8-111);
- PMU state against the plan's tables (docs/deep-sleep-plan-20260930.md
  §1.2-1.4): wake enables and CPU_INFORM clear while awake, TOP_OUT retention
  bits released, lpm durations, PCIe PHY isolation, MCT running;
- PMIC opmodes: each regulator's enable field against what the stock
  s2mpg12/13 regulator drivers write for its DT node (always-on rails get
  of_map_mode(regulator-initial-mode): SUSPEND 1, MIF 2, ON/other 3, shifted
  into the enable mask; s2mpg12-regulator.c:38-83, s2mpg13-regulator.c:38-83),
  and PCTRLSEL1-14 / 1-11 against sel_vgpio (s2mpg12-regulator.c:859-880,
  s2mpg13-regulator.c:686-705);
- FLEXPMU: mif_always_on and the counters.

--dt takes a decompiled stock DT (base + DTBO, e.g. the saved
out/restart-20260924/root-baseline/live.dts). Without it the table below,
taken from that same live DT (board_id 0x30306, dtbo index 6), is used.
"""
import argparse
import os
import re
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DT = os.path.join(HERE, '..', '..', 'out', 'restart-20260924', 'root-baseline', 'live.dts')

# Stock regulator enable fields: (pmic, enable_reg, enable_mask).
# S2MPG12: s2mpg12-regulator.c:233-294; S2MPG13: s2mpg13-regulator.c:217-300.
ENABLE = {
    'LDO1M': ('m', 0x2b, 0x80), 'LDO2M': ('m', 0x2c, 0x80), 'LDO3M': ('m', 0x2d, 0x80),
    'LDO4M': ('m', 0x2f, 0xc0), 'LDO5M': ('m', 0x30, 0xc0), 'LDO6M': ('m', 0x31, 0xc0),
    'LDO7M': ('m', 0x48, 0x03), 'LDO8M': ('m', 0x33, 0xc0), 'LDO9M': ('m', 0x34, 0xc0),
    'LDO10M': ('m', 0x35, 0xc0), 'LDO11M': ('m', 0x48, 0x0c), 'LDO12M': ('m', 0x48, 0x30),
    'LDO13M': ('m', 0x48, 0xc0), 'LDO14M': ('m', 0x39, 0xc0), 'LDO15M': ('m', 0x49, 0x03),
    'LDO16M': ('m', 0x3b, 0xc0), 'LDO17M': ('m', 0x49, 0x0c), 'LDO18M': ('m', 0x3d, 0xc0),
    'LDO19M': ('m', 0x49, 0x30), 'LDO20M': ('m', 0x3f, 0xc0), 'LDO21M': ('m', 0x40, 0x80),
    'LDO22M': ('m', 0x49, 0xc0), 'LDO23M': ('m', 0x42, 0xc0), 'LDO24M': ('m', 0x43, 0x80),
    'LDO25M': ('m', 0x44, 0x80), 'LDO26M': ('m', 0x45, 0xc0), 'LDO27M': ('m', 0x46, 0x80),
    'LDO28M': ('m', 0x47, 0x80),
    'BUCK1M': ('m', 0x17, 0xc0), 'BUCK2M': ('m', 0x19, 0xc0), 'BUCK3M': ('m', 0x1b, 0xc0),
    'BUCK4M': ('m', 0x1d, 0xc0), 'BUCK5M': ('m', 0x1f, 0xc0), 'BUCK6M': ('m', 0x21, 0xc0),
    'BUCK7M': ('m', 0x23, 0xc0), 'BUCK8M': ('m', 0x25, 0x80), 'BUCK9M': ('m', 0x27, 0xc0),
    'BUCK10M': ('m', 0x29, 0xc0),
    'LDO1S': ('s', 0x48, 0x03), 'LDO2S': ('s', 0x2d, 0xc0), 'LDO3S': ('s', 0x2e, 0xc0),
    'LDO4S': ('s', 0x2f, 0x80), 'LDO5S': ('s', 0x30, 0x80), 'LDO6S': ('s', 0x31, 0x80),
    'LDO7S': ('s', 0x32, 0x80), 'LDO8S': ('s', 0x33, 0xc0), 'LDO9S': ('s', 0x34, 0x80),
    'LDO10S': ('s', 0x35, 0x80), 'LDO11S': ('s', 0x36, 0x80), 'LDO12S': ('s', 0x37, 0x80),
    'LDO13S': ('s', 0x38, 0xc0), 'LDO14S': ('s', 0x39, 0x80), 'LDO15S': ('s', 0x3a, 0x80),
    'LDO16S': ('s', 0x3b, 0x80), 'LDO17S': ('s', 0x3c, 0x80), 'LDO18S': ('s', 0x3d, 0xc0),
    'LDO19S': ('s', 0x3e, 0xc0), 'LDO20S': ('s', 0x3f, 0xc0), 'LDO21S': ('s', 0x40, 0x80),
    'LDO22S': ('s', 0x41, 0x80), 'LDO23S': ('s', 0x48, 0x0c), 'LDO24S': ('s', 0x48, 0x30),
    'LDO25S': ('s', 0x48, 0x80), 'LDO26S': ('s', 0x49, 0x03), 'LDO27S': ('s', 0x46, 0xc0),
    'LDO28S': ('s', 0x47, 0x80),
    'BUCK1S': ('s', 0x0f, 0xc0), 'BUCK2S': ('s', 0x11, 0xc0), 'BUCK3S': ('s', 0x13, 0xc0),
    'BUCK4S': ('s', 0x15, 0xc0), 'BUCK5S': ('s', 0x17, 0xc0), 'BUCK6S': ('s', 0x19, 0xc0),
    'BUCK7S': ('s', 0x1b, 0xc0), 'BUCK8S': ('s', 0x1d, 0xc0), 'BUCK9S': ('s', 0x1f, 0xc0),
    'BUCK10S': ('s', 0x22, 0xc0), 'BUCKD': ('s', 0x24, 0xc0), 'BUCKA': ('s', 0x26, 0xc0),
    'BUCKC': ('s', 0x28, 0xc0), 'BUCKBOOST': ('s', 0x2a, 0x80),
}
NEVER = {'LDO14S'}  # eSE/eSIM rail: never read, never written

# (initial-mode, always-on, boot-on) from the live stock DT (base + dtbo 6).
MODES = {
    'BUCK1M': (1, 1, 0), 'BUCK2M': (1, 1, 0), 'BUCK3M': (1, 1, 0), 'BUCK4M': (1, 1, 0),
    'BUCK5M': (1, 1, 0), 'BUCK6M': (3, 1, 0), 'BUCK7M': (1, 1, 0), 'BUCK8M': (3, 1, 0),
    'BUCK9M': (3, 1, 0), 'BUCK10M': (1, 1, 0),
    'LDO1M': (3, 1, 0), 'LDO2M': (3, 1, 0), 'LDO3M': (1, 1, 0), 'LDO4M': (1, 1, 0),
    'LDO5M': (1, 1, 0), 'LDO6M': (1, 1, 0), 'LDO7M': (1, 0, 0), 'LDO8M': (1, 0, 0),
    'LDO9M': (3, 0, 0), 'LDO10M': (3, 0, 0), 'LDO11M': (1, 1, 0), 'LDO12M': (1, 1, 0),
    'LDO13M': (1, 1, 0), 'LDO14M': (1, 1, 0), 'LDO15M': (3, 1, 0), 'LDO16M': (1, 1, 0),
    'LDO17M': (1, 1, 0), 'LDO18M': (1, 1, 0), 'LDO19M': (0, 0, 0), 'LDO20M': (3, 1, 0),
    'LDO21M': (3, 1, 0), 'LDO22M': (0, 0, 0), 'LDO23M': (0, 0, 0), 'LDO24M': (0, 0, 0),
    'LDO25M': (3, 0, 0), 'LDO26M': (3, 0, 0), 'LDO27M': (3, 0, 0), 'LDO28M': (3, 0, 0),
    'BUCK1S': (1, 1, 0), 'BUCK2S': (1, 1, 0), 'BUCK3S': (3, 1, 0), 'BUCK4S': (3, 1, 0),
    'BUCK5S': (1, 1, 0), 'BUCK6S': (3, 1, 0), 'BUCK7S': (3, 1, 0), 'BUCK8S': (1, 1, 0),
    'BUCK9S': (3, 1, 0), 'BUCK10S': (3, 1, 0), 'BUCKA': (3, 1, 0), 'BUCKC': (1, 1, 0),
    'BUCKD': (1, 1, 0), 'BUCKBOOST': (3, 1, 0),
    'LDO1S': (1, 1, 0), 'LDO2S': (1, 1, 0), 'LDO3S': (1, 1, 0), 'LDO4S': (0, 0, 0),
    'LDO5S': (3, 0, 1), 'LDO6S': (0, 0, 0), 'LDO7S': (3, 0, 1), 'LDO8S': (0, 0, 0),
    'LDO9S': (3, 1, 0), 'LDO10S': (3, 1, 0), 'LDO11S': (3, 1, 0), 'LDO12S': (0, 0, 0),
    'LDO13S': (1, 1, 0), 'LDO14S': (3, 1, 0), 'LDO15S': (3, 1, 0), 'LDO16S': (3, 1, 0),
    'LDO17S': (3, 1, 0), 'LDO18S': (1, 1, 0), 'LDO19S': (3, 1, 0), 'LDO20S': (3, 1, 0),
    'LDO21S': (3, 1, 0), 'LDO22S': (0, 0, 0), 'LDO23S': (0, 0, 0), 'LDO24S': (0, 0, 0),
    'LDO25S': (0, 0, 0), 'LDO26S': (0, 0, 0), 'LDO27S': (0, 0, 0), 'LDO28S': (0, 0, 0),
}
# sel_vgpio: gs201-pmic.dtsi:124-125 (S2MPG12) and 746-747 (S2MPG13).
SEL_VGPIO = {
    'm': [0xC2, 0x0B, 0x22, 0xA2, 0x2D, 0x2F, 0x55, 0x55, 0x0B, 0x7D, 0x32, 0x3C, 0x80, 0x80],
    's': [0x92, 0x92, 0x69, 0x0A, 0x9A, 0x12, 0xCA, 0x81, 0x08, 0x00, 0x8A],
}
OPMODE = {0: 'OFF', 1: 'SUSPEND', 2: 'MIF', 3: 'ON'}
# Stock lpm_init PMU durations, flexpmu_cal_system_gs201.h:101-107.
TOP_OUT_BITS = (7, 9, 11, 12, 13, 14)   # exit_sleep SET_BIT_ATOMIC, lines 727-732


def parse_dt(path):
    txt = open(path).read()
    modes, sel = {}, {}
    names = {'OFF': 0, 'SUSPEND': 1, 'MIF': 2, 'TCXO': 2, 'LOWPOWER': 2, 'ON': 3}
    for m in re.finditer(r'\b((?:BUCK|LDO)\w*)\s*\{([^{}]*)\}', txt):
        body = m.group(2)
        if 'regulator-name' not in body:
            continue
        im = re.search(r'regulator-initial-mode\s*=\s*<\s*([^>\s]+)\s*>', body)
        mode = None
        if im:
            v = im.group(1)
            mode = names[v.replace('SEC_OPMODE_', '')] if 'SEC_OPMODE' in v else int(v, 0)
        modes[m.group(1)] = (mode, 'regulator-always-on' in body, 'regulator-boot-on' in body)
    for m in re.finditer(r'sel_vgpio\s*=\s*<([^>]*)>', txt):
        vals = [int(x, 0) for x in m.group(1).split()]
        sel['m' if len(vals) == 14 else 's'] = vals
    return modes, sel


def expected_field(mode, mask):
    mapped = {1: 1, 2: 2, 3: 3}.get(mode, 3)      # of_map_mode: default -> 3
    shift = (mask & -mask).bit_length() - 1
    return (mapped << shift) & mask


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('report')
    ap.add_argument('--dt', help='decompiled stock DT (default: saved live.dts if present)')
    ap.add_argument('--all', action='store_true', help='also list matching entries')
    args = ap.parse_args()

    modes, sel = MODES, SEL_VGPIO
    dt = args.dt or (DEFAULT_DT if os.path.exists(DEFAULT_DT) else None)
    if dt:
        dm, ds = parse_dt(dt)
        if dm:
            modes = {k: (v[0], int(v[1]), int(v[2])) for k, v in dm.items()}
        sel = dict(SEL_VGPIO, **ds)
        print('stock DT: %s (%d regulators)' % (dt, len(modes)))
    else:
        print('stock DT: built-in table from live.dts (dtbo index 6)')

    pmu, pd, intr, lpm, save, pmic, other = {}, {}, {}, [], [], {}, []
    flex = None
    for line in open(args.report):
        f = line.split()
        if not f or f[0].startswith('#'):
            continue
        if f[0] == 'pmu':
            pmu[f[2]] = int(f[3], 16)
        elif f[0] == 'pd':
            pd[f[2]] = f[3]
        elif f[0] == 'intr':
            intr[f[2]] = int(f[3], 16)
        elif f[0] == 'lpm':
            lpm.append(f)
        elif f[0] == 'save':
            save.append(f)
        elif f[0] == 'pmic' and len(f) >= 5 and f[1] in 'ms':
            if not f[4].startswith('err'):
                pmic[(f[1], int(f[2], 16))] = int(f[4], 16)
        elif f[0] == 'flexpmu':
            flex = f[1:]
        else:
            other.append(line.rstrip())
    problems = 0

    def bad(msg):
        nonlocal problems
        problems += 1
        print('  MISMATCH ' + msg)

    print('\n== pmucal_lpm_init (%d entries) ==' % len(lpm))
    skipped = Counter()
    for f in lpm:
        idx, line, pa, name, mask, want = f[1], f[2], f[3], f[4], int(f[5], 16), int(f[6], 16)
        if f[7] == '-':
            skipped[f[8]] += 1
            continue
        val = int(f[7], 16)
        if (val & mask) != (want & mask):
            bad('line %s %s %s = %#010x, stock %#010x under mask %#010x'
                % (line, name, pa, val, (val & ~mask & 0xffffffff) | (want & mask), mask))
        elif args.all:
            print('  ok line %s %s %s = %#010x' % (line, name, pa, val))
    if skipped:
        print('  not read: ' + ', '.join('%s %d' % kv for kv in sorted(skipped.items())))

    print('\n== PMU (plan §1.2-1.4) ==')
    for reg in ('WAKEUP_INT_EN', 'WAKEUP2_INT_EN'):
        if pmu.get(reg):
            bad('%s = %#x while awake (exit/early lists leave 0)' % (reg, pmu[reg]))
    for cpu in range(8):
        v = pmu.get('CPU_INFORM%d' % cpu)
        if v not in (None, 0):
            print('  note CPU_INFORM%d = %d (1 C2, 2 CPD, 3 SICD, 4 SLEEP)' % (cpu, v))
    if pmu.get('CPU_INFORM0') not in (None, 0, 1, 2):
        bad('CPU_INFORM0 = %d: a sleep hint left set' % pmu['CPU_INFORM0'])
    top = pmu.get('TOP_OUT')
    if top is not None:
        missing = [b for b in TOP_OUT_BITS if not top & (1 << b)]
        if missing:
            bad('TOP_OUT %#x lacks bits %s that exit_sleep sets' % (top, missing))
        print('  TOP_OUT %#010x, SYSTEM_CTRL %#010x (bit 14 %d), CPU0_INT_EN %#x'
              % (top, pmu.get('SYSTEM_CTRL', 0), (pmu.get('SYSTEM_CTRL', 0) >> 14) & 1,
                 pmu.get('CLUSTER0_CPU0_INT_EN', 0)))
    for reg in ('PCIE_PHY_CONTROL_HSI1', 'PCIE_PHY_CONTROL_HSI2'):
        if reg in pmu:
            print('  %s %#x: %s' % (reg, pmu[reg],
                                    'bypass (link may be up)' if pmu[reg] & 1 else 'isolated'))
    print('  EINT masks %s' % ' '.join('%08x' % pmu.get(r, 0) for r in
                                        ('EINT_WAKEUP_MASK', 'EINT_WAKEUP_MASK2', 'EINT_WAKEUP_MASK3')))
    print('  PD on: ' + ' '.join(sorted(k for k, v in pd.items() if v == 'on')))
    print('  PD off: ' + ' '.join(sorted(k for k, v in pd.items() if v == 'off')))
    pend = {k: v for k, v in intr.items() if 'UPEND' in k and v}
    print('  INTR_GEN pending: %s; GRP2 enable %#x'
          % (', '.join('%s %#x' % kv for kv in sorted(pend.items())) or 'none',
             intr.get('GRP2_INTR_BID_ENABLE', 0)))

    print('\n== save_sleep (%d entries) ==' % len(save))
    by_blk = defaultdict(Counter)
    for f in save:
        blk = f[4][:6]
        by_blk[blk]['read' if f[-1] == 'ok' else f[-1]] += 1
    for blk in sorted(by_blk):
        print('  %s0000 %s' % (blk, ', '.join('%s %d' % kv for kv in sorted(by_blk[blk].items()))))

    print('\n== other ==')
    for line in other:
        print('  ' + line)
        if line.startswith('mct') and not int(line.split()[-1], 16) & (1 << 8):
            bad('MCT G_TCON timer not started (bit 8)')

    print('\n== PMIC enable fields vs stock DT ==')
    for name in sorted(ENABLE, key=lambda n: (ENABLE[n][0], n)):
        chip, reg, mask = ENABLE[name]
        if name in NEVER:
            print('  %-9s never read (eSE/eSIM rail)' % name)
            continue
        if (chip, reg) not in pmic:
            print('  %-9s not in report' % name)
            continue
        val = pmic[(chip, reg)]
        field = val & mask
        mode, ao, bo = modes.get(name, (None, 0, 0))
        shift = (mask & -mask).bit_length() - 1
        desc = '%s %#04x field %d (mode %s, %s)' % (
            chip.upper(), reg, field >> shift, OPMODE.get(mode, mode),
            'always-on' if ao else 'boot-on' if bo else 'consumer')
        if ao:
            want = expected_field(mode, mask)
            if field != want:
                bad('%-9s %s, stock writes %d' % (name, desc, want >> shift))
                continue
        if args.all or not ao:
            print('  %-9s %s' % (name, desc))
    for chip, base in (('m', 0x9b), ('s', 0x97)):
        for i, want in enumerate(sel.get(chip, [])):
            got = pmic.get((chip, base + i))
            if got is None:
                continue
            if got != want:
                bad('%s PCTRLSEL%d (%#04x) = %#04x, sel_vgpio %#04x'
                    % (chip.upper(), i + 1, base + i, got, want))

    print('\n== FLEXPMU ==')
    if not flex or flex[0] == 'unavailable':
        print('  unavailable (kernel without patch 0003)')
    else:
        kv = dict(zip(flex[0::2], flex[1::2]))
        print('  ' + ' '.join(flex))
        if kv.get('mif_always_on') not in (None, '0'):
            print('  note mif_always_on=%s: the MIF-down sequence is disabled by firmware policy'
                  % kv['mif_always_on'])
    print('\n%d mismatches' % problems)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
