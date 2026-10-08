import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('assistant',
    Path(__file__).resolve().parents[1] / 'overlay/mobile/assistant.py')
assistant = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assistant)


class Base(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        root = Path(self.dir.name)
        for name, value in (('DATA', root / 'data'), ('DB', root / 'data/assistant.db'),
                            ('CONFIG', root / 'config/assistant.json'), ('MODELS', root / 'data/models'),
                            ('KOKORO', root / 'data/kokoro'), ('RUNTIME', root / 'run'),
                            ('STAMP', root / 'run/used'), ('INHIBIT', root / 'run/inhibit')):
            patcher = patch.object(assistant, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.db = assistant.connect()
        self.addCleanup(self.db.close)


class StorageTests(Base):
    def test_conversations_newest_first_with_last_message(self):
        a = assistant.new_conversation(self.db, 'Trip to Lisbon next spring')
        b = assistant.new_conversation(self.db, 'Battery tips')
        assistant.add_message(self.db, a, 'user', 'Trip to Lisbon next spring')
        assistant.add_message(self.db, b, 'user', 'Battery tips')
        assistant.add_message(self.db, a, 'assistant', 'Lisbon is lovely in April.')
        listed = assistant.list_conversations(self.db)
        self.assertEqual([c['id'] for c in listed], [a, b])
        self.assertEqual(listed[0]['snippet'], 'Lisbon is lovely in April.')

    def test_search_finds_words_in_messages_and_titles(self):
        a = assistant.new_conversation(self.db, 'Trip planning')
        assistant.add_message(self.db, a, 'assistant', 'Pack a raincoat for Lisbon.')
        b = assistant.new_conversation(self.db, 'Recipes')
        assistant.add_message(self.db, b, 'user', 'How do I make bread?')
        self.assertEqual([c['id'] for c in assistant.list_conversations(self.db, 'raincoat')], [a])
        self.assertEqual([c['id'] for c in assistant.list_conversations(self.db, 'lisb')], [a])
        self.assertEqual([c['id'] for c in assistant.list_conversations(self.db, 'recipes')], [b])
        self.assertIn('\x01raincoat\x02', assistant.list_conversations(self.db, 'raincoat')[0]['snippet'])
        # Search syntax from the user is only words.
        self.assertEqual(assistant.list_conversations(self.db, '"bread" (*'), [
            {'id': b, 'title': 'Recipes', 'updated': assistant.list_conversations(self.db)[0]['updated'],
             'snippet': assistant.list_conversations(self.db, 'bread')[0]['snippet']}])

    def test_deleting_a_conversation_deletes_its_messages_and_search(self):
        a = assistant.new_conversation(self.db, 'Secret plans')
        assistant.add_message(self.db, a, 'user', 'surprise party for Sam')
        self.db.execute('DELETE FROM conversations WHERE id = ?', (a,))
        self.db.commit()
        self.assertEqual(assistant.conversation_messages(self.db, a), [])
        self.assertEqual(assistant.list_conversations(self.db, 'surprise'), [])

    def test_titles_are_short(self):
        a = assistant.new_conversation(self.db, 'word ' * 40)
        title = assistant.list_conversations(self.db)[0]['title']
        self.assertLessEqual(len(title), 48)
        self.assertTrue(title.endswith('…'))
        self.assertEqual(assistant.list_conversations(self.db)[0]['id'], a)


class MemoryTests(Base):
    def test_memories_are_kept_once(self):
        self.assertIsNotNone(assistant.add_memory(self.db, 'The user has a dog named Biscuit'))
        self.assertIsNone(assistant.add_memory(self.db, 'the user has a dog named biscuit.'))
        self.assertIsNone(assistant.add_memory(self.db, 'x'))
        self.assertEqual([m['text'] for m in assistant.list_memories(self.db)], ['The user has a dog named Biscuit.'])

    def test_remember_that_is_explicit(self):
        self.assertEqual(assistant.explicit_memory('Remember that my sister is Ana'), 'my sister is Ana')
        self.assertEqual(assistant.explicit_memory('please remember I park on level 3'), 'I park on level 3')
        self.assertEqual(assistant.explicit_memory('Do you remember the movie?'), '')

    def test_memory_candidates_from_the_model(self):
        self.assertEqual(assistant.memory_candidate('NONE'), '')
        self.assertEqual(assistant.memory_candidate('none.'), '')
        self.assertEqual(assistant.memory_candidate('The user lives in Porto.\nMore text'), 'The user lives in Porto.')
        self.assertEqual(assistant.memory_candidate('Sure! The user lives in Porto.'), '')
        self.assertEqual(assistant.memory_candidate('The user is asking about a folklore myth, not a personal fact.'), '')

    def test_only_messages_about_the_user_are_mined(self):
        self.assertTrue(assistant.FIRST_PERSON.search("I'm allergic to peanuts"))
        self.assertTrue(assistant.FIRST_PERSON.search('Our cat is called Miso'))
        self.assertFalse(assistant.FIRST_PERSON.search("In one short sentence, what is a will-o'-the-wisp?"))
        self.assertFalse(assistant.FIRST_PERSON.search('Give me three quick ideas for a rainy Saturday afternoon.'))
        self.assertEqual(assistant.memory_candidate('The user is likely looking for enjoyable, indoor activities.'), '')


class PromptTests(unittest.TestCase):
    def test_system_text_has_the_date_and_memories(self):
        text = assistant.system_prompt([{'text': 'The user is vegetarian.'}], now=1791400000)
        self.assertIn(assistant.NAME, text)
        self.assertIn('2026', text)
        self.assertIn('- The user is vegetarian.', text)

    def test_history_keeps_the_newest_that_fits(self):
        history = [{'role': 'user' if i % 2 == 0 else 'assistant', 'content': f'{i} ' + 'x' * 3000}
                   for i in range(10)]
        messages = assistant.build_messages(history, [], 'hello', now=0)
        self.assertEqual(messages[0]['role'], 'system')
        self.assertEqual(messages[-1], {'role': 'user', 'content': 'hello'})
        kept = [m['content'].split()[0] for m in messages[1:-1]]
        # 12000 characters: three of about 3000, newest first.
        self.assertEqual(kept, ['7', '8', '9'])

    def test_a_picture_goes_with_the_message(self):
        messages = assistant.build_messages([{'role': 'user', 'content': 'earlier', 'image': '/x.jpg'}], [],
                                            '', 'data:image/jpeg;base64,AAAA', now=0)
        self.assertIn('picture', messages[1]['content'])
        self.assertEqual(messages[-1]['content'][0]['type'], 'image_url')
        self.assertEqual(messages[-1]['content'][1]['text'], 'What is in this picture?')


class SettingsTests(Base):
    def test_settings_validate_and_persist(self):
        with patch.object(assistant, 'stop_server') as stop:
            assistant.set_setting('model', 'qwen3-vl-4b')
            stop.assert_called_once()
        assistant.set_setting('speak', 'true')
        self.assertEqual(assistant.settings()['model'], 'qwen3-vl-4b')
        self.assertTrue(assistant.settings()['speak'])
        with self.assertRaises(ValueError):
            assistant.set_setting('model', 'gpt-9')
        with self.assertRaises(ValueError):
            assistant.set_setting('speak', 'yes')
        self.assertEqual(assistant.settings()['voice'], 'kokoro_af_heart')
        assistant.set_setting('voice', 'kokoro_am_puck')
        self.assertEqual(assistant.settings()['voice'], 'kokoro_am_puck')
        with self.assertRaises(ValueError):
            assistant.set_setting('voice', 'kokoro_robot')
        self.assertEqual(oct(assistant.CONFIG.stat().st_mode & 0o777), '0o600')

    def test_the_server_key_is_made_once(self):
        key = assistant.api_key()
        self.assertEqual(len(key), 32)
        self.assertEqual(assistant.api_key(), key)


class SendTests(Base):
    def run_send(self, text, args=(), pieces=('Hello', ' there.')):
        out = io.StringIO()
        with patch.object(assistant, 'ensure_server', return_value=True), \
                patch.object(assistant, 'stream_reply', return_value=iter(pieces)), \
                patch.object(assistant, 'ask_once', return_value='NONE'), \
                patch('sys.stdin', io.StringIO(text)), patch('sys.stdout', out):
            code = assistant.send(list(args))
        return code, [json.loads(line) for line in out.getvalue().splitlines()]

    def test_a_message_and_its_reply_are_kept(self):
        code, lines = self.run_send('Hi there')
        self.assertEqual(code, 0)
        conversation = lines[0]['conversation']
        self.assertEqual([l.get('delta') for l in lines[1:3]], ['Hello', ' there.'])
        self.assertEqual(lines[-1]['text'], 'Hello there.')
        stored = assistant.conversation_messages(self.db, conversation)
        self.assertEqual([(m['role'], m['content']) for m in stored], [('user', 'Hi there'), ('assistant', 'Hello there.')])
        # The phone may sleep again.
        self.assertFalse((assistant.INHIBIT / 'assistant').exists())

    def test_remember_that_is_kept_as_a_memory(self):
        self.run_send('Remember that my locker code is 4412')
        self.assertEqual([m['text'] for m in assistant.list_memories(self.db)],
                         ['The user asked you to remember: my locker code is 4412.'])

    def test_questions_are_not_mined_for_memories(self):
        out = io.StringIO()
        with patch.object(assistant, 'ensure_server', return_value=True), \
                patch.object(assistant, 'stream_reply', return_value=iter(['A marsh light.'])), \
                patch.object(assistant, 'ask_once', return_value='The user likes marshes.') as ask, \
                patch('sys.stdin', io.StringIO('What is a will-o-the-wisp, really?')), patch('sys.stdout', out):
            assistant.send([])
        ask.assert_not_called()
        with patch.object(assistant, 'ensure_server', return_value=True), \
                patch.object(assistant, 'stream_reply', return_value=iter(['Noted.'])), \
                patch.object(assistant, 'ask_once', return_value='The user lives in Porto.'), \
                patch('sys.stdin', io.StringIO('I just moved to Porto last month')), patch('sys.stdout', out):
            assistant.send([])
        self.assertEqual([m['text'] for m in assistant.list_memories(self.db)], ['The user lives in Porto.'])

    def test_a_model_failure_is_reported(self):
        out = io.StringIO()
        with patch.object(assistant, 'ensure_server', side_effect=RuntimeError('no model')), \
                patch('sys.stdin', io.StringIO('hi')), patch('sys.stdout', out):
            self.assertEqual(assistant.send([]), 1)
        self.assertEqual(json.loads(out.getvalue().splitlines()[-1]), {'error': 'no model'})
        self.assertFalse((assistant.INHIBIT / 'assistant').exists())


if __name__ == '__main__':
    unittest.main()
