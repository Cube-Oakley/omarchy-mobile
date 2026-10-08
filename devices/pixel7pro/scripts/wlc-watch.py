#!/usr/bin/env python3
"""Read-only wireless charging watch (test only).

Every 2 s: the MAX77759 charger's view of its wireless input (WCIN), the
battery current, and, once WCIN has voltage (the receiver is only powered on
a pad), the P9412 receiver's mode, voltages, current, frequency and die
temperature. Writes nothing.

wlc-watch.py [SECONDS]
"""
import ctypes, fcntl, os, sys, time

I2C_RDWR, I2C_M_RD = 0x0707, 0x0001
CHG_BUS, CHG_ADDR = 22, 0x69      # hsi2c_13, MAX77759 charger
RX_BUS, RX_ADDR = 23, 0x3c        # hsi2c_15, P9412
MODES = {0: 'no field', 1: 'BPP', 2: 'EPP', 3: 'proprietary', 8: 'TX', 9: 'TX FOD'}


class Msg(ctypes.Structure):
    _fields_ = [('addr', ctypes.c_uint16), ('flags', ctypes.c_uint16),
                ('len', ctypes.c_uint16), ('buf', ctypes.POINTER(ctypes.c_uint8))]


class RdwrData(ctypes.Structure):
    _fields_ = [('msgs', ctypes.POINTER(Msg)), ('nmsgs', ctypes.c_uint32)]


def read(bus, addr, reg, count, wide):
    fd = os.open(f'/dev/i2c-{bus}', os.O_RDWR)
    try:
        regbytes = [reg >> 8, reg & 0xff] if wide else [reg]
        wbuf = (ctypes.c_uint8 * len(regbytes))(*regbytes)
        rbuf = (ctypes.c_uint8 * count)()
        msgs = (Msg * 2)(Msg(addr, 0, len(regbytes), wbuf), Msg(addr, I2C_M_RD, count, rbuf))
        fcntl.ioctl(fd, I2C_RDWR, RdwrData(msgs, 2))
        return bytes(rbuf)
    finally:
        os.close(fd)


def rx16(reg):
    b = read(RX_BUS, RX_ADDR, reg, 2, True)
    return b[0] | b[1] << 8


def sysfs(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return '?'


def sample():
    c = read(CHG_BUS, CHG_ADDR, 0xb4, 18, False)   # 0xb4 .. 0xc5
    int_ok, d00, d01, d02, cnfg00, cnfg10, cnfg12 = c[0], c[1], c[2], c[3], c[5], c[15], c[17]
    wcin_ok = bool(int_ok & 0x20)
    ilim = cnfg10 & 0x3f
    ilim_ma = 0 if ilim == 0 else 125 if ilim < 4 else 125 + (ilim - 3) * 31.25
    line = (f'usb {sysfs("/sys/class/power_supply/usb/online")} '
            f'WCIN_OK {int(wcin_ok)} WCIN_DTLS {(d00 >> 3) & 3} details01 {d01:02x} details02 {d02:02x} '
            f'mode {cnfg00 & 0xf} WCINSEL {(cnfg12 >> 6) & 1} wcin_ilim {ilim_ma:.0f} mA '
            f'batt {int(sysfs("/sys/class/power_supply/battery/current_now") or 0) // 1000} mA '
            f'{sysfs("/sys/class/power_supply/battery/capacity")}%')
    if wcin_ok:
        try:
            chip = rx16(0x00)
            mode = read(RX_BUS, RX_ADDR, 0x4c, 1, True)[0]
            line += (f' | rx {chip:04x} {MODES.get(mode, hex(mode))} vrect {rx16(0x40)} mV '
                     f'vout {rx16(0x42)} mV iout {rx16(0x44)} mA {rx16(0x48)} kHz '
                     f'die {rx16(0x46)} C status {rx16(0x34):04x}')
        except OSError as error:
            line += f' | rx unreadable ({error.strerror})'
    return line


end = time.monotonic() + (float(sys.argv[1]) if len(sys.argv) > 1 else 60)
while time.monotonic() < end:
    print(time.strftime('%H:%M:%S'), sample(), flush=True)
    time.sleep(2)
