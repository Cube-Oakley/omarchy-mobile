#!/usr/bin/env python3
"""Read-only stock baseline. All device data stays under ignored out/."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", default=os.environ.get("PHONE_SERIAL"))
    parser.add_argument("--camera-assets", action="store_true",
                        help="Also copy stock camera tuning, libraries and APK (~1.6 GB)")
    args = parser.parse_args()
    serial = args.serial or (ROOT / "out/device.serial").read_text().strip()
    if not serial or any(c.isspace() for c in serial):
        parser.error("Set PHONE_SERIAL or out/device.serial to one handset serial")
    adb = ["adb", "-s", serial]

    def run(*argv, timeout=120):
        return subprocess.run(adb + list(argv), capture_output=True, timeout=timeout)

    def prop(key):
        result = run("shell", "getprop " + shlex.quote(key))
        if result.returncode:
            raise RuntimeError("Cannot read device identity; check ADB authorization")
        return result.stdout.decode().strip()

    identity = {key: prop(key) for key in (
        "ro.product.manufacturer", "ro.product.model", "ro.soc.model",
        "ro.build.display.id", "ro.boot.slot_suffix", "ro.boot.flash.locked")}
    if (identity["ro.product.manufacturer"] != "OnePlus"
            or identity["ro.product.model"] != "CPH2655"
            or identity["ro.soc.model"] != "SM8750"):
        raise RuntimeError("Expected the surveyed CPH2655 / SM8750; refusing another device")

    os.umask(0o077)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    dest = ROOT / "out" / ("stock-" + stamp)
    ignored = subprocess.run(["git", "check-ignore", "-q", str(dest / "probe")], cwd=ROOT)
    if ignored.returncode:
        raise RuntimeError("Backup destination must be Git-ignored")
    dest.mkdir(parents=True)
    manifest = {"created_utc": stamp, "identity": identity, "operations": [],
                "media_verified": False, "partition_backup": False}
    print(f"Saving stock baseline to {dest}", flush=True)

    def record(name, *argv, timeout=120):
        try:
            result = run(*argv, timeout=timeout)
            (dest / (name + ".stdout")).write_bytes(result.stdout)
            (dest / (name + ".stderr")).write_bytes(result.stderr)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
        manifest["operations"].append({"name": name, "argv": argv, "exit": code})
        (dest / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        return code

    probes = {
        "getprop": "getprop",
        "kernel": "uname -a; cat /proc/version; cat /proc/cmdline; cat /proc/bootconfig",
        "storage": "cat /proc/partitions; ls -l /dev/block/by-name; df -h; mount",
        "battery": "dumpsys battery",
        "display": "dumpsys display",
        "input": "getevent -lp",
        "sensors": "dumpsys sensorservice",
        "camera": "dumpsys media.camera",
        "audio": "dumpsys media.audio_policy; cat /proc/asound/cards",
        "thermal": "dumpsys thermalservice",
        "services": "service list; lshal",
        "modules": "cat /proc/modules; ls -l /vendor_dlkm/lib/modules /odm_dlkm/lib/modules",
        "firmware-inventory": "ls -lR /vendor/firmware /vendor/firmware_mnt /odm/firmware",
        "camera-inventory": "ls -lR /odm/etc/camera /vendor/lib64/camera /odm/lib64/camera; pm path com.oplus.camera",
    }
    for name, command in probes.items():
        record(name, "shell", command)
    record("kernel-config", "pull", "/proc/config.gz", str(dest / "config.gz"))

    # Hash each original on the phone before copying; filenames are shell-quoted.
    photos = dest / "photos"
    photos.mkdir()
    originals = []
    for folder in ("DCIM", "Pictures"):
        result = run("shell", f"find /sdcard/{folder} -type f -print0")
        if result.returncode:
            raise RuntimeError(f"Cannot enumerate {folder}; baseline incomplete at {dest}")
        paths = result.stdout.decode().split("\0")
        for path in filter(None, paths):
            result = run("shell", "sha256sum " + shlex.quote(path))
            if result.returncode:
                raise RuntimeError(f"Cannot checksum original {path}")
            digest = result.stdout.decode().split()[0]
            originals.append({"remote": path, "sha256": digest})
        if record("pull-" + folder, "pull", "-a", "/sdcard/" + folder, str(photos), timeout=600):
            raise RuntimeError(f"Photo copy failed; baseline incomplete at {dest}")
    for item in originals:
        relative = Path(item["remote"]).relative_to("/sdcard")
        if sha256(photos / relative) != item["sha256"]:
            raise RuntimeError(f"Checksum mismatch: {relative}")
    manifest["media"] = originals
    manifest["media_verified"] = True

    if args.camera_assets:
        for remote in ("/odm/etc/camera", "/vendor/lib64/camera", "/odm/lib64/camera",
                       "/product/app/OplusCamera"):
            local = dest / "stock-files" / remote.lstrip("/")
            local.parent.mkdir(parents=True, exist_ok=True)
            record("pull-" + remote.strip("/").replace("/", "-"),
                   "pull", "-a", remote, str(local), timeout=600)

    # Local integrity list includes diagnostics and copied assets; media also has
    # independent device-side checksums. Partial pulls remain marked in manifest.
    hashes = {str(p.relative_to(dest)): sha256(p) for p in sorted(dest.rglob("*"))
              if p.is_file() and p.name not in ("manifest.json", "sha256.json")}
    (dest / "sha256.json").write_text(json.dumps(hashes, indent=2) + "\n")
    (dest / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    failed = [op["name"] for op in manifest["operations"] if op["exit"] != 0]
    print(f"Verified {len(originals)} media files against phone SHA256.")
    print("Unavailable/partial probes: " + (", ".join(failed) or "none"))
    print("This is a stock reference snapshot, not a full partition/recovery backup.")


if __name__ == "__main__":
    main()
