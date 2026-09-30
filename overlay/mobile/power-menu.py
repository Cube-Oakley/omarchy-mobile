#!/usr/bin/env python3
"""The power menu's device-specific, orderly reboot adapter."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('status', 'restart', 'bootloader'))
    args = parser.parse_args()
    config = Path(os.environ.get('XDG_CONFIG_HOME', str(Path.home() / '.config')))
    adapter = config / 'omarchy-mobile/reboot'
    if not os.access(adapter, os.X_OK):
        if args.action == 'status':
            print(json.dumps(dict(restart=False, bootloader=False)))
            return
        raise SystemExit('Restart is not configured for this device')
    if args.action == 'status':
        result = subprocess.run([str(adapter), '--check'], capture_output=True, text=True, timeout=5)
        if result.returncode:
            raise SystemExit('Restart is unavailable')
        print(json.dumps(json.loads(result.stdout)))
        return
    # The adapter handles service shutdown and refuses unsafe restarts.
    os.execv(str(adapter), [str(adapter), args.action])


if __name__ == '__main__':
    main()
