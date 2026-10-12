# SPDX-License-Identifier: MIT
from pathlib import Path
import subprocess
import tempfile
import unittest
from data_network import Resolver, configuration, counters, guarded, looks_silent

class ResolverTests(unittest.TestCase):
    def test_fallback_restoration_preserves_new_wifi_dns(self):
        with tempfile.TemporaryDirectory() as temp:
            p=Path(temp)/'resolv'; p.write_text('wifi original\n'); r=Resolver(p)
            r.update(True,['192.0.2.53']); self.assertIn('192.0.2.53',p.read_text())
            r.update(False,[]); self.assertEqual(p.read_text(),'wifi original\n')
            r.update(True,['192.0.2.53']); p.write_text('wifi new\n')
            r.update(False,[]); self.assertEqual(p.read_text(),'wifi new\n')
    def test_rejects_inactive_or_wrong_bearer(self):
        for data in (b'',bytes(132),b'\0\0\2\1\3'+bytes(127)):
            with self.assertRaises(ValueError): configuration(data)

class RouteTests(unittest.TestCase):
    def test_counters_read_ip_json(self):
        stats=[{'stats64':{'rx':{'bytes':0,'packets':0},'tx':{'bytes':900,'packets':12}}}]
        self.assertEqual(counters(stats),(0,12))
    def test_sending_without_receiving_looks_silent(self):
        self.assertTrue(looks_silent(True,(0,10),(0,14)))
    def test_receiving_idle_or_unrouted_is_not_silent(self):
        self.assertFalse(looks_silent(True,(100,10),(900,40)))
        self.assertFalse(looks_silent(True,(0,10),(0,10)))
        self.assertFalse(looks_silent(False,(0,10),(0,40)))

class WatchTests(unittest.TestCase):
    def test_a_stalled_or_failed_ip_is_retried(self):
        def stalled(): raise subprocess.TimeoutExpired(('ip','route'),20)
        def failed(): raise RuntimeError('Cellular network operation failed: ip')
        def garbled(): raise ValueError('Expecting value')
        for step in (stalled,failed,garbled):
            self.assertIsNotNone(guarded(step))
        self.assertIsNone(guarded(lambda: None))
    def test_other_errors_still_end_the_service(self):
        def broken(): raise KeyError('dev')
        with self.assertRaises(KeyError): guarded(broken)
