#!/usr/bin/env python3
"""Restore wall time from the PMK8550 counter and a native-root time anchor.

Never sets the hardware RTC, alarms, UEFI variables or calibration partitions.
The reference epoch must come from a separately verified clock (e.g. USB host).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

BASE = Path('/opt/oneplus13-rtc')
STATE = Path('/var/lib/omarchy-mobile/rtc-anchor.json')
RTC = Path('/sys/class/rtc/rtc0')
RTC_NAME = 'rtc-pm8xxx c400000.spmi:pmic@0:rtc@6100'


def restored_epoch(anchor, counter):
    if (anchor.get('version') != 1 or anchor.get('rtc_name') != RTC_NAME
            or type(anchor.get('unix_epoch')) is not int
            or type(anchor.get('rtc_counter')) is not int):
        raise ValueError('Invalid RTC time anchor')
    epoch, old = anchor['unix_epoch'], anchor['rtc_counter']
    if not 1767225600 <= epoch < 4102444800 or not 0 <= old <= 0xffffffff:
        raise ValueError('RTC anchor outside supported range')
    delta = counter - old
    if not 0 <= delta <= 10 * 366 * 86400:
        raise ValueError('RTC reset/wrap or stale anchor; recalibration required')
    return epoch + delta


def save_anchor(anchor):
    STATE.parent.mkdir(parents=True, exist_ok=True)
    tmp = STATE.with_suffix('.new')
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as stream:
        json.dump(anchor, stream, indent=2); stream.write('\n')
        stream.flush(); os.fsync(stream.fileno())
    os.replace(tmp, STATE)
    fd = os.open(STATE.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument('--calibrate', type=int, metavar='UNIX_EPOCH')
    mode.add_argument('--restore', action='store_true')
    a = ap.parse_args()
    if (Path('/sys/firmware/devicetree/base/model').read_bytes().rstrip(b'\0') !=
            b'OnePlus 13 CPH2655 Omarchy USB probe'
            or Path('/etc/omarchy-mobile-oneplus13-root').read_text().strip() != 'oneplus13-cph2655-v1'):
        raise ValueError('Unsupported board/root')
    # Load-time RTC hctosys can temporarily expose the raw counter as Unix time.
    # Restore the previous software clock if any later validation fails.
    before, monotonic = time.time(), time.monotonic()
    try:
        if not RTC.exists():
            manifest = json.loads((BASE / 'manifest.json').read_text())
            module = BASE / 'rtc-pm8xxx.ko'
            if (manifest['kernel_release'] != os.uname().release
                    or hashlib.sha256(module.read_bytes()).hexdigest() != manifest['sha256']):
                raise ValueError('RTC module differs from the verified kernel build')
            subprocess.run(['/usr/bin/insmod', str(module)], check=True)
        if (RTC / 'name').read_text().strip() != RTC_NAME:
            raise ValueError('Unexpected RTC device')
        counter = int((RTC / 'since_epoch').read_text())
        if a.calibrate is not None:
            anchor = dict(version=1, rtc_name=RTC_NAME, rtc_counter=counter, unix_epoch=a.calibrate)
            epoch = restored_epoch(anchor, counter)
            save_anchor(anchor)
        else:
            anchor = json.loads(STATE.read_text())
            epoch = restored_epoch(anchor, counter)
        time.clock_settime(time.CLOCK_REALTIME, epoch)
        print('Wall clock restored from RTC counter and native-root anchor:', epoch)
    except Exception:
        time.clock_settime(time.CLOCK_REALTIME, before + time.monotonic() - monotonic)
        raise


if __name__ == '__main__':
    main()
