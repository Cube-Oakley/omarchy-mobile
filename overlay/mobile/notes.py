#!/usr/bin/env python3
"""Notes for the mobile shell: plain text and Markdown files in ~/Notes.

Usage: omarchy-mobile-notes list [QUERY] | read ID | save ID|- | delete ID
Prints one JSON object. A note's ID is its file name in ~/Notes (.md or
.txt; never hidden, never a link). `save` reads the text on stdin:
- "-" makes a new note;
- the file is named after the first line (its title, without Markdown's #),
  so the folder reads well in Files;
- when the title changes, the file is renamed to match.
`delete` moves the note to ~/Notes/.trash, from where Files can bring it
back. `list` with a QUERY keeps the notes whose title or text contain it,
ignoring case. The newest change is first.
"""
import json
import os
from pathlib import Path
import re
import sys
import time

MAX_BYTES = 1024 * 1024
SUFFIXES = ('.md', '.txt')
PREVIEW = 160


def folder():
    return Path.home() / 'Notes'


def note_path(note_id):
    """The note's file, or ValueError for anything else."""
    if (not note_id or '/' in note_id or '\0' in note_id or note_id.startswith('.')
            or not note_id.endswith(SUFFIXES)):
        raise ValueError('Invalid note')
    path = folder() / note_id
    if path.is_symlink():
        raise ValueError('Invalid note')
    return path


def title_of(text):
    for line in text.splitlines():
        line = re.sub(r'^\s*#+\s*', '', line).strip()
        if line:
            return line
    return ''


def file_name(title, now=None):
    """A file name for TITLE: no slashes or control characters, at most 60
    characters, .md; a dated name for a note without a title."""
    name = re.sub(r'[\x00-\x1f\x7f/\\:*?"<>|]+', ' ', title).strip().strip('.')
    name = re.sub(r'\s+', ' ', name)[:60].strip()
    if not name:
        name = time.strftime('Note %Y-%m-%d %H.%M', time.localtime(now))
    return name + '.md'


def unique(name, keep=None):
    """NAME, or NAME (2), (3)... where another note has it; KEEP is the
    note's own path, which may keep its name."""
    stem, suffix = os.path.splitext(name)
    candidate, n = name, 1
    while (folder() / candidate).exists() and (folder() / candidate) != keep:
        n += 1
        candidate = f'{stem} ({n}){suffix}'
    return candidate


def summary(path):
    try:
        text = path.read_bytes()[:MAX_BYTES].decode('utf-8', errors='replace')
        stat = path.stat()
    except OSError:
        return None
    title = title_of(text) or path.stem
    body = text.split('\n', 1)[1] if '\n' in text else ''
    preview = re.sub(r'\s+', ' ', body).strip()[:PREVIEW]
    return {'id': path.name, 'title': title, 'preview': preview,
            'modified': int(stat.st_mtime), 'text': text}


def list_notes(query=''):
    notes = []
    if folder().is_dir():
        for path in folder().iterdir():
            if path.name.startswith('.') or not path.name.endswith(SUFFIXES) or path.is_symlink() \
                    or not path.is_file():
                continue
            note = summary(path)
            if note is None:
                continue
            text = note.pop('text')
            if query and query.lower() not in (note['title'] + '\n' + text).lower():
                continue
            notes.append(note)
    notes.sort(key=lambda n: (-n['modified'], n['title'].lower()))
    return {'ok': True, 'notes': notes}


def read(note_id):
    path = note_path(note_id)
    if not path.is_file():
        raise ValueError('No such note')
    if path.stat().st_size > MAX_BYTES:
        raise ValueError('This note is too large to open here')
    return {'ok': True, 'id': note_id, 'text': path.read_text(errors='replace')}


def write_atomic(path, text):
    temporary = path.with_name('.' + path.name + '.tmp')
    with open(temporary, 'w') as stream:
        stream.write(text)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def save(note_id, text):
    if len(text.encode()) > MAX_BYTES:
        raise ValueError('This note is too large')
    folder().mkdir(exist_ok=True)
    if note_id == '-':
        if not text.strip():
            return {'ok': True, 'id': ''}
        path = folder() / unique(file_name(title_of(text)))
    else:
        current = note_path(note_id)
        if not current.is_file():
            raise ValueError('No such note')
        path = current
        title = title_of(text)
        # The name follows the title, in the note's own suffix.
        wanted = file_name(title)[:-3] + current.suffix if title else current.name
        if wanted != current.name:
            target = folder() / unique(wanted, keep=current)
            if target != current:
                os.rename(current, target)
                path = target
    write_atomic(path, text)
    return {'ok': True, 'id': path.name}


def delete(note_id):
    path = note_path(note_id)
    if not path.is_file():
        raise ValueError('No such note')
    trash = folder() / '.trash'
    trash.mkdir(exist_ok=True)
    target = trash / path.name
    if target.exists():
        target = trash / f'{path.stem} {time.strftime("%Y-%m-%d %H.%M.%S")}{path.suffix}'
    os.rename(path, target)
    return {'ok': True}


def main(args):
    try:
        if args[:1] == ['list'] and len(args) <= 2:
            result = list_notes(args[1] if len(args) == 2 else '')
        elif args[:1] == ['read'] and len(args) == 2:
            result = read(args[1])
        elif args[:1] == ['save'] and len(args) == 2:
            result = save(args[1], sys.stdin.read())
        elif args[:1] == ['delete'] and len(args) == 2:
            result = delete(args[1])
        else:
            print(__doc__.strip(), file=sys.stderr)
            return 2
    except (OSError, ValueError, UnicodeError) as error:
        result = {'ok': False, 'error': str(error)}
    print(json.dumps(result))
    return 0 if result.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
