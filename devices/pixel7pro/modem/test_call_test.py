# SPDX-License-Identifier: MIT
import struct
import unittest
import tempfile
from pathlib import Path
from unittest.mock import Mock, MagicMock, patch
import call_test
from call_test import dial_payload, call_status, approved_ring


class CallFramingTests(unittest.TestCase):
    def test_incoming_answers_once_and_restores_audio_on_release(self):
        number = b'+12025550123'
        def packet(ident, body):
            return struct.pack('<BBHHH', 2, 0, ident, 8+len(body), 0) + body
        ring = packet(0x0d01, struct.pack('<HBH', 1, 7, len(number))+number)
        def status(state):
            return packet(0x0d04, struct.pack('<HBBBBBBB', 1, 7, state, 0, 0, 0, 0, 0)+bytes(2))
        client = Mock()
        client.request.return_value = bytes(4)
        client.next_indication.side_effect = [ring, ring, status(0), status(6)]
        audio = MagicMock()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reg = root/'registration.json'
            reg.write_text('{"state":2,"features":3}')
            with patch.object(call_test, 'PRIVATE', root), patch.object(call_test, 'ATTEMPT', root/'call.json'), \
                 patch.object(call_test, 'REGISTRATION', reg), patch.object(call_test, 'Sit', return_value=client), \
                 patch.object(call_test, 'approved_destination', return_value=number.decode()), \
                 patch('voice_audio.VoiceAudio', return_value=audio) as audio_factory, patch('signal.signal'), \
                 patch('sys.argv', ['call_test.py', '--incoming']):
                call_test.main()
            self.assertIn('released', (root/'incoming-call-test-attempt-mic-boost.json').read_text())
        audio_factory.assert_called_once_with(mic_boost_db=6)
        self.assertEqual([c.args[0] for c in client.request.call_args_list], [0x0000, 0x0d02])
        self.assertEqual(client.request.call_args_list[1].args[1], struct.pack('<HHH', 1, 1, 0))
        audio.__enter__.assert_called_once()
        audio.__exit__.assert_called_once()
        client.close.assert_called_once()

    def test_incoming_only_matches_exact_approved_ordinary_caller(self):
        def ring(number, kind=1):
            body = struct.pack('<HBH', kind, 7, len(number)) + number
            return struct.pack('<BBHHH', 2, 0, 0x0d01, 8+len(body), 0) + body
        for number in (b'+12025550123', b'12025550123', b'2025550123'):
            self.assertEqual(approved_ring(ring(number), '+12025550123'), 7)
        for number in (b'2025550124', b'20255501230', b'anonymous'):
            self.assertIsNone(approved_ring(ring(number), '+12025550123'))
        self.assertIsNone(approved_ring(ring(b'2025550123', 2), '+12025550123'))
        with self.assertRaises(ValueError):
            approved_ring(ring(b'2025550123')[:-1], '+12025550123')

    def test_ordinary_voice_only_and_validated_destination(self):
        body = dial_payload('+12025550123')
        self.assertEqual(len(body), 679)
        self.assertEqual(struct.unpack_from('<HBH', body), (1, 0, 12))
        self.assertEqual(body[5:17], b'+12025550123')
        self.assertEqual(body[17:], bytes(662))
        for destination in ('911', '112', '*123#', '+10000000000'):
            with self.assertRaises(ValueError):
                dial_payload(destination)

    def test_call_status_exposes_no_private_fields(self):
        body = struct.pack('<HBBBBBBB', 1, 7, 2, 0, 0, 0, 0, 0) + bytes(2)
        packet = struct.pack('<BBHHH', 2, 0, 0x0d04, 8+len(body), 0)+body
        self.assertEqual(call_status(packet), (1, 7, 2, 0))
        with self.assertRaises(ValueError):
            call_status(packet[:18])
