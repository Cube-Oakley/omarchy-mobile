#!/usr/bin/env python3
"""Turn a pixel-csis-capture frame into a viewable PNG (host side).

    raw2png.py frame.raw out.png [--w 2016] [--h 1508] [--black 64] [--order RGGB]
                                 [--offset WORDS]

The frame is the WDMA's format 6: little-endian 16-bit words holding 10-bit
Bayer samples. Each 2x2 cell becomes one RGB pixel (half size, no
demosaicing), the black level is subtracted, the channels are balanced gray
world, the 99.5th percentile is stretched to white and a 2.2 gamma applied.
--offset skips words ahead of the image: the 3J1 front sends 4224 words of
PDAF and embedded data on VC0 first, which the WDMA writes back to back with
the image (kernel/camera/README.md). Good enough to see what the sensor
sees; real processing is libcamera's job.
Needs numpy; writes the PNG itself.
"""
import argparse
import struct
import zlib

import numpy as np


def save_png(path, img):
    h, w, _ = img.shape
    raw = b"".join(b"\0" + img[y].tobytes() for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--w", type=int, default=2016)
    ap.add_argument("--h", type=int, default=1508)
    ap.add_argument("--black", type=float, default=64)
    ap.add_argument("--order", default="RGGB", help="colors of the 2x2 cell, row by row")
    ap.add_argument("--gamma", type=float, default=2.2)
    ap.add_argument("--offset", type=int, default=0, help="16-bit words to skip first")
    args = ap.parse_args()
    raw = np.fromfile(args.src, dtype="<u2")[args.offset:args.offset + args.w * args.h]
    raw = raw.reshape(args.h, args.w)
    raw = np.clip(raw.astype(np.float32) - args.black, 0, None)
    planes = {"R": [], "G": [], "B": []}
    for (y, x), color in zip(((0, 0), (0, 1), (1, 0), (1, 1)), args.order.upper()):
        planes[color].append(raw[y::2, x::2])
    rgb = np.dstack([np.mean(planes[c], axis=0) for c in "RGB"])
    for c in range(3):
        rgb[..., c] *= rgb[..., 1].mean() / max(rgb[..., c].mean(), 1e-6)
    white = np.percentile(rgb, 99.5)
    rgb = np.clip(rgb / max(white, 1e-6), 0, 1) ** (1 / args.gamma)
    save_png(args.dst, (rgb * 255).astype(np.uint8))
    print(f"{args.dst}: {rgb.shape[1]}x{rgb.shape[0]}, white = {white:.1f} DN above black")


if __name__ == "__main__":
    main()
