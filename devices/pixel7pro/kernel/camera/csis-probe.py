#!/usr/bin/env python3
"""Find and bring up the CSIS link a streaming sensor lands on (bring-up tool).

    csis-probe.py setup LINK PHY [--lanes 4] [--rate 1548] [--size 2016x1508] [--cphy]
    csis-probe.py status LINK [--count 8]
    csis-probe.py stop LINK PHY [--lanes 4]

Needs pd_csis and pd_pdp on (the camera-dev boot flag) and pixel-csis-iso
loaded (DC-PHY isolation bypass). "setup" releases the PHY's reset in
SYSREG_CSIS 0x500 BEFORE any PHY register access (a PHY in reset hangs the
bus), replays the stock HAL's D-PHY sequence, or with --cphy its C-PHY one
(--lanes is then the trio count, --rate in Msps), both as the Pixel 6 port
recovered them, on that PHY, then configures the link: soft
reset, lane count, D-PHY, data lanes, VC0 RAW10, the resolution, shadow
update, clock lane and CSI enable. "status" reads only documented status
registers: INT_SRC0/1, FS/FE_INT_SRC, FRM_CNT_CH0 and PHY_STATUS.

Addresses: csis-link n 0x1a440000 + n * 0x10000, csis-phy 0x1a4f0000 (bias
0x1000; per PHY a clock-lane block, then one block per data lane or trio at
+0x100 each: PHY 0 0x1300, PHY 1 0x1b00, PHYs 2, 4, 6 at 0x2300, 0x2b00,
0x3300 with four data blocks, PHYs 3, 5, 7 at 0x2800, 0x3000, 0x3800 with
two; the stock HAL's m1_dphy_sNc_gnr_con0 offsets), SYSREG_CSIS 0x1a420000
(stock DT, docs/camera-plan-20260930.md); register fields from the CSIS v5.4
map.
"""
import argparse
import mmap
import os
import struct
import sys
import time

LINK_BASE, PHY_BASE, SYSREG_BASE = 0x1A440000, 0x1A4F0000, 0x1A420000
PHY_OFF = [0x1300, 0x1B00, 0x2300, 0x2800, 0x2B00, 0x3000, 0x3300, 0x3800]
SYSREG_PHY_RESET = 0x500

# CSIS link registers and fields
CMN_CTRL, UPD_SDW, INT_SRC0, INT_SRC1 = 0x04, 0x0C, 0x14, 0x1C
FS_INT_SRC, FE_INT_SRC, ISP_CONFIG_CH0, ISP_RESOL_CH0 = 0x24, 0x2C, 0x40, 0x44
FRM_CNT_CH0, PHY_STATUS, PHY_CMN_CTRL, VERSION = 0x500, 0x700, 0x704, 0x00
DT_RAW10 = 0x2B

# HAL D-PHY settle table (rate 4500 - 10 * i Mbps), from the Pixel 6 port.
SETTLE = bytes([
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 22, 22, 22,
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 21, 21, 21, 21, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 18, 18, 18, 18, 18, 18,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
    17, 17, 17, 17, 17, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
    13, 13, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 7, 7, 7, 7, 7, 7, 7, 7, 7, 73, 73, 72, 72, 71, 71, 70, 70, 69,
    69, 68, 68, 67, 67, 66, 66, 65, 65, 64, 64, 64, 63, 63, 62, 62, 61, 61, 60, 60, 59, 59, 58,
    58, 57, 57, 56, 56, 55, 55, 54, 54, 53, 53, 52, 52, 52, 51, 51, 50, 50, 49, 49, 48, 48, 47,
    47, 46, 46, 45, 45, 44, 44, 43, 43, 42, 42, 41, 41, 40, 40, 39, 39, 39, 38, 38, 37, 37, 36,
    36, 35, 35, 34, 34, 33, 33, 32, 32, 31, 31, 30, 30, 29, 29, 28, 28, 27, 27, 27, 26, 26, 25,
    25, 24, 24, 23, 23, 22, 22, 21, 21, 20, 20, 19, 19, 18, 18, 17, 17, 16, 16, 15, 15, 14, 14,
    14, 13, 13, 12, 12, 11, 11, 10, 10, 9, 9, 8, 8, 7, 7, 6, 6,
])


class Block:
    def __init__(self, base, size):
        fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        self.m = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
                           offset=base)
        os.close(fd)
        self.base = base

    def rd(self, off):
        return struct.unpack_from("<I", self.m, off)[0]

    def wr(self, off, val):
        struct.pack_into("<I", self.m, off, val & 0xFFFFFFFF)

    def field(self, off, shift, width, val):
        mask = ((1 << width) - 1) << shift
        self.wr(off, (self.rd(off) & ~mask) | ((val << shift) & mask))


def settle_for(rate):
    key = -(-rate // 10) * 10
    if key > 4500:
        return 25
    if key < 90:
        return 5
    return SETTLE[(4500 - key) // 10]


def check_power():
    pmu = Block(0x18062000, 0x1000)
    if not (pmu.rd(0x404) & 1 and pmu.rd(0x484) & 1):
        sys.exit("pd_csis or pd_pdp is off (camera-dev boot flag)")
    iso = Block(0x18063000, 0x1000).rd(0xEBC)
    if not iso & 1:
        sys.exit("DC-PHY isolation in place: load pixel-csis-iso first")


def dphy_set(phy, off, lanes, rate):
    settle = settle_for(rate)
    clk_sel = 0x100 if rate < 1500 else 0
    skew = 0x300 if 1500 <= rate < 2000 else 0x200 if 2000 <= rate < 3000 else 0
    phy.wr(off, 0)
    for i in range(lanes):
        phy.wr(off + 0x100 + i * 0x100, 0)
    for o, v in ((0x0, 0x10), (0x4, 0x110), (0x8, 0x3223), (0xC, 0), (0x10, 0x200)):
        phy.wr(0x1000 + o, v)                      # M_BIAS_CON0-4
    for o, v in ((0x4, 0x1450), (0x8, 0x9), (0xC, 0xEA40), (0x10, 0x2), (0x14, 0x8600),
                 (0x18, 0x4000), (0x1C, 0), (0x30, 0x301), (0x40, 0x1)):
        phy.wr(off + o, v)                         # clock lane
    phy.wr(off, 1)                                 # SC_GNR_CON0 enable
    for i in range(lanes):
        sd = off + 0x100 + i * 0x100
        for o, v in ((0x4, 0x1450), (0x8, 0x9), (0xC, 0xEA40), (0x10, 0x2 | skew),
                     (0x14, 0x8600), (0x18, 0x4000), (0x1C, 0), (0x20, 0), (0x24, 0x40)):
            phy.wr(sd + o, v)
        phy.wr(sd + 0x30, (phy.rd(sd + 0x30) & ~0x1FF) | settle | clk_sel)
        phy.wr(sd + 0x34, 0x3)
        phy.wr(sd + 0x40, phy.rd(sd + 0x40) | 1)
        phy.wr(sd + 0x50, 0x81A)
        phy.wr(sd, 1)                              # SD_GNR_CON0 enable
    print(f"PHY {off:#x}: {lanes} lanes, {rate} Mbps, settle {settle}, clk_sel {bool(clk_sel)}, "
          f"skew {skew:#x}")


def cphy_set(phy, off, trios, rate, settle=None):
    """The HAL's C-PHY branch: bias CON4 0x40, no clock lane, one block per
    trio, settle 7 at 1000 Msps and up (else 9)."""
    if settle is None:
        settle = 7 if rate >= 1000 else 9
    clk_sel = 0x100 if rate < 500 else 0
    for o, v in ((0x0, 0x10), (0x4, 0x110), (0x8, 0x3223), (0xC, 0), (0x10, 0x40)):
        phy.wr(0x1000 + o, v)                      # M_BIAS_CON0-4
    for i in range(trios):
        sd = off + 0x100 + i * 0x100
        phy.wr(sd, 1)                              # SD_GNR_CON0 enable
        for o, v in ((0x4, 0x1450), (0x8, 0x9), (0xC, 0x82B8), (0x10, 0x1), (0x14, 0x8600),
                     (0x18, 0x4000), (0x1C, 0x200), (0x20, 0x638), (0x24, 0x40)):
            phy.wr(sd + o, v)
        phy.wr(sd + 0x30, (phy.rd(sd + 0x30) & ~0x1FF) | settle | clk_sel)
        phy.wr(sd + 0x34, 0x32)
        phy.wr(sd + 0x64, 0x1503)
        phy.wr(sd + 0x68, 0x32)
    print(f"PHY {off:#x}: C-PHY, {trios} trios, {rate} Msps, settle {settle}, "
          f"clk_sel {bool(clk_sel)}")


def status(link, count, interval=0.25):
    for _ in range(count):
        vals = {n: link.rd(o) for n, o in (("int0", INT_SRC0), ("int1", INT_SRC1),
                                         ("fs", FS_INT_SRC), ("fe", FE_INT_SRC),
                                         ("frm", FRM_CNT_CH0), ("phy", PHY_STATUS))}
        print(" ".join(f"{k} {v:#010x}" for k, v in vals.items()), flush=True)
        for o in (INT_SRC0, INT_SRC1, FS_INT_SRC, FE_INT_SRC):
            if link.rd(o):
                link.wr(o, link.rd(o))             # write-1-to-clear
        time.sleep(interval)


def pair(a):
    """The PHYs whose registers a setup touches: a 4-lane receiver on a
    2-lane PHY (2-7) runs its data lanes on into the next PHY's space, as
    dcphy4 does on GS101 (clock 0x2b00, data 0x2c00-0x2f00). Only the even
    PHYs 2, 4 and 6 start such a pair; "stop" needs the setup's --lanes."""
    if a.phy >= 2 and a.lanes > 2:
        if a.phy % 2:
            sys.exit(f"a {a.lanes}-lane receiver needs PHY 0, 1, 2, 4 or 6, not {a.phy}")
        return [a.phy, a.phy + 1]
    return [a.phy]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cmd", choices=("setup", "status", "stop"))
    ap.add_argument("link", type=int, choices=range(8))
    ap.add_argument("phy", type=int, nargs="?", choices=(0, 1, 2, 3, 4, 5, 6, 7))
    ap.add_argument("--lanes", type=int, default=4, choices=(1, 2, 3, 4))
    ap.add_argument("--rate", type=int, default=1548)
    ap.add_argument("--size", default="2016x1508")
    ap.add_argument("--pixel-mode", type=int, default=2)
    ap.add_argument("--count", type=int, default=8)
    ap.add_argument("--cphy", action="store_true", help="C-PHY; --lanes counts trios (1-3)")
    ap.add_argument("--blocks", type=int, help="C-PHY: PHY data blocks to program (default: trios)")
    ap.add_argument("--settle", type=int, help="C-PHY: settle count (default 7 from 1000 Msps, else 9)")
    ap.add_argument("--dt", type=lambda v: int(v, 0), default=DT_RAW10,
                    help="channel 0 CSI-2 data type (default 0x2b, RAW10)")
    ap.add_argument("--park-vc", type=int, help="with --interleave: VC for channels 1-3 (default: their own)")
    ap.add_argument("--interleave", type=int, default=0, choices=range(4),
                    help="CMN_CTRL INTERLEAVE_MODE; the GS201 HAL uses 3 and parks channels 1-3")
    a = ap.parse_args()
    if a.cphy and a.lanes > 3:
        sys.exit("C-PHY has 1-3 trios: give --lanes")
    check_power()
    link = Block(LINK_BASE + a.link * 0x10000, 0x1000)
    print(f"link {a.link}: version {link.rd(VERSION):#x}")
    if a.cmd == "status":
        status(link, a.count)
        return
    if a.phy is None:
        sys.exit("give a PHY index")
    sysreg = Block(SYSREG_BASE, 0x1000)
    if a.cmd == "stop":
        link.field(CMN_CTRL, 0, 1, 0)                      # CSI_EN
        link.field(PHY_CMN_CTRL, 0, 5, 0)                   # clock and data lanes
        for p in pair(a):
            sysreg.field(SYSREG_PHY_RESET, p, 1, 0)
        print(f"link {a.link} off, PHY {pair(a)} back in reset")
        return
    w, h = (int(x) for x in a.size.split("x"))
    for p in pair(a):                                       # release before any access
        sysreg.field(SYSREG_PHY_RESET, p, 1, 1)
    print(f"SYSREG_CSIS 0x500 = {sysreg.rd(SYSREG_PHY_RESET):#x}")
    phy = Block(PHY_BASE, 0x10000)
    if a.cphy:
        cphy_set(phy, PHY_OFF[a.phy], a.blocks or a.lanes, a.rate, a.settle)
    else:
        dphy_set(phy, PHY_OFF[a.phy], a.lanes, a.rate)
    link.field(CMN_CTRL, 1, 1, 1)                           # SW_RESET
    time.sleep(0.001)
    link.field(CMN_CTRL, 8, 2, a.lanes - 1)                 # LANE_NUMBER
    link.field(CMN_CTRL, 21, 1, int(a.cphy))                # PHY_SEL: 1 = C-PHY
    link.field(CMN_CTRL, 10, 2, a.interleave)               # INTERLEAVE_MODE
    if a.interleave:
        for ch in (1, 2, 3):                                # HAL: VC ch, DT 0x3f (none)
            off = ISP_CONFIG_CH0 + ch * 0x10
            v = link.rd(off)
            v = (v & ~(0x1F << 16)) | ((ch if a.park_vc is None else a.park_vc) << 16)
            v = (v & ~(0x3 << 12)) | (a.pixel_mode << 12)
            link.wr(off, (v & ~(0x3F << 2)) | (0x3F << 2))
    # ENABLE_DAT: one bit per D-PHY lane; for C-PHY pablo's csi_hw_s_lane
    # enables 0x3 for one or two trios and all four bits (0xf) for three.
    dat = (0xF if a.lanes == 3 else 0x3) if a.cphy else (1 << a.lanes) - 1
    link.field(PHY_CMN_CTRL, 1, 4, dat)                     # ENABLE_DAT
    cfg = link.rd(ISP_CONFIG_CH0)
    cfg = (cfg & ~(0x1F << 16)) | (0 << 16)                 # VIRTUAL_CHANNEL 0
    cfg = (cfg & ~(0x3 << 12)) | (a.pixel_mode << 12)       # PIXEL_MODE
    cfg = (cfg & ~(0x3F << 2)) | (a.dt << 2)                # DATAFORMAT
    link.wr(ISP_CONFIG_CH0, cfg)
    link.wr(ISP_RESOL_CH0, (h << 16) | w)
    link.field(UPD_SDW, 0, 6, 0xF)                          # UPDATE_SHADOW
    link.field(PHY_CMN_CTRL, 0, 1, 1)                       # ENABLE_CLK
    link.field(CMN_CTRL, 0, 1, 1)                           # CSI_EN
    print(f"link {a.link}: CMN_CTRL {link.rd(CMN_CTRL):#x} PHY_CMN_CTRL "
          f"{link.rd(PHY_CMN_CTRL):#x} config {link.rd(ISP_CONFIG_CH0):#x} "
          f"resol {link.rd(ISP_RESOL_CH0):#x}")
    status(link, a.count)


if __name__ == "__main__":
    main()
