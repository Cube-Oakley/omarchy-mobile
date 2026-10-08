#!/usr/bin/env python3
"""Omarchy Mobile app launcher.

A new app is a directory with a manifest and a Quickshell window. The manifest
names only permissions from PERMISSIONS. Grants live in the user's state
directory. This is the contract helpers enforce. It is not a process sandbox:
the session user can still run other programs. Helpers such as the file browser
must refuse work the grant does not allow.

Launching replaces an existing window for that app, the same way Settings does,
and does not touch the main shell. An optional argument (a conversation, a
number to dial) reaches the app as OMARCHY_MOBILE_APP_ARG.

Web apps (sites the browser added to the home screen) are desktop entries
omarchy-mobile-webapp-SITE.desktop; `webapps` lists them and `webapp-remove
ID` removes one with its icon and its own browser profile (cookies, storage,
cache).
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


PERMISSIONS = {
    'files.home': {
        'title': 'Home folder',
        'detail': 'Read and change files in the home folder, including Documents, Downloads, and Pictures.',
    },
    'camera': {
        'title': 'Camera',
        'detail': 'Use the cameras, and save photos and videos to Pictures.',
    },
    'microphone': {
        'title': 'Microphone',
        'detail': 'Record sound, such as the sound of videos.',
    },
    'phone': {
        'title': 'Phone calls',
        'detail': 'Place and answer calls, and keep a list of recent calls.',
    },
    'messages': {
        'title': 'Text messages',
        'detail': 'Read, send and delete your text messages.',
    },
    'contacts': {
        'title': 'Contacts',
        'detail': 'Read and change your contacts.',
    },
    'calendar': {
        'title': 'Calendar',
        'detail': 'Read and change your calendars and their events.',
    },
}


def data_root():
    base = os.environ.get('XDG_DATA_HOME')
    return Path(base) if base else Path.home() / '.local/share'


def state_root():
    base = os.environ.get('XDG_STATE_HOME')
    return Path(base) if base else Path.home() / '.local/state'


def config_root():
    base = os.environ.get('XDG_CONFIG_HOME')
    return Path(base) if base else Path.home() / '.config'


def app_dir(app_id):
    if not app_id or '/' in app_id or app_id.startswith('.'):
        raise ValueError('Invalid app id')
    return data_root() / 'omarchy-mobile/apps' / app_id


def load_manifest(app_id):
    path = app_dir(app_id) / 'manifest.json'
    manifest = json.loads(path.read_text())
    if manifest.get('id') != app_id:
        raise ValueError('Manifest id does not match the app')
    unknown = [name for name in manifest.get('permissions', []) if name not in PERMISSIONS]
    if unknown:
        raise ValueError('Unknown permission: ' + ', '.join(unknown))
    return manifest


def grant_path(app_id):
    return state_root() / 'omarchy-mobile/grants' / f'{app_id}.json'


def read_grants(app_id):
    path = grant_path(app_id)
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError):
        data = {}
    if not isinstance(data, dict):
        return {}
    return {key: True for key, value in data.items() if value is True and key in PERMISSIONS}


def status(app_id):
    manifest = load_manifest(app_id)
    grants = read_grants(app_id)
    return {
        'ok': True,
        'id': app_id,
        'name': manifest.get('name') or app_id,
        'permissions': [
            {
                'id': name,
                'title': PERMISSIONS[name]['title'],
                'detail': PERMISSIONS[name]['detail'],
                'granted': grants.get(name, False),
            }
            for name in manifest.get('permissions', [])
        ],
    }


def grant(app_id, permission):
    manifest = load_manifest(app_id)
    if permission not in manifest.get('permissions', []):
        raise ValueError('This app does not request that permission')
    if permission not in PERMISSIONS:
        raise ValueError('Unknown permission')
    current = read_grants(app_id)
    current[permission] = True
    path = grant_path(app_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(current) + '\n')
    return status(app_id)


def revoke(app_id, permission):
    load_manifest(app_id)
    if permission not in PERMISSIONS:
        raise ValueError('Unknown permission')
    current = read_grants(app_id)
    current.pop(permission, None)
    path = grant_path(app_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(current) + '\n')
    return status(app_id)


def installed():
    root = data_root() / 'omarchy-mobile/apps'
    found = []
    if not root.is_dir():
        return {'ok': True, 'apps': found}
    for path in sorted(root.glob('*/manifest.json')):
        try:
            manifest = json.loads(path.read_text())
        except (OSError, ValueError):
            continue
        app_id = manifest.get('id') or path.parent.name
        if app_id != path.parent.name:
            continue
        try:
            found.append(status(app_id))
        except (OSError, ValueError):
            continue
    return {'ok': True, 'apps': found}


WEBAPP_ID = re.compile(r'^omarchy-mobile-webapp-([a-z0-9]+(?:-[a-z0-9]+)*)$')


def cache_root():
    base = os.environ.get('XDG_CACHE_HOME')
    return Path(base) if base else Path.home() / '.cache'


def desktop_values(path):
    values = {}
    for line in path.read_text().splitlines():
        if line.startswith('[') and values:
            break
        key, sep, value = line.partition('=')
        if sep:
            values.setdefault(key.strip(), value.strip())
    return values


def webapps():
    found = []
    for path in sorted((data_root() / 'applications').glob('omarchy-mobile-webapp-*.desktop')):
        if not WEBAPP_ID.match(path.stem):
            continue
        try:
            values = desktop_values(path)
        except (OSError, UnicodeDecodeError):
            continue
        found.append({'id': path.stem, 'name': values.get('Name', path.stem),
                      'url': values.get('X-Omarchy-Mobile-WebApp', '')})
    return {'ok': True, 'webapps': found}


def remove_webapp(webapp_id):
    match = WEBAPP_ID.match(webapp_id or '')
    if not match:
        raise ValueError('Invalid web app id')
    slug = match.group(1)
    entry = data_root() / 'applications' / (webapp_id + '.desktop')
    if not entry.is_file():
        raise ValueError('No such web app')
    entry.unlink()
    (data_root() / 'omarchy-mobile/webapps' / (slug + '.png')).unlink(missing_ok=True)
    for root in (data_root(), cache_root()):
        profile = root / 'omarchy-mobile-browser/QtWebEngine' / ('webapp-' + slug)
        if profile.is_dir() and not profile.is_symlink():
            shutil.rmtree(profile)
    return webapps()


# Android apps (Waydroid). Waydroid writes waydroid.PACKAGE.desktop for each
# Android app with a launcher activity, and keeps the keys it does not set
# when it updates one. An Android app is in the app drawer only once chosen:
# the shell lists a waydroid.* entry only with this category.
ANDROID_ID = re.compile(r'^waydroid\.([A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)+)$')
ANDROID_SHOWN = 'X-Omarchy-Mobile-Shown'


def android_apps():
    found = []
    for path in (data_root() / 'applications').glob('waydroid.*.desktop'):
        match = ANDROID_ID.match(path.stem)
        if not match:
            continue
        try:
            values = desktop_values(path)
        except (OSError, UnicodeDecodeError):
            continue
        categories = values.get('Categories', '').split(';')
        if 'X-WayDroid-App' not in categories:
            continue
        found.append({'package': match.group(1), 'id': path.stem,
                      'name': values.get('Name', match.group(1)),
                      'icon': values.get('Icon', ''),
                      'shown': ANDROID_SHOWN in categories,
                      # Waydroid hides Android's own apps from launchers.
                      'builtin': values.get('NoDisplay', '').lower() == 'true'})
    found.sort(key=lambda app: (app['builtin'], app['name'].lower()))
    return {'ok': True, 'android': found}


def android_show(package, shown):
    match = ANDROID_ID.match('waydroid.' + (package or ''))
    if not match:
        raise ValueError('Invalid Android package')
    path = data_root() / 'applications' / f'waydroid.{package}.desktop'
    if not path.is_file():
        raise ValueError('No such Android app')
    lines = path.read_text().splitlines()
    group, done = '', False
    for i, line in enumerate(lines):
        if line.startswith('['):
            if group == '[Desktop Entry]' and not done:
                break
            group = line.strip()
            continue
        if group == '[Desktop Entry]' and line.startswith('Categories='):
            values = [c for c in line.partition('=')[2].split(';') if c and c != ANDROID_SHOWN]
            lines[i] = 'Categories=' + ';'.join(values + ([ANDROID_SHOWN] if shown else [])) + ';'
            done = True
    if not done and shown:
        lines.insert(lines.index('[Desktop Entry]') + 1, f'Categories={ANDROID_SHOWN};')
    temporary = path.with_name(path.name + '.new')
    temporary.write_text('\n'.join(lines) + '\n')
    temporary.replace(path)
    return android_apps()


def has_grant(app_id, permission):
    return read_grants(app_id).get(permission, False)


def qml_path(app_id):
    return config_root() / 'quickshell/omarchy-mobile-apps' / app_id / 'shell.qml'


def stop_previous(qml):
    needle = str(qml)
    stopped = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            command = (entry / 'cmdline').read_bytes().replace(b'\0', b' ').decode()
        except OSError:
            continue
        if 'quickshell' in command and needle in command:
            os.kill(int(entry.name), 15)
            stopped.append(entry)
    # The new instance starts with -n (no duplicate): while the old one is
    # still closing (the camera releasing its sensor) it would exit at once
    # and nothing would open. Wait for the old ones, four seconds at most.
    deadline = time.monotonic() + 4
    while stopped and time.monotonic() < deadline:
        stopped = [entry for entry in stopped if entry.exists()]
        if stopped:
            time.sleep(0.05)


def launch(app_id, argument=''):
    load_manifest(app_id)
    if len(argument) > 200 or not argument.isprintable():
        raise ValueError('Invalid app argument')
    qml = qml_path(app_id)
    if not qml.is_file():
        raise FileNotFoundError(qml)
    runtime = os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}'
    os.environ['XDG_RUNTIME_DIR'] = runtime
    os.environ.setdefault('QT_IM_MODULE', 'none')
    imports = str(data_root() / 'omarchy-mobile/qml')
    previous = os.environ.get('QML_IMPORT_PATH', '')
    os.environ['QML_IMPORT_PATH'] = imports + (':' + previous if previous else '')
    os.environ['QML2_IMPORT_PATH'] = os.environ['QML_IMPORT_PATH']
    os.environ['OMARCHY_MOBILE_APP'] = app_id
    os.environ['OMARCHY_MOBILE_APP_ARG'] = argument
    if not os.environ.get('WAYLAND_DISPLAY'):
        raise RuntimeError('Launch from a Wayland session')
    if not os.environ.get('HYPRLAND_INSTANCE_SIGNATURE'):
        for socket in Path(runtime).glob('hypr/*/.socket.sock'):
            os.environ['HYPRLAND_INSTANCE_SIGNATURE'] = socket.parent.name
            break
    stop_previous(qml)
    os.execvp('quickshell', ['quickshell', '-n', '-d', '-p', str(qml)])


def main(argv):
    action = argv[1] if len(argv) > 1 else ''
    try:
        if action == 'status' and len(argv) == 3:
            print(json.dumps(status(argv[2])))
            return 0
        if action == 'grant' and len(argv) == 4:
            print(json.dumps(grant(argv[2], argv[3])))
            return 0
        if action == 'revoke' and len(argv) == 4:
            print(json.dumps(revoke(argv[2], argv[3])))
            return 0
        if action == 'apps' and len(argv) == 2:
            print(json.dumps(installed()))
            return 0
        if action == 'webapps' and len(argv) == 2:
            print(json.dumps(webapps()))
            return 0
        if action == 'webapp-remove' and len(argv) == 3:
            print(json.dumps(remove_webapp(argv[2])))
            return 0
        if action == 'android' and len(argv) == 2:
            print(json.dumps(android_apps()))
            return 0
        if action in ('android-show', 'android-hide') and len(argv) == 3:
            print(json.dumps(android_show(argv[2], action == 'android-show')))
            return 0
        if action == 'launch' and len(argv) in (3, 4):
            launch(argv[2], argv[3] if len(argv) == 4 else '')
        print('Usage: omarchy-mobile-app status|grant|revoke|apps|webapps|webapp-remove|'
              'android|android-show|android-hide|launch ...', file=sys.stderr)
        return 2
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
