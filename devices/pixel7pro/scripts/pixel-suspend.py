#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded Pixel s2idle with the persistent modem supervisor running.

The shell owns idle policy and display wake. --keep-dark leaves the display
alone; --allow-charging is for explicit, connected diagnostics only.
"""
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

POWER = Path('/sys/power')
RTC = Path('/sys/class/rtc/rtc0')
SOCKET = '/run/omarchy-mobile-telephony/socket'
DISPLAY = '/root/.local/bin/omarchy-mobile-display'
WIFI_PCI = '0000:01:00.0'
WIFI_RESTART = '/usr/local/sbin/pixel-wifi-restart'


def wifi_answers():
    """Whether the BCM4389 firmware still answers a query. After some s2idle
    resumes its control ring fills and every request fails; the device then
    refuses each later suspend (D3 entry times out), so the phone never
    sleeps again. `iw ... info` reports txpower only when the firmware
    answers brcmfmac's qtxpower query; power_save comes from cfg80211's
    cache and proves nothing. A D3 timeout in the recent kernel log counts
    as no answer too."""
    net = Path('/sys/bus/pci/devices', WIFI_PCI, 'net')
    names = [n.name for n in net.iterdir()] if net.is_dir() else []
    if not names:
        return True
    try:
        log = subprocess.run(['dmesg'], capture_output=True, text=True, timeout=15).stdout
        if 'Timeout on response for entering D3' in '\n'.join(log.splitlines()[-80:]):
            return False
        info = subprocess.run(['iw', 'dev', names[0], 'info'], capture_output=True,
                              text=True, timeout=15)
        return info.returncode == 0 and 'txpower' in info.stdout
    except subprocess.TimeoutExpired:
        return False


def read(path):
    return Path(path).read_text().strip()


def request(action='status', **kwargs):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(8)
        client.connect(SOCKET)
        client.sendall(json.dumps(dict(action=action, **kwargs)).encode() + b'\n')
        with client.makefile() as stream:
            reply = json.loads(stream.readline(65536))
    if not reply.get('ok'):
        raise RuntimeError('Modem service refused sleep preparation')
    return reply['result']


def idle_modem():
    status = request()
    if not status['modem']['ready']:
        raise RuntimeError('Waiting for modem registration')
    if any(call['state'] != 'terminated' for call in status['calls']) or status['audio']['running']:
        raise RuntimeError('Call in progress')


def prerequisites(allow_charging=False):
    if b'google,GS201 CHEETAH' not in Path('/sys/firmware/devicetree/base/compatible').read_bytes():
        raise RuntimeError('Unsupported phone')
    if '[s2idle]' not in read(POWER / 'mem_sleep') or '[none]' not in read(POWER / 'pm_test'):
        raise RuntimeError('Normal s2idle must be selected')
    for path in (RTC / 'device/power/wakeup', '/sys/bus/platform/devices/pixel-keys/power/wakeup'):
        if read(path) != 'enabled':
            raise RuntimeError('RTC or power-key wake is unavailable')
    if not Path('/sys/module/pixel_wdt_pm').exists():
        raise RuntimeError('Watchdog suspend support is unavailable')
    if not allow_charging and read('/sys/class/power_supply/usb/online') != '0':
        raise RuntimeError('Unplug before automatic sleep')
    if int(read('/sys/class/power_supply/battery/capacity')) < 10:
        raise RuntimeError('Charge before suspend testing')
    spec = importlib.util.spec_from_file_location('pixel_manager', '/usr/local/lib/omarchy-mobile/modem/manager.py')
    manager = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manager)
    if not all(manager.recorded_pid(name) for name in ('manager', 'watchdog', 'runtime', 'apps')):
        raise RuntimeError('Persistent modem supervision is unavailable')
    if manager.cp_state() != 4:
        raise RuntimeError('Modem is not online')
    idle_modem()


def wake_count():
    # This sysfs read blocks while any wake lock is active, even O_NONBLOCK.
    # Raise from SIGALRM so Python does not restart the read indefinitely.
    def expired(*_):
        raise TimeoutError('Wake sources are still active')
    previous = signal.signal(signal.SIGALRM, expired)
    signal.setitimer(signal.ITIMER_REAL, 3)
    try:
        return read(POWER / 'wakeup_count')
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous)


class Alarm:
    """Keep an existing earlier alarm; replace/restore only our own value."""
    def __init__(self, seconds):
        self.path = RTC / 'wakealarm'
        self.previous = read(self.path)
        now = int(read(RTC / 'since_epoch'))
        self.owned = ''
        target = now + seconds
        if self.previous and int(self.previous) <= target:
            if int(self.previous) <= now:
                raise RuntimeError('An RTC alarm is already due')
            return
        # Set ownership before writes so partial setup is also rolled back.
        self.owned = str(target)
        try:
            self.path.write_text('0\n')
            self.path.write_text(self.owned + '\n')
        except BaseException:
            self.restore()
            raise

    def restore(self):
        if not self.owned:
            return
        current = read(self.path)
        if current not in ('', self.owned):
            return  # A different owner scheduled something after us.
        self.path.write_text('0\n')
        if self.previous and int(self.previous) > int(read(RTC / 'since_epoch')):
            self.path.write_text(self.previous + '\n')


def wake_reason():
    try:
        irq = read(POWER / 'pm_wakeup_irq')
    except OSError:
        return 'unknown'
    for line in Path('/proc/interrupts').read_text().splitlines():
        fields = line.split()
        if fields and fields[0] == irq + ':':
            name = fields[-1]
            if 'cp2ap' in name:
                return 'modem'
            if 'pixel-keys' in name or 'pwrkey' in name:
                return 'power-key'
            if 'rtc' in name or 'alarm' in name:
                return 'alarm'
            return name
    return 'unknown'


def display(on):
    env = dict(os.environ, XDG_RUNTIME_DIR='/run/user/0')
    subprocess.run([DISPLAY, 'on' if on else 'off'], env=env, check=True, timeout=10,
                   stdout=subprocess.DEVNULL)


def suspend(seconds, keep_dark, allow_charging):
    prerequisites(allow_charging)
    alarm = None
    changed_screen = False
    prior = None
    cleanup_errors = []
    try:
        if not keep_dark:
            display(False)
        # The display hook keeps the modem's screen state with the display;
        # put back what it was (still off while the screen stays dark).
        prior = request('power-status').get('screen_on')
        changed_screen = True  # Restore even if a request was only partly accepted.
        request('screen-state', on=False)
        os.sync()
        alarm = Alarm(seconds)
        count = wake_count()
        idle_modem()
        POWER.joinpath('wakeup_count').write_text(count + '\n')
        before = (time.clock_gettime(time.CLOCK_BOOTTIME), time.monotonic())
        try:
            POWER.joinpath('state').write_text('mem\n')
        except OSError as error:
            device = read(POWER / 'suspend_stats/last_failed_dev')
            step = read(POWER / 'suspend_stats/last_failed_step')
            note = ''
            if device == WIFI_PCI and not wifi_answers():
                restarted = subprocess.run([WIFI_RESTART], capture_output=True, timeout=120)
                note = '; Wi-Fi firmware hung, ' + (
                    'restarted' if restarted.returncode == 0 else 'restart failed')
            raise RuntimeError(f'Suspend refused by {device or "a wake event"} ({step}){note}') from error
        slept = time.clock_gettime(time.CLOCK_BOOTTIME) - before[0] - (time.monotonic() - before[1])
        result = dict(woke=wake_reason(), slept=round(max(0, slept), 3))
    finally:
        cleanups = []
        if alarm:
            cleanups.append(alarm.restore)
        if changed_screen:
            cleanups.append(lambda: request('screen-state', on=prior is not False))
        if not keep_dark:
            cleanups.append(lambda: display(True))
        for cleanup in cleanups:
            try:
                cleanup()
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
                cleanup_errors.append(str(error))
        if cleanup_errors:
            raise RuntimeError('Resume cleanup failed: ' + '; '.join(cleanup_errors))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--keep-dark', action='store_true')
    parser.add_argument('--allow-charging', action='store_true')
    parser.add_argument('--wake-after', type=int, default=900)
    args = parser.parse_args()
    if not 10 <= args.wake_after <= 3600:
        parser.error('--wake-after must be 10 to 3600 seconds')
    def interrupted(*_):
        raise InterruptedError('Suspend interrupted')
    signal.signal(signal.SIGTERM, interrupted)
    try:
        with open('/run/pixel-suspend.lock', 'w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if args.check:
                prerequisites(args.allow_charging)
                print('Ready for bounded s2idle')
            else:
                print(json.dumps(suspend(args.wake_after, args.keep_dark, args.allow_charging)))
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(json.dumps(dict(woke='error', error=str(error))))
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
