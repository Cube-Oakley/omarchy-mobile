import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1] / 'overlay/mobile'


def load():
    spec = importlib.util.spec_from_file_location('dictate', ROOT / 'dictate.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def line(ident, text, complete):
    return SimpleNamespace(line=SimpleNamespace(line_id=ident, text=text, is_complete=complete))


class DictateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        base = Path(self.temporary.name)
        self.saved = dict(os.environ)
        os.environ.update(XDG_DATA_HOME=str(base / 'data'), XDG_CONFIG_HOME=str(base / 'config'))
        self.dictate = load()

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self.saved)
        self.temporary.cleanup()

    def install_model(self, name):
        directory = self.dictate.MODELS / 'download.moonshine.ai/model' / self.dictate.CHOICES[name][0] / 'quantized'
        directory.mkdir(parents=True)
        (directory / self.dictate.COMPLETE).touch()
        return directory

    def events(self, *events):
        out = io.StringIO()
        lines = self.dictate.Lines()
        with contextlib.redirect_stdout(out):
            for event in events:
                lines.event(event)
        return [json.loads(row) for row in out.getvalue().splitlines()], lines

    def test_a_model_counts_once_it_is_complete(self):
        self.assertIsNone(self.dictate.model_dir('tiny'))
        partial = self.dictate.MODELS / 'download.moonshine.ai/model/tiny-streaming-en/quantized'
        partial.mkdir(parents=True)
        (partial / 'encoder.ort').touch()
        self.assertIsNone(self.dictate.model_dir('tiny'))
        (partial / self.dictate.COMPLETE).touch()
        self.assertEqual(self.dictate.model_dir('tiny'), partial)
        self.assertIsNone(self.dictate.model_dir('../tiny'))

    def test_the_chosen_model_must_be_installed(self):
        self.assertEqual(self.dictate.chosen(), 'tiny')
        self.install_model('tiny')
        self.dictate.use('small')
        self.assertEqual(self.dictate.chosen(), 'tiny')      # picked, but not there
        self.install_model('small')
        self.assertEqual(self.dictate.chosen(), 'small')
        self.dictate.use('tiny')
        self.assertEqual(self.dictate.chosen(), 'tiny')
        with self.assertRaises(ValueError):
            self.dictate.use('large')

    def test_status_needs_the_engine_and_a_model(self):
        self.install_model('small')
        self.assertFalse(self.dictate.status()['installed'])
        python = self.dictate.engine_python()
        python.parent.mkdir(parents=True)
        python.touch()
        result = self.dictate.status()
        self.assertTrue(result['installed'])
        self.assertEqual(result['model'], 'small')
        self.assertEqual(result['models']['tiny'], {'installed': False, 'megabytes': 44})

    def test_remove_takes_the_model_away(self):
        self.install_model('tiny')
        self.dictate.remove('tiny')
        self.assertIsNone(self.dictate.model_dir('tiny'))
        with self.assertRaises(ValueError):
            self.dictate.remove('tiny')

    def test_phrases_come_once_and_partials_clear(self):
        out, lines = self.events(line(1, 'Ever', False), line(1, 'Ever tried', False), line(1, 'Ever tried', False),
                                 line(1, 'Ever tried.', True), line(1, 'Ever tried.', True),
                                 line(2, '', False), line(2, 'Ever', False))
        self.assertEqual(out, [{'partial': 'Ever'}, {'partial': 'Ever tried'}, {'line': 'Ever tried.'},
                               {'partial': ''}, {'partial': 'Ever'}])
        flushed = io.StringIO()
        with contextlib.redirect_stdout(flushed):
            lines.flush(SimpleNamespace(lines=[SimpleNamespace(line_id=1, text='Ever tried.', is_complete=True),
                                               SimpleNamespace(line_id=2, text='Ever failed', is_complete=False)]))
        self.assertEqual([json.loads(row) for row in flushed.getvalue().splitlines()],
                         [{'line': 'Ever failed'}, {'partial': ''}])

    def test_listening_ends_after_quiet(self):
        lines = self.dictate.Lines()
        lines.now = self.dictate.QUIET_START - 1
        self.assertFalse(lines.quiet())
        lines.now = self.dictate.QUIET_START + 1
        self.assertTrue(lines.quiet())
        with contextlib.redirect_stdout(io.StringIO()):
            lines.event(line(1, 'Hello', False))
        self.assertFalse(lines.quiet())
        lines.now += self.dictate.QUIET_AFTER + 0.5
        self.assertTrue(lines.quiet())

    def test_doubled_punctuation_is_tidied(self):
        self.assertEqual(self.dictate.tidy('  In short,,  the period.'), 'In short, the period.')
        self.assertEqual(self.dictate.tidy('Wait... what?'), 'Wait... what?')

    def test_listen_without_an_engine_asks_for_setup(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(self.dictate.main(['listen']), 1)
        self.assertEqual(json.loads(out.getvalue()), {'error': 'Voice typing is not installed yet', 'setup': True})


if __name__ == '__main__':
    unittest.main()
