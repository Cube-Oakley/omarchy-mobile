# SPDX-License-Identifier: MIT
"""Synthetic SMS encoding and duplicate-submission tests; no radio access."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import sms_test


class SmsTests(unittest.TestCase):
    def test_gsm_known_vector_and_international_address(self):
        self.assertEqual(sms_test.pack_text('hello').hex(), 'e8329bfd06')
        self.assertEqual(sms_test.submit_pdu('+12025550123', 'hello').hex(),
                         '01000b912120550521f3000005e8329bfd06')

    def test_rejects_short_numbers_and_unsupported_text(self):
        for number in ('911', '112', '+1911', '2025550123', '+10000000000'):
            with self.assertRaises(ValueError):
                sms_test.submit_pdu(number)
        for message in ('', 'a' * 161, 'test\ntext', 'test €'):
            with self.assertRaises(ValueError):
                sms_test.pack_text(message)

    def test_unknown_outcome_prevents_duplicate_send(self):
        class Client:
            allow_test_sms = True
            submissions = 0
            def request(self, ident, payload=b'', **kwargs):
                if ident in (0x0700, 0x0701):
                    return b'\1\0\0\x15'
                if ident == 0x0200:
                    return b'\1\0\1\2\5' + bytes(61)
                if ident == 0x0100:
                    self.submissions += 1
                    raise TimeoutError('uncertain submission')
                raise AssertionError('unexpected request')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            destination = root / 'recipient.json'
            destination.write_text(json.dumps({'destination': '+12025550123'}))
            destination.chmod(0o600)
            attempt = root / 'attempt.json'
            with patch.object(sms_test, 'PRIVATE', root), patch.object(sms_test, 'DESTINATION', destination), patch.object(sms_test, 'ATTEMPT', attempt):
                client = Client()
                with self.assertRaises(TimeoutError):
                    sms_test.send_test_sms(client)
                self.assertEqual(attempt.stat().st_mode & 0o777, 0o600)
                with self.assertRaises(FileExistsError):
                    sms_test.send_test_sms(client)
                self.assertEqual(client.submissions, 1)

    def test_ims_uses_modem_smsc_and_separate_durable_attempt(self):
        import ims
        import struct
        class Client:
            allow_test_sms = True
            sent = None
            def request(self, ident, payload=b'', **kwargs):
                if ident in (0x0700, 0x0701):
                    return b'\1\0\0\x0e'
                if ident == 0x0200:
                    return b'\1\0\1\2\5' + bytes(61)
                if ident == 0x0108:
                    return bytes([2, 1, 0x81]) + bytes(10)
                if ident == 0x0d1b:
                    self.sent = payload
                    return struct.pack('<I', 77) + bytes(249)
                raise AssertionError('unexpected request')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            destination = root/'recipient.json'
            destination.write_text(json.dumps({'destination': '+12025550123'}))
            destination.chmod(0o600)
            registration = root/'registration.json'
            registration.write_text('{"state":2,"features":3}')
            with patch.object(sms_test, 'PRIVATE', root), patch.object(sms_test, 'DESTINATION', destination), patch.object(sms_test, 'ATTEMPT', root/'attempt.json'), patch.object(ims, 'REGISTRATION', registration):
                client = Client()
                sms_test.send_test_sms(client, ims=True)
                self.assertEqual(len(client.sent), 260)
                self.assertEqual(client.sent[:13], bytes([2, 1, 0x81])+bytes(10))
                n = client.sent[13]
                self.assertEqual(client.sent[14:14+n], sms_test.submit_pdu('+12025550123'))
                self.assertEqual(client.sent[-2:], bytes(2))
                self.assertEqual(json.loads((root/'ims-sms-test-attempt.json').read_text())['cause'], 0)
                with self.assertRaises(FileExistsError):
                    sms_test.send_test_sms(client, ims=True)

    def test_sms_requires_explicit_enable(self):
        with self.assertRaises(RuntimeError):
            sms_test.send_test_sms(object())


if __name__ == '__main__':
    unittest.main()
