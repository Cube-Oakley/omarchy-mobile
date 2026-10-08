import importlib.machinery
import importlib.util
import os
from pathlib import Path
import socket
import tempfile
import threading
import unittest
from unittest import mock

PATH = Path(__file__).resolve().parents[1] / 'devices/pixel7pro/session/lock-secret-auth.py'
loader = importlib.machinery.SourceFileLoader('lock_secret_auth', str(PATH))
spec = importlib.util.spec_from_loader('lock_secret_auth', loader)
auth = importlib.util.module_from_spec(spec)
loader.exec_module(auth)
TRUSTED = auth.trusted_socket


class FakeService:
    """The fingerprint service's answers; records what it was asked."""

    def __init__(self, credential='pin', ok=True, throttled=False, uid=0):
        self.credential, self.ok, self.throttled, self.uid = credential, ok, throttled, uid
        self.asked = []
        self.closed = False

    def __call__(self, path):
        return self

    def peer_uid(self):
        return self.uid

    def ask(self, line, name):
        self.asked.append(line)
        if name == 'status':
            return {'event': 'status', 'credential': self.credential}
        return {'event': 'verified', 'ok': self.ok, 'throttled': self.throttled}

    def close(self):
        self.closed = True


class LockSecretAuthTests(unittest.TestCase):
    def setUp(self):
        patches = [mock.patch.object(auth, 'session_user', return_value='cube'),
                   mock.patch.object(auth, 'trusted_socket', return_value=True)]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def test_the_session_persons_pin_is_checked_by_the_service(self):
        service = FakeService()
        self.assertEqual(auth.check('cube', b'1234', connect=service), (0, None))
        self.assertEqual(service.asked, ['status', 'verify 31323334'])
        self.assertTrue(service.closed)

    def test_anyone_else_is_left_to_the_system(self):
        service = FakeService()
        self.assertEqual(auth.check('root', b'1234', connect=service), (1, None))
        self.assertEqual(service.asked, [])

    def test_without_a_credential_nothing_is_verified(self):
        # The service accepts any secret while none is set.
        service = FakeService(credential='none')
        code, message = auth.check('cube', b'1234', connect=service)
        self.assertEqual(code, 1)
        self.assertIn('Settings', message)
        self.assertEqual(service.asked, ['status'])

    def test_a_wrong_pin_fails_and_throttling_is_explained(self):
        self.assertEqual(auth.check('cube', b'0000', connect=FakeService(ok=False)), (1, None))
        code, message = auth.check('cube', b'0000', connect=FakeService(ok=False, throttled=True))
        self.assertEqual(code, 1)
        self.assertIn('Too many', message)

    def test_a_server_that_is_not_the_service_is_not_asked(self):
        service = FakeService(uid=1001)
        self.assertEqual(auth.check('cube', b'1234', connect=service), (1, None))
        self.assertEqual(service.asked, [])

    def test_an_untrusted_socket_is_not_used(self):
        service = FakeService()
        with mock.patch.object(auth, 'trusted_socket', return_value=False):
            code, _ = auth.check('cube', b'1234', connect=service)
        self.assertEqual(code, 1)
        self.assertEqual(service.asked, [])

    def test_the_token_ends_at_its_nul_and_has_the_services_limit(self):
        self.assertEqual(auth.secret_from(b'1234\0'), b'1234')
        self.assertEqual(auth.secret_from(b'pass word\0junk'), b'pass word')
        self.assertIsNone(auth.secret_from(b'\0'))
        self.assertIsNone(auth.secret_from(b'x' * 65 + b'\0'))
        self.assertEqual(auth.secret_from(b'x' * 64 + b'\0'), b'x' * 64)

    def test_the_socket_must_be_the_services_in_a_root_owned_folder(self):
        self.assertFalse(TRUSTED(Path('/nonexistent/fingerprint.sock')))
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'fingerprint.sock'
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(str(path))
                # A socket the user made, in the user's folder.
                self.assertFalse(TRUSTED(path))

    def test_the_client_skips_broadcasts_and_reads_its_answer(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'fingerprint.sock'
            server = socket.socket(socket.AF_UNIX)
            server.bind(str(path))
            server.listen(1)

            def serve():
                conn, _ = server.accept()
                with conn, conn.makefile('rwb') as stream:
                    for line in stream:
                        if line.startswith(b'status'):
                            stream.write(b'{"event":"status","credential":"pin"}\n')
                        else:
                            # Another client's status arrives first, in two pieces.
                            stream.write(b'{"event":"status","cred')
                            stream.flush()
                            stream.write(b'ential":"pin"}\nnot json\n{"event":"verified","ok":true}\n')
                        stream.flush()

            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            service = auth.Service(path)
            try:
                self.assertEqual(service.peer_uid(), os.getuid())
                self.assertEqual(service.ask('status', 'status')['credential'], 'pin')
                self.assertTrue(service.ask('verify 31', 'verified')['ok'])
            finally:
                service.close()
                server.close()


if __name__ == '__main__':
    unittest.main()
