#!/usr/bin/env python3
"""Build pixel-camera's sensor firmware from the extracted register tables.

    make-camera-firmware.py TABLE_DIR OUT_DIR

TABLE_DIR holds the tables extracted from the user's own stock camera HAL
(docs/camera-plan-20260930.md, section 2); they are proprietary and never
committed, and neither is the output. For each camera this writes
OUT_DIR/<camera>.bin, which pixel-camera.c loads as
/lib/firmware/pixel-camera/<camera>.bin when the camera starts streaming:
little-endian {u16 register, u16 value} pairs, register 0xffff meaning
"wait <value> ms".

Each camera gets the writes the stock HAL makes in code ahead of its tables
(the Samsung software reset and clock enable, each followed by 10 ms), then
its init table, then one mode table, then fixed extra writes. A camera with a
second mode (the main camera's full resolution for stills) also gets
OUT_DIR/<camera>-<mode>.bin: the same writes with that mode's table instead.
"""
import os
import struct
import sys

SAMSUNG_START = [(0x6028, 0x4000), (0x6010, 0x0001), (0xFFFF, 10), (0x6226, 0x0001), (0xFFFF, 10)]

CAMERAS = {
    "uw": {
        "start": [],
        "tables": ["sandworm_imx386_init_PD_212.txt",
                   "sandworm_imx386_mode_0x919860_2016x1508_73.txt"],
        "extra": [],
        "value_bits": 8,
    },
    "front": {
        "start": [],
        "tables": ["dokkaebi_3j1_init_368.txt", "dokkaebi_3j1_mode3_1920x1368_100.txt"],
        "extra": [],
        "value_bits": 16,
    },
    "main": {
        "start": SAMSUNG_START,
        "tables": ["nagual_gn1_init_a_1565.txt", "nagual_gn1_mode_0x141508_2016x1136_134.txt"],
        "extra": [],
        "value_bits": 16,
        "modes": {"4080x3072": "nagual_gn1_mode_0x140448_4080x3072_134.txt"},
    },
    "tele": {
        "start": SAMSUNG_START,
        "tables": ["kraken_gm5_init_default_4047.txt",
                   "kraken_gm5_mode_0x90b060_2016x1512_279.txt"],
        "extra": [],
        "value_bits": 16,
    },
}

DELAY_AFTER = {0x6010: 10, 0x6226: 10}


def load(path, value_bits):
    writes = []
    with open(path) as f:
        for line in f:
            fields = line.split("#", 1)[0].split()
            if len(fields) != 2:
                continue
            reg, val = int(fields[0], 16), int(fields[1], 16)
            if not 0 <= reg < 0xFFFF or not 0 <= val < 1 << value_bits:
                sys.exit(f"{path}: bad entry {fields}")
            writes.append((reg, val))
            if value_bits == 16 and reg in DELAY_AFTER:
                writes.append((0xFFFF, DELAY_AFTER[reg]))
    return writes


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.splitlines()[2].strip())
    tables, out = sys.argv[1:]
    os.makedirs(out, exist_ok=True)
    for name, cam in CAMERAS.items():
        outputs = {name: cam["tables"]}
        for mode, table in cam.get("modes", {}).items():
            outputs[f"{name}-{mode}"] = cam["tables"][:-1] + [table]
        for output, names in outputs.items():
            writes = list(cam["start"])
            try:
                for t in names:
                    writes += load(os.path.join(tables, t), cam["value_bits"])
            except FileNotFoundError as e:
                print(f"{output}: skipped, {e.filename} missing")
                continue
            writes += cam["extra"]
            with open(os.path.join(out, f"{output}.bin"), "wb") as f:
                for reg, val in writes:
                    f.write(struct.pack("<HH", reg, val))
            print(f"{output}.bin: {len(writes)} entries")


if __name__ == "__main__":
    main()
