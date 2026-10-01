#!/usr/bin/env python3
"""Play sensor register tables over i2c-dev and watch the sensor stream.

    cam-stream.py uw INIT MODE [--set 0x0204=0x03 ...] [--watch 3]
    cam-stream.py uw --set 0x0205=0x80        # just the writes, while streaming
    cam-stream.py uw --stop                   # 0x0100 = 0

Each table line is "0xADDR 0xVALUE" with a 16-bit register; values are 8
bits on the Sony IMX386 and 16 bits on the Samsung sensors ('#' comments).
The tables come from the user's own vendor image (docs/
camera-plan-20260930.md, section 2) and are never committed. Writes to the
Samsung software reset (0x6010) and clock enable (0x6226) are each followed
by a 10 ms wait; the HAL writes both in code, ahead of the tables. After the
tables, 0x0100 (mode_select, one byte on every sensor) = 1 starts
streaming; the frame counter (0x0005) then advances once per frame, which
shows the sensor runs with no SoC camera block involved. --set writes single registers (gain, exposure,
frame length) after the tables inside a group hold (0x0104); without tables
it only does those writes. The sensor must be powered first (README.md).
Address 0x08 is refused on every bus (the ST54J NFC controller and eSIM).
"""
import argparse
import ctypes
import fcntl
import importlib.util
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("cam_id", os.path.join(HERE, "cam-id.py"))
cam_id = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cam_id)

FRAME_COUNT = 0x0005
VALUE_BYTES = {"uw": 1, "front": 2, "main": 2, "tele": 2}
SETTLE_AFTER = (0x6010, 0x6226)     # Samsung: software reset, clock enable


def write_reg(bus, addr, reg, value, width=1):
    if addr in cam_id.FORBIDDEN:
        sys.exit(f"refused: address {addr:#04x} is {cam_id.FORBIDDEN[addr]}")
    data = value.to_bytes(width, "big")
    buf = (ctypes.c_uint8 * (2 + width))(reg >> 8, reg & 0xFF, *data)
    msg = cam_id.I2cMsg(addr, 0, 2 + width, ctypes.cast(buf, ctypes.POINTER(ctypes.c_uint8)))
    msgs = (cam_id.I2cMsg * 1)(msg)
    data = cam_id.I2cRdwrData(ctypes.cast(msgs, ctypes.POINTER(cam_id.I2cMsg)), 1)
    fd = os.open(f"/dev/i2c-{bus}", os.O_RDWR)
    try:
        fcntl.ioctl(fd, cam_id.I2C_RDWR, data)
    finally:
        os.close(fd)


DELAY = 0xFFFFFFFFFFFFFFFF   # the HAL tables' "wait <value> us" entry


def load_table(path, width):
    writes = []
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if len(line) != 2:
                continue
            reg, value = int(line[0], 16), int(line[1], 16)
            if reg == DELAY:
                writes.append((reg, value))
                continue
            if not 0 <= reg <= 0xFFFF or not 0 <= value < 1 << (8 * width):
                sys.exit(f"{path}: bad entry {line}")
            writes.append((reg, value))
    return writes


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("sensor", choices=sorted(VALUE_BYTES))
    ap.add_argument("tables", nargs="*")
    ap.add_argument("--watch", type=float, default=3.0, help="seconds to poll the frame counter")
    ap.add_argument("--stop", action="store_true", help="only write 0x0100 = 0")
    ap.add_argument("--set", nargs="+", default=[], metavar="REG=VAL",
                    help="extra register writes (the sensor's value width), in a group hold")
    args = ap.parse_args()
    adapter, addr, id_reg, expect, part = cam_id.SENSORS[args.sensor]
    width = VALUE_BYTES[args.sensor]
    bus = cam_id.adapters().get(adapter)
    if bus is None:
        sys.exit(f"{adapter} is not registered; load pixel-hsi2c-cam")
    if args.stop:
        write_reg(bus, addr, 0x0100, 0)
        print("stream off")
        return
    chip = int.from_bytes(cam_id.read_regs(bus, addr, id_reg, 2), "big")
    if chip != expect:
        sys.exit(f"{part}: chip ID {chip:#06x}, expected {expect:#06x}")
    for path in args.tables:
        writes = load_table(path, width)
        for reg, value in writes:
            if reg == DELAY:
                time.sleep(value / 1e6)
                continue
            write_reg(bus, addr, reg, value, width)
            if reg in SETTLE_AFTER and width == 2:
                time.sleep(0.01)
        print(f"{os.path.basename(path)}: {len(writes)} writes")
        time.sleep(0.01)
    if args.set:
        sets = [tuple(int(x, 16) for x in item.split("=")) for item in args.set]
        if any(not 0 <= r <= 0xFFFF or not 0 <= v < 1 << (8 * width) for r, v in sets):
            sys.exit(f"--set wants 16-bit registers and {8 * width}-bit values")
        write_reg(bus, addr, 0x0104, 1)
        for reg, value in sets:
            write_reg(bus, addr, reg, value, width)
        write_reg(bus, addr, 0x0104, 0)
        print(f"{len(sets)} register writes")
        if not args.tables:
            return
    write_reg(bus, addr, 0x0100, 1)
    print("stream on")
    reg = FRAME_COUNT
    end = time.monotonic() + args.watch
    while time.monotonic() < end:
        count = cam_id.read_regs(bus, addr, reg, 1)[0]
        print(f"{time.monotonic():.3f} frame count {count}")
        time.sleep(0.25)


if __name__ == "__main__":
    main()
