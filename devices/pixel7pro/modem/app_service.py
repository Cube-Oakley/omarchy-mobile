#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Single-owner Pixel modem service for the native Phone and Messages apps.

Start with a supervised ONLINE modem; --bringup registers a fresh CP first.
A private Unix socket accepts same-uid clients; no radio/NV/SIM commands are
exposed to applications.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import socket
import sqlite3
import struct
import time
import re
from sit import Sit, ModemError
from ims import REGISTRATION, record_registration
from voice_audio import VoiceAudio
from sms_codec import submit, received_pdu, number
from call_test import call_status, stop_supervised_cp

ROOT=Path('/run/omarchy-mobile-telephony')
PRIVATE=Path('/root/.local/state/modem-lab')
STATES={0:'active',1:'held',2:'dialing',3:'ringing-out',4:'ringing-in',5:'waiting',6:'terminated',7:'held',8:'active'}


def database(path):
    db=sqlite3.connect(path)
    db.row_factory=sqlite3.Row
    db.execute('PRAGMA journal_mode=WAL')
    db.execute('PRAGMA synchronous=FULL')
    db.executescript('''
      CREATE TABLE IF NOT EXISTS received (id TEXT PRIMARY KEY, number TEXT, text TEXT, timestamp TEXT, consumed INTEGER DEFAULT 0);
      CREATE TABLE IF NOT EXISTS imported (id TEXT PRIMARY KEY);
      CREATE TABLE IF NOT EXISTS parts (id TEXT PRIMARY KEY, sender TEXT, reference INTEGER, total INTEGER, seq INTEGER, text TEXT, timestamp TEXT, received REAL);
      CREATE TABLE IF NOT EXISTS outgoing (id TEXT PRIMARY KEY, digest TEXT, state TEXT, error TEXT, completed INTEGER DEFAULT 0);
    ''')
    # A request interrupted by a process restart must never be retried blindly.
    db.execute("UPDATE outgoing SET state='unknown', error='Delivery uncertain after service restart; check with the recipient' WHERE state='sending'")
    db.commit()
    return db


class Service:
    def __init__(self, client, db):
        self.client=client; self.db=db; self.calls={}; self.audio=None; self.muted=False
        self.pending=None; self.hangup_deadline=None
        self.screen_on=None
        self.indication_counts={}
        spec=importlib.util.spec_from_file_location('pixel_monitor',Path(__file__).with_name('monitor-sit.py'))
        self.monitor=importlib.util.module_from_spec(spec); spec.loader.exec_module(self.monitor)

    def registration(self):
        try: return json.loads(REGISTRATION.read_text())
        except (OSError,ValueError): return {}

    def ready(self, feature):
        reg=self.registration()
        if reg.get('state')!=2 or not reg.get('features',0)&feature:
            raise RuntimeError('Cellular service is not registered yet')

    def audio_start(self):
        if self.audio is None:
            self.audio=VoiceAudio()
            try: self.audio.__enter__()
            except BaseException:
                self.audio=None
                raise
            self.muted=False

    def audio_stop(self):
        if self.audio is not None:
            audio,self.audio=self.audio,None
            audio.__exit__(None,None,None)
        self.muted=False

    def live(self): return [c for c in self.calls.values() if c['state']!='terminated']

    def save_calls(self):
        # A restart must not silently abandon a possibly connected call.
        path=ROOT/'active.json'; tmp=path.with_suffix('.tmp')
        tmp.write_text(json.dumps(self.live())); tmp.replace(path)

    def new_call(self, number, direction, state, modem_id=None):
        call=dict(id=str(time.time_ns()),number=number,direction=direction,state=state,
                  reason='',multiparty=False,modem_id=modem_id,since=time.time())
        self.calls[call['id']]=call; self.save_calls(); return call

    def ingest(self, path):
        key=path.stem
        if self.db.execute('SELECT 1 FROM imported WHERE id=?',(key,)).fetchone(): return
        packet=path.read_bytes(); ident=struct.unpack_from('<H',packet,2)[0]
        if ident in (0x010c,0x0d1e):
            try: sms=received_pdu(packet[10:10+packet[9]])
            except (ValueError,IndexError,UnicodeError):
                print('Stored SMS could not be decoded; original retained privately',flush=True)
                return
            concat=sms.pop('concat')
            if concat:
                ref,total,seq=concat
                self.db.execute('INSERT OR IGNORE INTO parts VALUES (?,?,?,?,?,?,?,?)',
                                (key,sms['number'],ref,total,seq,sms['text'],sms['timestamp'],time.time()))
                parts=self.db.execute('SELECT * FROM parts WHERE sender=? AND reference=? AND total=? AND received>? ORDER BY received',
                                      (sms['number'],ref,total,time.time()-86400)).fetchall()
                by_seq={p['seq']:p for p in parts}
                if len(by_seq)==total:
                    sms['text']=''.join(by_seq[i]['text'] for i in range(1,total+1))
                    sms['timestamp']=by_seq[1]['timestamp']
                    self.db.execute('INSERT OR IGNORE INTO received(id,number,text,timestamp) VALUES (?,?,?,?)',
                                    (key,sms['number'],sms['text'],sms['timestamp']))
                    self.db.execute('DELETE FROM parts WHERE sender=? AND reference=? AND total=?',(sms['number'],ref,total))
            else:
                self.db.execute('INSERT OR IGNORE INTO received(id,number,text,timestamp) VALUES (?,?,?,?)',
                                (key,sms['number'],sms['text'],sms['timestamp']))
        self.db.execute('INSERT OR IGNORE INTO imported VALUES (?)',(key,)); self.db.commit()

    def event(self, packet):
        ident=struct.unpack_from('<H',packet,2)[0]
        key=f'{ident:04x}'
        self.indication_counts[key]=self.indication_counts.get(key,0)+1
        if ident in (0x010c,0x010d,0x0d1e,0x0d20):
            reference=self.monitor.persist_sms(packet)
            self.client.request(0x0d1d if ident>=0x0d00 else 0x0102,struct.pack('<IBI',1,reference,0))
            for path in self.monitor.INBOX.glob('*.sit'): self.ingest(path)
        elif ident==0x0d05: record_registration(packet)
        elif ident==0x0d01:
            if len(packet)<13: raise ValueError('Truncated incoming call')
            kind,mid,length=struct.unpack_from('<HBH',packet,8)
            if kind!=1 or length>513 or len(packet)<13+length: return
            if any(c['modem_id']==mid for c in self.live()): return
            # No auto-answer. The shell presents its normal incoming-call UI.
            caller=packet[13:13+length].rstrip(b'\0').decode('ascii',errors='replace')
            self.new_call(caller,'incoming','ringing-in',mid)
        elif ident==0x0d04:
            kind,mid,state,cause=call_status(packet)
            if kind!=1: return
            call=next((c for c in self.live() if c['modem_id']==mid),None)
            if call is None and self.pending:
                call=self.calls[self.pending]; call['modem_id']=mid; self.pending=None
            if call is None: return
            call['state']=STATES[state]; call['since']=time.time()
            if state==6:
                call['reason']='terminated' if cause==16 else 'error'
                if not self.live(): self.audio_stop(); self.hangup_deadline=None
            self.save_calls()

    def pump(self):
        for _ in range(256):
            packet=self.client.next_indication(0)
            if packet is None: break
            self.event(packet)
        if self.hangup_deadline and time.monotonic()>self.hangup_deadline:
            stop_supervised_cp()
            raise RuntimeError('Call release was not confirmed; modem stopped')

    def send_sms(self, request):
        key=request.get('request_id','')
        if not isinstance(key,str) or len(key)!=32 or any(c not in '0123456789abcdef' for c in key):
            raise ValueError('Invalid send request identifier')
        destination=number(request['number']); text=request['text']
        pdus=submit(destination,text)
        digest=hashlib.sha256(json.dumps([destination,text]).encode()).hexdigest()
        prior=self.db.execute('SELECT * FROM outgoing WHERE id=?',(key,)).fetchone()
        if prior:
            if prior['digest']!=digest: raise ValueError('Send request identifier was reused')
            return dict(prior)
        self.ready(2)
        smsc=self.client.request(0x0108)
        if len(smsc)!=13 or not 1<=smsc[0]<=12: raise RuntimeError('Carrier SMS configuration is unavailable')
        self.db.execute('INSERT INTO outgoing VALUES (?,?,?,?,?)',(key,digest,'sending','',0)); self.db.commit()
        completed=0; state='sent'; error=''
        try:
            for pdu in pdus:
                payload=bytearray(260); payload[:13]=smsc; payload[13]=len(pdu); payload[14:14+len(pdu)]=pdu
                result=self.client.request(0x0d1b,bytes(payload),timeout=90)
                if len(result)<253: raise TimeoutError('Unknown SMS response')
                cause=struct.unpack_from('<I',result,249)[0]
                if cause: raise ModemError(cause,b'')
                completed+=1
                self.db.execute('UPDATE outgoing SET completed=? WHERE id=?',(completed,key)); self.db.commit()
                self.pump()
        except ModemError:
            state='unknown' if completed else 'failed'
            error='Some message parts were sent; check with the recipient before sending again' if completed else 'The carrier rejected this message'
        except (OSError,RuntimeError,TimeoutError):
            state='unknown'; error='Delivery uncertain; check with the recipient before sending again'
        self.db.execute('UPDATE outgoing SET state=?,error=? WHERE id=?',(state,error,key)); self.db.commit()
        return dict(state=state,error=error,completed=completed)

    def dispatch(self, request):
        action=request.get('action')
        if action=='screen-state':
            on=request.get('on')
            if type(on) is not bool: raise ValueError('Screen state must be boolean')
            if not on and self.live(): raise RuntimeError('Call in progress')
            if on!=self.screen_on:
                # Stock ProtocolDeviceInfoBuilder::SetScreenState: SIT 0902,
                # 16-byte message, uint32 at payload offset 0 (off=0, on=1).
                self.screen_on=None
                self.client.request(0x0902,struct.pack('<I',int(on)),timeout=5)
                # Stock BuildSetUnsolicitedResponseFilter passes the AOSP
                # IndicationFilter mask unchanged in SIT 0928 (uint32).
                # NONE suppresses optional signal/dormancy reports, not calls,
                # SMS or significant registration/data changes. ALL restores
                # foreground reporting. Cache only after both replies succeed.
                self.client.request(0x0928,struct.pack('<I',0xffffffff if on else 0),timeout=5)
                self.screen_on=on
            return dict(screen_on=self.screen_on)
        if action=='power-status':
            return dict(screen_on=self.screen_on,indications=dict(self.indication_counts))
        if action=='status':
            reg=self.registration(); ready=reg.get('state')==2 and reg.get('features',0)&1!=0
            return dict(modem=dict(present=True,ready=ready,state='registered' if ready else 'searching',operator='Tello',tech='LTE',signal=0,own='',audio=True),
                        calls=[{k:v for k,v in c.items() if k!='modem_id'} for c in self.calls.values()],
                        audio=dict(available=True,running=self.audio is not None,mute=self.muted,speaker=False,speaker_available=False,error=''))
        if action=='sms-list':
            return [dict(path='pixel:'+r['id'],number=r['number'],text=r['text'],timestamp=r['timestamp'],data='',pdu='deliver',state='received',delivery='')
                    for r in self.db.execute('SELECT * FROM received WHERE consumed=0')]
        if action=='sms-delete':
            self.db.execute('UPDATE received SET consumed=1 WHERE id=?',(request['id'].removeprefix('pixel:'),)); self.db.commit(); return {}
        if action=='sms-send': return self.send_sms(request)
        if action=='sms-status':
            row=self.db.execute('SELECT state,error,completed FROM outgoing WHERE id=?',(request.get('request_id'),)).fetchone()
            return dict(row) if row else dict(state='unknown',error='Send outcome has not been confirmed')
        if action=='dial':
            self.ready(1)
            if self.live(): raise RuntimeError('End the current call before placing another')
            destination=number(request['number'],voice=True)
            calls=self.client.request(0x0000)
            if calls!=bytes(4): raise RuntimeError('The modem already has a call')
            self.audio_start()
            call=self.new_call(destination,'outgoing','dialing'); self.pending=call['id']
            payload=bytearray(679); encoded=destination.encode('ascii')
            struct.pack_into('<HBH',payload,0,1,0,len(encoded)); payload[5:5+len(encoded)]=encoded
            try: self.client.request(0x0d00,bytes(payload),timeout=8)
            except ModemError:
                call['state']='terminated'; call['reason']='error'; self.pending=None; self.audio_stop(); self.save_calls()
                raise RuntimeError('The carrier rejected the call')
            except (TimeoutError,OSError):
                # Keep ownership and resolve queued status before any repeat.
                self.hangup_deadline=time.monotonic()+12
                raise RuntimeError('Call setup is uncertain; checking the modem')
            self.pump()
            return dict(call=call['id'],number=destination)
        if action in ('accept','hangup','delete-call'):
            call=self.calls.get(str(request.get('id')))
            if call is None: raise ValueError('This call is no longer available')
            if action=='delete-call':
                if call['state']!='terminated': raise RuntimeError('Call has not ended')
                del self.calls[call['id']]; return {}
            if call['state']=='terminated': return {}
            if call['modem_id'] is None: raise RuntimeError('Call setup is still pending')
            if action=='accept':
                if call['state'] not in ('ringing-in','waiting'): raise RuntimeError('Call is not ringing')
                if len(self.live())!=1: raise RuntimeError('Call waiting is not supported yet')
                self.audio_start()
                try: self.client.request(0x0d02,struct.pack('<HHH',1,1,0))
                except BaseException:
                    self.hangup_deadline=time.monotonic()+12
                    raise
            else:
                self.hangup_deadline=time.monotonic()+12
                self.client.request(0x0d03,bytes([call['modem_id'],1,0]))
            self.pump(); return {}
        if action=='audio':
            if self.audio is None: raise RuntimeError('No call audio is running')
            if request.get('what')=='speaker': raise RuntimeError('Speakerphone routing is not configured yet')
            if request.get('what')!='mute' or request.get('value') not in ('on','off'): raise ValueError('Invalid audio command')
            self.muted=request['value']=='on'
            VoiceAudio.mixer('cset','Incall Mic Mute','1' if self.muted else '0')
            return self.dispatch({'action':'status'})['audio']
        if action=='dtmf':
            call=self.calls.get(str(request.get('id')))
            keys=request.get('keys','')
            if call is None or call['state']!='active' or call['modem_id'] is None:
                raise RuntimeError('Touch tones require an active call')
            if not isinstance(keys,str) or not re.fullmatch(r'[0-9*#]{1,32}',keys):
                raise ValueError('Invalid touch tones')
            # Matching RilReqSendDtmf: ID, one-shot type=0, ASCII digit.
            for key in keys:
                self.client.request(0x0d0a,bytes([call['modem_id'],0,ord(key)]))
            return {}
        raise ValueError('Unsupported phone action')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bringup',action='store_true',help='register a freshly booted supervised CP')
    args=parser.parse_args()
    os.umask(0o077); ROOT.mkdir(mode=0o700,parents=True,exist_ok=True); PRIVATE.mkdir(mode=0o700,parents=True,exist_ok=True)
    client=Sit(allow_test_sms=True,allow_test_call=True)
    server=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); path=ROOT/'socket'
    service=None; stopping=False
    def stop(*_):
        nonlocal stopping
        stopping=True
    signal.signal(signal.SIGTERM,stop); signal.signal(signal.SIGINT,stop)
    try:
        marker=ROOT/'active.json'
        if marker.exists() and json.loads(marker.read_text()):
            stop_supervised_cp(); raise RuntimeError('Previous call ownership unresolved; modem stopped')
        service=Service(client,database(PRIVATE/'apps.db'))
        for sms in service.monitor.INBOX.glob('*.sit'): service.ingest(sms)
        if args.bringup:
            from bringup import start
            start(service,lambda: stopping)
        path.unlink(missing_ok=True); server.bind(str(path)); server.listen(16); server.settimeout(.1)
        (ROOT/'service.pid').write_text(str(os.getpid()))
        print('Phone and Messages modem service ready',flush=True)
        while not stopping:
            service.pump()
            # next_indication(0) drains queued packets, but a positive timeout
            # is needed to read newly arrived device bytes.
            packet=client.next_indication(.05)
            if packet: service.event(packet)
            try: connection,_=server.accept()
            except socket.timeout: continue
            with connection:
                connection.settimeout(2)
                try:
                    _,uid,_=struct.unpack('3i',connection.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12))
                    if uid!=os.getuid(): raise PermissionError('Phone service access denied')
                    body=b''
                    while b'\n' not in body:
                        chunk=connection.recv(4096)
                        if not chunk: raise ValueError('Incomplete phone request')
                        body+=chunk
                        if len(body)>32768: raise ValueError('Phone request too large')
                    result=service.dispatch(json.loads(body.split(b'\n',1)[0]))
                    response=dict(ok=True,result=result)
                except (ValueError,KeyError,TypeError,RuntimeError,OSError) as error:
                    # Errors from wire data must never expose raw values.
                    response=dict(ok=False,error=str(error) if isinstance(error,RuntimeError) else 'Invalid or unavailable phone request')
                try: connection.sendall(json.dumps(response).encode()+b'\n')
                except OSError: pass  # Durable send IDs prevent uncertain retries.
    finally:
        try:
            if service and service.live():
                for call in service.live():
                    if call['modem_id'] is not None:
                        try: client.request(0x0d03,bytes([call['modem_id'],1,0]))
                        except (OSError,RuntimeError): pass
                end=time.monotonic()+8
                while service.live() and time.monotonic()<end:
                    packet=client.next_indication(.25)
                    if packet: service.event(packet)
                if service.live(): stop_supervised_cp()
            if service: service.audio_stop()
        finally:
            client.close(); server.close(); path.unlink(missing_ok=True)
            (ROOT/'service.pid').unlink(missing_ok=True)


if __name__=='__main__': main()
