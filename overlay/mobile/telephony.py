#!/usr/bin/env python3
"""Device telephony backend selected by the installed mobile adapter.

Unconfigured devices continue to use ModemManager in phone.py/messages.py.
Only the authenticated local service owns the Pixel's modem channel.
"""
import json
import os
from pathlib import Path
import socket


class RequestRejected(RuntimeError):
    """The service rejected a request before submitting it to the modem."""


def configuration():
    path=Path(os.environ.get('XDG_CONFIG_HOME',str(Path.home()/'.config')))/'omarchy-mobile/telephony.json'
    try: return json.loads(path.read_text())
    except FileNotFoundError: return {}


def enabled(): return configuration().get('backend')=='pixel-sit'


def request(action, **args):
    path=configuration().get('socket','/run/omarchy-mobile-telephony/socket')
    try:
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as client:
            client.settimeout(120 if action=='sms-send' else 25)
            client.connect(path)
            client.sendall(json.dumps(dict(action=action,**args)).encode()+b'\n')
            data=b''
            while b'\n' not in data:
                chunk=client.recv(65536)
                if not chunk: raise RuntimeError('Phone service disconnected')
                data+=chunk
                if len(data)>4*1024*1024: raise RuntimeError('Phone response too large')
        reply=json.loads(data)
    except (OSError,ValueError) as error:
        raise RuntimeError('Cellular service is unavailable; try again when it reconnects') from error
    if not reply.get('ok'): raise RequestRejected(reply.get('error') or 'Phone request failed')
    return reply['result']
