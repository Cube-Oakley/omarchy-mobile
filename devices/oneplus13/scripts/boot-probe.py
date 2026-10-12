#!/usr/bin/env python3
"""Identity-checked CPH2655 RAM boot. Never flash or switch slots."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--sha256", required=True)
    args = parser.parse_args()
    serial = os.environ.get("PHONE_SERIAL") or (ROOT / "out/device.serial").read_text().strip()
    if not re.fullmatch(r"[A-Za-z0-9_-]+", serial):
        parser.error("Set one explicit PHONE_SERIAL or out/device.serial")
    data = args.image.read_bytes()
    if hashlib.sha256(data).hexdigest() != args.sha256:
        parser.error("Image hash mismatch; phone untouched")
    if len(data) > 100663296 or data[:8] != b"ANDROID!" or struct.unpack_from("<I", data, 40)[0] != 4:
        parser.error("Expected a bounded Android v4 boot image")

    def run(tool, *argv, timeout=30):
        result = subprocess.run([tool, "-s", serial, *argv], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        if result.returncode:
            raise RuntimeError(result.stdout.strip())
        return result.stdout.strip()

    # Always start in Android so the model, project and firmware can be checked.
    # Fastboot's product=sun alone identifies multiple incompatible handsets.
    expected = {"ro.product.model": "CPH2655", "ro.soc.model": "SM8750",
                "ro.boot.prjname": "23893", "ro.build.display.id": "CPH2655_16.0.10.501(EX01)",
                "ro.boot.slot_suffix": "_a", "ro.boot.flash.locked": "0"}
    for prop, value in expected.items():
        if run("adb", "shell", "getprop", prop) != value:
            raise RuntimeError("Refusing boot: unexpected " + prop)
    run("adb", "reboot", "bootloader")
    deadline = time.monotonic() + 60
    while True:
        devices = subprocess.check_output(["fastboot", "devices"], text=True, timeout=5)
        if any(line.split()[:1] == [serial] for line in devices.splitlines()):
            break
        if time.monotonic() >= deadline:
            raise RuntimeError("Phone did not reach fastboot within 60 seconds")
        time.sleep(1)
    for name, value in [("product", "sun"), ("unlocked", "yes"), ("current-slot", "a")]:
        result = run("fastboot", "getvar", name)
        if not re.search(r"^" + re.escape(name) + r":\s*" + re.escape(value) + r"\s*$", result, re.M):
            raise RuntimeError("Refusing RAM boot: unexpected " + name)
        print(f"{name}: {value}", flush=True)
    print(run("fastboot", "boot", str(args.image.resolve()), timeout=90), flush=True)
    print("RAM image sent. The image's configured PID1 reboot deadline applies if startup reaches it.")


if __name__ == "__main__":
    main()
