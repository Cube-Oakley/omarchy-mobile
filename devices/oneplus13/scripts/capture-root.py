#!/usr/bin/env python3
"""Read-only rooted CPH2655 capture; never flash or restore these live snapshots."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    os.umask(0o077)
    serial = os.environ.get("PHONE_SERIAL") or (ROOT / "out/device.serial").read_text().strip()
    if not serial or any(c.isspace() for c in serial):
        raise RuntimeError("A single explicit handset serial is required")
    adb = ["adb", "-s", serial]

    def command(cmd):
        # shell v2 without a PTY preserves binary stdout, separate stderr and
        # the remote exit status (exec-out does not reliably report failures).
        return adb + ["shell", "-T", "/debug_ramdisk/su -c " + shlex.quote(cmd)]

    def read(cmd):
        return subprocess.check_output(command(cmd), timeout=180).decode().strip()

    identity = read("getprop ro.product.model; getprop ro.soc.model; getprop ro.build.display.id")
    if identity.splitlines() != ["CPH2655", "SM8750", "CPH2655_16.0.10.501(EX01)"]:
        raise RuntimeError("Unexpected handset/build")
    if read("id -u") != "0":
        raise RuntimeError("Root permission must already be granted")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    dest = ROOT / "out" / ("root-stock-" + stamp)
    subprocess.run(["git", "check-ignore", "-q", str(dest / "probe")], cwd=ROOT, check=True)
    dest.mkdir()
    (dest / "partitions").mkdir()
    manifest = {"created_utc": stamp, "identity": identity, "partitions": [], "probes": [],
                "warning": "Live reads, not an atomic or tested restore set. init_boot_a is Magisk patched.",
                "excluded": "Whole disks, userdata, super, hybridswap, rawdump, metadata; partitions over 512 MiB"}

    def save():
        (dest / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    def capture(name, cmd, timeout=600):
        target = dest / name
        with target.open("wb") as output, (dest / (name.replace("/", "_") + ".stderr")).open("wb") as error:
            result = subprocess.run(command(cmd), stdout=output, stderr=error, timeout=timeout)
        with target.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        return {"file": name, "exit": result.returncode, "bytes": target.stat().st_size, "sha256": digest}

    print(f"Capture: {dest}", flush=True)
    probes = {
        "identity.txt": "getprop",
        "partitions.txt": "ls -l /dev/block/by-name; cat /proc/partitions; mount",
        "iomem.txt": "cat /proc/iomem",
        "boot.txt": "cat /proc/cmdline; cat /proc/bootconfig; cat /proc/meminfo",
        "modules.txt": "cat /proc/modules",
        "dmesg.txt": "dmesg",
        "live.dtb": "cat /sys/firmware/fdt",
        "device-tree.tar": "tar -C /sys/firmware/devicetree/base -cf - .",
        "persist-files.tar": "tar -C /mnt/vendor/persist -cf - .",
        "hardware.txt": "ls -l /sys/bus/i2c/devices; ls -l /sys/bus/spi/devices; cat /proc/bus/input/devices; cat /proc/asound/cards",
    }
    for name, cmd in probes.items():
        manifest["probes"].append(capture(name, cmd))
        save()
    names = read("ls -1 /dev/block/by-name").splitlines()
    priority = ["persist", "modemst1", "modemst2", "fsg", "fsc", "ocdt", "cdt", "oplusdycnvbk", "oplusstanvbk_a", "oplusstanvbk_b"]
    names = priority + sorted(set(names) - set(priority))
    for name in names:
        if not re.fullmatch(r"[A-Za-z0-9_-]+", name):
            raise RuntimeError("Unexpected partition name")
        if name in {"userdata", "super", "hybridswap", "rawdump", "metadata"} or re.fullmatch("sd[a-z]+", name):
            continue
        block = "/dev/block/by-name/" + name
        size = int(read("blockdev --getsize64 " + block))
        if size > 512 * 1024 * 1024:
            continue
        before = read("sha256sum " + block).split()[0]
        entry = capture("partitions/" + name + ".img", "cat " + block)
        after = read("sha256sum " + block).split()[0]
        entry.update(name=name, partition_bytes=size, before_sha256=before, after_sha256=after)
        entry["stable_verified"] = entry["exit"] == 0 and entry["bytes"] == size and before == entry["sha256"] == after
        manifest["partitions"].append(entry)
        save()
        print(f"{name}: {size} bytes; stable verified={entry['stable_verified']}", flush=True)
        if entry["exit"] or entry["bytes"] != size:
            raise RuntimeError("Incomplete partition read; stopping")
    print("Finished; inspect manifest for live-changing partitions and partial probes", flush=True)


if __name__ == "__main__":
    main()
