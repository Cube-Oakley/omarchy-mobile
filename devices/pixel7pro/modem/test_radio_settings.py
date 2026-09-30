# SPDX-License-Identifier: MIT
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
import radio_settings


class NrModeTests(unittest.TestCase):
    def test_saved_original_survives_failure_and_restores_exactly(self):
        class Client:
            value = b'\3'
            fail = True
            def request(self, ident, payload=b'', **kwargs):
                if ident == 0x0749:
                    return self.value
                if self.fail:
                    raise TimeoutError('unknown result')
                self.value = payload
                return b''
        with tempfile.TemporaryDirectory() as directory:
            saved = Path(directory)/'nr'
            with patch.object(radio_settings, 'SAVED_NR', saved):
                client = Client()
                with self.assertRaises(TimeoutError):
                    radio_settings.nr_mode(client)
                self.assertEqual(saved.read_bytes(), b'\3')
                with self.assertRaises(RuntimeError):
                    radio_settings.nr_mode(client)
                client.value = b'\0'
                client.fail = False
                radio_settings.nr_mode(client, restore=True)
                self.assertEqual(client.value, b'\3')
                self.assertFalse(saved.exists())
