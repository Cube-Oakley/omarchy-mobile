#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Package installs and upgrades on the Pixel without breaking the phone.

    pixel-update plan                 what a full upgrade would change (read only)
    pixel-update run [--yes]          snapshot, full upgrade, checks, rollback script
    pixel-update install PKG...       full upgrade first, then install PKG...
    pixel-update check                the post-upgrade checks alone

Rules it enforces (docs/updates-20261001.md):
- Only full upgrades (pacman -Syu). Installing never syncs without upgrading,
  so the system is never left half on an old database (a partial upgrade).
- Pinned packages (PINNED) are held with --ignore and named in the plan: an
  upgrade of the compositor or the shell is a deliberate, separate step.
- Downloads only on Wi-Fi or the USB link, never mobile data (the user's plan
  has 2 GB a month), unless --allow-mobile.
- Before a run: a snapshot of what pacman cannot restore (the custom Mesa in
  /opt/pixel-mesa, libcamera in /usr/local, the camera's QML plugin, /etc and
  the package list) under /var/lib/omarchy-mobile/updates/<time>/, and a
  rollback.sh that reinstalls every changed package's previous version from
  pacman's cache and puts the snapshot back. The cache is never cleaned here.
- While pacman runs, the phone stays awake: a file in the shell's sleep
  inhibit directory holds off its sleep policy (overlay/mobile/sleep.py).
- After a run: every custom build is checked for missing libraries, and the
  GPU must still render in hardware (pixel-gpu-render-test rejects llvmpipe).
  Watched packages that changed say what to rebuild.

The package server is the system's mirrorlist unless --server is given (for
example the official http://mirror.archlinuxarm.org/$arch/$repo); either way
signatures are checked as pacman.conf says.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

UPDATES = Path('/var/lib/omarchy-mobile/updates')
# Held unless the user upgrades them on purpose: the mobile Hyprland config
# and the shell's QML follow these versions.
PINNED = ('hyprland', 'quickshell')
# A change to one of these needs the named follow-up.
WATCHED = {
    'qt6-base': 'rebuild Omarchy Camera\'s QML plugin (/root/src-camera)',
    'qt6-declarative': 'rebuild Omarchy Camera\'s QML plugin (/root/src-camera)',
    'spirv-tools': 'check /opt/pixel-mesa (unversioned libSPIRV-Tools.so); rebuild it if the GPU test fails',
    'libdrm': 'check /opt/pixel-mesa and the GPU test',
    'expat': 'check /opt/pixel-mesa',
    'zstd': 'check /opt/pixel-mesa',
    'libglvnd': 'check /opt/pixel-mesa and libcamera (EGL/GLES)',
    'wayland': 'check /opt/pixel-mesa and the session',
    'mesa': 'the session runs /opt/pixel-mesa (Mesa 26.2.3 + the G710 patch); keep their versions in step',
    'gnutls': 'check libcamera in /usr/local',
    'systemd-libs': 'check libcamera (libudev)',
    'pipewire': 'check audio (AoC routing) after the upgrade',
    'wireplumber': 'check audio (AoC routing) after the upgrade',
    'networkmanager': 'check Wi-Fi and the USB link profile',
    'python': 'check the shell helpers and the modem service',
    'glibc': 'reboot after the upgrade',
}
# Hyprland's own libraries: with hyprland held, a new ABI here makes pacman
# refuse the whole upgrade (nothing changes); then upgrade Hyprland on purpose.
for _lib in ('aquamarine', 'hyprutils', 'hyprlang', 'hyprgraphics', 'hyprcursor', 'hyprtoolkit'):
    WATCHED[_lib] = 'Hyprland library: if pacman refuses because hyprland is held, upgrade Hyprland deliberately'
# Custom builds pacman does not own: (name, glob roots).
CUSTOM = (
    ('custom Mesa', ['/opt/pixel-mesa/lib']),
    ('libcamera', ['/usr/local/lib']),
    ('camera QML plugin', ['/root/.local/share/omarchy-mobile/qml']),
)
GPU_TEST = Path('/root/pixel-gpu-render-test')
MESA_ENV = {'LD_LIBRARY_PATH': '/opt/pixel-mesa/lib', 'GBM_BACKENDS_PATH': '/opt/pixel-mesa/lib/gbm',
            '__EGL_VENDOR_LIBRARY_FILENAMES': '/opt/pixel-mesa/share/glvnd/egl_vendor.d/50_mesa.json'}


class AwakeHold:
    """Holds the shell's sleep policy off while pacman works: sleep.py
    sleeps only with its inhibit directory empty. Every session runtime
    directory that has the policy's lock gets the file."""

    def __enter__(self):
        self.paths = []
        for lock in Path('/run/user').glob('*/omarchy-mobile-sleep.lock'):
            folder = lock.parent / 'omarchy-mobile-sleep-inhibit'
            folder.mkdir(exist_ok=True)
            path = folder / 'pixel-update'
            path.touch()
            self.paths.append(path)
        return self

    def __exit__(self, *exc):
        for path in self.paths:
            path.unlink(missing_ok=True)


def run(cmd, check=True, **kw):
    return subprocess.run(cmd, check=check, text=True, **kw)


def output(cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def pacman_config(server):
    """pacman.conf, with every repository pointed at `server` if given."""
    if not server:
        return '/etc/pacman.conf'
    text = Path('/etc/pacman.conf').read_text()
    text = re.sub(r'(?m)^Include\s*=\s*/etc/pacman\.d/mirrorlist\s*$', f'Server = {server}', text)
    f = tempfile.NamedTemporaryFile('w', prefix='pixel-update-', suffix='.conf', delete=False)
    f.write(text)
    f.close()
    return f.name


def network_ok(allow_mobile):
    """Wi-Fi or the USB link up; mobile data alone is refused."""
    active = output(['nmcli', '-t', '-f', 'TYPE,STATE', 'connection', 'show', '--active'])
    kinds = {line.split(':')[0] for line in active.splitlines() if line.endswith(':activated')}
    if kinds & {'802-11-wireless', '802-3-ethernet'}:
        return True, ', '.join(sorted(kinds))
    if allow_mobile and 'gsm' in kinds:
        return True, 'mobile data (allowed)'
    return False, ', '.join(sorted(kinds)) or 'no connection'


def preflight(allow_mobile):
    problems = []
    ok, net = network_ok(allow_mobile)
    if not ok:
        problems.append(f'network: {net} (Wi-Fi or USB needed)')
    cap = int(Path('/sys/class/power_supply/battery/capacity').read_text())
    online = any(Path(f'/sys/class/power_supply/{supply}/online').read_text().strip() == '1'
                 for supply in ('usb', 'wireless')
                 if Path(f'/sys/class/power_supply/{supply}/online').exists())
    if cap < 40 and not online:
        problems.append(f'battery {cap}% and not charging (40% needed)')
    free = shutil.disk_usage('/').free // 2**30
    if free < 5:
        problems.append(f'{free} GiB free (5 needed)')
    return problems, {'network': net, 'battery': cap, 'charging': online, 'free_gib': free}


def pending(config):
    """Upgrades a full sync would bring, from a throwaway copy of the sync
    databases (checkupdates' method): the real ones are not touched."""
    with tempfile.TemporaryDirectory(prefix='pixel-update-db.') as db:
        os.chmod(db, 0o755)
        os.symlink('/var/lib/pacman/local', f'{db}/local')
        sync = subprocess.run(['pacman', '--config', config, '-Sy', '--dbpath', db, '--logfile', '/dev/null'],
                              capture_output=True, text=True)
        if sync.returncode:
            raise SystemExit('package databases could not be synced:\n' + sync.stderr.strip())
        lines = output(['pacman', '--config', config, '-Qu', '--dbpath', db]).splitlines()
    upgrades = []
    for line in lines:
        m = re.match(r'(\S+) (\S+) -> (\S+)', line)
        if m:
            upgrades.append({'name': m[1], 'from': m[2], 'to': m[3]})
    return upgrades


def plan(args):
    problems, state = preflight(args.allow_mobile)
    config = pacman_config(args.server)
    upgrades = pending(config)
    held = [u for u in upgrades if u['name'] in PINNED]
    watched = [dict(u, follow=WATCHED[u['name']]) for u in upgrades if u['name'] in WATCHED]
    report = {'state': state, 'problems': problems, 'upgrades': len(upgrades),
              'held': held, 'watched': watched}
    if args.json:
        print(json.dumps(report, indent=1))
    else:
        print(f"{len(upgrades)} upgrades pending; {state['network']}, battery {state['battery']}%, "
              f"{state['free_gib']} GiB free")
        for p in problems:
            print('  not ready:', p)
        for u in held:
            print(f"  held (pinned): {u['name']} {u['from']} -> {u['to']}")
        for u in watched:
            print(f"  watched: {u['name']} {u['from']} -> {u['to']}: {u['follow']}")
    return upgrades, problems, config


def snapshot(upgrades):
    when = time.strftime('%Y%m%d-%H%M%S')
    snap = UPDATES / when
    snap.mkdir(parents=True)
    (snap / 'packages.txt').write_text(output(['pacman', '-Q']))
    run(['tar', '-C', '/', '-czf', str(snap / 'etc.tar.gz'), 'etc'])
    for name, roots in (('pixel-mesa', ['opt/pixel-mesa']), ('local-lib', ['usr/local/lib']),
                        ('camera-qml', ['root/.local/share/omarchy-mobile/qml'])):
        present = [r for r in roots if Path('/', r).exists()]
        if present:
            run(['tar', '-C', '/', '-czf', str(snap / f'{name}.tar.gz'), *present])
    cache = Path('/var/cache/pacman/pkg')
    lines = ['#!/bin/sh', '# Put back the versions from before this update.', 'set -e']
    olds = []
    for u in upgrades:
        found = sorted(cache.glob(f"{u['name']}-{u['from']}-*.pkg.tar.*"))
        found = [f for f in found if not f.name.endswith('.sig')]
        if found:
            olds.append(str(found[0]))
        else:
            lines.append(f"# {u['name']} {u['from']}: not in the cache, cannot be put back")
    if olds:
        lines.append('pacman -U --noconfirm ' + ' '.join(olds))
    for name in ('pixel-mesa', 'local-lib', 'camera-qml'):
        if (snap / f'{name}.tar.gz').exists():
            lines.append(f'tar -C / -xzf {snap}/{name}.tar.gz')
    (snap / 'rollback.sh').write_text('\n'.join(lines) + '\n')
    (snap / 'rollback.sh').chmod(0o755)
    (snap / 'plan.json').write_text(json.dumps(upgrades, indent=1))
    return snap


def missing_libraries():
    """Custom builds with a library ldd cannot find."""
    broken = []
    for name, roots in CUSTOM:
        for root in roots:
            for f in Path(root).rglob('*.so*') if Path(root).exists() else []:
                if f.is_symlink() or not f.is_file():
                    continue
                out = subprocess.run(['ldd', str(f)], capture_output=True, text=True, env=dict(os.environ, **MESA_ENV)).stdout
                gone = [line.split()[0] for line in out.splitlines() if 'not found' in line]
                if gone:
                    broken.append(f'{name}: {f} needs {", ".join(gone)}')
    for exe in ('/usr/bin/Hyprland', '/usr/bin/quickshell'):
        out = subprocess.run(['ldd', exe], capture_output=True, text=True, env=dict(os.environ, **MESA_ENV)).stdout
        gone = [line.split()[0] for line in out.splitlines() if 'not found' in line]
        if gone:
            broken.append(f'{exe} needs {", ".join(gone)}')
    return broken


def gpu_renders():
    if not GPU_TEST.exists():
        return None
    r = subprocess.run([str(GPU_TEST)], capture_output=True, text=True, timeout=60, env=dict(os.environ, **MESA_ENV))
    return r.returncode == 0


def check(args):
    broken = missing_libraries()
    gpu = gpu_renders()
    for b in broken:
        print('  missing:', b)
    print('  GPU:', 'renders in hardware' if gpu else 'NOT rendering in hardware' if gpu is False else 'no test')
    return not broken and gpu is not False


def upgrade(args):
    upgrades, problems, config = plan(args)
    if problems:
        raise SystemExit('not upgrading: ' + '; '.join(problems))
    if not upgrades:
        print('nothing to upgrade')
        return
    if not args.yes:
        raise SystemExit('re-run with --yes to upgrade (pacman -Syu, pinned packages held)')
    snap = snapshot(upgrades)
    print('snapshot and rollback script:', snap)
    cmd = ['pacman', '--config', config, '-Syu', '--noconfirm']
    for name in PINNED:
        cmd += ['--ignore', name]
    with AwakeHold():
        ok = run(cmd, check=False).returncode == 0
        healthy = check(args)
    (snap / 'result.txt').write_text(f'pacman {"ok" if ok else "FAILED"}; checks {"ok" if healthy else "FAILED"}\n')
    if not (ok and healthy):
        raise SystemExit(f'upgrade problems: see above; to undo, run {snap}/rollback.sh')
    print('upgraded; reboot if glibc, systemd or the session libraries changed')


def install(args):
    upgrades, problems, config = plan(args)
    if problems:
        raise SystemExit('not installing: ' + '; '.join(problems))
    if upgrades:
        if not args.yes:
            raise SystemExit(f'{len(upgrades)} upgrades pending: installing now would be a partial '
                             'upgrade. Re-run with --yes to upgrade first, then install.')
        upgrade(args)
    cmd = ['pacman', '--config', config, '-S', '--needed', '--noconfirm', *args.packages]
    for name in PINNED:
        cmd += ['--ignore', name]
    with AwakeHold():
        run(cmd)
        check(args)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--server', help='package server for every repository, e.g. http://mirror.archlinuxarm.org/$arch/$repo')
    ap.add_argument('--allow-mobile', action='store_true', help='allow downloads over mobile data')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--yes', action='store_true')
    sub = ap.add_subparsers(dest='cmd')
    sub.add_parser('plan')
    sub.add_parser('run')
    sub.add_parser('check')
    p = sub.add_parser('install')
    p.add_argument('packages', nargs='+')
    args = ap.parse_args()
    if os.geteuid():
        raise SystemExit('run as root')
    {'plan': plan, None: plan, 'run': upgrade, 'check': check, 'install': install}[args.cmd](args)


if __name__ == '__main__':
    main()
