"""The NFC switch helper, against a stand-in for the enable line."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('nfc', Path(__file__).resolve().parents[1] / 'overlay/mobile/nfc.py')
nfc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nfc)


class NfcTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.env = patch.dict(os.environ, {'XDG_STATE_HOME': str(self.root / 'state')})
        self.env.start()
        self.param = self.root / 'nfc_ven'

    def tearDown(self):
        self.env.stop()
        self.temp.cleanup()

    def test_without_the_line_nfc_is_not_switchable(self):
        self.assertEqual(nfc.status(self.param), {'supported': False, 'on': False, 'saved': False})

    def test_switching_writes_the_line_and_remembers_the_choice(self):
        self.param.write_text('1\n')
        self.assertEqual(nfc.switch(False, self.param), {'supported': True, 'on': False, 'saved': False})
        self.assertEqual(self.param.read_text(), '0\n')
        self.assertEqual(nfc.switch(True, self.param), {'supported': True, 'on': True, 'saved': True})
        self.assertEqual(json.loads(nfc.state_path().read_text()), {'on': True})

    def test_a_refused_write_is_reported_and_still_remembered(self):
        result = nfc.switch(True, self.root / 'missing' / 'nfc_ven')
        self.assertIn('error', result)
        self.assertTrue(nfc.saved())


if __name__ == '__main__':
    unittest.main()
