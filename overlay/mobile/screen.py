#!/usr/bin/env python3
"""Screenshots and screen recordings.

  shot [DELAY_MS]       the screen as a PNG in ~/Pictures/Screenshots, copied
                        to the clipboard, with a notification
  record start|stop|toggle|status
                        the screen as an MP4 (H.264, half size, 30 fps) in
                        ~/Videos/Screen recordings; the notification's Stop or
                        `record stop` ends it, and 30 minutes at most
"""
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

HOME = Path.home()
RUNTIME = Path(os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}'))
RECORDING = RUNTIME / 'omarchy-mobile-recording.json'
MAX_RECORDING = 30 * 60


def notify(summary, body, actions=(), image='', replaces=0, timeout=-1):
    """A notification through gdbus; returns its ID, or 0."""
    hints = "{'desktop-entry': <'omarchy-mobile-screen'>"
    if image:
        hints += ", 'image-path': <'" + Path(image).as_uri().replace("'", "%27") + "'>"
    hints += '}'
    action_list = '[' + ', '.join("'" + a + "'" for a in actions) + ']'
    try:
        result = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications',
                                 '--object-path', '/org/freedesktop/Notifications',
                                 '--method', 'org.freedesktop.Notifications.Notify', '--',
                                 'Screen', str(replaces), 'camera-photo', summary, body, action_list, hints, str(timeout)],
                                capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return 0
    match = re.search(r'uint32 (\d+)', result.stdout)
    return int(match.group(1)) if match else 0


def close_notification(ident):
    if not ident:
        return
    subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications',
                    '--object-path', '/org/freedesktop/Notifications',
                    '--method', 'org.freedesktop.Notifications.CloseNotification', str(ident)],
                   capture_output=True, timeout=5)


def shot(delay_ms=0):
    if delay_ms > 0:
        time.sleep(delay_ms / 1000)
    folder = HOME / 'Pictures/Screenshots'
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / time.strftime('Screenshot_%Y%m%d_%H%M%S.png')
    # Compression level 1: the full 1440x3120 PNG is written in a fraction of
    # the default's time.
    subprocess.run(['grim', '-l', '1', str(path)], check=True, timeout=15)
    with path.open('rb') as image:
        subprocess.Popen(['wl-copy', '--type', 'image/png'], stdin=image,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    notify('Screenshot saved', 'Pictures/Screenshots · copied to the clipboard', image=str(path), timeout=5000)
    print(json.dumps({'ok': True, 'path': str(path)}))
    return 0


def recording():
    try:
        state = json.loads(RECORDING.read_text())
        os.kill(state['pid'], 0)
        return state
    except (OSError, ValueError, KeyError):
        return None


def record_run():
    """The recorder: wf-recorder, its notification, and waiting for Stop."""
    folder = HOME / 'Videos/Screen recordings'
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / time.strftime('Screen recording %Y-%m-%d %H.%M.%S.mp4')
    # Half size and 30 fps keep x264's ultrafast preset ahead of the screen on
    # the phone's CPU (no hardware encoder yet).
    recorder = subprocess.Popen(['wf-recorder', '-y', '-c', 'libx264', '-p', 'preset=ultrafast', '-p', 'crf=26',
                                 '-p', 'tune=zerolatency', '-F', 'scale=720:-2', '-r', '30', '-f', str(path)],
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ident = notify('Recording the screen', 'Tap Stop to finish', actions=('stop', 'Stop'), timeout=0)
    RECORDING.write_text(json.dumps({'pid': os.getpid(), 'path': str(path), 'started': time.time(), 'notification': ident}))
    stopping = []
    signal.signal(signal.SIGTERM, lambda *_: stopping.append(True))
    monitor = subprocess.Popen(['gdbus', 'monitor', '--session', '--dest', 'org.freedesktop.Notifications'],
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    os.set_blocking(monitor.stdout.fileno(), False)
    started = time.monotonic()
    buffered = ''
    while not stopping and recorder.poll() is None and time.monotonic() - started < MAX_RECORDING:
        try:
            buffered += monitor.stdout.read() or ''
        except (OSError, TypeError):
            pass
        if f'ActionInvoked (uint32 {ident}, ' in buffered:
            break
        buffered = buffered[-400:]
        time.sleep(0.2)
    monitor.terminate()
    if recorder.poll() is None:
        recorder.send_signal(signal.SIGINT)
        try:
            recorder.wait(timeout=15)
        except subprocess.TimeoutExpired:
            recorder.kill()
    close_notification(ident)
    RECORDING.unlink(missing_ok=True)
    seconds = round(time.monotonic() - started)
    if path.exists() and path.stat().st_size > 0:
        notify('Screen recording saved', f'Videos/Screen recordings · {seconds // 60}:{seconds % 60:02d}', timeout=6000)
    else:
        notify('Screen recording failed', 'Nothing was recorded.', timeout=6000)
    return 0


def record(action):
    current = recording()
    if action == 'status':
        print(json.dumps({'ok': True, 'recording': bool(current), **(current or {})}))
        return 0
    if action == 'toggle':
        action = 'stop' if current else 'start'
    if action == 'start':
        if current:
            print(json.dumps({'ok': True, 'recording': True, **current}))
            return 0
        subprocess.Popen([sys.executable, str(Path(__file__).resolve()), 'record', 'run'],
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
        print(json.dumps({'ok': True, 'recording': True}))
        return 0
    if action == 'stop':
        if current:
            os.kill(current['pid'], signal.SIGTERM)
        print(json.dumps({'ok': True, 'recording': False}))
        return 0
    if action == 'run':
        return record_run()
    return 2


def main(args):
    if args[:1] == ['shot'] and len(args) <= 2:
        return shot(int(args[1]) if len(args) == 2 else 0)
    if len(args) == 2 and args[0] == 'record':
        return record(args[1])
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == '__main__':
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (OSError, subprocess.SubprocessError) as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        raise SystemExit(1)
