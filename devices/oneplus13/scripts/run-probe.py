#!/usr/bin/env python3
"""RAM boot a prepared probe, collect USB diagnostics, observe Android return."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import select
import subprocess
import termios
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, help="Ignored build directory with manifests and USB port identity")
    parser.add_argument("--commands", type=Path, help="Additional reviewed, single-line shell diagnostics")
    parser.add_argument("--observe-seconds", type=int, default=260, help="Host observation window; does not change phone timer")
    args = parser.parse_args()
    out = args.capture.resolve()
    extra = args.commands.read_text().strip() if args.commands else ""
    if "\n" in extra or len(extra) > 3000:
        parser.error("Additional diagnostics must be one shell line of at most 3000 characters")
    subprocess.run(["git", "check-ignore", "-q", str(out / "runtime.json")], cwd=ROOT, check=True)
    if (out / "runtime.json").exists() or (out / "serial.log").exists():
        parser.error("Runtime capture already exists; preserve it and use a fresh directory")
    manifest = json.loads((out / "manifest.json").read_text())
    ports = json.loads((out / "usb-port.json").read_text())
    if len(ports) != 1 or not re.fullmatch(r"[0-9]+-[0-9.]+", ports[0]["port"]):
        parser.error("Exactly one physical USB port must have been captured for this handset")
    serial = os.environ.get("PHONE_SERIAL") or (ROOT / "out/device.serial").read_text().strip()
    usbroot = Path("/sys/bus/usb/devices") / ports[0]["port"]
    if (usbroot / "serial").read_text().strip() != serial:
        parser.error("Phone is no longer on the captured USB port")
    result = subprocess.run(["python", str(ROOT / "scripts/boot-probe.py"),
                             "--image", str(out / "boot.img"),
                             "--sha256", manifest["boot.img"]["sha256"]],
                            capture_output=True, text=True, timeout=180)
    (out / "boot-send.txt").write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, flush=True)
    result.check_returncode()
    started = time.monotonic()
    fd, sent, native, returned, lastusb = None, False, False, False, None
    events = []
    with (out / "serial.log").open("wb") as sink:
        try:
            while time.monotonic() - started < args.observe_seconds:
                elapsed = round(time.monotonic() - started, 1)
                try:
                    usb = ((usbroot / "idVendor").read_text().strip(),
                           (usbroot / "idProduct").read_text().strip())
                except OSError:
                    usb = None
                if usb != lastusb:
                    print("USB transition", elapsed, usb, flush=True)
                    events.append({"seconds": elapsed, "usb": usb})
                    lastusb = usb
                if usb in [("0525", "a4a7"), ("0525", "a4aa")] and fd is None:
                    for tty in Path("/sys/class/tty").glob("ttyACM*"):
                        if usbroot.resolve() not in (tty / "device").resolve().parents:
                            continue
                        try:
                            fd = os.open("/dev/" + tty.name, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        except OSError as error:
                            if fd is not None:
                                os.close(fd)
                                fd = None
                            if not events or events[-1].get("console_error") != str(error):
                                events.append({"seconds": elapsed, "console_error": str(error)})
                                print("Waiting for console access:", error, flush=True)
                            continue
                        attrs = termios.tcgetattr(fd)
                        attrs[0] = attrs[1] = attrs[3] = 0
                        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
                        attrs[4] = attrs[5] = termios.B115200
                        attrs[6][termios.VMIN], attrs[6][termios.VTIME] = 0, 1
                        termios.tcsetattr(fd, termios.TCSANOW, attrs)
                        native = True
                        print("Native serial opened", tty.name, elapsed, flush=True)
                        time.sleep(1)
                if fd is not None:
                    try:
                        if not sent:
                            commands = ("echo OMARCHY_NATIVE_READY; uname -a; id; "
                                        "cat /sys/devices/system/cpu/online; cat /proc/mounts; "
                                        "cat /proc/meminfo; " + (extra + "; " if extra else "") +
                                        "dmesg; echo OMARCHY_NATIVE_DONE\n")
                            (out / "commands.txt").write_text(commands)
                            os.write(fd, b"\x03\n" + commands.encode())
                            sent = True
                        ready, _, _ = select.select([fd], [], [], 0.5)
                        if ready:
                            data = os.read(fd, 65536)
                            if data:
                                sink.write(data)
                                sink.flush()
                    except OSError as error:
                        print("Serial disconnected", str(error), flush=True)
                        os.close(fd)
                        fd = None
                if usb and usb[0] == "22d9" and elapsed > 10:
                    result = subprocess.run(["adb", "-s", serial, "shell", "getprop sys.boot_completed"],
                                            capture_output=True, text=True, timeout=5)
                    if result.returncode == 0 and result.stdout.strip() == "1":
                        print("Android returned", elapsed, flush=True)
                        returned = True
                        break
                time.sleep(1)
        finally:
            if fd is not None:
                os.close(fd)
            contents = (out / "serial.log").read_bytes().replace(b"\r", b"")
            report = {"native_usb_seen": native, "android_returned": returned,
                      "shell_commands_completed": bool(re.search(rb"^OMARCHY_NATIVE_DONE$", contents, re.M)),
                      "elapsed": time.monotonic() - started, "transitions": events}
            (out / "runtime.json").write_text(json.dumps(report, indent=2) + "\n")
            print("Probe result:", report, flush=True)


if __name__ == "__main__":
    main()
