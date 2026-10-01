#!/usr/bin/env python3
"""Grab raw frames from a V4L2 capture node (MMAP), with no other tools.

    v4l2-grab.py /dev/video0 OUT.raw [--frames 10] [--keep 1] [--buffers 4]
                 [--subdev /dev/v4l-subdev0 --set exposure=1500 gain=896 ...]

Queues --buffers buffers, streams, dequeues --frames frames and writes the
last --keep of them to OUT.raw (concatenated). Prints each frame's sequence
number, timestamp and flags, then the frame rate. Used to test pixel-camera
before libcamera is on the phone. --set writes sensor controls (exposure,
gain, vblank) on --subdev before streaming.
"""
import argparse
import ctypes
import fcntl
import mmap
import os
import struct
import sys
import time

V4L2_BUF_TYPE_VIDEO_CAPTURE = 1
V4L2_MEMORY_MMAP = 1
V4L2_BUF_FLAG_ERROR = 0x40


def _IOC(d, t, nr, size):
    return (d << 30) | (size << 16) | (ord(t) << 8) | nr


class timeval(ctypes.Structure):
    _fields_ = [("sec", ctypes.c_long), ("usec", ctypes.c_long)]


class timecode(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("flags", ctypes.c_uint32), ("frames", ctypes.c_uint8),
                ("seconds", ctypes.c_uint8), ("minutes", ctypes.c_uint8), ("hours", ctypes.c_uint8),
                ("userbits", ctypes.c_uint8 * 4)]


class m_union(ctypes.Union):
    _fields_ = [("offset", ctypes.c_uint32), ("userptr", ctypes.c_ulong), ("planes", ctypes.c_void_p),
                ("fd", ctypes.c_int32)]


class v4l2_buffer(ctypes.Structure):
    _fields_ = [("index", ctypes.c_uint32), ("type", ctypes.c_uint32), ("bytesused", ctypes.c_uint32),
                ("flags", ctypes.c_uint32), ("field", ctypes.c_uint32), ("timestamp", timeval),
                ("timecode", timecode), ("sequence", ctypes.c_uint32), ("memory", ctypes.c_uint32),
                ("m", m_union), ("length", ctypes.c_uint32), ("reserved2", ctypes.c_uint32),
                ("request_fd", ctypes.c_int32)]


class v4l2_requestbuffers(ctypes.Structure):
    _fields_ = [("count", ctypes.c_uint32), ("type", ctypes.c_uint32), ("memory", ctypes.c_uint32),
                ("capabilities", ctypes.c_uint32), ("flags", ctypes.c_uint8),
                ("reserved", ctypes.c_uint8 * 3)]


VIDIOC_REQBUFS = _IOC(3, "V", 8, ctypes.sizeof(v4l2_requestbuffers))
VIDIOC_QUERYBUF = _IOC(3, "V", 9, ctypes.sizeof(v4l2_buffer))
VIDIOC_QBUF = _IOC(3, "V", 15, ctypes.sizeof(v4l2_buffer))
VIDIOC_DQBUF = _IOC(3, "V", 17, ctypes.sizeof(v4l2_buffer))
VIDIOC_STREAMON = _IOC(1, "V", 18, 4)
VIDIOC_S_CTRL = _IOC(3, "V", 28, 8)
CTRLS = {"exposure": 0x00980911, "gain": 0x009E0903, "vblank": 0x009E0901}
VIDIOC_STREAMOFF = _IOC(1, "V", 19, 4)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("device")
    ap.add_argument("out")
    ap.add_argument("--frames", type=int, default=10)
    ap.add_argument("--keep", type=int, default=1)
    ap.add_argument("--buffers", type=int, default=4)
    ap.add_argument("--subdev")
    ap.add_argument("--set", nargs="+", default=[], metavar="NAME=VALUE")
    a = ap.parse_args()
    if a.set:
        sfd = os.open(a.subdev, os.O_RDWR)
        for item in a.set:
            name, value = item.split("=")
            fcntl.ioctl(sfd, VIDIOC_S_CTRL, struct.pack("<Ii", CTRLS[name], int(value, 0)))
        os.close(sfd)
    fd = os.open(a.device, os.O_RDWR)
    req = v4l2_requestbuffers(count=a.buffers, type=V4L2_BUF_TYPE_VIDEO_CAPTURE, memory=V4L2_MEMORY_MMAP)
    fcntl.ioctl(fd, VIDIOC_REQBUFS, req)
    maps = []
    for i in range(req.count):
        b = v4l2_buffer(index=i, type=V4L2_BUF_TYPE_VIDEO_CAPTURE, memory=V4L2_MEMORY_MMAP)
        fcntl.ioctl(fd, VIDIOC_QUERYBUF, b)
        maps.append(mmap.mmap(fd, b.length, mmap.MAP_SHARED, mmap.PROT_READ, offset=b.m.offset))
        fcntl.ioctl(fd, VIDIOC_QBUF, b)
    typ = ctypes.c_int(V4L2_BUF_TYPE_VIDEO_CAPTURE)
    fcntl.ioctl(fd, VIDIOC_STREAMON, typ)
    kept = []
    t0 = None
    try:
        for n in range(a.frames):
            b = v4l2_buffer(type=V4L2_BUF_TYPE_VIDEO_CAPTURE, memory=V4L2_MEMORY_MMAP)
            fcntl.ioctl(fd, VIDIOC_DQBUF, b)
            ts = b.timestamp.sec + b.timestamp.usec / 1e6
            t0 = t0 or ts
            err = " ERROR" if b.flags & V4L2_BUF_FLAG_ERROR else ""
            print(f"frame {n}: buffer {b.index} seq {b.sequence} t {ts - t0:.3f} bytes {b.bytesused}{err}")
            if n >= a.frames - a.keep:
                kept.append(bytes(maps[b.index][:b.bytesused]))
            fcntl.ioctl(fd, VIDIOC_QBUF, b)
        if a.frames > 1:
            print(f"{(a.frames - 1) / (ts - t0):.1f} fps")
    finally:
        fcntl.ioctl(fd, VIDIOC_STREAMOFF, typ)
    with open(a.out, "wb") as f:
        for k in kept:
            f.write(k)
    print(f"wrote {len(kept)} frame(s) to {a.out}")


if __name__ == "__main__":
    main()
