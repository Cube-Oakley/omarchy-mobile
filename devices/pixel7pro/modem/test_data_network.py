# SPDX-License-Identifier: MIT
from pathlib import Path
import tempfile
import unittest
from data_network import Resolver, configuration

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
