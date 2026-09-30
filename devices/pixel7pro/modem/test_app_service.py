# SPDX-License-Identifier: MIT
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import Mock, MagicMock, patch
import app_service

class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(); self.root=Path(self.temp.name)
        self.paths=patch.object(app_service,'ROOT',self.root); self.paths.start()
        self.db=app_service.database(self.root/'db')
        self.client=Mock(); self.client.next_indication.return_value=None
        self.service=app_service.Service(self.client,self.db)
        self.service.ready=Mock()
    def tearDown(self):
        self.db.close(); self.paths.stop(); self.temp.cleanup()
    def test_send_idempotency_after_success_and_unknown_response(self):
        for i,reply in enumerate((bytes(253),TimeoutError())):
            self.client.request.side_effect=[bytes([1])+bytes(12),reply]
            req=dict(request_id=str(i)*32,number='+12025550123',text='Hello €')
            result=self.service.send_sms(req)
            self.assertEqual(result['state'],'sent' if i==0 else 'unknown')
            before=self.client.request.call_count
            repeated=self.service.send_sms(req)
            self.assertEqual(repeated['state'],result['state'])
            self.assertEqual(self.client.request.call_count,before)
    def test_restart_does_not_retry_pending_sms(self):
        self.db.execute('INSERT INTO outgoing VALUES (?,?,?,?,?)',('a'*32,'digest','sending','',0)); self.db.commit()
        other=app_service.database(self.root/'db')
        self.assertEqual(other.execute('SELECT state FROM outgoing').fetchone()[0],'unknown'); other.close()
    def test_ring_does_not_autoanswer_and_release_restores_audio(self):
        n=b'+12025550123'; body=struct.pack('<HBH',1,7,len(n))+n
        packet=struct.pack('<BBHHH',2,0,0x0d01,8+len(body),0)+body
        self.service.event(packet)
        self.client.request.assert_not_called()
        call=self.service.live()[0]
        self.assertEqual(call['state'],'ringing-in')
        audio=MagicMock(); self.service.audio=audio
        body=struct.pack('<HBBBBBBB',1,7,6,0,0,0,16,0)+bytes(2)
        self.service.event(struct.pack('<BBHHH',2,0,0x0d04,8+len(body),0)+body)
        audio.__exit__.assert_called_once()
        self.assertFalse(self.service.live())

    def test_keypad_only_targets_an_active_owned_call(self):
        call=self.service.new_call('+12025550123','outgoing','active',7)
        self.service.dispatch(dict(action='dtmf',id=call['id'],keys='5#'))
        self.assertEqual([c.args for c in self.client.request.call_args_list],[(0x0d0a,bytes([7,0,53])),(0x0d0a,bytes([7,0,35]))])
        call['state']='terminated'
        with self.assertRaises(RuntimeError): self.service.dispatch(dict(action='dtmf',id=call['id'],keys='5'))

    def test_screen_state_blocks_sleep_during_calls_and_only_caches_success(self):
        self.client.request.side_effect=TimeoutError()
        with self.assertRaises(TimeoutError):
            self.service.dispatch(dict(action='screen-state',on=False))
        self.assertIsNone(self.service.screen_on)
        self.client.request.side_effect=None
        self.service.dispatch(dict(action='screen-state',on=False))
        self.assertEqual([c.args for c in self.client.request.call_args_list[-2:]],
                         [(0x0902,bytes(4)),(0x0928,bytes(4))])
        count=self.client.request.call_count
        self.service.dispatch(dict(action='screen-state',on=False))
        self.assertEqual(count,self.client.request.call_count)
        self.service.new_call('+12025550123','incoming','ringing-in',7)
        with self.assertRaises(RuntimeError):
            self.service.dispatch(dict(action='screen-state',on=False))
        self.service.dispatch(dict(action='screen-state',on=True))
        self.assertEqual([c.args for c in self.client.request.call_args_list[-2:]],
                         [(0x0902,b'\x01\0\0\0'),(0x0928,b'\xff'*4)])
        with self.assertRaises(ValueError):
            self.service.dispatch(dict(action='screen-state',on=2))

    def test_failed_filter_does_not_hide_partial_screen_change(self):
        self.service.screen_on=True
        self.client.request.side_effect=[b'',TimeoutError()]
        with self.assertRaises(TimeoutError):
            self.service.dispatch(dict(action='screen-state',on=False))
        self.assertIsNone(self.service.screen_on)
        self.client.request.side_effect=None
        self.service.dispatch(dict(action='screen-state',on=True))
        self.assertTrue(self.service.screen_on)
        self.client.request.assert_called_with(0x0928,b'\xff'*4,timeout=5)
