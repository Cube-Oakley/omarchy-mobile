#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded Pixel sleep with the persistent modem supervisor running.

The shell owns idle policy and display wake. --keep-dark leaves the display
alone; --allow-charging is for explicit, connected diagnostics only.

With the boot's pixel-sleep (kernel/suspend) and a pixel-ufs that can take
the UFS link down, each sleep is the stock SYS_SLEEP (deep), otherwise
s2idle. SYS_SLEEP needs Wi-Fi unloaded, so Wi-Fi is off while the screen is
dark: it is loaded again after the shell's background alarm (the periodic
check for mail and messages) and the power key, and by the display hook when
the screen lights; a modem wake keeps it off. Bluetooth is unloaded too (its
chip loses power in SYS_SLEEP) and only the display hook loads it again.
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
WIFI_MODULES = ('brcmfmac_wcc', 'brcmfmac', 'pixel_pcie')
# In SYS_SLEEP the PERIC0 pads take their power-down state, which makes
# BT_REG_ON an input: the Bluetooth chip loses power and comes back without
# its patch firmware, and BlueZ's resume waits out a 2 s command timeout on
# every wake. pixel-bt powers the chip down cleanly on unload; the display
# hook loads it again (pixel-bt-restart) while this marker exists.
BT_MODULES = ('hci_uart', 'pixel_bt')
BT_SLEEPING = Path('/run/pixel-bt-sleeping')
COULOMB = Path('/sys/module/pixel_battery/parameters/coulomb')
SLEEP_PARAMS = Path('/sys/module/pixel_sleep/parameters')
UFS_PARAMS = Path('/sys/module/pixel_ufs/parameters')
SUSPEND_MODULES = Path('/usr/local/lib/omarchy-mobile/suspend')
# pixel-sleep refuses while the modem's PCIe link is up; it drops within a
# second or two of idle.
DEEP_TRIES = 4
DEEP_RETRY_GAP = 2
# Interrupts that tell what ended a deep sleep: the wake source stays
# pending through the resume and is counted once the kernel runs.
WAKE_IRQS = {'pixel-keys': 'power-key', 'pixel-rtc': 'alarm', 'cp2ap_wakeup': 'modem'}
# Most modem wakes carry nothing for the shell (link and network upkeep);
# they come every few seconds. The modem holds its wake locks while it sends,
# and the service reads new packets every 50 ms; this long after the locks
# are released, with no call, no text indication and no change to the
# message store, deep sleep goes on until the shell's alarm.
MODEM_SETTLE = 0.3
# The service's indications for an incoming text or a call (app_service.py).
MODEM_EVENTS = ('010c', '010d', '0d1e', '0d20', '0d01', '0d04')
MESSAGES = Path('/root/.local/share/omarchy-mobile/messages')


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


def deep_ready():
    """SYS_SLEEP is possible: pixel-sleep loaded for real firmware entry and a
    pixel-ufs with the link-off path (kernel/storage)."""
    try:
        return ((SLEEP_PARAMS / 'real_sleep').read_text().strip() == 'Y'
                and (UFS_PARAMS / 'deep_link_off').exists()
                and 'deep' in read(POWER / 'mem_sleep'))
    except OSError:
        return False


def deep_watchdog():
    """The modem bundle's pixel-wdt-pm predates deep sleep, which powers both
    watchdogs off; the root copy restores them afterwards (kernel/watchdog)."""
    if Path('/sys/module/pixel_wdt_pm/parameters/deep_suspends').exists():
        return
    subprocess.run(['rmmod', 'pixel_wdt_pm'], check=True, timeout=15)
    subprocess.run(['insmod', str(SUSPEND_MODULES / 'pixel-wdt-pm.ko')], check=True, timeout=15)


def restarted(name):
    """Wait for a running pixel-wifi-restart or pixel-bt-restart (the display
    hook starts them when the screen lights) before unloading what it loads."""
    with open(f'/run/{name}.lock', 'w') as lock:
        for _ in range(120):
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return
            except BlockingIOError:
                time.sleep(0.5)
    raise RuntimeError(f'{name} is still running')


def wifi_off():
    restarted('pixel-wifi-restart')
    for module in WIFI_MODULES:
        if Path('/sys/module', module).exists():
            subprocess.run(['rmmod', module], check=True, timeout=30)


def wifi_on():
    if not Path('/sys/module/brcmfmac').exists():
        subprocess.run([WIFI_RESTART, '--start'], capture_output=True, timeout=120)


def bluetooth_off():
    """Unload Bluetooth if the boot started it. A module that will not unload
    costs the 2 s per wake but does not stop the sleep."""
    restarted('pixel-bt-restart')
    if not Path('/sys/module/pixel_bt').exists():
        return
    BT_SLEEPING.touch()
    for module in BT_MODULES:
        if Path('/sys/module', module).exists():
            subprocess.run(['rmmod', module], capture_output=True, timeout=30)


def coulomb():
    """The gauge's coulomb counter in nAh (it falls while discharging)."""
    try:
        return int(COULOMB.read_text().split()[1])
    except (OSError, IndexError, ValueError):
        return None


def irq_counts():
    counts = dict.fromkeys(WAKE_IRQS.values(), 0)
    for line in Path('/proc/interrupts').read_text().splitlines()[1:]:
        fields = line.split()
        if fields and fields[-1] in WAKE_IRQS:
            counts[WAKE_IRQS[fields[-1]]] += sum(int(f) for f in fields[1:] if f.isdigit())
    return counts


def message_marker():
    marker = []
    for name in ('messages.db', 'messages.db-wal'):
        try:
            info = (MESSAGES / name).stat()
            marker.append((info.st_mtime_ns, info.st_size))
        except OSError:
            marker.append(None)
    return marker


def modem_events():
    counts = request('power-status').get('indications', {})
    return sum(counts.get(ident, 0) for ident in MODEM_EVENTS)


def modem_brought_something(marker, events):
    """Whether a modem wake brought a call or a text: once the modem has
    released its wake locks (it has sent what it woke the phone for), the
    service has had time to read it."""
    try:
        wake_count()
    except TimeoutError:
        return True
    time.sleep(MODEM_SETTLE)
    if message_marker() != marker:
        return True
    try:
        if modem_events() != events:
            return True
        return any(call['state'] != 'terminated' for call in request()['calls'])
    except (OSError, RuntimeError, ValueError):
        return True


def deep_wake_reason(before, after):
    for reason in ('power-key', 'alarm', 'modem'):
        if after[reason] > before[reason]:
            return reason
    return 'unknown'


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


def deep_sleep():
    """One SYS_SLEEP, retried while pixel-sleep refuses to arm (the modem's
    link still up); the UFS link goes down with it (pixel-ufs deep_link_off)."""
    refused = int(read(SLEEP_PARAMS / 'refused'))
    early = int(read(SLEEP_PARAMS / 'early_count'))
    irqs = irq_counts()
    (UFS_PARAMS / 'deep_link_off').write_text('1\n')
    try:
        for attempt in range(DEEP_TRIES):
            (SLEEP_PARAMS / 'budget').write_text('1\n')
            POWER.joinpath('mem_sleep').write_text('deep\n')
            try:
                count = wake_count()
                idle_modem()
                POWER.joinpath('wakeup_count').write_text(count + '\n')
                irqs = irq_counts()
                charge = coulomb()
                before = (time.clock_gettime(time.CLOCK_BOOTTIME), time.monotonic())
                POWER.joinpath('state').write_text('mem\n')
                slept = time.clock_gettime(time.CLOCK_BOOTTIME) - before[0] - (time.monotonic() - before[1])
                after = coulomb()
                return dict(woke=deep_wake_reason(irqs, irq_counts()), slept=round(max(0, slept), 3),
                            sleep='deep', used_nah=None if None in (charge, after) else charge - after)
            except OSError as error:
                # A wake event during entry takes pixel-sleep's early path:
                # the SoC never went down, and the write fails. That is a
                # wake, not a refusal.
                if int(read(SLEEP_PARAMS / 'early_count')) > early:
                    return dict(woke=deep_wake_reason(irqs, irq_counts()), slept=0.0, sleep='deep-early')
                now = int(read(SLEEP_PARAMS / 'refused'))
                if now == refused or attempt == DEEP_TRIES - 1:
                    step = read(POWER / 'suspend_stats/last_failed_step')
                    device = read(POWER / 'suspend_stats/last_failed_dev')
                    raise RuntimeError(f'Deep sleep refused by {device or "pixel-sleep"} ({step})') from error
                refused = now
                time.sleep(DEEP_RETRY_GAP)
            finally:
                POWER.joinpath('mem_sleep').write_text('s2idle\n')
    finally:
        (UFS_PARAMS / 'deep_link_off').write_text('0\n')


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
        deep = deep_ready()
        if deep:
            deep_watchdog()
            wifi_off()
            bluetooth_off()
        os.sync()
        alarm = Alarm(seconds)
        if deep:
            deadline = time.monotonic() + seconds
            slept = 0.0
            # Charge (nAh) and time over the sleeps the gauge measured.
            used, measured = 0, 0.0
            quiet = 0
            marker = message_marker()
            events = modem_events()
            while True:
                result = deep_sleep()
                slept += result['slept']
                part = result.pop('used_nah', None)
                if part is not None:
                    used += part
                    measured += result['slept']
                if (result['woke'] != 'modem' or deadline - time.monotonic() < 10
                        or modem_brought_something(marker, events)):
                    break
                quiet += 1
            result.update(slept=round(slept, 3), quiet_modem_wakes=quiet)
            # The average current in deep sleep, each sleep's entry and exit
            # included: the gauge's coulomb counter around every state write.
            if measured >= 60:
                result['deep_ma'] = round(used / 1e6 * 3600 / measured, 1)
            if result['woke'] in ('alarm', 'power-key'):
                wifi_on()
            return result
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
                print('Ready for bounded ' + ('SYS_SLEEP' if deep_ready() else 's2idle'))
            else:
                print(json.dumps(suspend(args.wake_after, args.keep_dark, args.allow_charging)))
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(json.dumps(dict(woke='error', error=str(error))))
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
