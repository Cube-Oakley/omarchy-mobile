#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Keep the owned LTE bearer available as a Wi-Fi fallback in this session.

CPIF's ARPHRD_RAWIP interface is not exposed by this NetworkManager build.
Own only rmnet0's /32 address, metric-700 route, and cellular-only DNS override.
Wi-Fi keeps its metric-600 default. All state/rollback records stay in RAM.

The carrier's DNS is used only while rmnet0 is the only way out. A bearer that
sends but receives nothing, and then answers no ping either, loses its default
route, and gets it back as soon as nothing else is left (October 6: a dead bearer's route plus its DNS outlived a
one-minute Wi-Fi outage, NetworkManager's check then failed through it and
penalised Wi-Fi, and the phone stayed offline with Wi-Fi up).
"""
import fcntl
import ipaddress
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import time

ROOT=Path('/run/modem-lab')
RESOLVER=Path('/etc/resolv.conf')


def run(*args):
    p=subprocess.run(args,capture_output=True,timeout=20)
    if p.returncode: raise RuntimeError('Cellular network operation failed: '+args[0])
    return p.stdout


def configuration(data):
    if len(data)<132: raise ValueError('Short internet bearer response')
    status,cid,active,pdp=struct.unpack_from('<HBBB',data)
    if status or cid!=1 or active not in (1,2) or pdp not in (1,3):
        raise ValueError('An active IPv4 internet bearer is required')
    addr=ipaddress.IPv4Address(data[5:9])
    dns=list(dict.fromkeys(str(ipaddress.IPv4Address(data[i:i+4])) for i in (0x1a,0x2e) if any(data[i:i+4])))
    if addr.is_unspecified or addr.is_multicast or not dns: raise ValueError('Missing internet address or carrier DNS')
    return str(addr),dns


def counters(stats):
    """(received bytes, sent packets) from `ip -j -s link` output for rmnet0."""
    link=stats[0]['stats64']
    return link['rx']['bytes'],link['tx']['packets']


def looks_silent(routed, before, after):
    """The routed bearer sent packets but received nothing since the last
    check. Not proof: a few router solicitations get no answer on a live one."""
    return routed and after[1]>before[1] and after[0]==before[0]


def answers():
    """Whether the bearer carries anything: a ping out through rmnet0."""
    for target in ('1.1.1.1','8.8.8.8'):
        p=subprocess.run(['ping','-n','-q','-c','1','-W','3','-I','rmnet0',target],capture_output=True,timeout=10)
        if p.returncode==0: return True
    return False


class Resolver:
    def __init__(self,path):
        self.path=path.resolve(); self.original=None; self.owned=None
    def update(self, cellular, servers):
        if cellular:
            content='# Pixel cellular fallback (session only)\n'+''.join('nameserver '+s+'\n' for s in servers)
            current=self.path.read_text()
            if self.owned is None or current!=self.owned:
                self.original=current
            if current!=content: self.path.write_text(content)
            self.owned=content
        else: self.restore()
    def restore(self):
        if self.owned is not None:
            if self.path.read_text()==self.owned: self.path.write_text(self.original)
            self.owned=None


def main():
    os.umask(0o077)
    lock=os.open(ROOT/'data-network.lock',os.O_CREAT|os.O_RDWR,0o600)
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    address,dns=configuration((ROOT/'data-call-response.bin').read_bytes())
    info=json.loads(run('ip','-j','address','show','dev','rmnet0'))[0]
    if 'UP' in info['flags'] or info['addr_info']: raise RuntimeError('rmnet0 already configured; refusing to replace it')
    resolver=Resolver(RESOLVER)
    original=resolver.path.read_bytes(); (ROOT/'resolver-before-cellular').write_bytes(original)
    stopping=False; addressed=False; routed=False
    def stop(*_):
        nonlocal stopping
        stopping=True
    signal.signal(signal.SIGTERM,stop); signal.signal(signal.SIGINT,stop)
    cp=os.open('/dev/umts_boot0',os.O_RDWR|os.O_CLOEXEC)
    try:
        run('ip','link','set','dev','rmnet0','up')
        run('ip','addr','add',address+'/32','dev','rmnet0'); addressed=True
        run('ip','route','add','default','dev','rmnet0','src',address,'metric','700'); routed=True
        (ROOT/'data-network.pid').write_text(str(os.getpid()))
        print('Cellular fallback ready; Wi-Fi remains preferred',flush=True)
        last=None
        seen=counters(json.loads(run('ip','-j','-s','link','show','dev','rmnet0'))); check_at=time.monotonic()+30
        while not stopping and fcntl.ioctl(cp,0x6f27,0)==4:
            routes=json.loads(run('ip','-j','route','show','default'))
            routed=any(r.get('dev')=='rmnet0' for r in routes)
            others=[r for r in routes if r.get('dev')!='rmnet0']
            # With Wi-Fi up but rated limited, the carrier's DNS would keep
            # NetworkManager's own check failing and Wi-Fi penalised.
            cellular=routed and not others
            resolver.update(cellular,dns)
            if cellular!=last:
                print('Default internet path', 'cellular' if cellular else 'Wi-Fi/other',flush=True); last=cellular
            if not routed and not others:
                run('ip','route','add','default','dev','rmnet0','src',address,'metric','700')
                print('Nothing else leads out; the cellular default route is back',flush=True)
            elif time.monotonic()>=check_at:
                now=counters(json.loads(run('ip','-j','-s','link','show','dev','rmnet0')))
                if looks_silent(routed,seen,now) and not answers():
                    run('ip','route','del','default','dev','rmnet0','metric','700')
                    print('Cellular bearer carries nothing; its default route is removed',flush=True)
                    now=counters(json.loads(run('ip','-j','-s','link','show','dev','rmnet0')))
                seen=now; check_at=time.monotonic()+30
            time.sleep(.5)
    finally:
        resolver.restore()
        routes=json.loads(run('ip','-j','route','show','default'))
        if any(r.get('dev')=='rmnet0' and r.get('metric')==700 for r in routes):
            run('ip','route','del','default','dev','rmnet0','metric','700')
        if addressed: run('ip','addr','del',address+'/32','dev','rmnet0')
        run('ip','link','set','dev','rmnet0','down')
        (ROOT/'data-network.pid').unlink(missing_ok=True)
        os.close(cp); os.close(lock)
        print('Cellular fallback configuration removed',flush=True)


if __name__=='__main__': main()
