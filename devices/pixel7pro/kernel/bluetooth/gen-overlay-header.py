#!/usr/bin/env python3
"""Generate pixel_bt_overlay.h from pixel-bt-overlay.dts: the whole overlay,
and a variant without the bluetooth node (a plain tty, for testing)."""
import re, subprocess, sys
from pathlib import Path

here = Path(__file__).resolve().parent
src = (here / 'pixel-bt-overlay.dts').read_text()
tty = re.sub(r'\n\t\tbluetooth \{.*?\n\t\t\};\n', '\n', src, flags=re.S)
assert tty != src

def dtb(text):
    return subprocess.run(['dtc', '-@', '-q', '-I', 'dts', '-O', 'dtb'], input=text.encode(),
                          stdout=subprocess.PIPE, check=True).stdout

def array(name, data):
    rows = [', '.join(f'0x{b:02x}' for b in data[i:i + 12]) for i in range(0, len(data), 12)]
    return (f'static const unsigned char {name}[] __aligned(8) = {{\n\t' +
            ',\n\t'.join(rows) + ',\n};\n')

out = ('/* SPDX-License-Identifier: GPL-2.0-only */\n'
       '/* Generated from kernel/bluetooth/pixel-bt-overlay.dts by gen-overlay-header.py */\n' +
       array('pixel_bt_overlay', dtb(src)) + array('pixel_bt_overlay_tty', dtb(tty)))
Path(sys.argv[1] if len(sys.argv) > 1 else here / 'pixel_bt_overlay.h').write_text(out)
