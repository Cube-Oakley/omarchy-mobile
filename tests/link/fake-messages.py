#!/usr/bin/env python3
"""A stand-in for omarchy-mobile-messages in the link's loopback test.

Conversations live in $XDG_DATA_HOME/omarchy-mobile/messages/fake.json;
every change also rewrites messages.db, which the link watches."""
import json
import os
from pathlib import Path
import sys
import time

root = Path(os.environ['XDG_DATA_HOME']) / 'omarchy-mobile/messages'
root.mkdir(parents=True, exist_ok=True)
store = root / 'fake.json'
log = root / 'calls.log'


def load():
    if store.exists():
        return json.loads(store.read_text())
    return [
        {'id': 1, 'thread': '5035550100', 'number': '+15035550100', 'direction': 'in', 'body': 'Hi there',
         'time': 1759600000.0, 'state': 'received', 'read': False, 'error': ''},
        {'id': 2, 'thread': '5035550100', 'number': '+15035550100', 'direction': 'out', 'body': 'Hello!',
         'time': 1759600060.0, 'state': 'sent', 'read': True, 'error': ''},
        {'id': 3, 'thread': 'carrier', 'number': 'Carrier', 'direction': 'in', 'body': 'Your bill is ready',
         'time': 1759500000.0, 'state': 'received', 'read': True, 'error': ''},
    ]


def save(rows):
    store.write_text(json.dumps(rows))
    (root / 'messages.db').write_text(str(time.time()))


def threads(rows):
    out = {}
    for row in sorted(rows, key=lambda r: (r['time'], r['id'])):
        entry = dict(row, name='Ada' if row['thread'] == '5035550100' else row['number'])
        entry['unread'] = sum(1 for r in rows if r['thread'] == row['thread'] and not r['read'])
        out[row['thread']] = entry
    return sorted(out.values(), key=lambda r: -r['time'])


def main(args):
    with log.open('a') as handle:
        handle.write(json.dumps(args) + '\n')
    rows = load()
    if args == ['threads']:
        print(json.dumps({'ok': True, 'threads': threads(rows), 'unread': sum(not r['read'] for r in rows)}))
    elif args[0] == 'thread':
        mine = [r for r in rows if r['thread'] == args[1]][-int(args[2]):]
        print(json.dumps({'ok': True, 'thread': args[1], 'messages': mine, 'name': 'Ada', 'number': args[1]}))
    elif args[0] == 'read':
        for row in rows:
            if row['thread'] == args[1]:
                row['read'] = True
        save(rows)
        print(json.dumps({'ok': True, 'unread': 0}))
    elif args[0] == 'send':
        digits = ''.join(c for c in args[1] if c.isdigit())[-10:]
        row = {'id': max(r['id'] for r in rows) + 1, 'thread': digits, 'number': args[1], 'direction': 'out',
               'body': args[2], 'time': time.time(), 'state': 'sent', 'read': True, 'error': ''}
        rows.append(row)
        save(rows)
        print(json.dumps({'ok': True, 'message': row}))
    else:
        print(json.dumps({'ok': False, 'error': 'unknown'}))
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
