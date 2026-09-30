#!/usr/bin/env python3
"""Read a system battery through Linux's standard power_supply interface.

  (no arguments)   battery and charger state as JSON
  limit PERCENT    charge only up to PERCENT (50-100, 100 = no limit) and
                   remember it; any supply with charge_control_end_threshold
  restore          apply the remembered limit (at boot, once the charger is up)
"""
import json
import os
from pathlib import Path
import sys

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


def main(args):
    try:
        if not args:
            print(json.dumps(read_battery()))
        elif args[0] == 'limit' and len(args) == 2:
            print(json.dumps(set_limit(int(args[1]))))
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
