#!/usr/bin/env python3
"""Small JSON preferences for the portable mobile shell and settings app."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

CONFIG = Path(os.environ.get('XDG_CONFIG_HOME', str(Path.home() / '.config'))) / 'omarchy-mobile/prefs.json'
ALLOWED = {'dnd', 'fontFamily', 'corners', 'alwaysOn', 'sleep', 'sleepCheck', 'screenTimeout'}
# Minutes between background wakes while asleep (0: only calls, texts and the
# power key wake it), and seconds of inactivity before the screen goes off
# (0: never).
SLEEP_CHECKS = (0, 5, 15, 30, 60)
SCREEN_TIMEOUTS = (0, 15, 30, 60, 120, 300, 600)
DEFAULT_FONT = 'JetBrainsMono Nerd Font'
FONT_NAME = re.compile(r'^[A-Za-z0-9][A-Za-z0-9 +._-]{0,78}$')


def read(path=CONFIG):
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError):
        data = {}
    font = data.get('fontFamily') if isinstance(data.get('fontFamily'), str) else DEFAULT_FONT
    if not FONT_NAME.fullmatch(font):
        font = DEFAULT_FONT
    corners = data.get('corners') if data.get('corners') in ('round', 'square') else ''
    check = data.get('sleepCheck')
    timeout = data.get('screenTimeout')
    return {'dnd': bool(data.get('dnd')), 'fontFamily': font, 'corners': corners,
            'alwaysOn': data.get('alwaysOn') is True,
            'sleep': data.get('sleep') is not False,
            'sleepCheck': check if check in SLEEP_CHECKS and type(check) is int else 15,
            'screenTimeout': timeout if timeout in SCREEN_TIMEOUTS and type(timeout) is int else 60}


def save(data, path=CONFIG):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix('.tmp')
    stored = {'dnd': bool(data.get('dnd')), 'fontFamily': data.get('fontFamily') or DEFAULT_FONT,
              'alwaysOn': data.get('alwaysOn') is True, 'sleep': data.get('sleep') is not False,
              'sleepCheck': data.get('sleepCheck', 15), 'screenTimeout': data.get('screenTimeout', 60)}
    if data.get('corners') in ('round', 'square'):
        stored['corners'] = data['corners']
    temp.write_text(json.dumps(stored))
    temp.replace(path)


def fonts():
    try:
        text = subprocess.check_output(['fc-list', '-f', '%{family[0]}\\n'], text=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        return [DEFAULT_FONT]
    names = []
    seen = set()
    for line in text.splitlines():
        family = line.split(',', 1)[0].strip()
        if FONT_NAME.fullmatch(family) and family not in seen:
            seen.add(family)
            names.append(family)
    if DEFAULT_FONT not in seen:
        names.insert(0, DEFAULT_FONT)
    return sorted(names, key=str.lower)


def main(argv=None, stdin=None, path=CONFIG):
    argv = sys.argv[1:] if argv is None else argv
    prefs = read(path)
    if argv and argv[0] == 'fonts':
        return {'fonts': fonts(), 'fontFamily': prefs['fontFamily']}
    if argv and argv[0] == 'set':
        incoming = json.load(sys.stdin if stdin is None else stdin)
        if not isinstance(incoming, dict) or set(incoming) - ALLOWED:
            raise ValueError('Unknown preference')
        if 'dnd' in incoming:
            prefs['dnd'] = bool(incoming['dnd'])
        if 'fontFamily' in incoming:
            name = incoming['fontFamily']
            if not isinstance(name, str) or not FONT_NAME.fullmatch(name):
                raise ValueError('Font name is not valid')
            prefs['fontFamily'] = name
        if 'alwaysOn' in incoming:
            if not isinstance(incoming['alwaysOn'], bool):
                raise ValueError('alwaysOn must be true or false')
            prefs['alwaysOn'] = incoming['alwaysOn']
        if 'sleep' in incoming:
            if not isinstance(incoming['sleep'], bool):
                raise ValueError('sleep must be true or false')
            prefs['sleep'] = incoming['sleep']
        for key, choices in (('sleepCheck', SLEEP_CHECKS), ('screenTimeout', SCREEN_TIMEOUTS)):
            if key in incoming:
                if type(incoming[key]) is not int or incoming[key] not in choices:
                    raise ValueError(f'{key} must be one of {choices}')
                prefs[key] = incoming[key]
        if 'corners' in incoming:
            choice = incoming['corners']
            if choice not in ('round', 'square'):
                raise ValueError('Corners must be round or square')
            prefs['corners'] = choice
        save(prefs, path)
    return prefs


if __name__ == '__main__':
    try:
        result = main()
    except (OSError, ValueError, json.JSONDecodeError):
        result = {'error': 'Preference update failed'}
    print(json.dumps(result))
