import importlib.util
import io
import json
import os
from pathlib import Path
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

    def test_picture_message_gets_a_placeholder(self):
        added = self.messages.ingest(self.db, [received('/SMS/4', '5035550100', '', data='0605040b8423f0')])
        self.assertIn('picture message', added[0]['body'])

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


if __name__ == '__main__':
    unittest.main()
