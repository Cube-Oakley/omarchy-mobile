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
"""
import json
import os
from pathlib import Path
import subprocess
import sys


PERMISSIONS = {
    'files.home': {
        'title': 'Home folder',
        'detail': 'Read and change files in the home folder, including Documents, Downloads, and Pictures.',
    },
    'camera': {
        'title': 'Camera',
        'detail': 'Use the cameras, and save photos to Pictures.',
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


def has_grant(app_id, permission):
    return read_grants(app_id).get(permission, False)


def qml_path(app_id):
    return config_root() / 'quickshell/omarchy-mobile-apps' / app_id / 'shell.qml'


def stop_previous(qml):
    needle = str(qml)
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            command = (entry / 'cmdline').read_bytes().replace(b'\0', b' ').decode()
        except OSError:
            continue
        if 'quickshell' in command and needle in command:
            os.kill(int(entry.name), 15)


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
        if action == 'launch' and len(argv) in (3, 4):
            launch(argv[2], argv[3] if len(argv) == 4 else '')
        print('Usage: omarchy-mobile-app status|grant|revoke|apps|launch ...', file=sys.stderr)
        return 2
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
