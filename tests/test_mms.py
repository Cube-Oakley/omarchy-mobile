import http.server
import importlib.util
from pathlib import Path
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1] / 'overlay/mobile'
spec = importlib.util.spec_from_file_location('mms', ROOT / 'mms.py')
mms = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mms)

SENDER = b'+12025550123/TYPE=PLMN\0'
# M-Notification.ind as a carrier sends it (OMA-TS-MMS_ENC 6.2).
NOTIFICATION = (b'\x8c\x82' + b'\x98T5f3a\0' + b'\x8d\x92'
                + b'\x89' + bytes([len(SENDER) + 1]) + b'\x80' + SENDER
                + b'\x96Hello\0' + b'\x8a\x80'
                + b'\x8e\x02\x0b\xb8'                       # 3000 bytes
                + b'\x88\x05\x81\x03\x03\xf4\x80'           # expires in 3 days
                + b'\x83http://mms.example/get?id=42\0')
# A connectionless WSP push: id, type, headers (MMS, X-Wap-Application-Id mms.ua).
PUSH = b'\x01\x06\x03\xbe\xaf\x84' + NOTIFICATION


class MmsTests(unittest.TestCase):
    def test_a_push_holds_a_notification(self):
        message = mms.decode(mms.push(PUSH))
        self.assertEqual(message['message-type'], 'notification-ind')
        self.assertEqual(message['transaction-id'], 'T5f3a')
        self.assertEqual(message['from'], '+12025550123')
        self.assertEqual(message['subject'], 'Hello')
        self.assertEqual(message['message-size'], 3000)
        self.assertEqual(message['content-location'], 'http://mms.example/get?id=42')
        self.assertAlmostEqual(message['expiry'], time.time() + 259200, delta=5)
        self.assertEqual(message['mms-version'], '1.2')

    def test_other_pushes_are_not_mms(self):
        self.assertIsNone(mms.push(b'\x01\x06\x01\xad' + b'<si/>'))      # a service indication
        self.assertIsNone(mms.push(b'\x01\x06'))
        self.assertIsNone(mms.push(b'\x01\x08\x01\xbe'))                  # not a push

    def test_a_short_message_is_an_error(self):
        with self.assertRaises(ValueError):
            mms.decode(NOTIFICATION[:20])

    def test_the_response_to_a_notification(self):
        self.assertEqual(mms.notify_response('T5f3a'), b'\x8c\x83\x98T5f3a\0\x8d\x92\x95\x81\x91\x80')
        self.assertEqual(mms.decode(mms.notify_response('T1', 'deferred', False))['status'], 0x83)

    def send_request(self):
        return mms.send_request(['+12025550199', 'friend@example.com'], 'Hi there',
                                [('My photo.jpg', 'image/jpeg', b'\xff\xd8 jpeg data')], subject='Trip')

    def test_a_sent_message_decodes_as_written(self):
        transaction, request = self.send_request()
        message = mms.decode(request)
        self.assertEqual(message['message-type'], 'send-req')
        self.assertEqual(message['transaction-id'], transaction)
        self.assertEqual(message['to'], ['+12025550199', 'friend@example.com'])
        self.assertEqual(message['subject'], 'Trip')
        self.assertEqual(message['content-type'], 'application/vnd.wap.multipart.related')
        self.assertEqual(message['content-parameters'], {'type': 'application/smil', 'start': '<smil>'})
        smil, photo, text = message['parts']
        self.assertEqual((smil['type'], smil['id'], smil['name']), ('application/smil', 'smil', 'smil.xml'))
        self.assertIn(b'src="My_photo.jpg"', smil['data'])
        self.assertEqual((photo['type'], photo['name'], photo['data']), ('image/jpeg', 'My_photo.jpg', b'\xff\xd8 jpeg data'))
        self.assertEqual(photo['parameters']['name'], 'My_photo.jpg')
        self.assertEqual((text['type'], mms.part_text(text)), ('text/plain', 'Hi there'))
        with self.assertRaises(ValueError):
            mms.send_request([], 'Hi')

    def retrieved(self, subject='Café ☕'):
        _, request = mms.send_request(['+12025550199'], 'Ünïcode text ✓', [('a.png', 'image/png', b'\x89PNG')])
        body = request[request.index(0x84):]
        encoded = b'\xea' + subject.encode() + b'\0'
        return (b'\x8c\x84' + b'\x8d\x92' + b'\x85\x04\x68\x00\x00\x00'
                + b'\x89' + bytes([len(SENDER) + 1]) + b'\x80' + SENDER
                + b'\x97+12025550199/TYPE=PLMN\0' + b'\x97+12025550100/TYPE=PLMN\0'
                + b'\x96' + bytes([len(encoded)]) + encoded + body)

    def test_a_retrieved_message_with_unicode(self):
        message = mms.decode(self.retrieved())
        self.assertEqual(message['message-type'], 'retrieve-conf')
        self.assertEqual(message['date'], 0x68000000)
        self.assertEqual(message['subject'], 'Café ☕')
        self.assertEqual(message['to'], ['+12025550199', '+12025550100'])
        kinds = [part['type'] for part in message['parts']]
        self.assertEqual(kinds, ['application/smil', 'image/png', 'text/plain'])
        self.assertEqual(mms.part_text(message['parts'][2]), 'Ünïcode text ✓')

    def test_transfer_over_http(self):
        seen = {}
        answer = self.retrieved('Hi')

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if not self.path.startswith('/get'):
                    self.send_error(404)
                    return
                seen['get'] = (self.path, self.headers['User-Agent'], self.headers['x-wap-profile'])
                self.send_response(200)
                self.send_header('Content-Type', mms.MMS_TYPE)
                self.end_headers()
                self.wfile.write(answer)

            def do_POST(self):
                seen['post'] = (self.headers['Content-Type'], self.rfile.read(int(self.headers['Content-Length'])))
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'\x8c\x81\x98T1\0\x8d\x92\x92\x80\x8bid-7\0')

            def log_message(self, *args):
                pass

        server = http.server.HTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            base = f'http://127.0.0.1:{server.server_port}'
            config = {'interface': 'lo', 'mmsc': base + '/mms', 'proxy': ''}
            message = mms.retrieve(base + '/get?id=42', config)
            self.assertEqual(message['subject'], 'Hi')
            self.assertEqual(seen['get'], ('/get?id=42', mms.USER_AGENT, mms.UA_PROFILE))
            mms.acknowledge('T5f3a', config)
            self.assertEqual(seen['post'], (mms.MMS_TYPE, mms.notify_response('T5f3a')))
            self.assertEqual(mms.send(['+12025550199'], 'Hi', config=config), 'id-7')
            with self.assertRaises(RuntimeError):
                mms.retrieve(base + '/missing', dict(config, interface=''))
        finally:
            server.shutdown()
            server.server_close()


if __name__ == '__main__':
    unittest.main()
