#!/usr/bin/env python3
"""Read-only SMEM item reader, and OnePlus's SMEM_PROJECT_INFO (item 136).

SMEM sits in no-map reserved memory, so /dev/mem can read it on this kernel
(STRICT_DEVMEM only refuses System RAM). Layout follows mainline
drivers/soc/qcom/smem.c: the partition table ($TOC) in the last 4 KiB, each
partition ($PRT) holding uncached entries from its start. Nothing is written.

Only the table and the partitions the AP may read are touched: the global
heap (hosts 0xfffe/0xfffe) and pairs that include APPS (host 0). Partitions
between two remote processors are firewalled; reading one aborts the CPU and
panics the kernel (a whole-region read rebooted the phone on 2026-09-26).

Run on the phone:  python3 - [item ... | list] < scripts/phone-smem.py
"""
import mmap
import os
import struct
import sys

SMEM_BASE, SMEM_SIZE = 0x86000000, 0x200000
PROJECT_INFO = 136

# include/linux/project_info.h in the OnePlus 7 Pro kernel source
PROJECT_FIELDS = ("prj_version hw_version rf_v1 rf_v2 rf_v3 uart_boot_mode platform_id "
                  "ddr_manufacture_info ddr_row ddr_column ddr_fw_version ddr_reserve_info "
                  "ddr_type reserve02 reserve03 reserve04 reserve05 a_board_version "
                  "feature_id ftm_uart_boot_mode operator modem").split()


GLOBAL_HOST, APPS_HOST = 0xFFFE, 0


def read(fd, offset, size):
    """Copy [offset, offset+size) of SMEM through a page-aligned mapping."""
    start = (SMEM_BASE + offset) & ~0xFFF
    skip = SMEM_BASE + offset - start
    m = mmap.mmap(fd, skip + size, mmap.MAP_SHARED, mmap.PROT_READ, offset=start)
    try:
        return bytes(m[skip:skip + size])
    finally:
        m.close()


def items(fd):
    """Yield (host0, host1, item, data) for each uncached entry the AP may read."""
    toc = read(fd, SMEM_SIZE - 4096, 4096)
    magic, _version, count = struct.unpack_from("<4sII", toc)
    if magic != b"$TOC":
        sys.exit("no SMEM partition table")
    for i in range(min(count, (4096 - 32) // 48)):
        off, size, _flags, h0, h1 = struct.unpack_from("<IIIHH", toc, 32 + i * 48)
        readable = (h0, h1) == (GLOBAL_HOST, GLOBAL_HOST) or APPS_HOST in (h0, h1)
        if not size or not readable or off + size > SMEM_SIZE:
            continue
        part = read(fd, off, size)
        if part[:4] != b"$PRT":
            continue
        free_uncached = struct.unpack_from("<I", part, 12)[0]
        e = 32
        while e + 16 <= min(free_uncached, size):
            canary, item, esize, pad_data, pad_hdr = struct.unpack_from("<HHIHH", part, e)
            if canary != 0xA5A5:
                break
            start = e + 16 + pad_hdr
            yield h0, h1, item, part[start:start + esize - pad_data]
            e = start + esize


def main():
    fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
    if sys.argv[1:] == ["list"]:
        for h0, h1, item, data in items(fd):
            print(f"hosts {h0:#06x}/{h1:#06x} item {item:4} {len(data):7} bytes")
        return
    wanted = [int(a) for a in sys.argv[1:]] or [PROJECT_INFO]
    for h0, h1, item, data in items(fd):
        if item not in wanted:
            continue
        print(f"item {item} (hosts {h0:#x}/{h1:#x}), {len(data)} bytes")
        if item == PROJECT_INFO and len(data) >= 40 + 4 * len(PROJECT_FIELDS):
            name, codename, reserve = struct.unpack_from("<8s20s12s", data)
            print(f"  project_name={name.rstrip(b'\0').decode(errors='replace')!r} "
                  f"codename={codename.rstrip(b'\0').decode(errors='replace')!r}")
            vals = struct.unpack_from(f"<{len(PROJECT_FIELDS)}I", data, 40)
            for k, v in zip(PROJECT_FIELDS, vals):
                if not k.startswith("ddr"):
                    print(f"  {k} = {v} ({v:#x})")
        else:
            print("  " + data[:64].hex())


if __name__ == "__main__":
    main()
