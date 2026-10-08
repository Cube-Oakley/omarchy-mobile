#!/usr/bin/env python3
"""Read a system battery through Linux's standard power_supply interface.

  (no arguments)   battery and charger state as JSON
  limit PERCENT    charge only up to PERCENT (50-100, 100 = no limit) and
                   remember it; any supply with charge_control_end_threshold
  restore          apply the remembered limit (at boot, once the charger is up)
  usage [HOURS]    use over the last HOURS (default 24) for Settings → Battery,
                   from the sleep policy's battery history and sleep log
  details [HOURS]  the same window by component (the device's rail meter,
                   when it has one), process, wake source, interrupt and network
"""
import json
import os
from pathlib import Path
import sys
import time

THRESHOLD = 'charge_control_end_threshold'


def limit_path():
    base = os.environ.get('XDG_STATE_HOME')
    return (Path(base) if base else Path.home() / '.local/state') / 'omarchy-mobile/charge-limit.json'


def limit_supplies(root):
    return [supply for supply in sorted(root.glob('*')) if (supply / THRESHOLD).exists()]


def saved_limit():
    try:
        value = json.loads(limit_path().read_text()).get('limit')
    except (OSError, ValueError, AttributeError):
        return 100
    return value if isinstance(value, int) and 50 <= value <= 100 else 100


def apply_limit(percent, root=Path('/sys/class/power_supply')):
    supplies = limit_supplies(root)
    for supply in supplies:
        (supply / THRESHOLD).write_text(f'{percent}\n')
    return [supply.name for supply in supplies]


def set_limit(percent, root=Path('/sys/class/power_supply')):
    if not 50 <= percent <= 100:
        raise ValueError('The charge limit is 50 to 100 percent')
    applied = apply_limit(percent, root)
    path = limit_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({'limit': percent}) + '\n')
    return {'ok': True, 'limit': percent, 'applied': applied}


def read_battery(root=Path('/sys/class/power_supply')):
    for supply in sorted(root.glob('*')):
        try:
            fields = dict(line.split('=', 1) for line in (supply / 'uevent').read_text().splitlines() if '=' in line)
        except OSError:
            continue
        if (fields.get('POWER_SUPPLY_TYPE') != 'Battery'
                or fields.get('POWER_SUPPLY_SCOPE') == 'Device'
                or fields.get('POWER_SUPPLY_PRESENT', '1') != '1'):
            continue
        try:
            capacity = int(fields['POWER_SUPPLY_CAPACITY'])
        except (KeyError, ValueError):
            continue
        if not 0 <= capacity <= 100:
            continue
        status = fields.get('POWER_SUPPLY_STATUS', 'Unknown')
        result = {'available': True, 'name': supply.name, 'capacity': capacity,
                  'status': status, 'charging': status == 'Charging'}
        for key, field, scale in (
                ('current_ma', 'CURRENT_NOW', 1000), ('voltage_v', 'VOLTAGE_NOW', 1000000),
                ('temperature_c', 'TEMP', 10), ('charge_mah', 'CHARGE_NOW', 1000),
                ('full_mah', 'CHARGE_FULL', 1000), ('design_mah', 'CHARGE_FULL_DESIGN', 1000),
                ('energy_wh', 'ENERGY_NOW', 1000000)):
            try: result[key] = int(fields['POWER_SUPPLY_' + field]) / scale
            except (KeyError, ValueError): result[key] = None
        result['health'] = fields.get('POWER_SUPPLY_HEALTH', 'Unknown')
        # The charge limit, where a charger (or the battery) offers one.
        limiters = limit_supplies(root)
        result['limit_supported'] = bool(limiters)
        result['limit'] = saved_limit()
        if limiters:
            try:
                result['limit'] = int((limiters[0] / THRESHOLD).read_text())
            except (OSError, ValueError):
                pass
        result['chargers'] = []
        for other in sorted(root.glob('*')):
            try:
                data = dict(line.split('=', 1) for line in (other / 'uevent').read_text().splitlines() if '=' in line)
                if data.get('POWER_SUPPLY_ONLINE') != '1' or data.get('POWER_SUPPLY_TYPE') == 'Battery': continue
                result['chargers'].append({'name': other.name,
                    'status': data.get('POWER_SUPPLY_STATUS', 'Unknown'),
                    'type': data.get('POWER_SUPPLY_USB_TYPE', data.get('POWER_SUPPLY_TYPE', '')),
                    'input_limit_ma': int(data['POWER_SUPPLY_CURRENT_MAX']) / 1000 if 'POWER_SUPPLY_CURRENT_MAX' in data else None})
            except (OSError, ValueError): continue
        return result
    return {'available': False}


def state_root():
    base = os.environ.get('XDG_STATE_HOME')
    return (Path(base) if base else Path.home() / '.local/state') / 'omarchy-mobile'


def json_lines(path):
    entries = []
    try:
        text = path.read_text()
    except OSError:
        return entries
    for line in text.splitlines():
        try:
            entries.append(json.loads(line))
        except ValueError:
            continue
    return [e for e in entries if isinstance(e, dict)]


SCREENS = ('on', 'ambient', 'off')


def usage(hours=24, now=None, root=None):
    """The battery over the last HOURS: its level, the time and charge spent
    in each screen state (on battery only), deep sleep, and what woke the
    phone. The gauge measures the whole phone, so use is told by state, not
    by component."""
    now = time.time() if now is None else now
    root = root or state_root()
    start = now - hours * 3600
    history = [e for e in json_lines(root / 'battery-history.jsonl') if e.get('time', 0) >= start]
    sleeps = [e for e in json_lines(root / 'sleep.log') if e.get('time', 0) >= start and 'slept' in e]
    states = {screen: {'seconds': 0.0, 'mah': 0.0} for screen in SCREENS}
    for a, b in zip(history, history[1:]):
        span = b['time'] - a['time']
        state = states.get(a.get('screen'))
        # A gap longer than a few hours is the phone off, not a state.
        if state is None or not 0 < span <= 6 * 3600:
            continue
        state['seconds'] += span
        if not a.get('plugged') and not b.get('plugged') and 'charge' in a and 'charge' in b:
            state['mah'] += max(0, a['charge'] - b['charge']) / 1000
    deep = sum(e.get('slept', 0) for e in sleeps)
    measured = [e for e in sleeps if e.get('deep_ma') is not None and e.get('slept')]
    deep_ma = (sum(e['deep_ma'] * e['slept'] for e in measured) / sum(e['slept'] for e in measured)
               if measured else None)
    wakes = {}
    for e in sleeps:
        wakes[e.get('woke', 'unknown')] = wakes.get(e.get('woke', 'unknown'), 0) + 1
    since = None
    last_plugged = max((i for i, e in enumerate(history) if e.get('plugged')), default=-1)
    unplugged = history[last_plugged + 1:]
    if unplugged and 'capacity' in unplugged[0] and 'capacity' in history[-1]:
        first, last = unplugged[0], history[-1]
        elapsed = now - first['time']
        used = max(0, first.get('charge', 0) - last.get('charge', 0)) / 1000
        since = {'seconds': elapsed, 'percent': max(0, first['capacity'] - last['capacity']),
                 'mah': used, 'average_ma': used / (elapsed / 3600) if elapsed > 60 else None}
    return {'ok': True, 'hours': hours, 'start': start, 'now': now,
            'points': [{'time': e['time'], 'capacity': e['capacity'], 'plugged': bool(e.get('plugged'))}
                       for e in history if 'capacity' in e],
            'states': states, 'deep': {'seconds': deep, 'average_ma': deep_ma},
            'wakes': wakes, 'minute_ticks': sum(e.get('minute_ticks', 0) for e in sleeps),
            'quiet_modem_wakes': sum(e.get('quiet_modem_wakes', 0) for e in sleeps),
            'since_unplugged': since}


# The metered rails by component (Pixel's ODPM channels, kernel/odpm). The
# VSYS channels are shunts, exact; the buck rails read 40-60 mW high while
# their block is off, so CPU, GPU and camera figures carry that floor.
COMPONENTS = [
    ('display', 'Display', ('VSYS_DISPLAY',)),
    ('cellular', 'Cellular radio', ('VSYS_MODEM', 'VSYS_RFFE', 'VSYS_MMWAVE')),
    ('wifi', 'Wi-Fi and Bluetooth', ('VSYS_WLAN_BT',)),
    ('cpu_little', 'CPU, little cores', ('S4M_VDD_CPUCL0',)),
    ('cpu_mid', 'CPU, mid cores', ('S3M_VDD_CPUCL1',)),
    ('cpu_big', 'CPU, big cores', ('S2M_VDD_CPUCL2',)),
    ('gpu', 'GPU', ('S2S_VDD_G3D',)),
    ('memory', 'Memory', ('S1M_VDD_MIF', 'S4S_VDD2H_MEM', 'S5S_VDDQ_MEM', 'L15M_VDD_SLC_M')),
    ('camera', 'Camera', ('S1S_VDD_CAM',)),
    ('soc', 'Chip core and I/O', ('S5M_VDD_INT', 'S7M_VDD_INT_M', 'S6M_LLDO1', 'S9M_LLDO3', 'S3S_LLDO1',
                                  'S6S_LLDO2', 'S10S_LLDO3', 'S7S_MLDO', 'SA', 'SD')),
]
OFFSET_RAILS = {'cpu_little', 'cpu_mid', 'cpu_big', 'gpu', 'camera'}


def details(hours=24, now=None, root=None):
    """The window by component (mWh and average mW, also per screen state),
    with what the battery itself gave for comparison, and by process, wake
    source, interrupt and network interface."""
    now = time.time() if now is None else now
    root = root or state_root()
    start = now - hours * 3600
    history = [e for e in json_lines(root / 'battery-history.jsonl') if e.get('time', 0) >= start]
    owner = {rail: key for key, _, rails in COMPONENTS for rail in rails}
    components = {key: {'name': name, 'mwh': 0.0, 'by_screen': dict.fromkeys(SCREENS, 0.0),
                        'floor': key in OFFSET_RAILS} for key, name, _ in COMPONENTS}
    metered = {screen: 0.0 for screen in SCREENS}
    battery_mwh = {screen: 0.0 for screen in SCREENS}
    cpu, wakeups, irqs, net = {}, {}, {}, {}
    radio_seconds = {'wifi': 0.0, 'bluetooth': 0.0}
    span_total = 0.0
    for before, entry in zip(history, history[1:]):
        # A line's figures cover the time since the line before it, spent in
        # the screen state that line recorded.
        screen = before.get('screen') if before.get('screen') in SCREENS else 'off'
        span = entry['time'] - before['time']
        if not 0 < span <= 6 * 3600:
            continue
        span_total += span
        if entry.get('rails'):
            metered[screen] += entry.get('metered', 0)
            for rail, uj in entry['rails'].items():
                key = owner.get(rail)
                if key:
                    mwh = uj / 3.6e6
                    components[key]['mwh'] += mwh
                    components[key]['by_screen'][screen] += mwh
        if not before.get('plugged') and not entry.get('plugged') and 'charge' in before and 'charge' in entry:
            volts = (before.get('voltage', 3.85e6) + entry.get('voltage', 3.85e6)) / 2e6
            battery_mwh[screen] += max(0, before['charge'] - entry['charge']) / 1000 * volts
        for label, seconds in (entry.get('cpu') or {}).items():
            cpu[label] = cpu.get(label, 0) + seconds
        for name, (count, active) in (entry.get('wakeups') or {}).items():
            old = wakeups.get(name, [0, 0])
            wakeups[name] = [old[0] + count, old[1] + active]
        for name, count in (entry.get('irqs') or {}).items():
            irqs[name] = irqs.get(name, 0) + count
        for name, (rx, tx) in (entry.get('net') or {}).items():
            old = net.get(name, [0, 0])
            net[name] = [old[0] + rx, old[1] + tx]
        for radio, on in (before.get('radios') or {}).items():
            if on and radio in radio_seconds:
                radio_seconds[radio] += span
    metered_seconds = sum(metered.values())
    for item in components.values():
        item['mwh'] = round(item['mwh'], 2)
        item['average_mw'] = round(item['mwh'] * 3600 / metered_seconds, 1) if metered_seconds else None
        item['by_screen'] = {k: round(v, 2) for k, v in item['by_screen'].items()}
    ranked = sorted(components.values(), key=lambda item: item['mwh'], reverse=True)
    return {'ok': True, 'hours': hours, 'metered_seconds': round(metered_seconds),
            'covered_seconds': round(span_total), 'components': ranked if metered_seconds else [],
            'metered_mwh': round(sum(c['mwh'] for c in ranked), 1),
            'battery_mwh': {k: round(v, 1) for k, v in battery_mwh.items()},
            'cpu': sorted(([k, round(v, 1)] for k, v in cpu.items()), key=lambda x: -x[1])[:15],
            'wakeups': sorted(([k, v[0], round(v[1] / 1000, 1)] for k, v in wakeups.items()),
                              key=lambda x: (-x[1], -x[2]))[:15],
            'irqs': sorted(([k, v] for k, v in irqs.items()), key=lambda x: -x[1])[:15],
            'net': {k: v for k, v in net.items()},
            'radio_seconds': {k: round(v) for k, v in radio_seconds.items()}}


def main(args):
    try:
        if not args:
            print(json.dumps(read_battery()))
        elif args[0] == 'limit' and len(args) == 2:
            print(json.dumps(set_limit(int(args[1]))))
        elif args[0] == 'usage' and len(args) in (1, 2):
            hours = int(args[1]) if len(args) == 2 else 24
            if not 1 <= hours <= 24 * 14:
                raise ValueError('Usage covers 1 hour to 14 days')
            print(json.dumps(usage(hours)))
        elif args[0] == 'details' and len(args) in (1, 2):
            hours = int(args[1]) if len(args) == 2 else 24
            if not 1 <= hours <= 24 * 14:
                raise ValueError('Details cover 1 hour to 14 days')
            print(json.dumps(details(hours)))
        elif args[0] == 'restore' and len(args) == 1:
            percent = saved_limit()
            print(json.dumps({'ok': True, 'limit': percent, 'applied': apply_limit(percent)}))
        else:
            print(__doc__.strip(), file=sys.stderr)
            return 2
    except (OSError, ValueError) as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
