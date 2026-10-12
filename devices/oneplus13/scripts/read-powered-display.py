#!/usr/bin/env python3
"""Selected register reads only while our diagnostic holds display power/clocks.

No clear/status FIFOs, panel commands or MMIO writes. This is a RAM-probe
diagnostic; never run it against unowned/unpowered display registers.
"""
import json
import mmap
import os
from pathlib import Path
import struct
import time

dev = Path('/sys/bus/platform/devices/ae00000.display-subsystem')
assert '-omarchy-oneplus13-probe12' == os.uname().release.removeprefix('7.3.0-rc2')
assert Path('/sys/firmware/devicetree/base/model').read_bytes().rstrip(b'\0') == \
       b'OnePlus 13 CPH2655 Omarchy USB probe'
assert (dev / 'driver').resolve().name == 'oneplus13-display-probe'
assert (dev / 'power/runtime_status').read_text().strip() == 'active'
# A successful sysfs read also checks that the module completed its probe.
state = (dev / 'state').read_text()
assert 'version=c0000000' in state
blocks = {}
for n in range(2):
    blocks[f'vig{n}'] = (0x5000 + 0x2000*n, [0, 8, 12, 16, 20, 36, 48, 52, 56])
for n in range(6):
    blocks[f'ctl{n}'] = (0x16000 + 0x1000*n,
                         [0, 4, 8, 12, 0x14, 0x18, 0x1c, 0x40, 0x44,
                          0xE4, 0xE8, 0xF4, 0xFC, 0x12C, 0x130, 0x134])
blocks['intf1'] = (0x36000, [0, 4, 0x84, 0xA8, 0xAC, 0xB0,
                            0x26C, 0x280, 0x284, 0x288, 0x28C, 0x290,
                            0x294, 0x298, 0x29C, 0x2A0, 0x2A4, 0x2A8,
                            0x2AC, 0x2B0, 0x2B4, 0x800, 0x804])
result = {'kernel': os.uname().release, 'power_held_by': 'oneplus13-display-probe',
          'read_only': True, 'sysfs_state': state, 'blocks': {}}
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC | os.O_CLOEXEC)
try:
    with mmap.mmap(fd, 0x93800, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ,
                   offset=0xAE00000) as regs:
        def read(offset):
            return struct.unpack_from('<I', regs, offset)[0]
        for name, (base, offsets) in blocks.items():
            result['blocks'][name] = {hex(o): f'0x{read(base+o):08x}' for o in offsets}
        result['counter_samples'] = []
        for _ in range(6):
            result['counter_samples'].append({'seconds': time.monotonic(),
                'frames': read(0x360AC), 'line': read(0x362B0),
                'interrupt_status': read(0x36804)})
            time.sleep(0.1)
finally:
    os.close(fd)
print(json.dumps(result, indent=2))
