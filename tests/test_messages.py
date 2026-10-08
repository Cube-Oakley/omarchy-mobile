import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1] / 'overlay/mobile'


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def received(path, number, text, stamp='2026-09-27T00:59:49-07', data=''):
    return {'path': path, 'number': number, 'text': text, 'data': data, 'pdu': 'deliver',
            'state': 'received', 'timestamp': stamp, 'delivery': ''}


class MessagesTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        base = Path(self.temporary.name)
        self.saved = dict(os.environ)
        for name in ('state', 'data', 'config', 'cache', 'run', 'home'):
            (base / name).mkdir()
        os.environ.update(XDG_STATE_HOME=str(base / 'state'), XDG_DATA_HOME=str(base / 'data'),
                          XDG_CONFIG_HOME=str(base / 'config'), XDG_CACHE_HOME=str(base / 'cache'),
                          XDG_RUNTIME_DIR=str(base / 'run'), HOME=str(base / 'home'))
        os.environ.pop('OMARCHY_MOBILE_APP', None)
        app = base / 'data/omarchy-mobile/apps/messages'
        app.mkdir(parents=True)
        (app / 'manifest.json').write_text(json.dumps({'id': 'messages', 'name': 'Messages',
                                                        'permissions': ['messages']}))
        self.messages = load('messages')
        self.deleted = []
        self.calls = []
        self.patches = [
            patch.object(self.messages, 'delete_modem_text', self.deleted.append),
            patch.object(self.messages, 'modem_path', return_value='/org/freedesktop/ModemManager1/Modem/0'),
            patch.object(self.messages, 'contacts', return_value=None),
        ]
        for item in self.patches:
            item.start()
        self.db = self.messages.database()

    def tearDown(self):
        for item in self.patches:
            item.stop()
        self.db.close()
        os.environ.clear()
        os.environ.update(self.saved)
        self.temporary.cleanup()

    def fake_busctl(self, fail_send=False):
        def busctl(*args, timeout=60):
            self.calls.append(args)
            if args[3] == self.messages.MESSAGING and args[4] == 'Create':
                return {'type': 'o', 'data': ['/org/freedesktop/ModemManager1/SMS/9']}
            if args[4] == 'Send':
                if fail_send:
                    raise RuntimeError('Call failed: not registered')
                return {}
            raise AssertionError(args)
        return busctl

    def test_conversations_join_number_formats(self):
        key = self.messages.thread_key
        self.assertEqual(key('+15035550100'), '5035550100')
        self.assertEqual(key('(503) 555-0100'), '5035550100')
        self.assertEqual(key('611'), '611')
        self.assertEqual(key('T-Mobile'), 't-mobile')
        self.assertEqual(self.messages.pretty('+15035550100'), '(503) 555-0100')

    def test_segments_follow_gsm_and_unicode_limits(self):
        seg = self.messages.segments
        self.assertEqual(seg('a' * 160), {'parts': 1, 'left': 0, 'unicode': False})
        self.assertEqual(seg('a' * 161)['parts'], 2)
        self.assertEqual(seg('€' * 80)['parts'], 1, 'extension characters count double')
        self.assertEqual(seg('€' * 81)['parts'], 2)
        self.assertEqual(seg('é' * 70)['unicode'], False)
        self.assertEqual(seg('ł' * 70), {'parts': 1, 'left': 0, 'unicode': True})
        self.assertEqual(seg('ł' * 71)['parts'], 2)
        self.assertEqual(seg('')['parts'], 0)

    def test_received_text_is_filed_once_and_removed_from_the_modem(self):
        texts = [received('/SMS/3', '+15035550100', 'Hi')]
        added = self.messages.ingest(self.db, texts)
        self.assertEqual([m['body'] for m in added], ['Hi'])
        self.assertEqual(self.deleted, ['/SMS/3'])
        self.assertEqual(self.messages.ingest(self.db, texts), [], 'the same text is not filed twice')
        threads = self.messages.threads(self.db)
        self.assertEqual(len(threads), 1)
        self.assertEqual(threads[0]['unread'], 1)
        self.assertEqual(threads[0]['name'], '(503) 555-0100')
        self.assertEqual(threads[0]['time'], 1790495989.0)

    def test_first_import_is_quiet(self):
        self.messages.ingest(self.db, [received('/SMS/1', '5035550100', 'old')], quiet=True)
        self.assertEqual(self.messages.unread(self.db), 0)

    def test_other_binary_messages_get_a_placeholder(self):
        added = self.messages.ingest(self.db, [received('/SMS/4', '5035550100', '', data='0605040b8423f0')])
        self.assertIn('cannot show', added[0]['body'])

    def picture_notice(self, transaction='T5f3a'):
        sender = b'+15035550100/TYPE=PLMN\0'
        notification = (b'\x8c\x82\x98' + transaction.encode() + b'\0\x8d\x92\x89' + bytes([len(sender) + 1])
                        + b'\x80' + sender + b'\x8e\x02\x0b\xb8\x88\x05\x81\x03\x03\xf4\x80'
                        + b'\x83http://mms.example/get?id=42\0')
        return (b'\x01\x06\x03\xbe\xaf\x84' + notification).hex()

    def retrieve_conf(self, text='Look at this'):
        mms = self.messages.mms_module()
        _, request = mms.send_request(['+15035550199'], text, [('cat.jpg', 'image/jpeg', b'\xff\xd8cat')])
        sender = b'+15035550100/TYPE=PLMN\0'
        return (b'\x8c\x84\x8d\x92\x89' + bytes([len(sender) + 1]) + b'\x80' + sender
                + b'\x97+15035550199/TYPE=PLMN\0\x97+15035550142/TYPE=PLMN\0' + request[request.index(0x84):])

    def test_a_picture_message_is_downloaded_then_told_of(self):
        texts = [received('/SMS/5', '5035550100', '', data=self.picture_notice())]
        self.assertEqual(self.messages.ingest(self.db, texts), [], 'no notification before it is here')
        self.assertEqual(self.messages.ingest(self.db, [dict(texts[0], path='/SMS/6')]), [], 'a repeat is ignored')
        pending = self.db.execute('SELECT * FROM messages').fetchall()
        self.assertEqual(len(pending), 1)
        self.assertEqual((pending[0]['kind'], pending[0]['state']), ('mms', 'downloading'))
        self.assertEqual(self.messages.row(pending[0])['preview'], 'Picture message, downloading…')
        mms = self.messages.mms_module()
        calls = []

        def transfer(url, body=None, config=None, timeout=60):
            calls.append((url, body))
            return self.retrieve_conf() if body is None else b''
        with patch.object(mms, 'transfer', transfer), \
                patch.object(mms, 'settings', return_value={'interface': 'rmnet0', 'mmsc': 'http://mmsc', 'proxy': ''}):
            message = self.messages.fetch_mms(pending[0]['id'])
        self.assertEqual(message['state'], 'received')
        self.assertEqual(message['body'], 'Look at this')
        self.assertEqual(message['group'], ['+15035550142', '+15035550199'])
        self.assertEqual([a['name'] for a in message['attachments']], ['cat.jpg'])
        self.assertEqual(Path(message['attachments'][0]['path']).read_bytes(), b'\xff\xd8cat')
        self.assertEqual(calls[0], ('http://mms.example/get?id=42', None))
        self.assertEqual(calls[1], ('http://mmsc', mms.notify_response('T5f3a')))
        folder = self.messages.attachment_folder(message['id'])
        self.assertEqual(self.messages.main(['delete', str(message['id'])]), 0)
        self.assertFalse(folder.exists())

    def test_a_picture_message_that_fails_is_tried_three_times(self):
        self.messages.ingest(self.db, [received('/SMS/7', '5035550100', '', data=self.picture_notice('T9'))])
        ident = self.db.execute('SELECT id FROM messages').fetchone()[0]
        mms = self.messages.mms_module()
        with patch.object(mms, 'transfer', side_effect=OSError('No route')):
            self.assertIsNone(self.messages.fetch_mms(ident))
            self.assertIsNone(self.messages.fetch_mms(ident))
            final = self.messages.fetch_mms(ident)
        self.assertEqual((final['state'], final['preview']), ('download-failed', 'Picture message, not downloaded'))
        downloads = self.messages.Downloads()
        downloads.start(self.db)
        self.assertEqual(downloads.running, set(), 'no fourth try on its own')
        self.assertEqual(self.messages.main(['download', str(ident)]), 0)
        self.assertEqual(self.db.execute('SELECT state FROM messages').fetchone()[0], 'downloading')

    def test_a_picture_message_is_sent_and_kept(self):
        photo = Path(os.environ['HOME']) / 'cat.jpg'
        photo.write_bytes(b'\xff\xd8 a small cat')
        mms = self.messages.mms_module()
        posted = []

        def transfer(url, body=None, config=None, timeout=60):
            posted.append(body)
            return b'\x8c\x81\x98T1\0\x8d\x92\x92\x80\x8bmid-1\0'
        with patch.object(mms, 'transfer', transfer), \
                patch.object(mms, 'settings', return_value={'interface': '', 'mmsc': 'http://mmsc', 'proxy': ''}):
            sent = self.messages.send_mms(self.db, '(503) 555-0100', 'Look', [str(photo)])
        self.assertEqual((sent['state'], sent['kind'], sent['body'], sent['preview']), ('sent', 'mms', 'Look', 'Look'))
        kept = Path(sent['attachments'][0]['path'])
        self.assertEqual(kept.read_bytes(), b'\xff\xd8 a small cat')
        request = mms.decode(posted[0])
        self.assertEqual(request['to'], ['5035550100'])
        self.assertEqual([part['type'] for part in request['parts']], ['application/smil', 'image/jpeg', 'text/plain'])
        self.assertEqual(json.loads(self.db.execute('SELECT mms FROM messages').fetchone()[0])['id'], 'mid-1')

    def test_a_picture_message_without_an_answer_is_uncertain_and_a_failed_one_retried(self):
        photo = Path(os.environ['HOME']) / 'cat.png'
        photo.write_bytes(b'\x89PNG')
        mms = self.messages.mms_module()
        settings = patch.object(mms, 'settings', return_value={'interface': '', 'mmsc': 'http://mmsc', 'proxy': ''})
        settings.start()
        self.addCleanup(settings.stop)
        with patch.object(mms, 'transfer', side_effect=mms.Uncertain('reset')):
            with self.assertRaises(RuntimeError):
                self.messages.send_mms(self.db, '5035550100', '', [str(photo)])
        self.assertEqual(self.db.execute('SELECT state FROM messages').fetchone()[0], 'unknown')
        with patch.object(mms, 'transfer', side_effect=OSError('No route')):
            with self.assertRaises(RuntimeError):
                self.messages.send_mms(self.db, '5035550100', 'again', [str(photo)])
        failed = self.db.execute("SELECT id FROM messages WHERE state = 'failed'").fetchone()[0]
        with patch.object(mms, 'transfer', return_value=b'\x8c\x81\x98T1\0\x8d\x92\x92\x80'):
            self.assertEqual(self.messages.retry(self.db, failed)['state'], 'sent')

    @unittest.skipUnless(shutil.which('ffmpeg'), 'needs ffmpeg')
    def test_a_large_photo_is_made_small_enough(self):
        photo = Path(os.environ['HOME']) / 'big.jpg'
        subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=4000x3000', '-frames:v', '1',
                        '-q:v', '1', str(photo)], check=True)
        self.assertGreater(photo.stat().st_size, 300_000)
        name, kind, data = self.messages.shrink(photo, 300_000, Path(os.environ['HOME']), 0)
        self.assertEqual((name, kind), ('big.jpg', 'image/jpeg'))
        self.assertLessEqual(len(data), 300_000)
        self.assertEqual(data[:2], b'\xff\xd8')
        with self.assertRaises(ValueError):
            self.messages.shrink(photo, 100, Path(os.environ['HOME']), 0)

    def test_send_records_the_modem_text_and_follows_delivery(self):
        with patch.object(self.messages, 'busctl', self.fake_busctl()):
            sent = self.messages.send(self.db, '(503) 555-0100', 'Hello, "friend"; yes')
        self.assertEqual(sent['state'], 'sent')
        create = self.calls[0]
        self.assertIn('Hello, "friend"; yes', create, 'the text is one argument, never quoted')
        self.assertIn('5035550100', create)
        report = {'path': '/org/freedesktop/ModemManager1/SMS/9', 'number': '5035550100', 'text': 'x',
                  'data': '', 'pdu': 'submit', 'state': 'sent', 'timestamp': '', 'delivery': 'completed-received'}
        self.assertTrue(self.messages.follow_delivery(self.db, [report]))
        message = self.messages.thread(self.db, '5035550100')['messages'][0]
        self.assertEqual(message['state'], 'delivered')
        self.assertIn('/org/freedesktop/ModemManager1/SMS/9', self.deleted)

    def test_sent_text_without_a_report_is_let_go(self):
        with patch.object(self.messages, 'busctl', self.fake_busctl()):
            self.messages.send(self.db, '5035550100', 'hello')
        self.db.execute('UPDATE messages SET sent = ?', (time.time() - self.messages.DELIVERY_WAIT - 1,))
        self.db.commit()
        pending = {'path': '/org/freedesktop/ModemManager1/SMS/9', 'number': '5035550100', 'text': 'hello',
                   'data': '', 'pdu': 'submit', 'state': 'sent', 'timestamp': '', 'delivery': 'unknown'}
        self.assertTrue(self.messages.follow_delivery(self.db, [pending]))
        self.assertEqual(self.messages.thread(self.db, '5035550100')['messages'][0]['state'], 'sent')
        self.assertFalse(self.db.execute("SELECT 1 FROM messages WHERE modem != ''").fetchone())

    def test_failed_send_can_be_retried(self):
        with patch.object(self.messages, 'busctl', self.fake_busctl(fail_send=True)):
            with self.assertRaises(RuntimeError):
                self.messages.send(self.db, '5035550100', 'hello')
        message = self.messages.thread(self.db, '5035550100')['messages'][0]
        self.assertEqual(message['state'], 'failed')
        self.assertIn('not registered', message['error'])
        with patch.object(self.messages, 'busctl', self.fake_busctl()):
            again = self.messages.retry(self.db, message['id'])
        self.assertEqual(again['state'], 'sent')
        with self.assertRaises(ValueError):
            self.messages.retry(self.db, message['id'])

    def test_bad_input_is_refused(self):
        with self.assertRaises(ValueError):
            self.messages.send(self.db, 'call me', 'hi')
        with self.assertRaises(ValueError):
            self.messages.send(self.db, '5035550100', '   ')
        with self.assertRaises(ValueError):
            self.messages.send(self.db, '5035550100', 'x' * (self.messages.MAX_TEXT + 1))

    def test_threads_read_and_delete(self):
        self.messages.ingest(self.db, [received('/SMS/1', '5035550100', 'one', '2026-09-27T01:00:00-07'),
                                       received('/SMS/2', '5035550199', 'two', '2026-09-27T02:00:00-07')])
        threads = self.messages.threads(self.db)
        self.assertEqual([t['body'] for t in threads], ['two', 'one'], 'newest first')
        with patch('sys.stdout', new_callable=io.StringIO) as out:
            self.messages.main(['read', '5035550199'])
        self.assertEqual(json.loads(out.getvalue())['unread'], 1)
        with patch('sys.stdout', new_callable=io.StringIO):
            self.messages.main(['delete-thread', '5035550100'])
        self.assertEqual([t['thread'] for t in self.messages.threads(self.db)], ['5035550199'])

    def test_old_sent_texts_are_adopted(self):
        sent = {'path': '/SMS/0', 'number': '+15035550100', 'text': 'from mmcli', 'data': '', 'pdu': 'submit',
                'state': 'sent', 'timestamp': '', 'delivery': ''}
        self.messages.adopt_sent(self.db, [sent])
        self.messages.adopt_sent(self.db, [sent])
        messages = self.messages.thread(self.db, '5035550100')['messages']
        self.assertEqual([(m['direction'], m['body']) for m in messages], [('out', 'from mmcli')])

    def test_app_needs_the_messages_grant(self):
        os.environ['OMARCHY_MOBILE_APP'] = 'messages'
        with patch('sys.stdout', new_callable=io.StringIO) as out:
            self.assertEqual(self.messages.main(['threads']), 1)
        self.assertIn('not granted', out.getvalue())
        self.messages.load_apps().grant('messages', 'messages')
        with patch('sys.stdout', new_callable=io.StringIO) as out:
            self.assertEqual(self.messages.main(['threads']), 0)
        self.assertTrue(json.loads(out.getvalue())['ok'])

    def test_notification_arguments_follow_a_separator(self):
        # Without --, gdbus reads the -1 expiry as an option and posts nothing.
        with patch.object(self.messages.shutil, 'which', return_value='/usr/bin/gdbus'), \
                patch.object(self.messages.subprocess, 'run') as run:
            run.return_value.stdout = '(uint32 7,)\n'
            ident = self.messages.notify({'number': '+442079460123', 'body': 'Hello', 'thread': 't'})
        args = run.call_args.args[0]
        self.assertEqual(args[args.index('--method') + 2], '--')
        self.assertEqual(args[-1], '-1')
        self.assertEqual(ident, 7)

    def test_notification_offers_reply_and_read(self):
        with patch.object(self.messages.shutil, 'which', return_value='/usr/bin/gdbus'), \
                patch.object(self.messages.subprocess, 'run') as run:
            run.return_value.stdout = '(uint32 9,)\n'
            self.messages.notify({'number': '+442079460123', 'body': 'Hello', 'thread': 't'})
        args = run.call_args.args[0]
        self.assertIn("'inline-reply', 'Reply'", args[-3])
        self.assertIn("'read', 'Mark as read'", args[-3])
        self.assertIn("'x-kde-reply-placeholder-text'", args[-2])

    def test_notification_actions_and_replies_are_read(self):
        # The session monitor's lines: Open, Mark as read and an inline reply
        # whose text needs GVariant's escapes.
        monitors = self.messages.Monitors(True)
        read_end, write_end = os.pipe()
        process = type('Process', (), {'stdout': os.fdopen(read_end, 'rb'), 'poll': lambda self: None})()
        monitors.processes['actions'] = process
        monitors.tails['actions'] = ''
        monitors.selector.register(process.stdout, self.messages.selectors.EVENT_READ, 'actions')
        os.write(write_end, (
            "/org/freedesktop/Notifications: org.freedesktop.Notifications.ActionInvoked (uint32 3, 'open')\n"
            "/org/freedesktop/Notifications: org.freedesktop.Notifications.ActionInvoked (uint32 4, 'read')\n"
            "/org/freedesktop/Notifications: org.freedesktop.Notifications.NotificationReplied "
            "(uint32 5, \"it's \\\"fine\\\"\")\n").encode())
        _, opened = monitors.wait(0.3)
        os.close(write_end)
        process.stdout.close()
        self.assertEqual(opened, [(3, 'open'), (4, 'read'), (5, ('reply', 'it\'s "fine"'))])


if __name__ == '__main__':
    unittest.main()
