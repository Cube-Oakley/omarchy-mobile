#!/usr/bin/env python3
"""NFC on or off. Off holds the NFC controller and its secure element in
hard power-down (the Pixel's pixel-gpio nfc_ven, which the boot hands to the
session's group). The choice is kept in $XDG_STATE_HOME/omarchy-mobile/nfc.json
and applied at boot; with none, NFC stays off.

  status     JSON: supported, on, saved
  on | off   switch now and remember the choice
"""
import json
import os
from pathlib import Path
import sys

PARAM = Path('/sys/module/pixel_gpio/parameters/nfc_ven')


def state_path():
    base = os.environ.get('XDG_STATE_HOME')
    return (Path(base) if base else Path.home() / '.local/state') / 'omarchy-mobile/nfc.json'


def saved():
    try:
        return json.loads(state_path().read_text()).get('on') is True
    except (OSError, ValueError, AttributeError):
        return False


def status(param=PARAM):
    # The line cannot be read while its power domain is down, nor without
    # the provider: then NFC is not switchable here.
    try:
        on = param.read_text().strip() == '1'
    except OSError:
        return {'supported': False, 'on': False, 'saved': saved()}
    return {'supported': True, 'on': on, 'saved': saved()}


def switch(on, param=PARAM):
    path = state_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({'on': on}) + '\n')
    try:
        param.write_text('1\n' if on else '0\n')
    except OSError as error:
        return dict(status(param), error=str(error))
    return status(param)


def main(args):
    if args in ([], ['status']):
        print(json.dumps(status()))
        return 0
    if args in (['on'], ['off']):
        result = switch(args[0] == 'on')
        print(json.dumps(result))
        return 1 if 'error' in result else 0
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
