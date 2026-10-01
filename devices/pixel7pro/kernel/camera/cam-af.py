#!/usr/bin/env python3
"""Read or move the ultrawide's AF actuator (AKM AK737x at hsi2c_3 0x0f).

    cam-af.py                 # dump registers 0x00-0x03
    cam-af.py POS             # active mode, lens to POS (0-4095)
    cam-af.py --standby

Protocol of the AK7375 family (mainline drivers/media/i2c/ak7375.c): the
position is 12 bits, written MSB first as (POS << 4) to registers 0x00/0x01;
register 0x02 is the mode, 0x00 active, 0x40 standby. Asleep it reads
00 ff ff ff; the first write after waking NACKs unless it waits a few ms.
The actuator sits on
L12S + SLG51002 LDO4, which pixel-camera-power turns on with the UW sensor.
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

ADAPTER, ADDR = "Pixel hsi2c_3", 0x0F
REG_POSITION, REG_MODE = 0x00, 0x02
MODE_ACTIVE, MODE_STANDBY = 0x00, 0x40


def xfer(bus, msgs):
    arr = (cam_id.I2cMsg * len(msgs))(*msgs)
    data = cam_id.I2cRdwrData(ctypes.cast(arr, ctypes.POINTER(cam_id.I2cMsg)), len(msgs))
    fd = os.open(f"/dev/i2c-{bus}", os.O_RDWR)
    try:
        fcntl.ioctl(fd, cam_id.I2C_RDWR, data)
    finally:
        os.close(fd)


def write(bus, reg, *values):
    buf = (ctypes.c_uint8 * (1 + len(values)))(reg, *values)
    xfer(bus, [cam_id.I2cMsg(ADDR, 0, len(buf), ctypes.cast(buf, ctypes.POINTER(ctypes.c_uint8)))])


def read(bus, reg, n):
    wbuf = (ctypes.c_uint8 * 1)(reg)
    rbuf = (ctypes.c_uint8 * n)()
    xfer(bus, [cam_id.I2cMsg(ADDR, 0, 1, ctypes.cast(wbuf, ctypes.POINTER(ctypes.c_uint8))),
               cam_id.I2cMsg(ADDR, cam_id.I2C_M_RD, n, ctypes.cast(rbuf, ctypes.POINTER(ctypes.c_uint8)))])
    return bytes(rbuf)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pos", nargs="?", type=int)
    ap.add_argument("--standby", action="store_true")
    args = ap.parse_args()
    bus = cam_id.adapters().get(ADAPTER)
    if bus is None:
        sys.exit(f"{ADAPTER} is not registered; load pixel-hsi2c-cam")
    if args.standby:
        write(bus, REG_MODE, MODE_STANDBY)
    elif args.pos is not None:
        if not 0 <= args.pos <= 4095:
            sys.exit("POS is 0-4095")
        write(bus, REG_MODE, MODE_ACTIVE)
        time.sleep(0.01)            # out of standby, the next write NACKs
        value = args.pos << 4
        write(bus, REG_POSITION, value >> 8, value & 0xFF)
    print("registers 0x00-0x03:", read(bus, 0x00, 4).hex(" "))


if __name__ == "__main__":
    main()
