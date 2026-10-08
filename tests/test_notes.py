import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1] / 'overlay/mobile'
spec = importlib.util.spec_from_file_location('notes', ROOT / 'notes.py')
notes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notes)


class NotesTests(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.TemporaryDirectory()
        self.addCleanup(self.home.cleanup)
        patch = mock.patch.object(notes.Path, 'home', return_value=Path(self.home.name))
        patch.start()
        self.addCleanup(patch.stop)
        self.folder = Path(self.home.name) / 'Notes'

    def test_a_new_note_is_named_after_its_first_line(self):
        saved = notes.save('-', '# Shopping\nmilk\neggs\n')
        self.assertEqual(saved, {'ok': True, 'id': 'Shopping.md'})
        self.assertEqual((self.folder / 'Shopping.md').read_text(), '# Shopping\nmilk\neggs\n')
        self.assertEqual(notes.save('-', 'Shopping\nbread')['id'], 'Shopping (2).md')

    def test_an_empty_new_note_is_not_saved(self):
        self.assertEqual(notes.save('-', '  \n'), {'ok': True, 'id': ''})
        self.assertFalse(self.folder.exists() and any(self.folder.iterdir()))

    def test_a_changed_title_renames_the_file_and_keeps_its_suffix(self):
        notes.save('-', 'Ideas\none')
        self.assertEqual(notes.save('Ideas.md', 'Better ideas\none\ntwo')['id'], 'Better ideas.md')
        self.assertFalse((self.folder / 'Ideas.md').exists())
        (self.folder / 'plain.txt').write_text('old title\n')
        self.assertEqual(notes.save('plain.txt', 'New title\n')['id'], 'New title.txt')
        # The same title keeps the name.
        self.assertEqual(notes.save('New title.txt', 'New title\nmore')['id'], 'New title.txt')

    def test_titles_become_safe_file_names(self):
        self.assertEqual(notes.file_name('a/b: c?'), 'a b c.md')
        self.assertEqual(notes.file_name('..hidden'), 'hidden.md')
        self.assertTrue(notes.file_name('', now=0).startswith('Note '))
        self.assertEqual(len(notes.file_name('x' * 200)), 63)

    def test_list_searches_titles_and_text_newest_first(self):
        notes.save('-', 'Alpha\napple pie')
        notes.save('-', 'Beta\nbanana')
        os.utime(self.folder / 'Alpha.md', (1, 1))
        (self.folder / '.hidden.md').write_text('secret')
        (self.folder / 'picture.png').write_bytes(b'png')
        listed = notes.list_notes()['notes']
        self.assertEqual([n['id'] for n in listed], ['Beta.md', 'Alpha.md'])
        self.assertEqual(listed[1]['preview'], 'apple pie')
        self.assertEqual([n['id'] for n in notes.list_notes('APPLE')['notes']], ['Alpha.md'])

    def test_only_notes_in_the_folder_are_touched(self):
        notes.save('-', 'Real\n')
        outside = Path(self.home.name) / 'elsewhere.md'
        outside.write_text('x')
        (self.folder / 'link.md').symlink_to(outside)
        for bad in ('../elsewhere.md', '.trash', 'link.md', 'Real', '', 'a/b.md'):
            with self.assertRaises(ValueError):
                notes.read(bad)
        with self.assertRaises(ValueError):
            notes.delete('link.md')
        self.assertTrue(outside.exists())

    def test_delete_moves_the_note_to_the_trash(self):
        notes.save('-', 'Gone\n')
        notes.delete('Gone.md')
        self.assertFalse((self.folder / 'Gone.md').exists())
        self.assertTrue((self.folder / '.trash/Gone.md').exists())
        notes.save('-', 'Gone\nagain')
        notes.delete('Gone.md')
        self.assertEqual(len(list((self.folder / '.trash').iterdir())), 2)

    def test_the_command_line_prints_json(self):
        with mock.patch('sys.stdin') as stdin, mock.patch('builtins.print') as printed:
            stdin.read.return_value = 'Hello\nworld'
            self.assertEqual(notes.main(['save', '-']), 0)
            self.assertEqual(json.loads(printed.call_args[0][0])['id'], 'Hello.md')
            self.assertEqual(notes.main(['read', '../x.md']), 1)


if __name__ == '__main__':
    unittest.main()
