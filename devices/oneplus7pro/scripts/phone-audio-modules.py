#!/usr/bin/env python3
"""Load the selected matching audio modules in dependency order, without depmod."""
from pathlib import Path
import subprocess
import os

base = Path('/root/audio-bringup/modules')
modules = {p.stem.replace('-', '_'): p for p in base.glob('*.ko')}
voice = Path('/root/audio-bringup/voice')
modules.update({p.stem.replace('-', '_'): p for p in voice.glob('q6*.ko')})
kernel = os.uname().release
visited = set()

def info(path, field):
    return subprocess.check_output(['modinfo', '-F', field, str(path)], text=True).strip()

def load(name):
    name = name.replace('-', '_')
    if (Path('/sys/module') / name).exists():
        return
    if name in visited:
        raise RuntimeError(f'Cyclic module dependency: {name}')
    visited.add(name)
    path = modules[name]  # Missing dependencies stop the test, never force load.
    assert info(path, 'vermagic').split()[0] == kernel, path
    for dependency in info(path, 'depends').split(','):
        if dependency:
            load(dependency)
    print('Loading', name, flush=True)
    subprocess.run(['insmod', str(path)], check=True)

for name in ('q6core', 'q6afe', 'q6afe-dai', 'q6afe-clocks', 'q6asm',
             'q6asm-dai', 'q6adm', 'q6routing', 'slim-qcom-ngd-ctrl',
             'wcd934x', 'gpio-wcd934x', 'soundwire-qcom', 'snd-soc-wcd934x'):
    load(name)
if Path('/sys/module/guacamole_speaker_route').exists():
    load('snd-soc-tfa9874')
# Call audio: with its card link applied, the voice DAI must register before
# the card, or the card would wait for it.
if Path('/sys/module/guacamole_voice_link').exists():
    for name in ('q6mvm', 'q6cvs', 'q6cvp', 'q6voice-dai'):
        load(name)
load('snd-soc-sm8150')
