#!/usr/bin/env python3
"""Validate a built native probe's initramfs, then pack its RAM-boot image."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import stat
import struct
import subprocess

BUSYBOX_SHA256 = "999cb969d09093a71716cfc747bb53cdada3f332c05eb5046c56e0f66a4d6d22"


def cpio_entries(data):
    offset = 0
    entries = {}
    while offset + 110 <= len(data):
        header = data[offset:offset + 110]
        if header[:6] != b"070701":
            raise ValueError("Expected uncompressed newc initramfs")
        mode = int(header[14:22], 16)
        size, namesize = int(header[54:62], 16), int(header[94:102], 16)
        if not namesize or offset + 110 + namesize > len(data):
            raise ValueError("Truncated CPIO name")
        name = data[offset + 110:offset + 110 + namesize - 1].decode()
        start = (offset + 110 + namesize + 3) & ~3
        end = start + size
        if end > len(data):
            raise ValueError("Truncated CPIO file")
        if name == "TRAILER!!!":
            return entries
        name = name.removeprefix("./").removeprefix("/")
        if name in entries:
            raise ValueError("Duplicate archive path")
        entries[name] = (mode, data[start:end])
        offset = (end + 3) & ~3
    raise ValueError("Missing CPIO trailer")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    kernel, out = args.kernel.resolve(), args.output.resolve()
    subprocess.run(["git", "check-ignore", "-q", str(out / "boot.img")],
                   cwd=Path(__file__).resolve().parents[3], check=True)
    if (out / "boot.img").exists():
        parser.error("Preserve the existing image; use a new output")
    config = (kernel / ".config").read_text()
    for expected in [f'CONFIG_INITRAMFS_SOURCE="{out / "root"}"',
                     "CONFIG_INITRAMFS_FORCE=y",
                     "# CONFIG_RANDOMIZE_BASE is not set"]:
        if expected not in config.splitlines():
            parser.error("Kernel configuration mismatch: " + expected)
    if not any(x in config.splitlines() for x in ["CONFIG_USB_G_SERIAL=y", "CONFIG_USB_CDC_COMPOSITE=y"]):
        parser.error("A built-in USB serial transport is required")
    entries = cpio_entries((kernel / "usr/initramfs_data.cpio").read_bytes())
    if "bin/framebuffer-pattern" in entries:
        for expected in ["CONFIG_DRM=y", "CONFIG_DRM_SIMPLEDRM=y", "CONFIG_DRM_FBDEV_EMULATION=y"]:
            if expected not in config.splitlines():
                parser.error("Framebuffer test requires built-in support: " + expected)
    mode, init = entries["init"]
    if not stat.S_ISREG(mode) or not mode & 0o111 or init != (out / "root/init").read_bytes():
        parser.error("Built /init must be executable and match the reviewed source")
    mode, busybox = entries["bin/busybox"]
    if not mode & 0o111 or hashlib.sha256(busybox).hexdigest() != BUSYBOX_SHA256:
        parser.error("Built BusyBox mismatch")
    mode, target = entries["bin/sh"]
    if not stat.S_ISLNK(mode) or target.removesuffix(b"\0") != b"busybox":
        parser.error("Built /bin/sh must link to BusyBox")
    image = (kernel / "arch/arm64/boot/Image").read_bytes()
    if image[56:60] != b"ARM\x64":
        parser.error("Expected uncompressed ARM64 Image")
    if (out / "embedded.dtb").read_bytes() not in image:
        parser.error("Kernel does not contain the reviewed embedded device tree")
    (out / "Image").write_bytes(image)
    shutil.copy(kernel / ".config", out / "kernel.config")
    shutil.copy(kernel / "System.map", out / "System.map")
    subprocess.run(["mkbootimg", "--header_version", "4", "--kernel", str(out / "Image"),
                    "--os_version", "15.0.0", "--os_patch_level", "2026-08",
                    "--output", str(out / "boot.img")], check=True)
    boot = (out / "boot.img").read_bytes()
    if (len(boot) > 100663296 or struct.unpack_from("<I", boot, 8)[0] != len(image)
            or boot[4096:4096 + len(image)] != image):
        raise RuntimeError("Packed kernel verification failed")
    manifest = {}
    for name in ["boot.img", "Image", "embedded.dtb", "kernel.config", "System.map", "root/init", "root/bin/busybox"]:
        path = out / name
        manifest[name] = {"bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Validated archive and packed boot image:", manifest["boot.img"])


if __name__ == "__main__":
    main()
