#!/usr/bin/env python3
"""Read a camera sensor's chip ID over i2c-dev, with 16-bit register addresses.

    cam-id.py uw                     # hsi2c_3 0x1a, register 0x0016: expect 03 86
    cam-id.py --bus "Pixel hsi2c_3" --addr 0x50 --reg 0x0000 --len 4
    cam-id.py --list

Read-only: one I2C_RDWR transaction, the two register address bytes then a
read, with a repeated start. Buses are found by adapter name, since i2c-dev
numbers change. Address 0x08 is refused on every bus: on hsi2c_8 it is the
ST54J NFC controller, which holds the phone's eSIM.

The sensor must be powered first (kernel/camera/README.md), for example
    echo 1 > /sys/devices/platform/pixel-camera-power/uw/power
"""
import argparse
import ctypes
import fcntl
import glob
import os
import sys

I2C_RDWR = 0x0707
I2C_M_RD = 0x0001
FORBIDDEN = {0x08: "the ST54J NFC controller and eSIM on hsi2c_8"}

# name: (adapter, address, ID register, expected ID, part)
SENSORS = {
    "uw": ("Pixel hsi2c_3", 0x1A, 0x0016, 0x0386, "Sony IMX386"),
    "front": ("Pixel hsi2c_2", 0x10, 0x0000, 0x30A1, "Samsung 3J1"),
    "main": ("Pixel hsi2c_1", 0x3D, 0x0000, 0x08E1, "Samsung GN1"),
    "tele": ("Pixel hsi2c_4", 0x2D, 0x0000, 0x08D5, "Samsung GM5"),
}


class I2cMsg(ctypes.Structure):
    _fields_ = [
        ("addr", ctypes.c_uint16),
        ("flags", ctypes.c_uint16),
        ("len", ctypes.c_uint16),
        ("buf", ctypes.POINTER(ctypes.c_uint8)),
    ]


class I2cRdwrData(ctypes.Structure):
    _fields_ = [("msgs", ctypes.POINTER(I2cMsg)), ("nmsgs", ctypes.c_uint32)]


def adapters():
    """{name: bus number} of the registered I2C adapters."""
    found = {}
    for path in glob.glob("/sys/bus/i2c/devices/i2c-*/name"):
        node = os.path.basename(os.path.dirname(path))[4:]
        if not node.isdigit():
            continue  # a client, such as i2c-ELAN0688:00
        try:
            with open(path) as f:
                name = f.read().strip()
        except OSError:
            continue
        found.setdefault(name, int(node))
    return found


def read_regs(bus, addr, reg, length):
    if addr in FORBIDDEN:
        sys.exit(f"refused: address {addr:#04x} is {FORBIDDEN[addr]}")
    if not 0x03 <= addr <= 0x77:
        sys.exit(f"refused: {addr:#x} is not a 7-bit device address")
    if not 0 <= reg <= 0xFFFF or not 1 <= length <= 64:
        sys.exit("register must be 0-0xffff and length 1-64")
    wbuf = (ctypes.c_uint8 * 2)(reg >> 8, reg & 0xFF)
    rbuf = (ctypes.c_uint8 * length)()
    u8p = ctypes.POINTER(ctypes.c_uint8)
    msgs = (I2cMsg * 2)(
        I2cMsg(addr, 0, 2, ctypes.cast(wbuf, u8p)),
        I2cMsg(addr, I2C_M_RD, length, ctypes.cast(rbuf, u8p)),
    )
    data = I2cRdwrData(ctypes.cast(msgs, ctypes.POINTER(I2cMsg)), 2)
    fd = os.open(f"/dev/i2c-{bus}", os.O_RDWR)
    try:
        fcntl.ioctl(fd, I2C_RDWR, data)
    finally:
        os.close(fd)
    return bytes(rbuf)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("sensor", nargs="?", choices=sorted(SENSORS))
    ap.add_argument("--bus", help='adapter name, e.g. "Pixel hsi2c_3"')
    ap.add_argument("--addr", type=lambda v: int(v, 0))
    ap.add_argument("--reg", type=lambda v: int(v, 0))
    ap.add_argument("--len", type=int, default=2)
    ap.add_argument("--list", action="store_true", help="list the I2C adapters")
    args = ap.parse_args()

    for addr in (args.addr,) + ((SENSORS[args.sensor][1],) if args.sensor else ()):
        if addr in FORBIDDEN:
            sys.exit(f"refused: address {addr:#04x} is {FORBIDDEN[addr]}")

    found = adapters()
    if args.list:
        for name, nr in sorted(found.items(), key=lambda item: item[1]):
            print(f"i2c-{nr}\t{name}")
        return 0

    expect = None
    if args.sensor:
        name, addr, reg, expect, part = SENSORS[args.sensor]
        length = 2
        what = f"{args.sensor} ({part})"
    elif args.bus and args.addr is not None and args.reg is not None:
        name, addr, reg, length = args.bus, args.addr, args.reg, args.len
        what = "device"
    else:
        ap.error("give a sensor, or --bus, --addr and --reg")
    if name not in found:
        sys.exit(f'no adapter "{name}" (load pixel-hsi2c-cam with that bus; see --list)')
    bus = found[name]
    if not os.path.exists(f"/dev/i2c-{bus}"):
        sys.exit(f"no /dev/i2c-{bus}: load i2c-dev")

    try:
        data = read_regs(bus, addr, reg, length)
    except OSError as e:
        sys.exit(f"{name} (i2c-{bus}) {addr:#04x} register {reg:#06x}: {e.strerror}")
    print(f"{what}: {name} (i2c-{bus}) {addr:#04x} register {reg:#06x}: {data.hex(' ')}")
    if expect is not None:
        value = int.from_bytes(data, "big")
        if value != expect:
            print(f"unexpected: {value:#06x}, expected {expect:#06x}")
            return 1
        print(f"chip ID {value:#06x}: as expected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
