#!/usr/bin/env python3
"""Read-only GS201 USB power, clock and PHY snapshot (never writes a register).

Prints the HSI0 power domain, the PMU USB PHY isolation controls, the
CMU_TOP/CMU_HSI0 USB clock mux, divider and gate registers, the USB PHY
controller block and a few DWC3 registers. The pixel boot script saves one
per boot so a normal (bootloader, not fastboot) boot can be compared with a
RAM boot, whose USB hardware ABL's fastboot leaves running.

Addresses: the stock DT (pd-hsi0@18062080, exynos-pmu 0x18060000 with
pmu_offset 0x3eb0 / pmu_offset_dp 0x3eb4, phy@11200000, usb@11210000) and
Google's GS201 cmucal-sfr.c (CMU_HSI0 0x11000000, CMU_TOP 0x1e080000).
"""
import mmap
import os
import struct
import sys

BLOCKS = [
    ('pd_hsi0', 0x18062080, [0x0, 0x4, 0x8, 0xc]),
    ('pmu_usb', 0x18063eb0, [0x0, 0x4]),
    ('cmu_top', 0x1e080000, [0x108c, 0x1888, 0x20b0]),
    ('cmu_hsi0', 0x11000000, [0x650, 0x654, 0x1008, 0x1808, 0x208c, 0x2090, 0x2094,
                              0x2098, 0x209c, 0x20a0, 0x20a4, 0x20ac]),
    ('usbcon', 0x11200000, list(range(0, 0x200, 4))),
    ('dwc3', 0x11210000, [0xc110, 0xc118, 0xc120, 0xc200, 0xc2c0, 0xc704, 0xc70c]),
]


def main():
    fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
    for name, base, offsets in BLOCKS:
        page = base & ~0xFFFF
        span = (base - page) + max(offsets) + 4
        m = mmap.mmap(fd, (span + 0xFFF) & ~0xFFF, mmap.MAP_SHARED, mmap.PROT_READ, offset=page)
        values = [struct.unpack_from('<I', m, base - page + off)[0] for off in offsets]
        m.close()
        if name == 'usbcon':
            # Only the non-zero words of the 512-byte controller block.
            print(name, ' '.join(f'+{o:03x}={v:08x}' for o, v in zip(offsets, values) if v))
        else:
            print(name, ' '.join(f'+{o:04x}={v:08x}' for o, v in zip(offsets, values)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
