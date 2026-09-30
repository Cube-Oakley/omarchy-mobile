# SPDX-License-Identifier: MIT
"""One fresh-CP Tello registration sequence, using the app's IPC0 owner.

No retransmission of SMS/calls and no warm CP restart. A failed or uncertain
setup returns to the supervisor for ordered power-off.
"""
import importlib.util
import os
from pathlib import Path
import re
import time

from data_call import initial_attach, provision_default_profile, activate
from ims import register
from radio_settings import nr_mode


def sim_ready(payload):
    if len(payload) < 3 or payload[2] > 8 or len(payload) < 3 + payload[2] * 63:
        raise ValueError('Unexpected SIM status layout')
    return any(
        payload[i:i + 2] == bytes([2, 5])
        for i in range(3, 3 + payload[2] * 63, 63))


def packet_registered(payload, *, lte=False):
    return len(payload) >= 4 and payload[0] in (1, 5) and (not lte or payload[3] == 14)


def radio(enabled):
    spec = importlib.util.spec_from_file_location('pixel_at', Path(__file__).with_name('at-lab.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    fd = os.open('/dev/umts_router', os.O_RDWR | os.O_NONBLOCK | os.O_CLOEXEC)
    try:
        response = module.exchange(fd, 'AT+CFUN=' + str(int(enabled)), 25)
        if not re.search(r'(?:^|[\r\n])OK(?:[\r\n]|$)', response):
            raise RuntimeError('Radio command was not accepted')
    finally:
        os.close(fd)


def wait_for(service, predicate, seconds, stopped):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if stopped():
            raise InterruptedError('Modem startup stopped')
        service.pump()
        try:
            ready = predicate()
        except TimeoutError:
            # CP may briefly stop answering status requests while its SIM
            # application initializes. Only these read-only polls retry.
            ready = False
        if ready:
            service.pump()
            return
        # Receive and durably handle SMS throughout startup. Calls never
        # auto-answer; the same Service retains them for the application UI.
        until = time.monotonic() + 1
        while time.monotonic() < until:
            if stopped():
                raise InterruptedError('Modem startup stopped')
            packet = service.client.next_indication(.1)
            if packet:
                service.event(packet)
    raise TimeoutError('Modem registration deadline exceeded')


def start(service, stopped=lambda: False):
    client = service.client
    previous = None
    def usim_ready():
        nonlocal previous
        payload = client.request(0x0200)
        ready = sim_ready(payload)
        state = (payload[0], tuple((payload[i], payload[i + 1])
                 for i in range(3, 3 + payload[2] * 63, 63)))
        if state != previous:
            print('SIM card state and application type/state:', state, flush=True)
            previous = state
        return ready
    print('Startup: waiting for physical USIM ready', flush=True)
    wait_for(service, usim_ready, 120, stopped)
    radio(False)
    initial_attach(client, 'wholesale')
    provision_default_profile(client)
    service.pump()
    radio(True)
    print('Startup: waiting for packet registration', flush=True)
    wait_for(service, lambda: packet_registered(client.request(0x0701)), 180, stopped)
    nr_mode(client)
    wait_for(service, lambda: packet_registered(client.request(0x0701), lte=True), 120, stopped)
    activate(client)
    service.pump()
    client.request(0x0d39, b'\x01\x01', timeout=30)
    activate(client, 'ims')
    register(client)
    print('Startup: waiting for IMS voice and SMS registration', flush=True)
    wait_for(service, lambda: service.registration().get('state') == 2 and
             service.registration().get('features', 0) & 3 == 3, 120, stopped)
    print('Startup: LTE, IMS voice and SMS registered', flush=True)
