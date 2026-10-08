#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded Pixel sleep with the persistent modem supervisor running.

The shell owns idle policy and display wake. --keep-dark leaves the display
alone; --allow-charging is for explicit, connected diagnostics only.

With the boot's pixel-sleep (kernel/suspend) and a pixel-ufs that can take
the UFS link down, each sleep is the stock SYS_SLEEP (deep), otherwise
s2idle. SYS_SLEEP needs Wi-Fi unloaded, so Wi-Fi is off while the screen is
dark: it is loaded again after the shell's background alarm (the periodic
check for mail and messages) unless the shell asks for that check over mobile
data (OMARCHY_MOBILE_CHECK_NETWORK=data, Settings > Battery), after the power
key, and by the display hook when the screen lights; a modem wake keeps it
off. Bluetooth stays on through SYS_SLEEP when it is on and its low-power
mode is wired (pixel-bt sleep_ready): its links stay up, and the chip's host
wake (EINT 51) wakes the phone for their traffic. Otherwise it is unloaded,
and only the display hook loads it again.

Under the always-on clock the shell sets OMARCHY_MOBILE_MINUTE_TICK to a file
it watches. The phone then wakes a second after each minute until
--wake-after runs out, writes the file (the shell draws the new minute),
waits until the display has sent it and powered down again, and sleeps
again, all in this one call: nothing else is set up or put back between
minutes.
"""
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import pwd
import signal
import socket
import stat
import subprocess
import time

POWER = Path('/sys/power')
RTC = Path('/sys/class/rtc/rtc0')
SOCKET = '/run/omarchy-mobile-telephony/socket'
WIFI_PCI = '0000:01:00.0'
WIFI_RESTART = '/usr/local/sbin/pixel-wifi-restart'
WIFI_MODULES = ('brcmfmac_wcc', 'brcmfmac', 'pixel_pcie')
# In SYS_SLEEP the PERIC0 pads take their power-down state. pixel-bt sets
# BT_REG_ON's to "previous", so the chip stays powered; with its low-power
# mode wired (sleep_ready: device wake and the host-wake interrupt through
# pixel-gpio) it sleeps on its own and wakes the phone through gpa6-6, EINT
# wake bit 51 in pixel-sleep's numbering. Without that mode, or with
# Bluetooth off, it is unloaded instead: pixel-bt powers the chip down
# cleanly, and the display hook loads it again (pixel-bt-restart) while this
# marker exists.
BT_MODULES = ('hci_uart', 'pixel_bt')
BT_SLEEPING = Path('/run/pixel-bt-sleeping')
BT_PARAMS = Path('/sys/module/pixel_bt/parameters')
BT_HOST_WAKE_EINT = 51
# Kept through deep sleep while in use (bluetooth_in_use) since October 3:
# the HCI timeouts after kept deep sleeps were pixel-sleep dropping
# BT_REG_ON at retention release (192c24d). This file opts out (Bluetooth
# unloaded for every deep sleep).
BT_UNLOAD_FLAG = Path('/etc/omarchy-mobile/bluetooth-unload-in-sleep')
BT_SLEEP_APPS = 'omarchy-mobile-bluetooth-sleep.d'
ALLOW_CHARGING_FLAG = Path('/run/pixel-suspend-allow-charging')
BT_POWERED = ['dbus-send', '--system', '--print-reply', '--reply-timeout=5000', '--dest=org.bluez',
              '/org/bluez/hci0', 'org.freedesktop.DBus.Properties.Get',
              'string:org.bluez.Adapter1', 'string:Powered']
COULOMB = Path('/sys/module/pixel_battery/parameters/coulomb')
# pixel-sensor-proxy (sensors/): a sensor left running through a sleep has the
# AoC write its samples to DRAM, waking the memory interface about 28 times a
# second (+17 mA in SYS_SLEEP), so sensors pause for every sleep.
SENSOR_POWER = ['dbus-send', '--system', '--print-reply', '--reply-timeout=5000',
                '--dest=net.hadess.SensorProxy', '/net/hadess/SensorProxy']
SLEEP_PARAMS = Path('/sys/module/pixel_sleep/parameters')
UFS_PARAMS = Path('/sys/module/pixel_ufs/parameters')
SUSPEND_MODULES = Path('/usr/local/lib/omarchy-mobile/suspend')
# The modem bundle's watchdog module, loaded at boot (modem/manager.py).
MODEM_WATCHDOG = Path('/var/lib/omarchy-mobile/modem/pixel-wdt-pm.ko')
# pixel-sleep refuses while the modem's PCIe link is up; it drops within a
# second or two of idle.
DEEP_TRIES = 4
DEEP_RETRY_GAP = 2
FREEZE_RETRY_GAP = 0.5
# Interrupts that tell what ended a deep sleep: the wake source stays
# pending through the resume and is counted once the kernel runs.
# The touch controller's interrupt wakes the phone from the always-on display
# (a long press on the fingerprint mark; pixel-sleep touch_wake).
WAKE_IRQS = {'pixel-keys': 'power-key', 'pixel-touch-attn': 'touch', 'pixel-rtc': 'alarm',
             'cp2ap_wakeup': 'modem', 'host_wake': 'bluetooth'}
# pixel-scanout's counters (kernel/display-v29): a frame sent, and the
# pipeline powered off after it (pixel-sleep refuses while it is up).
SCANOUT_STATS = Path('/sys/kernel/debug/dri/pixel-scanout/pixel_scanout')
# Without the counters, this long for the shell to draw and the display to
# send the frame and power off (100 ms after it).
REDRAW_WAIT = 0.6
# Most modem wakes carry nothing for the shell (link and network upkeep);
# they come every few seconds. The modem holds its wake locks while it sends,
# and the service reads new packets every 50 ms; this long after the locks
# are released, with no call, no text indication and no change to the
# message store, deep sleep goes on until the shell's alarm.
MODEM_SETTLE = 0.3
# The service's indications for an incoming text or a call (app_service.py).
MODEM_EVENTS = ('010c', '010d', '0d1e', '0d20', '0d01', '0d04')
# The message store's place in the session's home (session_account).
MESSAGES = '.local/share/omarchy-mobile/messages'
# Push for Android apps through deep sleep (waydroid/README, "Push in deep
# sleep"). Wi-Fi is unloaded for SYS_SLEEP, so microG's connection to Google
# (port 5228), which every app's push rides on, must run over mobile data:
# the modem wakes the phone when a packet arrives. Android's sockets do not
# follow a change of uplink by themselves (the container sits behind NAT and
# never sees one), so they are closed, as Android does when its network
# changes, and the apps open them again on the new route.
ANDROID_LXC = ['lxc-info', '-P', '/var/lib/waydroid/lxc', '-n', 'waydroid', '-pH']
DATA_IFACE = 'rmnet0'
PUSH_PORT = 5228
# The uplink Android's sockets were last moved to; NetworkManager's
# dispatcher (pixel-android-uplink) keeps it while the phone is awake, and
# leaves the moves to this adapter while it runs.
PUSH_UPLINK = Path('/run/pixel-push-uplink')
SUSPEND_ACTIVE = Path('/run/pixel-suspend.active')
# microG reconnected 5.3 s after its socket was closed (October 6).
PUSH_RECONNECT_WAIT = 20
# Android's own alarms (microG's heartbeat, which keeps the carrier's NAT
# from dropping the connection) cannot wake the phone from the container: it
# wakes this often while the push connection is up, and stays up this long
# for the alarms that came due meanwhile.
PUSH_KEEPALIVE = 600
PUSH_KEEPALIVE_HOLD = 5
# After the move, mobile data quiet this long (at most the second value)
# before the first sleep.
DATA_QUIET = 3
DATA_SETTLE_MAX = 15
# Sleeps that end at once (a wake during entry, or within EARLY_SLEEP
# seconds) are each a full suspend and resume of every device, the UFS link
# taken down and up included. After one the phone waits EARLY_PAUSE awake;
# after EARLY_WAKES in a row the call ends and the policy holds the phone
# up before it tries again (October 7: 22 such cycles in 13 s, then a hang).
EARLY_SLEEP = 2
EARLY_PAUSE = 2
EARLY_WAKES = 3
# And whatever the cause, at most this many suspend cycles (pixel-sleep's
# attempts) in any CYCLE_WINDOW seconds of one call; the always-on clock's
# minute wakes are one a minute.
CYCLE_CAP = 6
CYCLE_WINDOW = 30


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
        # Lost to a failed swap (October 3: the check refused every sleep
        # for 14 hours after that, and the phone ran without its watchdog):
        # load the deep-sleep copy again.
        try:
            deep_watchdog()
        except (OSError, subprocess.SubprocessError) as error:
            raise RuntimeError(f'Watchdog suspend support is unavailable ({error})') from error
    if not allow_charging and read('/sys/class/power_supply/usb/online') != '0':
        raise RuntimeError('Unplug before automatic sleep')
    # Sleep is what saves a low battery: from the testing days this refused
    # below 10 %, which kept a nearly empty phone awake at several times the
    # drain. Only an empty one stays up (to shut down cleanly).
    if int(read('/sys/class/power_supply/battery/capacity')) < 3:
        raise RuntimeError('Battery nearly empty')
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


def sleep_refusal():
    """pixel-sleep's latest refusal, from the kernel log ('' if none)."""
    try:
        log = subprocess.run(['dmesg', '--level=warn'], capture_output=True, text=True,
                             timeout=10).stdout.splitlines()
    except (OSError, subprocess.SubprocessError):
        return ''
    for line in reversed(log[-200:]):
        if 'pixel-sleep: deep suspend refused: ' in line:
            return line.split('pixel-sleep: deep suspend refused: ', 1)[1].strip()
    return ''


def suspend_attempts():
    """pixel-sleep's count of suspend attempts (each a full suspend and
    resume of the devices)."""
    try:
        return int(read(SLEEP_PARAMS / 'attempts'))
    except (OSError, ValueError):
        return None


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
    if Path('/sys/module/pixel_wdt_pm').exists():
        subprocess.run(['rmmod', 'pixel_wdt_pm'], check=True, timeout=15)
    try:
        subprocess.run(['insmod', str(SUSPEND_MODULES / 'pixel-wdt-pm.ko')], check=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        # Never leave the phone without a watchdog: the modem bundle's copy
        # back (it cannot deep sleep, so the next check tries again).
        subprocess.run(['insmod', str(MODEM_WATCHDOG)], check=False, timeout=30)
        raise


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
    """Unloads Wi-Fi; whether it was loaded (the uplink changes now)."""
    restarted('pixel-wifi-restart')
    loaded = Path('/sys/module/brcmfmac').exists()
    for module in WIFI_MODULES:
        if Path('/sys/module', module).exists():
            subprocess.run(['rmmod', module], check=True, timeout=30)
    return loaded


def wifi_on():
    if not Path('/sys/module/brcmfmac').exists():
        subprocess.run([WIFI_RESTART, '--start'], capture_output=True, timeout=120)


def android_pid():
    """Android's init, whose network namespace holds the apps' sockets; None
    while the container is not running."""
    try:
        pid = subprocess.run(ANDROID_LXC, capture_output=True, text=True, timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    return pid if pid.isdigit() else None


def android_ss(pid, *args):
    result = subprocess.run(['nsenter', '-t', pid, '-n', 'ss', '-tnH', *args],
                            capture_output=True, text=True, timeout=10)
    return result.stdout.splitlines()


def push_ports(pid):
    """Local ports of Android's established connections to the push server."""
    return {line.split()[2].rsplit(':', 1)[-1]
            for line in android_ss(pid, 'state', 'established', f'( dport = :{PUSH_PORT} )')
            if len(line.split()) >= 4}


def uplink():
    """The interface of the default route."""
    try:
        route = subprocess.run(['ip', 'route', 'get', '8.8.8.8'], capture_output=True,
                               text=True, timeout=5).stdout.split()
    except (OSError, subprocess.SubprocessError):
        return None
    return route[route.index('dev') + 1] if 'dev' in route else None


def push_over_data(wifi_went):
    """Before a deep sleep with Wi-Fi off: Android's connections onto mobile
    data, and microG's push connection open again there. Returns the push
    state for the result (None without Android or mobile data).

    They move whenever this sleep unloaded Wi-Fi (WIFI_WENT): its
    connections died with it. With Wi-Fi already off they stay, unless the
    record says they were last moved elsewhere. The record alone was wrong
    on October 7: an Android restart made new connections over Wi-Fi after
    it said mobile data, nothing moved, and a message waited until Wi-Fi
    came back."""
    pid = android_pid()
    if not pid or uplink() != DATA_IFACE:
        return None
    try:
        moved = read(PUSH_UPLINK) == DATA_IFACE
    except OSError:
        moved = False
    if not wifi_went and moved and push_ports(pid):
        return 'data'
    before = push_ports(pid)
    # Every connection but loopback: the apps' own (Signal, WhatsApp) moved
    # too, and microG's.
    android_ss(pid, '-K', 'state', 'established',
               '( ! dst 127.0.0.0/8 and ! dst [::1] and ! dst [::ffff:127.0.0.1] )')
    PUSH_UPLINK.write_text(DATA_IFACE)
    deadline = time.monotonic() + PUSH_RECONNECT_WAIT
    state = 'reconnecting'
    while time.monotonic() < deadline:
        if push_ports(pid) - before:
            state = 'moved'
            break
        time.sleep(0.5)
    data_settle()
    return state


def data_settle():
    """Wait until mobile data is quiet (the apps' reconnections done, the
    modem's link idle), at most DATA_SETTLE_MAX seconds: a modem still
    sending wakes each sleep at once, and the first unplugged sleep after the
    move (October 7) went through 22 suspend cycles in 13 s and hung."""
    def count():
        try:
            stats = Path('/sys/class/net', DATA_IFACE, 'statistics')
            return int(read(stats / 'rx_packets')) + int(read(stats / 'tx_packets'))
        except (OSError, ValueError):
            return None
    deadline = time.monotonic() + DATA_SETTLE_MAX
    last, since = count(), time.monotonic()
    while time.monotonic() < deadline:
        time.sleep(0.5)
        now = count()
        if now != last:
            last, since = now, time.monotonic()
        elif time.monotonic() - since >= DATA_QUIET:
            return


def push_connected():
    pid = android_pid()
    return bool(pid and push_ports(pid))


def data_packets():
    try:
        return int(read(Path('/sys/class/net', DATA_IFACE, 'statistics/rx_packets')))
    except (OSError, ValueError):
        return None


def bluetooth_in_use():
    """Whether anything uses Bluetooth through sleep: a connected device, or
    an app registered with a file in the session's
    $XDG_RUNTIME_DIR/omarchy-mobile-bluetooth-sleep.d/ (a key that must
    answer while the phone sleeps). Kept on but idle, the chip alone drew
    10 mW in deep sleep (VSYS_WLAN_BT) and the phone 4.4 mA more (October 3)."""
    try:
        if any((session_runtime() / BT_SLEEP_APPS).iterdir()):
            return True
    except OSError:
        pass
    try:
        listed = subprocess.run(['bluetoothctl', 'devices', 'Connected'], capture_output=True,
                                text=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        return True     # unknown: keep what may be a link
    return listed.returncode != 0 or any(line.startswith('Device ') for line in listed.stdout.splitlines())


def bluetooth_stays():
    """Whether Bluetooth stays on through deep sleep: BlueZ has it on, and
    hci_bcm runs its low-power mode (pixel-bt wired the wake lines, and the
    host-wake interrupt is requested once the firmware is loaded), something
    uses it, and the opt-out flag is not set."""
    try:
        if BT_UNLOAD_FLAG.exists() or (BT_PARAMS / 'sleep_ready').read_text().strip() != 'Y':
            return False
        if not any(line.split()[-1:] == ['host_wake']
                   for line in Path('/proc/interrupts').read_text().splitlines()):
            return False
        powered = subprocess.run(BT_POWERED, capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return False
    return powered.returncode == 0 and 'boolean true' in powered.stdout and bluetooth_in_use()


def bluetooth_wake(on):
    """Add or remove the Bluetooth host wake in pixel-sleep's wake EINTs. Set
    before every deep sleep, so a run cut short leaves nothing behind."""
    path = SLEEP_PARAMS / 'eint_wake'
    bits = [bit for bit in read(path).split(',') if bit and int(bit) != BT_HOST_WAKE_EINT]
    path.write_text(','.join(bits + [str(BT_HOST_WAKE_EINT)] * on) + '\n')


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


def sensors(running):
    """Pause or resume the sensor service's sensors; without the service there
    is nothing to pause."""
    method = 'org.omarchy.Mobile.SensorPower.' + ('Resume' if running else 'Pause')
    try:
        subprocess.run(SENSOR_POWER + [method], capture_output=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        pass


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


# The desktop's account, named in this file ("user=NAME"); without it, root's.
# This runs as root (through sudo, which drops the caller's environment), so
# the account's paths come from here, never from the caller.
SESSION = Path('/etc/omarchy-mobile/session')


def session_account():
    name = 'root'
    try:
        for line in SESSION.read_text().splitlines():
            key, _, value = line.partition('=')
            if key.strip() == 'user' and value.strip():
                name = value.strip()
    except OSError:
        pass
    try:
        return pwd.getpwnam(name)
    except KeyError:
        return pwd.getpwnam('root')


def session_runtime():
    return Path('/run/user') / str(session_account().pw_uid)


def message_marker():
    marker = []
    for name in ('messages.db', 'messages.db-wal'):
        try:
            info = (Path(session_account().pw_dir) / MESSAGES / name).stat()
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
    """What woke the phone. A finger outranks the clock's minute and the
    modem: their wakes sleep again at once, and a press that came with one
    was lost until the power key (October 2). Bluetooth traffic does too:
    the shell keeps the phone up a while for it."""
    for reason in ('power-key', 'touch', 'bluetooth', 'alarm', 'modem'):
        if after.get(reason, 0) > before.get(reason, 0):
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
            if 'touch' in name:
                return 'touch'
            if name == 'host_wake':
                return 'bluetooth'
            return name
    return 'unknown'


def scanout():
    """pixel-scanout's frame and power counters, or None without debugfs."""
    try:
        text = SCANOUT_STATS.read_text()
    except OSError:
        return None
    stats = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[1].isdigit():
            stats[fields[0]] = int(fields[1])
    return stats if {'completed_transfers', 'display_powered_off'} <= stats.keys() else None


def seconds_to_minute(now=None):
    """Seconds to a second past the next minute, at least five away."""
    now = time.time() if now is None else now
    seconds = 61 - int(now) % 60
    return seconds + 60 if seconds < 5 else seconds


def write_user_file(path, text):
    """A file in a directory the session's user owns, written as root: no
    link followed, and only a plain file with one link (a hard link to a
    root file would be overwritten through it)."""
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o644)
    except OSError:
        return
    with os.fdopen(fd, 'w') as out:
        info = os.fstat(fd)
        if stat.S_ISREG(info.st_mode) and info.st_nlink == 1:
            out.truncate(0)
            out.write(text)


def minute_tick(path):
    """Have the shell draw the new minute, and wait until the display has
    sent it and powered off again."""
    if not SCANOUT_STATS.exists() and not Path('/sys/kernel/debug/dri').exists():
        subprocess.run(['mount', '-t', 'debugfs', 'none', '/sys/kernel/debug'], capture_output=True, timeout=5)
    before = scanout()
    write_user_file(path, f'{int(time.time())}\n')
    if before is None:
        time.sleep(REDRAW_WAIT)
        return
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        time.sleep(0.05)
        now = scanout()
        if (now is None or now['completed_transfers'] > before['completed_transfers']
                and now['display_powered_off']):
            return


def display(on):
    """The screen through the session's own display script, as its user."""
    account = session_account()
    env = dict(HOME=account.pw_dir, USER=account.pw_name, PATH=os.environ.get('PATH', '/usr/bin'),
               XDG_RUNTIME_DIR=str(session_runtime()))
    command = [f'{account.pw_dir}/.local/bin/omarchy-mobile-display', 'on' if on else 'off']
    if account.pw_uid:
        command = ['setpriv', f'--reuid={account.pw_uid}', f'--regid={account.pw_gid}', '--init-groups', *command]
    subprocess.run(command, env=env, check=True, timeout=10, stdout=subprocess.DEVNULL)


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
                try:
                    count = wake_count()
                    idle_modem()
                    POWER.joinpath('wakeup_count').write_text(count + '\n')
                except OSError:
                    # Wake sources still active (the modem sending), or an
                    # event since the read: a wake, not a refusal.
                    return dict(woke=deep_wake_reason(irqs, irq_counts()), slept=0.0, sleep='deep-early')
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
                # The freezer stops for a wake event that came in during
                # entry (a wake source still settling after the screen went
                # dark, say). Not a refusal: as a refusal, the policy stayed
                # up a minute before it tried again. Its interrupt makes it a
                # wake; without one, try again shortly.
                if read(POWER / 'suspend_stats/last_failed_step') == 'freeze':
                    reason = deep_wake_reason(irqs, irq_counts())
                    if reason != 'unknown' or attempt == DEEP_TRIES - 1:
                        return dict(woke=reason, slept=0.0, sleep='deep-early')
                    time.sleep(FREEZE_RETRY_GAP)
                    continue
                now = int(read(SLEEP_PARAMS / 'refused'))
                if now == refused:
                    step = read(POWER / 'suspend_stats/last_failed_step')
                    device = read(POWER / 'suspend_stats/last_failed_dev')
                    raise RuntimeError(f'Deep sleep refused by {device or "a device"} ({step})') from error
                # pixel-sleep refused: retry only what passes in a moment (the
                # modem's link going idle). Each try is a full suspend and
                # resume, and a display left powered stays so.
                reason = sleep_refusal()
                if attempt == DEEP_TRIES - 1 or 'display' in reason:
                    raise RuntimeError(f'Deep sleep refused by pixel-sleep: {reason or "see dmesg"}') from error
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
    paused = False
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
        bluetooth = None
        push = None
        if deep:
            SUSPEND_ACTIVE.touch()
            deep_watchdog()
            wifi_went = wifi_off()
            bluetooth = bluetooth_stays()
            if not bluetooth:
                bluetooth_off()
            bluetooth_wake(bluetooth)
            push = push_over_data(wifi_went)
        paused = True
        sensors(False)
        os.sync()
        # The caller only asks for minute ticks; the file is the shell's, by a
        # fixed name in the session's runtime directory.
        tick = session_runtime() / 'omarchy-mobile-minute-tick' \
            if deep and os.environ.get('OMARCHY_MOBILE_MINUTE_TICK') else None
        # Wall time: the monotonic clock stops in suspend.
        end = time.time() + seconds
        tick_at = time.time() + seconds_to_minute() if tick else None
        keep_at = time.time() + PUSH_KEEPALIVE if push else None
        wake_at = min(t for t in (end, tick_at, keep_at) if t)
        alarm = Alarm(max(1, round(wake_at - time.time())))
        if deep:
            slept = 0.0
            # Charge (nAh) and time over the sleeps the gauge measured.
            used, measured = 0, 0.0
            quiet = ticks = keeps = early = 0
            marker = message_marker()
            events = modem_events()
            cycles, counted = [], suspend_attempts()
            while True:
                # The minute or the push keepalive: its alarm woke the phone,
                # or came due while a quiet modem wake was looked at (then it
                # fired awake).
                due = False
                if tick_at and time.time() >= tick_at - 1 and end - time.time() >= 5:
                    minute_tick(tick)
                    ticks += 1
                    tick_at = time.time() + seconds_to_minute()
                    due = True
                if keep_at and time.time() >= keep_at - 1 and end - time.time() >= 5:
                    # Android's alarms that came due meanwhile run now.
                    time.sleep(PUSH_KEEPALIVE_HOLD)
                    keeps += 1
                    keep_at = time.time() + PUSH_KEEPALIVE if push_connected() else None
                    due = True
                if due:
                    alarm.restore()
                    wake_at = min(t for t in (end, tick_at, keep_at) if t)
                    alarm = Alarm(max(1, round(wake_at - time.time())))
                packets = data_packets()
                result = deep_sleep()
                now_attempts = suspend_attempts()
                if counted is not None and now_attempts is not None:
                    cycles += [time.monotonic()] * max(0, now_attempts - counted)
                    counted = now_attempts
                cycles = [t for t in cycles if time.monotonic() - t < CYCLE_WINDOW]
                slept += result['slept']
                part = result.pop('used_nah', None)
                if part is not None:
                    used += part
                    measured += result['slept']
                left = end - time.time()
                early = early + 1 if result['slept'] < EARLY_SLEEP else 0
                if early >= EARLY_WAKES:
                    result['early_wakes'] = early
                    break
                if len(cycles) > CYCLE_CAP:
                    result['suspend_cycles'] = len(cycles)
                    break
                if (tick_at or keep_at) and result['woke'] == 'alarm' and left >= 5:
                    continue
                if result['woke'] != 'modem' or left < 10:
                    break
                if modem_brought_something(marker, events):
                    break
                if early:
                    time.sleep(EARLY_PAUSE)
                # A packet on mobile data: a push for an app (microG's
                # connection, or an app's own). The shell keeps the phone up
                # for it to arrive and be shown.
                if packets is not None and (data_packets() or 0) > packets:
                    result['woke'] = 'network'
                    break
                quiet += 1
            result.update(slept=round(slept, 3), quiet_modem_wakes=quiet,
                          bluetooth='kept' if bluetooth else 'unloaded')
            if tick:
                result['minute_ticks'] = ticks
            if push:
                result.update(push=push, push_keepalives=keeps)
            # The average current in deep sleep, each sleep's entry and exit
            # included: the gauge's coulomb counter around every state write.
            if measured >= 60:
                result['deep_ma'] = round(used / 1e6 * 3600 / measured, 1)
            # The always-on clock's minute wakes bring nothing up: the panel
            # shows the clock and the phone sleeps again in a second.
            check_network = os.environ.get('OMARCHY_MOBILE_CHECK_NETWORK', 'wifi')
            check_network = check_network if check_network in ('wifi', 'data') else 'wifi'
            purpose = os.environ.get('OMARCHY_MOBILE_WAKE_PURPOSE', 'check')
            purpose = purpose if purpose in ('check', 'ambient', 'clock') else 'check'
            if result['woke'] == 'power-key' or (result['woke'] == 'alarm' and check_network != 'data'
                                                 and purpose != 'ambient'):
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
        cleanups = [lambda: SUSPEND_ACTIVE.unlink(missing_ok=True)]
        if alarm:
            cleanups.append(alarm.restore)
        if paused:
            cleanups.append(lambda: sensors(True))
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
    if not 5 <= args.wake_after <= 3600:
        parser.error('--wake-after must be 5 to 3600 seconds')
    # Unattended diagnostics on the cable (with pixel_battery input_off, so
    # the battery supplies everything): root creates this file; /run is
    # empty after every boot.
    args.allow_charging = args.allow_charging or ALLOW_CHARGING_FLAG.exists()
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
