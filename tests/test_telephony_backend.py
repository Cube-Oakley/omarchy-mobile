import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT=Path(__file__).resolve().parents[1]/'overlay/mobile'

def load(name):
    spec=importlib.util.spec_from_file_location('backend_test_'+name,ROOT/(name+'.py'))
    module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module

class BackendTests(unittest.TestCase):
    def test_phone_routes_app_actions_without_modemmanager(self):
        phone=load('phone'); backend=Mock()
        backend.request.return_value={'call':'123','number':'+12025550123'}
        with patch.object(phone,'device_backend',return_value=backend), patch.object(phone,'mmcli') as mm:
            self.assertTrue(phone.dial('+12025550123')['ok'])
            phone.call_action('accept','123'); phone.call_action('hangup','123')
            phone.audio_set('mute','on')
        mm.assert_not_called()
        self.assertEqual([c.args[0] for c in backend.request.call_args_list],['dial','accept','hangup','audio'])

    def test_unknown_sms_is_not_marked_sent_or_retryable(self):
        messages=load('messages'); backend=Mock()
        class Rejected(RuntimeError): pass
        backend.RequestRejected=Rejected
        with tempfile.TemporaryDirectory() as directory, patch.object(messages,'data_root',return_value=Path(directory)), patch.object(messages,'device_backend',return_value=backend):
            db=messages.database()
            try:
                backend.request.side_effect=RuntimeError('connection lost')
                with self.assertRaises(RuntimeError): messages.send(db,'+12025550123','hello')
                row=db.execute('SELECT * FROM messages').fetchone()
                self.assertEqual(row['state'],'unknown')
                with self.assertRaises(ValueError): messages.retry(db,row['id'])
                backend.request.side_effect = None
                backend.request.return_value = {'state': 'sent', 'error': ''}
                self.assertTrue(messages.follow_delivery(db))
                self.assertEqual(db.execute('SELECT state FROM messages WHERE id=?', (row['id'],)).fetchone()[0], 'sent')
                backend.request.assert_called_with('sms-status', request_id=row['modem'][6:])
                backend.request.side_effect=Rejected('not registered')
                with self.assertRaises(RuntimeError): messages.send(db,'+12025550123','hello again')
                self.assertEqual(db.execute('SELECT state FROM messages ORDER BY id DESC LIMIT 1').fetchone()[0],'failed')
            finally: db.close()
