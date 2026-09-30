import base64
import csv
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1] / 'overlay/mobile'
# Enough of an image for the helper, which only reads the first bytes.
JPEG = b'\xff\xd8\xff\xe0\x00\x10JFIF test jpeg\xff\xd9'
PNG = b'\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR test png'


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def crlf(text):
    return text.replace('\n', '\r\n')


def unfold(data):
    text = data.decode() if isinstance(data, bytes) else data
    return text.replace('\r\n ', '').split('\r\n')


def csv_text(header, *rows):
    out = io.StringIO()
    writer = csv.writer(out)
    writer.writerow(header)
    writer.writerows(rows)
    return out.getvalue()


SMITH = crlf(r'''BEGIN:VCARD
VERSION:3.0
PRODID:-//Apple Inc.//iPhone OS 17.0//EN
FN:Smith\, John
N:Smith;John;;;
item1.TEL;type=pref:+1 (503) 555-0100
item1.X-ABLabel:_$!<Mobile>!$_
item2.EMAIL;type=INTERNET:john@example.com
item2.X-ABLabel:_$!<Work>!$_
ORG:Acme\; Inc.;Research
NOTE:Line one\nLine two\, with comma\\ and backslash\; semi
X-SOCIALPROFILE;type=twitter:https://twitter.com/john
CATEGORIES:Friends,starred
item3.X-ABDATE;type=pref:2010-06-01
item3.X-ABLabel:_$!<Anniversary>!$_
ADR;TYPE=HOME:;;12 Elm St\nApt 4;Portland;OR;97201;USA
END:VCARD
''')


def android21():
    photo = base64.b64encode(JPEG).decode()
    first = crlf(
        'BEGIN:VCARD\n'
        'VERSION:2.1\n'
        'N;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:M=C3=BCller;J=C3=BCrgen;;;\n'
        # A soft line break in the middle of one UTF-8 character.
        'FN;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:J=C3=BCrgen M=C3=\n'
        '=BCller\n'
        'TEL;CELL;VOICE:+49 170 1234567\n'
        'TEL;HOME:030/1234567\n'
        'TITLE:Head of\n'
        ' Engineering\n'
        'NOTE;ENCODING=QUOTED-PRINTABLE;CHARSET=ISO-8859-1:Caf=E9 on the=0D=0Acorner, =\n'
        'second floor\n'
        'X-ANDROID-CUSTOM:vnd.android.cursor.item/relation;Bob;1;;;;;;;;;;;;;\n'
        'X-NICK;ENCODING=QUOTED-PRINTABLE;CHARSET=UTF-8:J=C3=BCrgi\n'
        f'PHOTO;JPEG;ENCODING=BASE64:{photo[:12]}\n'
        f'  {photo[12:]}\n'
        '\n'
        'END:VCARD\n').encode()
    second = crlf(
        'BEGIN:VCARD\n'
        'VERSION:2.1\n'
        'N;CHARSET=ISO-8859-1:Br\xe4u;Ren\xe9;;;\n'
        'ORG;CHARSET=ISO-8859-1:Br\xe4uhaus\n'
        'TEL;WORK;FAX:+49 30 555\n'
        'END:VCARD\n').encode('latin-1')
    return first + second


class ContactsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.saved = dict(os.environ)
        for name in ('state', 'data', 'cache', 'home'):
            (self.base / name).mkdir()
        os.environ.update(XDG_STATE_HOME=str(self.base / 'state'), XDG_DATA_HOME=str(self.base / 'data'),
                          XDG_CACHE_HOME=str(self.base / 'cache'), HOME=str(self.base / 'home'))
        os.environ.pop('OMARCHY_MOBILE_APP', None)
        self.home = self.base / 'home'
        self.folder = self.base / 'data/omarchy-mobile/contacts'
        self.cache = self.base / 'cache/omarchy-mobile/contacts'
        self.contacts = load('contacts')

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self.saved)
        self.temporary.cleanup()

    def run_main(self, *args, stdin=None):
        with patch('sys.stdout', new_callable=io.StringIO) as out, \
                patch('sys.stderr', new_callable=io.StringIO), \
                patch('sys.stdin', io.StringIO(stdin if stdin is not None else '')):
            code = self.contacts.main(list(args))
        text = out.getvalue()
        return code, json.loads(text) if text else None

    def write(self, name, data):
        path = self.home / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data if isinstance(data, bytes) else data.encode())
        return path

    def imported(self, name, data):
        code, result = self.run_main('import', str(self.write(name, data)))
        self.assertEqual(code, 0, result)
        return result

    def saved_contact(self, data):
        code, result = self.run_main('save', stdin=json.dumps(data))
        self.assertEqual(code, 0, result)
        return result['contact']

    def by_name(self):
        return {card['name']: card for card in (self.contacts.full(c) for c in self.contacts.load_all())}

    def card_text(self, uid):
        return unfold((self.folder / f'{uid}.vcf').read_bytes())

    def test_vcard_21_quoted_printable_charsets_and_folding(self):
        self.assertEqual(self.imported('android.vcf', android21()),
                         {'ok': True, 'added': 2, 'skipped': 0, 'failed': 0})
        found = self.by_name()
        jurgen = found['Jürgen Müller']
        self.assertEqual((jurgen['given'], jurgen['family']), ('Jürgen', 'Müller'))
        self.assertEqual(jurgen['phones'], [{'type': 'mobile', 'value': '+49 170 1234567'},
                                            {'type': 'home', 'value': '030 1234567'}])
        self.assertEqual(jurgen['title'], 'Head of Engineering')
        self.assertEqual(jurgen['note'], 'Café on the\ncorner, second floor')
        self.assertEqual(jurgen['initials'], 'JM')
        self.assertTrue(jurgen['photo'].endswith('.jpg'))
        self.assertEqual(Path(jurgen['photo']).read_bytes(), JPEG)
        lines = self.card_text(jurgen['uid'])
        self.assertIn('X-ANDROID-CUSTOM:vnd.android.cursor.item/relation;Bob;1;;;;;;;;;;;;;', lines)
        self.assertIn('X-NICK:Jürgi', lines, 'quoted-printable is stored as plain UTF-8')
        self.assertIn('VERSION:3.0', lines)
        rene = found['René Bräu']
        self.assertEqual(rene['org'], 'Bräuhaus')
        self.assertEqual(rene['phones'], [{'type': 'fax', 'value': '+49 30 555'}])
        raw = (self.folder / f"{rene['uid']}.vcf").read_bytes()
        self.assertIn('Bräuhaus'.encode(), raw, 'written as UTF-8')
        self.assertIn(b'\r\n', raw)
        self.assertEqual(stat.S_IMODE((self.folder / f"{rene['uid']}.vcf").stat().st_mode), 0o600)

    def test_vcard_30_groups_labels_and_escapes(self):
        self.imported('smith.vcf', SMITH)
        smith = self.by_name()['Smith, John']
        self.assertEqual(smith['phones'], [{'type': 'mobile', 'value': '+1 (503) 555-0100'}])
        self.assertEqual(smith['emails'], [{'type': 'work', 'value': 'john@example.com'}])
        self.assertEqual(smith['org'], 'Acme; Inc.')
        self.assertEqual(smith['note'], 'Line one\nLine two, with comma\\ and backslash; semi')
        self.assertEqual(smith['addresses'], [{'type': 'home', 'street': '12 Elm St\nApt 4', 'city': 'Portland',
                                               'region': 'OR', 'postcode': '97201', 'country': 'USA'}])
        self.assertTrue(smith['favorite'])
        self.assertEqual(smith['sort'], 'smith john')
        lines = self.card_text(smith['uid'])
        self.assertIn('FN:Smith\\, John', lines)
        self.assertIn('ORG:Acme\\; Inc.;Research', lines)
        self.assertIn('CATEGORIES:Friends,starred', lines)
        self.assertIn('X-SOCIALPROFILE;type=twitter:https://twitter.com/john', lines)
        self.assertIn('item3.X-ABDATE;type=pref:2010-06-01', lines)
        self.assertIn('item3.X-ABLabel:_$!<Anniversary>!$_', lines)
        self.assertFalse([line for line in lines if line.startswith(('item1.', 'item2.', 'PRODID:-//Apple'))])

    def test_vcard_40_data_uri_photo_and_several_cards(self):
        png = base64.b64encode(PNG).decode()
        data = crlf(
            'BEGIN:VCARD\n'
            'VERSION:4.0\n'
            'UID:urn:uuid:4fbe8971-0bc3-424c-9c26-36c3e1eff6b1\n'
            'FN:Ada Lovelace\n'
            'N:Lovelace;Ada;;;\n'
            'TEL;VALUE=uri;TYPE="cell,voice":tel:+1-503-555-0101\n'
            'EMAIL;TYPE=work:ada@example.org\n'
            'BDAY:--1210\n'
            f'PHOTO:data:image/png;base64,{png}\n'
            'GENDER:F\n'
            'END:VCARD\n'
            'BEGIN:VCARD\n'
            'VERSION:3.0\n'
            'UID:not a safe/name\n'
            'FN:Remote Photo\n'
            'PHOTO;VALUE=uri:https://example.com/p.jpg\n'
            'END:VCARD\n'
            'BEGIN:VCARD\n'
            'VERSION:4.0\n'
            'KIND:group\n'
            'FN:Book club\n'
            'END:VCARD\n')
        self.assertEqual(self.imported('four.vcf', data), {'ok': True, 'added': 2, 'skipped': 1, 'failed': 0})
        found = self.by_name()
        ada = found['Ada Lovelace']
        self.assertEqual(ada['uid'], '4fbe8971-0bc3-424c-9c26-36c3e1eff6b1')
        self.assertEqual(ada['phones'], [{'type': 'mobile', 'value': '+1-503-555-0101'}])
        self.assertEqual(ada['birthday'], '--12-10')
        digest = hashlib.sha1(PNG).hexdigest()[:12]
        self.assertEqual(ada['photo'], str(self.cache / f"{ada['uid']}-{digest}.png"))
        self.assertEqual(Path(ada['photo']).read_bytes(), PNG)
        self.assertIn('GENDER:F', self.card_text(ada['uid']))
        remote = found['Remote Photo']
        self.assertNotEqual(remote['uid'], 'not a safe/name')
        self.assertRegex(remote['uid'], r'^[0-9a-f-]{36}$')
        self.assertEqual(remote['photo'], '', 'a link is never fetched')
        self.assertIn('PHOTO;VALUE=uri:https://example.com/p.jpg', self.card_text(remote['uid']))

    def test_google_csv_with_multiple_values_and_star(self):
        header = ['First Name', 'Middle Name', 'Last Name', 'Name Prefix', 'Name Suffix', 'Nickname',
                  'Birthday', 'Notes', 'Organization Name', 'Organization Title', 'Labels',
                  'E-mail 1 - Label', 'E-mail 1 - Value', 'Phone 1 - Label', 'Phone 1 - Value',
                  'Phone 2 - Label', 'Phone 2 - Value', 'Address 1 - Label', 'Address 1 - Street',
                  'Address 1 - City', 'Address 1 - Region', 'Address 1 - Postal Code', 'Address 1 - Country',
                  'Website 1 - Label', 'Website 1 - Value']
        row = ['Grace', 'Brewster', 'Hopper', 'Rear Adm.', '', 'Amazing Grace', '1906-12-09',
               'Wrote the first compiler.\nLiked clocks.', 'US Navy', 'Rear Admiral',
               '* myContacts ::: * starred ::: Pioneers', 'Work', 'grace@navy.example ::: g.hopper@example.com',
               'Mobile', '+1 503-555-0102 ::: +1 503-555-0103', 'Work Fax', '+1 503 555 0104', 'Home',
               '1 Main St', 'Arlington', 'VA', '22201', 'USA', '', 'https://example.com/grace']
        data = csv_text(header, row).encode('utf-8-sig')
        self.assertEqual(self.imported('contacts.csv', data)['added'], 1)
        grace = self.by_name()['Rear Adm. Grace Brewster Hopper']
        self.assertTrue(grace['favorite'])
        self.assertEqual(grace['nickname'], 'Amazing Grace')
        self.assertEqual(grace['phones'], [{'type': 'mobile', 'value': '+1 503-555-0102'},
                                           {'type': 'mobile', 'value': '+1 503-555-0103'},
                                           {'type': 'fax', 'value': '+1 503 555 0104'}])
        self.assertEqual([e['value'] for e in grace['emails']], ['grace@navy.example', 'g.hopper@example.com'])
        self.assertEqual({e['type'] for e in grace['emails']}, {'work'})
        self.assertEqual(grace['addresses'][0]['city'], 'Arlington')
        self.assertEqual(grace['urls'], ['https://example.com/grace'])
        self.assertEqual(grace['birthday'], '1906-12-09')
        self.assertEqual(grace['note'], 'Wrote the first compiler.\nLiked clocks.')
        self.assertEqual((grace['org'], grace['title']), ('US Navy', 'Rear Admiral'))
        self.assertEqual(grace['initials'], 'GH')
        self.assertIn('CATEGORIES:Pioneers,starred', self.card_text(grace['uid']))

    def test_old_google_and_outlook_csv(self):
        old = csv_text(
            ['Name', 'Given Name', 'Additional Name', 'Family Name', 'Group Membership', 'E-mail 1 - Type',
             'E-mail 1 - Value', 'Phone 1 - Type', 'Phone 1 - Value', 'Phone 2 - Type', 'Phone 2 - Value',
             'Organization 1 - Type', 'Organization 1 - Name', 'Organization 1 - Title'],
            ['Alan Turing', 'Alan', '', 'Turing', '* My Contacts ::: * Starred', '* Home', 'alan@example.co.uk',
             'Home', '+44 20 7946 0000', 'Mobile', '07700 900000', '', 'Bletchley Park', 'Cryptanalyst'])
        self.assertEqual(self.imported('google.csv', old)['added'], 1)
        outlook = csv_text(
            ['Title', 'First Name', 'Middle Name', 'Last Name', 'Suffix', 'Company', 'Department', 'Job Title',
             'Business Street', 'Business City', 'Business State', 'Business Postal Code',
             'Business Country/Region', 'Business Fax', 'Business Phone', 'Home Phone', 'Mobile Phone',
             'Birthday', 'E-mail Address', 'E-mail 2 Address', 'Notes', 'Web Page'],
            ['Dr.', 'José', '', 'Núñez', '', 'Contoso', 'Research', 'Engineer', '1 Microsoft Way', 'Redmond',
             'WA', '98052', 'United States', '+1 425 555 0105', '+1 425 555 0106', '', '(425) 555-0107',
             '3/15/1985', 'jose@contoso.example', 'jose.n@example.com', 'Works late', 'www.contoso.example'])
        self.assertEqual(self.imported('outlook.csv', outlook.encode('cp1252'))['added'], 1)
        found = self.by_name()
        alan = found['Alan Turing']
        self.assertTrue(alan['favorite'])
        self.assertEqual(alan['emails'], [{'type': 'home', 'value': 'alan@example.co.uk'}])
        self.assertEqual(alan['phones'], [{'type': 'home', 'value': '+44 20 7946 0000'},
                                          {'type': 'mobile', 'value': '07700 900000'}])
        self.assertEqual((alan['org'], alan['title']), ('Bletchley Park', 'Cryptanalyst'))
        jose = found['Dr. José Núñez']
        self.assertEqual((jose['prefix'], jose['given'], jose['family']), ('Dr.', 'José', 'Núñez'))
        self.assertEqual((jose['org'], jose['title']), ('Contoso', 'Engineer'))
        self.assertEqual(sorted((p['type'], p['value']) for p in jose['phones']),
                         [('fax', '+1 425 555 0105'), ('mobile', '(425) 555-0107'), ('work', '+1 425 555 0106')])
        self.assertEqual([e['value'] for e in jose['emails']], ['jose@contoso.example', 'jose.n@example.com'])
        self.assertEqual(jose['birthday'], '1985-03-15')
        self.assertEqual(jose['addresses'][0]['type'], 'work')
        self.assertEqual(jose['addresses'][0]['postcode'], '98052')
        self.assertEqual(jose['note'], 'Works late')
        self.assertEqual(jose['sort'], 'nunez jose')
        self.assertIn('ORG:Contoso;Research', self.card_text(jose['uid']))

    def test_duplicates_are_skipped_and_empty_cards_fail(self):
        self.imported('smith.vcf', SMITH)
        self.assertEqual(self.imported('smith.vcf', SMITH), {'ok': True, 'added': 0, 'skipped': 1, 'failed': 0})
        twice = SMITH + SMITH.replace('555-0100', '555-0109') + SMITH
        empty = crlf('BEGIN:VCARD\nVERSION:3.0\nNOTE:only a note\nEND:VCARD\n')
        self.assertEqual(self.imported('more.vcf', twice + empty),
                         {'ok': True, 'added': 1, 'skipped': 2, 'failed': 1})
        self.assertEqual(len(self.contacts.load_all()), 2)
        # The same numbers written differently are still the same card.
        same = SMITH.replace('+1 (503) 555-0100', '503.555.0100')
        self.assertEqual(self.imported('same.vcf', same)['skipped'], 1)

    def test_file_type_comes_from_content(self):
        self.assertEqual(self.imported('backup.txt', SMITH)['added'], 1)
        self.assertEqual(self.imported('odd.vcf', csv_text(['Given Name', 'Family Name'], ['Ann', 'Lee']))['added'], 1)
        code, result = self.run_main('import', str(self.write('notes.csv', 'just,some\nwords,here\n')))
        self.assertEqual(code, 1)
        self.assertIn('not a contacts file', result['error'])

    def test_save_creates_and_updates_keeping_what_it_does_not_model(self):
        photo = base64.b64encode(JPEG).decode()
        self.imported('smith.vcf', SMITH.replace('END:VCARD', f'PHOTO;ENCODING=b;TYPE=JPEG:{photo}\r\nEND:VCARD'))
        uid = self.contacts.load_all()[0]['uid']
        code, got = self.run_main('get', uid)
        self.assertEqual(code, 0)
        contact = got['contact']
        self.assertEqual(set(contact), {'uid', 'name', 'given', 'family', 'middle', 'prefix', 'suffix', 'nickname',
                                        'org', 'title', 'phones', 'emails', 'addresses', 'urls', 'birthday',
                                        'note', 'favorite', 'photo', 'initials', 'sort'})
        contact['phones'].append({'type': 'home', 'value': '+1 503 555 0111'})
        contact['note'] = 'Changed'
        saved = self.saved_contact(contact)
        self.assertEqual(saved['photo'], contact['photo'], 'the photo stays')
        self.assertEqual(saved['name'], 'Smith, John', 'a name set on its own stays')
        lines = self.card_text(uid)
        self.assertIn('X-SOCIALPROFILE;type=twitter:https://twitter.com/john', lines)
        self.assertIn('CATEGORIES:Friends,starred', lines)
        self.assertIn('ORG:Acme\\; Inc.;Research', lines)
        self.assertTrue([line for line in lines if line.startswith('PHOTO;ENCODING=b;TYPE=JPEG:')])
        self.assertEqual(len(self.contacts.load(uid)['phones']), 2)

        made = self.saved_contact({'given': 'Mary', 'family': 'Major',
                                   'phones': [{'type': 'mobile', 'value': '+1 503 555 0120'}]})
        self.assertEqual(made['name'], 'Mary Major')
        path = self.folder / f"{made['uid']}.vcf"
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        # The shown name was made from the parts, so it follows them.
        made['given'] = 'Maria'
        self.assertEqual(self.saved_contact(made)['name'], 'Maria Major')
        made['name'] = 'Aunt Maria'
        self.assertEqual(self.saved_contact(made)['name'], 'Aunt Maria')
        kept = self.saved_contact({'given': 'Solo'})
        kept.update(given='', phones=[])
        self.assertEqual(self.saved_contact(kept)['name'], 'Solo', 'emptied parts leave the name')
        only_number = self.saved_contact({'phones': [{'type': 'mobile', 'value': '5035550130'}]})
        self.assertEqual(only_number['name'], '(503) 555-0130')
        self.assertEqual(only_number['initials'], '')
        self.assertEqual(self.saved_contact(only_number), only_number)

        bad = [
            {'name': 'X', 'phones': [{'type': 'mobile', 'value': 'call me'}]},
            {'name': 'X', 'phones': [{'type': 'satellite', 'value': '123'}]},
            {'name': 'X', 'emails': [{'type': 'home', 'value': 'a@b@c'}]},
            {'given': 'x' * 201},
            {'name': 'X', 'note': 'n' * 4001},
            {'name': 'X', 'phones': [{'type': 'mobile', 'value': str(n)} for n in range(21)]},
            {'note': 'no name, number or email'},
            {'name': 'X', 'favorite': 'yes'},
            {'name': 'X', 'birthday': '1985-13-01'},
            {'name': 'X', 'given': 42},
            {'uid': 'no-such-contact', 'name': 'X'},
            ['not', 'an', 'object'],
        ]
        for data in bad:
            code, result = self.run_main('save', stdin=json.dumps(data))
            self.assertEqual(code, 1, data)
            self.assertFalse(result['ok'])
        self.assertEqual(self.run_main('save', stdin='{not json')[0], 1)

    def test_favorite_round_trips_through_categories(self):
        self.imported('fam.vcf', crlf('BEGIN:VCARD\nVERSION:3.0\nFN:Pat Kim\nN:Kim;Pat;;;\n'
                                      'CATEGORIES:Family,Work\nEND:VCARD\n'))
        uid = self.contacts.load_all()[0]['uid']
        code, result = self.run_main('favorite', uid, 'on')
        self.assertEqual(code, 0)
        self.assertTrue(result['contact']['favorite'])
        self.assertIn('CATEGORIES:Family,Work,starred', self.card_text(uid))
        self.assertTrue(self.contacts.full(self.contacts.load(uid))['favorite'])
        self.run_main('favorite', uid, 'off')
        self.assertIn('CATEGORIES:Family,Work', self.card_text(uid))
        self.assertFalse(self.contacts.load(uid)['favorite'])
        self.assertEqual(self.run_main('favorite', uid, 'maybe')[0], 2)

    def test_photo_is_cached_by_hash_and_replaced(self):
        uid = self.saved_contact({'name': 'Photo Person'})['uid']
        other = self.saved_contact({'name': 'Other Person'})['uid']
        code, result = self.run_main('set-photo', other, str(self.write('other.png', PNG)))
        self.assertEqual(code, 0, result)
        code, result = self.run_main('set-photo', uid, str(self.write('me.jpg', JPEG)))
        self.assertEqual(code, 0, result)
        first = Path(result['contact']['photo'])
        self.assertEqual(first, self.cache / f"{uid}-{hashlib.sha1(JPEG).hexdigest()[:12]}.jpg")
        self.assertEqual(first.read_bytes(), JPEG)
        self.assertTrue([line for line in self.card_text(uid) if line.startswith('PHOTO;ENCODING=b;TYPE=JPEG:')])
        first.unlink()
        self.assertEqual(self.run_main('get', uid)[1]['contact']['photo'], str(first), 'made again on use')
        self.assertTrue(first.is_file())
        code, result = self.run_main('set-photo', uid, str(self.write('me.png', PNG)))
        second = Path(result['contact']['photo'])
        self.assertEqual(second.suffix, '.png')
        self.assertFalse(first.exists(), 'the old photo leaves the cache')
        code, result = self.run_main('clear-photo', uid)
        self.assertEqual(result['contact']['photo'], '')
        self.assertFalse([p for p in self.cache.iterdir() if p.name.startswith(uid)])
        self.assertFalse([line for line in self.card_text(uid) if line.startswith('PHOTO')])
        self.assertEqual(len([p for p in self.cache.iterdir() if p.name.startswith(other)]), 1)
        self.assertEqual(self.run_main('set-photo', uid, str(self.write('a.gif', b'GIF89a...')))[0], 1)
        big = self.write('big.jpg', JPEG[:4] + bytes(5 * 1024 * 1024))
        code, result = self.run_main('set-photo', uid, str(big))
        self.assertEqual(code, 1)
        self.assertIn('5 MB', result['error'])

    def test_delete_removes_card_and_cached_photo(self):
        uid = self.saved_contact({'name': 'Gone Soon'})['uid']
        self.run_main('set-photo', uid, str(self.write('me.jpg', JPEG)))
        self.assertTrue(list(self.cache.glob(f'{uid}-*')))
        self.assertEqual(self.run_main('delete', uid), (0, {'ok': True}))
        self.assertFalse((self.folder / f'{uid}.vcf').exists())
        self.assertFalse(list(self.cache.glob(f'{uid}-*')))
        self.assertEqual(self.run_main('delete', uid)[0], 1)
        self.assertEqual(self.run_main('get', '../escape')[0], 1)

    def test_export_and_import_round_trip(self):
        self.imported('android.vcf', android21())
        self.imported('smith.vcf', SMITH)
        uid = self.saved_contact({'given': 'Zoë', 'family': 'Ångström', 'birthday': '--02-29',
                                  'phones': [{'type': 'pager', 'value': '*611'}],
                                  'addresses': [{'type': 'other', 'street': 'Kungsgatan 1\nLgh 1102',
                                                 'city': 'Stockholm', 'region': '', 'postcode': '111 43',
                                                 'country': 'Sweden'}],
                                  'urls': ['https://example.se/a,b;c'], 'note': 'Tab\there; comma, done'})['uid']
        self.run_main('set-photo', uid, str(self.write('z.png', PNG)))
        before = {card['uid']: (self.contacts.full(card), card['photo'], card['extra'])
                  for card in self.contacts.load_all()}
        code, result = self.run_main('export', str(self.home / 'Documents/all.vcf'))
        self.assertEqual(code, 0, result)
        self.assertEqual(result['count'], 4)
        exported = self.home / 'Documents/all.vcf'
        self.assertTrue(all(len(line) <= 75 for line in exported.read_bytes().split(b'\r\n')))
        self.assertEqual(self.run_main('export', str(self.home / 'one.vcf'), uid)[1]['count'], 1)

        os.environ['XDG_DATA_HOME'] = str(self.base / 'data2')
        os.environ['XDG_CACHE_HOME'] = str(self.base / 'cache2')
        self.assertEqual(self.imported('Documents/all.vcf', exported.read_bytes())['added'], 4)
        after = {card['uid']: (self.contacts.full(card), card['photo'], card['extra'])
                 for card in self.contacts.load_all()}
        self.assertEqual(set(after), set(before))
        for key, (contact, photo, extra) in before.items():
            again, photo_again, extra_again = after[key]
            self.assertEqual(photo_again, photo)
            self.assertEqual(extra_again, extra)
            contact.pop('photo')
            again.pop('photo')
            self.assertEqual(again, contact)
        # Every contact goes back through save unchanged.
        for card in self.contacts.load_all():
            contact = self.contacts.full(card)
            self.assertEqual(self.saved_contact(contact), contact)

        (self.home / 'notes.txt').write_text('keep me')
        code, result = self.run_main('export', str(self.home / 'notes.txt'))
        self.assertEqual(code, 1)
        self.assertEqual((self.home / 'notes.txt').read_text(), 'keep me')
        self.assertEqual(self.run_main('export', str(exported))[0], 0, 'a .vcf may be replaced')
        self.assertEqual(self.run_main('export', str(self.home / 'x.vcf'), 'missing-uid')[0], 1)

    def test_folding_never_splits_utf8(self):
        name = 'Zoë Ångström-Łukasiewicz 👩🏽‍🔬 née Dvořák ' * 4
        contact = self.saved_contact({'name': name.strip(), 'note': 'Ωμέγα ☕ ' * 60})
        raw = (self.folder / f"{contact['uid']}.vcf").read_bytes()
        for line in raw.split(b'\r\n'):
            self.assertLessEqual(len(line), 75)
            line.decode('utf-8')
        self.assertTrue(any(line.startswith(b' ') for line in raw.split(b'\r\n')))
        again = self.contacts.load(contact['uid'])
        self.assertEqual(self.contacts.display(again), name.strip())
        self.assertEqual(again['note'], ('Ωμέγα ☕ ' * 60).strip())
        self.assertEqual(self.contacts.fold('x' * 200).split('\r\n')[1][0], ' ')

    def test_lookup_matches_numbers_and_follows_changes(self):
        ann = self.saved_contact({'given': 'Ann', 'family': 'Lee',
                                  'phones': [{'type': 'mobile', 'value': '+1 (503) 555-0100'},
                                             {'type': 'other', 'value': '*611'}]})
        bob = self.saved_contact({'name': 'Bob', 'phones': [{'type': 'home', 'value': '555-0100'}]})
        for number in ('5035550100', '+15035550100', '503.555.0100', '+1 503 555 0100', '611'):
            self.assertEqual(self.contacts.lookup(number)['uid'], ann['uid'], number)
        self.assertEqual(self.contacts.lookup('5550100')['uid'], bob['uid'])
        self.assertIsNone(self.contacts.lookup('999'))
        self.assertIsNone(self.contacts.lookup(''))
        self.assertTrue((self.cache / 'index.json').is_file())
        self.assertEqual(set(self.contacts.lookup('5035550100')),
                         {'uid', 'name', 'initials', 'sort', 'favorite', 'photo', 'phones', 'email', 'org'})

        ann['phones'][0]['value'] = '+1 503 555 0199'
        self.saved_contact(ann)
        self.assertEqual(self.contacts.lookup('5035550199')['uid'], ann['uid'])
        self.assertIsNone(self.contacts.lookup('5035550100'))
        self.assertIn('5035550199', self.contacts.index())

        # A sync tool adds a card: the count changes.
        card = (self.folder / f"{bob['uid']}.vcf").read_text()
        (self.folder / 'synced.vcf').write_text(card.replace('555-0100', '+44 20 7946 0000')
                                                .replace(bob['uid'], 'synced'))
        self.assertEqual(self.contacts.lookup('+442079460000')['uid'], 'synced')
        # It edits one in place: the folder time changes.
        (self.folder / 'synced.vcf').write_text(card.replace('555-0100', '+44 20 7946 0001')
                                                .replace(bob['uid'], 'synced'))
        future = time.time_ns() + 5_000_000_000
        os.utime(self.folder, ns=(future, future))
        self.assertEqual(self.contacts.lookup('02079460001')['uid'], 'synced')

        self.assertEqual(self.run_main('lookup', '+15035550199')[1]['contact']['name'], 'Ann Lee')
        self.assertEqual(self.run_main('lookup', '+1 999 999 9999'), (0, {'ok': True, 'contact': None}))

    def test_list_is_sorted_with_summaries(self):
        for data in ({'given': 'Zed', 'family': 'Adams'}, {'given': 'amy', 'family': 'Baker'},
                     {'org': 'Acme Rockets'}, {'given': 'Émile', 'family': 'Zola', 'favorite': True},
                     {'phones': [{'type': 'mobile', 'value': '+1 503 555 0140'}],
                      'emails': [{'type': 'work', 'value': 'x@example.com'}]}):
            self.saved_contact(data)
        code, result = self.run_main('list')
        self.assertEqual(code, 0)
        names = [(item['name'], item['initials'], item['sort']) for item in result['contacts']]
        self.assertEqual(names, [('(503) 555-0140', '', '(503) 555-0140'),
                                 ('Acme Rockets', 'AR', 'acme rockets'),
                                 ('Zed Adams', 'ZA', 'adams zed'),
                                 ('amy Baker', 'AB', 'baker amy'),
                                 ('Émile Zola', 'ÉZ', 'zola emile')])
        self.assertEqual(result['contacts'][0]['email'], 'x@example.com')
        self.assertTrue(result['contacts'][-1]['favorite'])

    def test_apps_need_the_contacts_grant(self):
        apps = self.contacts.load_apps()
        apps.PERMISSIONS['contacts'] = {'title': 'Contacts', 'detail': 'Read and change your contacts.'}
        app = self.base / 'data/omarchy-mobile/apps/contacts'
        app.mkdir(parents=True)
        (app / 'manifest.json').write_text(json.dumps({'id': 'contacts', 'name': 'Contacts',
                                                       'permissions': ['contacts', 'files.home']}))
        path = str(self.write('smith.vcf', SMITH))
        with patch.object(self.contacts, 'load_apps', return_value=apps):
            os.environ['OMARCHY_MOBILE_APP'] = 'contacts'
            code, result = self.run_main('list')
            self.assertEqual(code, 1)
            self.assertIn('not granted', result['error'])
            apps.grant('contacts', 'contacts')
            self.assertEqual(self.run_main('list')[0], 0)
            for args in (('import', path), ('importable',), ('export', str(self.home / 'o.vcf')),
                         ('set-photo', 'x', path)):
                code, result = self.run_main(*args)
                self.assertEqual(code, 1, args)
                self.assertIn('Home folder', result['error'])
            apps.grant('contacts', 'files.home')
            self.assertEqual(self.run_main('import', path)[0], 0)
            self.assertEqual(self.run_main('importable')[0], 0)
            os.environ['OMARCHY_MOBILE_APP'] = 'someone-else'
            self.assertEqual(self.run_main('lookup', '5035550100')[0], 1)
        del os.environ['OMARCHY_MOBILE_APP']
        self.assertEqual(self.run_main('list')[1]['contacts'][0]['name'], 'Smith, John')

    def test_paths_outside_home_are_refused(self):
        outside = self.base / 'outside'
        outside.mkdir()
        (outside / 'card.vcf').write_text(SMITH)
        (outside / 'me.jpg').write_bytes(JPEG)
        (self.home / 'link.vcf').symlink_to(outside / 'card.vcf')
        (self.home / 'away').symlink_to(outside)
        uid = self.saved_contact({'name': 'Somebody'})['uid']
        for args in (('import', str(outside / 'card.vcf')), ('import', str(self.home / 'link.vcf')),
                     ('import', str(self.home / '../outside/card.vcf')), ('import', '/etc/passwd'),
                     ('export', str(outside / 'out.vcf')), ('export', str(self.home / 'away/out.vcf')),
                     ('set-photo', uid, str(outside / 'me.jpg'))):
            code, result = self.run_main(*args)
            self.assertEqual(code, 1, args)
            self.assertIn('home folder', result['error'])
        self.assertFalse((outside / 'out.vcf').exists())

    def test_importable_lists_candidates_newest_first(self):
        now = time.time()
        for name, age in (('Downloads/a.vcf', 30), ('Documents/b.csv', 20), ('c.VCF', 10),
                          ('Documents/deep/d.vcf', 0), ('e.txt', 0), ('.hidden.vcf', 0)):
            path = self.write(name, SMITH)
            os.utime(path, (now - age, now - age))
        code, result = self.run_main('importable')
        self.assertEqual(code, 0)
        self.assertEqual([item['name'] for item in result['files']], ['c.VCF', 'b.csv', 'a.vcf'])
        self.assertEqual(set(result['files'][0]), {'path', 'name', 'size', 'modified'})
        self.assertEqual(result['files'][2]['path'], str((self.home / 'Downloads/a.vcf').resolve()))

    def test_usage(self):
        self.assertEqual(self.run_main(), (2, None))
        self.assertEqual(self.run_main('get'), (2, None))
        self.assertEqual(self.run_main('get', 'missing')[0], 1)


if __name__ == '__main__':
    unittest.main()
