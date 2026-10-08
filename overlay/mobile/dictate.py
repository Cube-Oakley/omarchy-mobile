#!/usr/bin/env python3
"""Voice typing: speech to text on the phone, with Moonshine.

  status                 {installed, model, models: {tiny: {...}, small: {...}}}
  install [tiny|small]   the engine (a Python environment of its own) and a model
  remove tiny|small      delete a model
  use tiny|small         the model listen uses
  listen [--file WAV]    transcribe the microphone (or a WAV file, at the pace
                         it was spoken) until "stop" on stdin, a long silence
                         or the time limit
  fetch tiny|small       (in the engine) download a model; install runs it

listen prints JSON lines: {"state": "listening"} once the microphone hears
(about 0.3 s after it opens; speech from then on is kept, even while the
model is still loading), {"partial": TEXT} while a phrase is spoken, {"line": TEXT} once it ends (at a pause), then
{"state": "done"}, or {"error": TEXT} ("setup": true with it when nothing is
installed). Nothing is kept: the audio goes from the microphone to the model
and nowhere else, and no network is used.

The models are Moonshine's streaming English ones (moonshine-ai/moonshine,
MIT licensed), made for live speech: tiny, 44 MB, takes about 0.12 s per
second of speech on the Pixel 7 Pro; small, 136 MB, about 0.28 s, and makes
fewer mistakes. install and status print one JSON object.
"""
import array
import json
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import wave

PACKAGE = 'moonshine-voice==0.1.5'
DATA = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share') / 'omarchy-mobile/dictation'
CONFIG = Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config') / 'omarchy-mobile/dictation.json'
VENV = DATA / 'venv'
MODELS = DATA / 'models'
# name: (Moonshine's model, its architecture, megabytes)
CHOICES = {'tiny': ('tiny-streaming-en', 'TINY_STREAMING', 44), 'small': ('small-streaming-en', 'SMALL_STREAMING', 136)}
COMPLETE = '.complete'
RATE = 16000
CHUNK = RATE // 10                  # samples read at a time: 100 ms
QUIET_START = 8.0                   # seconds with no speech before listening stops
QUIET_AFTER = 10.0                  # seconds with no new words once speech came
LIMIT = 300.0
PASS = 0.4                          # seconds of new audio between partial updates


def emit(**fields):
    print(json.dumps(fields), flush=True)


def model_dir(name):
    """The directory of an installed model, or None."""
    if name not in CHOICES:
        return None
    for marker in sorted((MODELS / 'download.moonshine.ai/model' / CHOICES[name][0]).glob('*/' + COMPLETE)):
        return marker.parent
    return None


def chosen():
    """The model listen uses: the one picked, else the installed one."""
    try:
        name = json.loads(CONFIG.read_text()).get('model')
    except (OSError, ValueError, AttributeError):
        name = None
    if name in CHOICES and model_dir(name):
        return name
    for other in ('small', 'tiny'):
        if model_dir(other):
            return other
    return name if name in CHOICES else 'tiny'


def engine_python():
    return VENV / 'bin/python3'


def in_engine():
    return Path(sys.prefix).resolve() == VENV.resolve()


def engine_ready():
    python = engine_python()
    return python.exists() and subprocess.run([str(python), '-c', 'import moonshine_voice'],
                                              capture_output=True, timeout=60).returncode == 0


def status():
    models = {}
    for name, (_, _, size) in CHOICES.items():
        models[name] = {'installed': model_dir(name) is not None, 'megabytes': size}
    installed = engine_python().exists() and any(m['installed'] for m in models.values())
    return {'ok': True, 'installed': installed, 'model': chosen(), 'models': models}


def use(name):
    if name not in CHOICES:
        raise ValueError('The model is tiny or small')
    CONFIG.parent.mkdir(parents=True, exist_ok=True)
    CONFIG.write_text(json.dumps({'model': name}) + '\n')


def install(name):
    """The engine, then the model (fetched by the engine, which checks it)."""
    if name not in CHOICES:
        raise ValueError('The model is tiny or small')
    if not engine_python().exists():
        DATA.mkdir(parents=True, exist_ok=True)
        subprocess.run([sys.executable, '-m', 'venv', str(VENV)], check=True, capture_output=True, timeout=300)
    if not engine_ready():
        result = subprocess.run([str(VENV / 'bin/pip'), 'install', '--disable-pip-version-check', '-q', PACKAGE],
                                capture_output=True, text=True, timeout=1800)
        if result.returncode != 0:
            raise RuntimeError('The speech engine did not install: ' + (result.stderr.strip().splitlines() or ['pip failed'])[-1])
    result = subprocess.run([str(engine_python()), str(Path(__file__).resolve()), 'fetch', name],
                            capture_output=True, text=True, timeout=1800)
    if result.returncode != 0:
        raise RuntimeError('The model did not download: ' + (result.stderr.strip().splitlines() or ['failed'])[-1])
    if not CONFIG.exists():
        use(name)
    return status()


def fetch(name):
    """In the engine: download a model (Moonshine checks sizes and CRCs)."""
    from moonshine_voice.download import download_model_from_info, find_model_info
    from moonshine_voice.moonshine_api import ModelArch
    info = find_model_info('en', ModelArch[CHOICES[name][1]])
    root, _ = download_model_from_info(info, cache_root=MODELS)
    (Path(root) / COMPLETE).touch()


def remove(name):
    directory = model_dir(name)
    if directory is None:
        raise ValueError('That model is not installed')
    shutil.rmtree(directory.parent)
    return status()


def microphone(chunks, stop):
    """Read 16 kHz mono from the default microphone into chunks; "listening"
    with the first audio, when the microphone is live."""
    recorder = subprocess.Popen(['pw-record', '--rate', str(RATE), '--channels', '1', '--format', 's16', '--raw',
                                 '--media-category', 'Capture', '--media-role', 'Communication',
                                 '-P', '{ media.name = "Voice typing" }', '-'],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        while not stop.is_set():
            data = recorder.stdout.read(CHUNK * 2)
            if not data:
                break
            if chunks.empty() and not getattr(microphone, 'live', False):
                microphone.live = True
                emit(state='listening')
            chunks.put(data)
    finally:
        recorder.terminate()
        try:
            recorder.wait(timeout=2)
        except subprocess.TimeoutExpired:
            recorder.kill()
        chunks.put(None)


def wav_file(path, chunks, stop):
    """A WAV file as if spoken now (for testing)."""
    with wave.open(path) as source:
        if source.getsampwidth() != 2 or source.getnchannels() != 1:
            raise ValueError('The file must be 16-bit mono')
        rate = source.getframerate()
        frames = source.readframes(source.getnframes())
    step = rate // 10 * 2
    started = time.monotonic()
    emit(state='listening')
    for n, offset in enumerate(range(0, len(frames), step)):
        if stop.is_set():
            break
        delay = started + n * 0.1 - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        chunks.put((frames[offset:offset + step], rate))
    chunks.put(None)


def stdin_stop(stop):
    for line in sys.stdin:
        if line.strip() == 'stop':
            break
    stop.set()


def tidy(text):
    """The model's text, with a doubled comma (",," happens) made single."""
    return re.sub(r'([,;:])\1+', r'\1', ' '.join(text.split()))


class Lines:
    """The engine's events as output: the phrase in progress, and each phrase
    once, when it is complete."""

    def __init__(self):
        self.done = set()
        self.partial = ''
        self.heard = None           # seconds into the audio of the last new words
        self.now = 0.0

    def event(self, event):
        line = getattr(event, 'line', None)
        if line is None:
            return
        text = tidy(line.text)
        if line.is_complete:
            if line.line_id in self.done:
                return
            self.done.add(line.line_id)
            if text:
                emit(line=text)
                self.heard = self.now
            if self.partial:
                self.partial = ''
                emit(partial='')
        elif text != self.partial:
            self.partial = text
            if text:
                self.heard = self.now
            emit(partial=text)

    def flush(self, transcript):
        """At the end: every phrase not given yet, complete or not."""
        for line in transcript.lines if transcript else []:
            if line.line_id not in self.done and tidy(line.text):
                self.done.add(line.line_id)
                emit(line=tidy(line.text))
        if self.partial:
            self.partial = ''
            emit(partial='')

    def quiet(self):
        """Whether listening should end: nothing said yet, or nothing new."""
        if self.now > LIMIT:
            return True
        if self.heard is None:
            return self.now > QUIET_START
        return self.now - self.heard > QUIET_AFTER


def listen(path=None):
    name = chosen()
    directory = model_dir(name)
    if directory is None:
        emit(error='Voice typing is not installed yet', setup=True)
        return 1
    # ONNX Runtime aborts as it loads while CPUs are offline (the Pixel's
    # dark screen takes the big ones down): say so instead.
    try:
        offline = Path('/sys/devices/system/cpu/offline').read_text().strip()
    except OSError:
        offline = ''
    if offline:
        emit(error='Voice typing needs every CPU online (the screen on)')
        return 1
    # The microphone first, then the model: it took 0.4 s to load after
    # the tap, and the microphone 0.3 s more to open, so the first word
    # said was lost. What is said while the model loads waits in chunks.
    stop = threading.Event()
    chunks = queue.Queue()
    if path:
        source = threading.Thread(target=wav_file, args=(path, chunks, stop), daemon=True)
    else:
        source = threading.Thread(target=microphone, args=(chunks, stop), daemon=True)
    source.start()
    threading.Thread(target=stdin_stop, args=(stop,), daemon=True).start()
    from moonshine_voice.moonshine_api import ModelArch
    from moonshine_voice.transcriber import Transcriber
    transcriber = Transcriber(str(directory), ModelArch[CHOICES[name][1]])
    # Passes are made here, not by add_audio: one only when no audio is
    # waiting, so a slow pass (each re-reads the phrase so far, which grows)
    # skips partial updates instead of falling behind the speaker.
    stream = transcriber.create_stream(update_interval=1e9)
    lines = Lines()
    stream.add_listener(lines.event)
    stream.start()
    passed = 0.0
    try:
        while True:
            try:
                chunk = chunks.get(timeout=0.2)
            except queue.Empty:
                if stop.is_set() and not source.is_alive():
                    break
                continue
            if chunk is None:
                break
            data, rate = chunk if isinstance(chunk, tuple) else (chunk, RATE)
            samples = array.array('h', data)
            lines.now += len(samples) / rate
            stream.add_audio([s / 32768.0 for s in samples], rate)
            if chunks.empty() and lines.now - passed >= PASS:
                passed = lines.now
                stream.update_transcription(0)      # events go to lines
            if lines.quiet():
                stop.set()
        # What was said before stop is in the queue still: the source ends
        # it with None, which ended the loop.
        stop.set()
        lines.flush(stream.stop())
        emit(state='done')
    finally:
        stream.close()
        transcriber.close()
    return 0


def main(argv):
    command = argv[0] if argv else 'status'
    # The engine's own Python runs what needs Moonshine.
    if command in ('listen', 'fetch') and not in_engine():
        python = engine_python()
        if not python.exists():
            emit(error='Voice typing is not installed yet', setup=True)
            return 1
        os.execv(str(python), [str(python), str(Path(__file__).resolve()), *argv])
    try:
        if command == 'status' and len(argv) <= 1:
            result = status()
        elif command == 'install' and len(argv) <= 2:
            result = install(argv[1] if len(argv) == 2 else 'tiny')
        elif command == 'remove' and len(argv) == 2:
            result = remove(argv[1])
        elif command == 'use' and len(argv) == 2:
            use(argv[1])
            result = status()
        elif command == 'fetch' and len(argv) == 2:
            fetch(argv[1])
            return 0
        elif command == 'listen' and (len(argv) == 1 or (len(argv) == 3 and argv[1] == '--file')):
            try:
                return listen(argv[2] if len(argv) == 3 else None)
            except Exception as error:   # the engine's own errors included
                emit(error=str(error) or 'Voice typing stopped')
                return 1
        else:
            print(__doc__.strip(), file=sys.stderr)
            return 2
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result = {'ok': False, 'error': str(error)}
    print(json.dumps(result))
    return 0 if result.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
