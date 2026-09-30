#!/usr/bin/env python3
"""Whole-phone power from USB: VBUS (TCPC 0x70, 25 mV) x IIn (gauge 0xD0,
0.125 mA), plus battery current. With the charger in buck-only mode (charge
limit below the current percentage) the battery rests and USB carries all.
Runs on the phone; needs the in-tree i2c-dev module and pixel-battery.
Usage: pixel-usb-power.py SECONDS [LABEL]"""
import ctypes, fcntl, glob, os, sys, time

I2C_RDWR, I2C_M_RD = 0x0707, 0x0001
class Msg(ctypes.Structure):
    _fields_ = [('addr', ctypes.c_uint16), ('flags', ctypes.c_uint16),
                ('len', ctypes.c_uint16), ('buf', ctypes.POINTER(ctypes.c_uint8))]
class Rdwr(ctypes.Structure):
    _fields_ = [('msgs', ctypes.POINTER(Msg)), ('nmsgs', ctypes.c_uint32)]
bus = next(p for p in glob.glob('/sys/bus/i2c/devices/i2c-*')
           if open(p + '/name').read().strip() == 'Pixel hsi2c_13').rsplit('-', 1)[1]
fd = os.open(f'/dev/i2c-{bus}', os.O_RDWR)
def rd16(addr, reg):
    w = (ctypes.c_uint8 * 1)(reg); r = (ctypes.c_uint8 * 2)()
    fcntl.ioctl(fd, I2C_RDWR, Rdwr((Msg * 2)(Msg(addr, 0, 1, w), Msg(addr, I2C_M_RD, 2, r)), 2))
    return r[0] | r[1] << 8
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20
label = sys.argv[2] if len(sys.argv) > 2 else ''
p, ib, n = [], [], 0
end = time.time() + secs
while time.time() < end:
    vbus = (rd16(0x25, 0x70) & 0x3ff) * 0.025
    iin = rd16(0x36, 0xd0) * 0.125e-3
    cur = rd16(0x36, 0x0a); cur = (cur - 65536 if cur & 0x8000 else cur) * 312.5e-6
    vbat = rd16(0x36, 0x09) * 78.125e-6
    p.append(vbus * iin + (-cur * vbat if cur < 0 else 0)); ib.append(cur)
    time.sleep(0.25)
p.sort()
print(f'{label:24s} {sum(p)/len(p):5.2f} W avg  (p10 {p[len(p)//10]:.2f}, p90 {p[len(p)*9//10]:.2f})  '
      f'battery {1000*sum(ib)/len(ib):+.0f} mA  n={len(p)}')
