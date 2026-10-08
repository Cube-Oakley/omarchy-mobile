import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1] / 'overlay/mobile'


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ShareTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.saved = dict(os.environ)
        os.environ.update(XDG_RUNTIME_DIR=str(self.base / 'run'), HOME=str(self.base / 'home'))
        (self.base / 'run').mkdir()
        (self.base / 'home').mkdir()
        self.share = load('share')

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self.saved)
        self.temporary.cleanup()

    def test_text_that_is_a_link_is_a_link(self):
        request = self.share.make_request('', text='  https://example.com/a  ')
        self.assertEqual(request['kind'], 'url')
        self.assertEqual(request['url'], 'https://example.com/a')
        self.assertEqual(self.share.make_request('', text='hello')['kind'], 'text')
        with self.assertRaises(ValueError):
            self.share.make_request('', text='   ')
        with self.assertRaises(ValueError):
            self.share.make_request('', url='not a link')

    def test_files_are_described_and_must_exist(self):
        photo = self.base / 'home/photo.jpg'
        photo.write_bytes(b'\xff\xd8 jpeg')
        request = self.share.make_request('', files=[str(photo)])
        self.assertEqual(request['files'][0]['mime'], 'image/jpeg')
        self.assertEqual(request['files'][0]['size'], 7)
        with self.assertRaises(ValueError):
            self.share.make_request('', files=[str(self.base / 'missing.png')])

    def test_targets_follow_the_kind(self):
        with patch.object(self.share, 'laptop_connected', return_value=[]):
            url = [t['id'] for t in self.share.targets({'kind': 'url'})]
            text = [t['id'] for t in self.share.targets({'kind': 'text'})]
            files = [t['id'] for t in self.share.targets({'kind': 'files'})]
        self.assertEqual(url, ['copy', 'messages', 'notes', 'browser'])
        self.assertEqual(text, ['copy', 'messages', 'notes'])
        self.assertEqual(files, ['copy', 'downloads'])
        with patch.object(self.share, 'laptop_connected', return_value=[{'id': 'abc', 'name': 'lithium'}]):
            self.assertIn('laptop:abc', [t['id'] for t in self.share.targets({'kind': 'text'})])

    def test_requests_are_kept_and_read_back(self):
        with patch.object(self.share, 'ask_shell', return_value=False) as ask:
            self.assertEqual(self.share.main(['--title', 'Page', '--url', 'https://example.com']), 0)
        ident = ask.call_args.args[0].split()[1]
        request = self.share.load(ident)
        self.assertEqual(self.share.body_text(request), 'Page\nhttps://example.com')
        with self.assertRaises(ValueError):
            self.share.request_path('../escape')

    def test_launch_arguments_survive_the_fifo(self):
        with patch.object(self.share, 'ask_shell', return_value=True) as ask:
            self.share.launch_app('notes', 'note:Share test.md')
        self.assertEqual(ask.call_args.args[0], 'over notes note:Share%20test.md')
        self.assertEqual(len(ask.call_args.args[0].split()), 3)

    def test_downloads_never_overwrite(self):
        source = self.base / 'home/report.pdf'
        source.write_bytes(b'%PDF')
        downloads = self.base / 'home/Downloads'
        downloads.mkdir()
        (downloads / 'report.pdf').write_bytes(b'old')
        request = self.share.make_request('', files=[str(source)])
        self.share.downloads(request)
        self.assertEqual((downloads / 'report.pdf').read_bytes(), b'old')
        self.assertEqual((downloads / 'report (1).pdf').read_bytes(), b'%PDF')


if __name__ == '__main__':
    unittest.main()
