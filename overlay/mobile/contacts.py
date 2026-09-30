#!/usr/bin/env python3
"""Omarchy Mobile contacts, one vCard file per contact.

  list                          every contact as a short summary, sorted
  get UID                       one contact in full
  save                          create or update the JSON contact on stdin
  delete UID
  favorite UID on|off
  set-photo UID PATH            a JPEG or PNG up to 5 MB
  clear-photo UID
  import PATH                   a .vcf (vCard 2.1, 3.0, 4.0) or a Google or Outlook .csv
  export PATH [UID...]          one vCard 3.0 file, photos included
  importable                    .vcf and .csv files in ~/Downloads, ~/Documents and ~
  lookup NUMBER                 the contact with that phone number, or null

Contacts live in ~/.local/share/omarchy-mobile/contacts as <uid>.vcf, vCard
3.0, so a vdir sync tool can share the folder. Properties this helper does not
model stay in the card as they came. Photos stay inside the cards; the shell
shows a decoded copy from ~/.cache/omarchy-mobile/contacts.

Other helpers load this file the way files.py loads app.py, for lookup(number)
and index(). Those two check no grant: the calling helper enforces its own.

When an app runs this (OMARCHY_MOBILE_APP set), its contacts grant is
required, and files.home too for import, importable, export and set-photo.
Their paths must resolve inside the home folder.
"""
import base64
import binascii
import codecs
import csv
import datetime
import hashlib
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import time
import unicodedata
import urllib.parse
import uuid


def load_apps():
    # Installed copies are named omarchy-mobile-app, with no .py suffix.
    # The source tree keeps app.py so tests can import it directly.
    here = Path(__file__).resolve().parent
    for name in ('app.py', 'omarchy-mobile-app'):
        path = here / name
        if not path.is_file():
            continue
        loader = importlib.machinery.SourceFileLoader('omarchy_mobile_apps', str(path))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        module = importlib.util.module_from_spec(spec)
        loader.exec_module(module)
        return module
    raise ImportError('omarchy-mobile-app is not installed next to the contacts helper')


PRODID = '-//Omarchy Mobile//Contacts//EN'
# A UID is also the file name, so only plain names are kept. No leading dot:
# that would hide the file.
SAFE_UID = re.compile(r'[A-Za-z0-9_-][A-Za-z0-9._-]{0,119}')
PHONE = re.compile(r'\+?[0-9*#() .\-]{1,40}')
PHONE_KINDS = ('mobile', 'home', 'work', 'main', 'other', 'fax', 'pager')
PLACE_KINDS = ('home', 'work', 'other')
# TYPE and label words, strongest first: a home fax is a fax, a work mobile a mobile.
PHONE_WORDS = (('fax', 'fax'), ('pager', 'pager'), ('cell', 'mobile'), ('mobile', 'mobile'),
               ('iphone', 'mobile'), ('main', 'main'), ('work', 'work'), ('home', 'home'))
PLACE_WORDS = (('work', 'work'), ('home', 'home'))
TEL_TYPES = {'mobile': 'CELL', 'home': 'HOME', 'work': 'WORK', 'main': 'MAIN',
             'other': 'VOICE', 'fax': 'FAX', 'pager': 'PAGER'}
N_KEYS = ('family', 'given', 'middle', 'prefix', 'suffix')
NAME_KEYS = ('given', 'family', 'middle', 'prefix', 'suffix', 'nickname', 'org', 'title')
ADDRESS_KEYS = ('street', 'city', 'region', 'postcode', 'country')
# Properties regenerated from the model. Apple ties labels to them by group.
GROUPED = {'TEL', 'EMAIL', 'ADR', 'URL'}
BARE_ENCODINGS = {'QUOTED-PRINTABLE', 'BASE64', '8BIT', '7BIT'}
FIELD_LIMIT = 200
NOTE_LIMIT = 4000
URL_LIMIT = 1000
ITEM_LIMIT = 20
PHOTO_LIMIT = 5 * 1024 * 1024
IMPORT_LIMIT = 20 * 1024 * 1024
IMPORT_COUNT = 5000
FILE_ACTIONS = {'import', 'importable', 'export', 'set-photo'}
OUTLOOK_PHONES = {
    'mobile phone': 'mobile', 'home phone': 'home', 'home phone 2': 'home',
    'business phone': 'work', 'business phone 2': 'work', 'company main phone': 'main',
    'primary phone': 'main', 'other phone': 'other', 'car phone': 'other', 'pager': 'pager',
    'business fax': 'fax', 'home fax': 'fax', 'other fax': 'fax', 'callback': 'other',
    "assistant's phone": 'other', 'radio phone': 'other', 'isdn': 'other',
}
CSV_KEYS = {'first name', 'last name', 'given name', 'family name', 'name', 'display name',
            'e-mail 1 - value', 'phone 1 - value', 'e-mail address', 'mobile phone',
            'organization name', 'company'}
HEAD = re.compile(rb'((?:[^:"]|"[^"]*")*):')


def data_root():
    base = os.environ.get('XDG_DATA_HOME')
    return Path(base) if base else Path.home() / '.local/share'


def cache_root():
    base = os.environ.get('XDG_CACHE_HOME')
    return Path(base) if base else Path.home() / '.cache'


def contacts_dir():
    return data_root() / 'omarchy-mobile/contacts'


def cache_dir():
    return cache_root() / 'omarchy-mobile/contacts'


def index_path():
    return cache_dir() / 'index.json'


def contact_path(uid):
    return contacts_dir() / f'{uid}.vcf'


def require_grant(action):
    app_id = os.environ.get('OMARCHY_MOBILE_APP')
    if not app_id:
        return
    apps = load_apps()
    if not apps.has_grant(app_id, 'contacts'):
        raise PermissionError('Contacts access is not granted')
    if action in FILE_ACTIONS and not apps.has_grant(app_id, 'files.home'):
        raise PermissionError('Home folder access is not granted')


def home_path(raw):
    """The real path, refused unless it is inside the home folder."""
    home = os.path.realpath(Path.home())
    path = os.path.realpath(os.path.expanduser(raw))
    if os.path.commonpath([home, path]) != home:
        raise PermissionError('Only files in the home folder can be used')
    return Path(path)


def write_private(path, data):
    """Owner-only, through a temporary file, so no reader sees half a file."""
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    handle, tmp = tempfile.mkstemp(dir=path.parent, prefix='.', suffix='.tmp')
    try:
        with os.fdopen(handle, 'wb') as out:
            out.write(data)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def pretty(number):
    """(503) 555-0100 for North American numbers, as phone.pretty shows them."""
    number = number or ''
    digits = re.sub(r'\D', '', number)
    plain = re.fullmatch(r'[+0-9()\-.\s]+', number) is not None
    if number.startswith('+'):
        nanp = number.startswith('+1') and len(digits) == 11
    else:
        nanp = len(digits) == 10 or (len(digits) == 11 and digits[0] == '1')
    if plain and nanp:
        t = digits[-10:]
        return f'({t[:3]}) {t[3:6]}-{t[6:]}'
    return number or 'Unknown number'


def digits_key(number):
    """The digits lookup compares: the last ten, or all of a shorter number."""
    digits = re.sub(r'[^0-9]', '', number or '')
    return digits[-10:] if len(digits) >= 10 else digits


def label_size(size):
    step = 1024.0
    if size < step:
        return f'{int(size)} B'
    for unit in ('KB', 'MB', 'GB', 'TB'):
        size /= step
        if size < step:
            return f'{size:.1f} {unit}'
    return f'{size:.1f} PB'


# Text and values

def lines_of(text):
    return text.replace('\r\n', '\n').replace('\r', '\n')


def one_line(text):
    return re.sub(r'\s*\n\s*', ' ', lines_of(text)).strip()


def clean(text):
    return re.sub(r'[\x00-\x08\x0b-\x1f\x7f]', '', lines_of(text))


def words(text):
    return re.findall(r'[a-z]+', text.casefold())


def kind_of(found, table):
    found = set(found)
    return next((kind for word, kind in table if word in found), '')


def decode_bytes(raw, charset=''):
    for name in (charset, 'utf-8', 'cp1252'):
        if not name:
            continue
        try:
            return raw.decode(name)
        except (LookupError, UnicodeDecodeError):
            continue
    return raw.decode('latin-1')


def escape(text):
    return (text.replace('\\', '\\\\').replace('\n', '\\n')
            .replace(',', '\\,').replace(';', '\\;'))


def unescape(text):
    return re.sub(r'\\(.)', lambda m: '\n' if m.group(1) in 'nN' else m.group(1), text, flags=re.S)


def split_escaped(text, separator):
    parts, current, escaped = [], [], False
    for char in text:
        if escaped:
            escaped = False
        elif char == '\\':
            escaped = True
        elif char == separator:
            parts.append(''.join(current))
            current = []
            continue
        current.append(char)
    parts.append(''.join(current))
    return parts


def components(text, count):
    parts = [unescape(part) for part in split_escaped(text, ';')] + [''] * count
    return parts[:count]


def tidy_phone(value):
    value = value.strip()
    if value[:4].lower() == 'tel:':
        value = value[4:]
    value = re.sub(r'[  -  　/]', ' ', value)
    value = re.sub(r'[‐-―−]', '-', value)
    return ' '.join(value.split())


def good_email(value):
    local, at, domain = value.partition('@')
    return (at and local and domain and '@' not in domain and len(value) <= 254
            and not re.search(r'\s', value))


def birthday(text):
    """YYYY-MM-DD, or --MM-DD without a year; '' when it is not a date."""
    text = text.strip()
    match = (re.fullmatch(r'(\d{4})-?(\d{2})-?(\d{2})(?:T[\d:.]*(?:Z|[+-][\d:]+)?)?', text)
             or re.fullmatch(r'(--)(\d{2})-?(\d{2})', text))
    if match:
        # Apple writes 1604 for a birthday without a year.
        year = None if match.group(1) in ('--', '0000', '1604') else int(match.group(1))
        month, day = int(match.group(2)), int(match.group(3))
    else:
        # Outlook CSV: M/D/Y, or D/M/Y when the first number cannot be a month.
        match = re.fullmatch(r'(\d{1,2})/(\d{1,2})/(\d{4}|\d{2})', text)
        if not match:
            return ''
        month, day, year = map(int, match.groups())
        if year < 100:
            year += 1900 if year > time.localtime().tm_year % 100 else 2000
        if month > 12:
            month, day = day, month
    try:
        datetime.date(year or 2000, month, day)
    except ValueError:
        return ''
    return f'{year:04d}-{month:02d}-{day:02d}' if year else f'--{month:02d}-{day:02d}'


def image_kind(data):
    if not data or len(data) > PHOTO_LIMIT:
        return ''
    if data.startswith(b'\xff\xd8'):
        return 'jpg'
    if data.startswith(b'\x89PNG'):
        return 'png'
    return ''


def unbase64(raw):
    raw = re.sub(rb'\s+', b'', raw)
    try:
        return base64.b64decode(raw + b'=' * (-len(raw) % 4))
    except (binascii.Error, ValueError):
        return b''


# The contact model

def blank():
    return {'uid': '', 'fn': '', 'given': '', 'family': '', 'middle': '', 'prefix': '',
            'suffix': '', 'nickname': '', 'org': '', 'units': [], 'title': '', 'phones': [],
            'emails': [], 'addresses': [], 'urls': [], 'birthday': '', 'note': '',
            'favorite': False, 'categories': [], 'photo': b'', 'extra': [], 'rev': '',
            'group': False}


def built(card):
    return ' '.join(card[key] for key in ('prefix', 'given', 'middle', 'family', 'suffix') if card[key])


def derived(card):
    if built(card):
        return built(card)
    if card['org']:
        return card['org']
    if card['phones']:
        return pretty(card['phones'][0]['value'])
    return card['emails'][0]['value'] if card['emails'] else ''


def display(card):
    return card['fn'] or derived(card)


def identified(card):
    return bool(card['fn'] or card['given'] or card['family'] or card['org']
                or card['phones'] or card['emails'])


def first_letter(text):
    return next((char.upper()[0] for char in text if char.isalpha()), '')


def initials(card):
    name = display(card)
    if name == built(card) and (card['given'] or card['family']):
        parts = [card['given'], card['family']]
    else:
        parts = name.split()
        parts = parts[:1] + parts[-1:] if len(parts) > 1 else parts
    return ''.join(first_letter(part) for part in parts if part)[:2]


def sort_key(card):
    if card['family'] or card['given']:
        key = f"{card['family']} {card['given']}".strip()
    else:
        key = display(card)
    # Accents go too, so Émile sorts with the E names.
    key = unicodedata.normalize('NFKD', key)
    return ''.join(char for char in key if not unicodedata.combining(char)).casefold()


def fingerprint(card):
    return (display(card).casefold(),
            frozenset(digits_key(item['value']) for item in card['phones']),
            frozenset(item['value'].casefold() for item in card['emails']))


def photo_file(card):
    """The decoded photo in the cache, written on first use."""
    kind = image_kind(card['photo'])
    if not kind:
        return ''
    digest = hashlib.sha1(card['photo']).hexdigest()[:12]
    path = cache_dir() / f"{card['uid']}-{digest}.{kind}"
    if not path.is_file():
        try:
            write_private(path, card['photo'])
            drop_photos(card['uid'], keep=path.name)
        except OSError:
            return ''
    return str(path)


def drop_photos(uid, keep=''):
    pattern = re.compile(re.escape(uid) + r'-[0-9a-f]{12}\.(?:jpg|png)')
    try:
        entries = list(os.scandir(cache_dir()))
    except FileNotFoundError:
        return
    for entry in entries:
        if entry.name != keep and pattern.fullmatch(entry.name):
            try:
                os.unlink(entry.path)
            except FileNotFoundError:
                pass


def full(card):
    item = {'uid': card['uid'], 'name': display(card)}
    item.update({key: card[key] for key in NAME_KEYS})
    item.update({
        'phones': [dict(phone) for phone in card['phones']],
        'emails': [dict(email) for email in card['emails']],
        'addresses': [dict(address) for address in card['addresses']],
        'urls': list(card['urls']),
        'birthday': card['birthday'],
        'note': card['note'],
        'favorite': card['favorite'],
        'photo': photo_file(card),
        'initials': initials(card),
        'sort': sort_key(card),
    })
    return item


def summary(card):
    return {
        'uid': card['uid'],
        'name': display(card),
        'initials': initials(card),
        'sort': sort_key(card),
        'favorite': card['favorite'],
        'photo': photo_file(card),
        'phones': [dict(phone) for phone in card['phones']],
        'email': card['emails'][0]['value'] if card['emails'] else '',
        'org': card['org'],
    }


# Reading vCards

def unfolded(data):
    """Logical lines. 2.1 keeps the folding whitespace; 3.0 and 4.0 drop it.
    A quoted-printable value that ends in = goes on to the next line."""
    lines = []
    keep_space = qp_line = False
    open_qp = False
    for raw in re.split(rb'\r\n|\r|\n', data):
        if lines and open_qp:
            lines[-1][-1] = lines[-1][-1][:-1]
            lines[-1].append(raw)
        elif lines and raw[:1] in (b' ', b'\t'):
            lines[-1].append(raw if keep_space else raw[1:])
        else:
            lines.append([raw])
            mark = raw.strip().upper() if len(raw) < 40 else b''
            if mark == b'BEGIN:VCARD':
                keep_space = False
            elif mark.startswith(b'VERSION:'):
                keep_space = mark == b'VERSION:2.1'
            qp_line = b'QUOTED-PRINTABLE' in raw.split(b':', 1)[0].upper()
        open_qp = qp_line and raw.endswith(b'=')
    return [b''.join(parts) for parts in lines]


class Prop:
    """One content line: group.NAME;params:value, the value still raw bytes."""

    def __init__(self, line, head, value):
        self.line = line
        self.raw = value
        self.tokens = re.findall(r'(?:[^;"]|"[^"]*")+', head.decode('latin-1')) or ['']
        self.group, _, name = self.tokens[0].strip().rpartition('.')
        self.name = name.upper()
        self.params = {}
        for token in self.tokens[1:]:
            key, equals, values = token.partition('=')
            if not equals:
                key, values = ('ENCODING' if token.strip().upper() in BARE_ENCODINGS else 'TYPE'), token
            self.params.setdefault(key.strip().upper(), []).extend(
                value.strip().lower() for value in values.replace('"', '').split(','))

    def param(self, key):
        values = self.params.get(key)
        return values[0] if values else ''

    def types(self):
        return [word for value in self.params.get('TYPE', []) for word in words(value)]

    def quoted_printable(self):
        return self.param('ENCODING') in ('quoted-printable', 'q')

    def text(self):
        raw = binascii.a2b_qp(self.raw) if self.quoted_printable() else self.raw
        return lines_of(decode_bytes(raw, self.param('CHARSET')))

    def verbatim(self):
        """The line as it came, re-encoded as plain UTF-8 when it was not."""
        if not self.quoted_printable() and not self.param('CHARSET'):
            return decode_bytes(self.line)
        keep = [token for token in self.tokens[1:] if not re.fullmatch(
            r'\s*(?:CHARSET=.*|(?:ENCODING=)?QUOTED-PRINTABLE)\s*', token, re.I)]
        return ';'.join([self.tokens[0], *keep]) + ':' + self.text().replace('\n', '\\n')


def parse_line(line):
    match = HEAD.match(line)
    if not match or not match.group(1).strip():
        return None
    return Prop(line, match.group(1), line[match.end():])


def photo_data(prop):
    raw = prop.raw.strip()
    if raw[:5].lower() == b'data:':
        head, _, body = raw.partition(b',')
        data = unbase64(body) if b';base64' in head.lower() else urllib.parse.unquote_to_bytes(body)
    elif prop.param('ENCODING') in ('b', 'base64') and prop.param('VALUE') != 'uri':
        data = unbase64(raw)
    else:
        # A link. Never fetched: the line stays in the card as it is.
        return b''
    return data if image_kind(data) else b''


def add_phone(card, kind, value):
    value = tidy_phone(value)
    if PHONE.fullmatch(value):
        card['phones'].append({'type': kind, 'value': value})
    elif value:
        card['extra'].append(tel_line({'type': kind, 'value': value}))


def add_email(card, kind, value):
    value = value.strip()
    if good_email(value):
        card['emails'].append({'type': kind, 'value': value})
    elif value:
        card['extra'].append(email_line({'type': kind, 'value': value}))


def add_address(card, address):
    if any(address[key] for key in ADDRESS_KEYS):
        card['addresses'].append(address)


def add_url(card, value):
    value = one_line(value)
    if len(value) > URL_LIMIT:
        card['extra'].append(url_line(value))
    elif value:
        card['urls'].append(value)


def take(card, prop, label):
    """Put one property into the model. False leaves it as an unmodelled line."""
    name = prop.name
    if name in ('VERSION', 'PRODID'):
        return True
    if name == 'REV':
        card['rev'] = one_line(prop.text())
        return True
    if name == 'UID':
        value = one_line(prop.text())
        card['uid'] = value[9:] if value[:9].lower() == 'urn:uuid:' else value
        return True
    if name == 'FN' and not card['fn']:
        card['fn'] = one_line(unescape(prop.text()))
        return True
    if name == 'N' and not any(card[key] for key in N_KEYS):
        for key, value in zip(N_KEYS, components(prop.text(), 5)):
            card[key] = one_line(value)
        return True
    if name in ('NICKNAME', 'TITLE') and not card[name.lower()]:
        card[name.lower()] = one_line(unescape(prop.text()))
        return True
    if name == 'ORG' and not card['org']:
        parts = [one_line(part) for part in components(prop.text(), 20)]
        while parts and not parts[-1]:
            parts.pop()
        card['org'], card['units'] = (parts[0], parts[1:]) if parts else ('', [])
        return True
    if name == 'TEL':
        value = tidy_phone(unescape(prop.text()))
        if not PHONE.fullmatch(value):
            return False
        kind = kind_of(prop.types(), PHONE_WORDS) or kind_of(words(label), PHONE_WORDS) or 'other'
        card['phones'].append({'type': kind, 'value': value})
        return True
    if name == 'EMAIL':
        value = unescape(prop.text()).strip()
        value = value[7:] if value[:7].lower() == 'mailto:' else value
        if not good_email(value):
            return False
        kind = kind_of(prop.types(), PLACE_WORDS) or kind_of(words(label), PLACE_WORDS) or 'other'
        card['emails'].append({'type': kind, 'value': value})
        return True
    if name == 'ADR':
        box, extended, street, city, region, postcode, country = components(prop.text(), 7)
        kind = kind_of(prop.types(), PLACE_WORDS) or kind_of(words(label), PLACE_WORDS) or 'other'
        add_address(card, {
            'type': kind,
            'street': '\n'.join(part.strip() for part in (box, extended, street) if part.strip()),
            'city': one_line(city), 'region': one_line(region),
            'postcode': one_line(postcode), 'country': one_line(country),
        })
        return True
    if name == 'URL':
        value = one_line(unescape(prop.text()))
        if len(value) > URL_LIMIT:
            return False
        if value:
            card['urls'].append(value)
        return True
    if name == 'BDAY' and not card['birthday']:
        card['birthday'] = birthday(prop.text())
        return bool(card['birthday'])
    if name == 'NOTE' and not card['note']:
        card['note'] = unescape(prop.text()).strip()
        return True
    if name == 'CATEGORIES':
        for part in split_escaped(prop.text(), ','):
            part = one_line(unescape(part))
            if part.casefold() == 'starred':
                card['favorite'] = True
            elif part and part not in card['categories']:
                card['categories'].append(part)
        return True
    if name == 'PHOTO':
        data = photo_data(prop)
        card['photo'] = card['photo'] or data
        return bool(data)
    if name in ('KIND', 'X-ADDRESSBOOKSERVER-KIND'):
        card['group'] = card['group'] or one_line(prop.text()).lower() == 'group'
    return False


def fit(card):
    """Hold a card to what save accepts. What does not fit stays in the card
    as lines this helper does not show, so nothing is lost."""
    for key in ('fn', *NAME_KEYS):
        card[key] = card[key][:FIELD_LIMIT]
    for address in card['addresses']:
        for key in ADDRESS_KEYS:
            address[key] = address[key][:FIELD_LIMIT]
    if len(card['note']) > NOTE_LIMIT:
        card['extra'].append('NOTE:' + escape(card['note']))
        card['note'] = ''
    for key, line in (('phones', tel_line), ('emails', email_line),
                      ('addresses', adr_line), ('urls', url_line)):
        card['extra'] += [line(item) for item in card[key][ITEM_LIMIT:]]
        del card[key][ITEM_LIMIT:]
    return card


def card_from(lines):
    props = [prop for prop in map(parse_line, lines) if prop]
    card = blank()
    labels = {prop.group.lower(): prop.text() for prop in props
              if prop.group and prop.name == 'X-ABLABEL'}
    modelled, left = set(), []
    for prop in props:
        if take(card, prop, labels.get(prop.group.lower(), '')):
            if prop.group and prop.name in GROUPED:
                modelled.add(prop.group.lower())
        else:
            left.append(prop)
    # A label for a phone or email written again without its group would
    # label nothing, so it goes with the old group.
    card['extra'] += [prop.verbatim() for prop in left
                      if not (prop.group.lower() in modelled and prop.name not in GROUPED)]
    return fit(card)


def vcards(data):
    """Every vCard in the data, or None for one that cannot be read."""
    found = []
    lines = None
    depth = 0
    for line in unfolded(data):
        mark = line.strip().upper() if len(line) < 40 else b''
        if mark == b'BEGIN:VCARD':
            depth += 1
            lines = [] if depth == 1 else lines
        elif mark == b'END:VCARD' and depth:
            depth -= 1
            if depth == 0:
                try:
                    found.append(card_from(lines))
                except (ValueError, LookupError):
                    found.append(None)
        elif depth == 1 and line.strip():
            lines.append(line)
    return found


# Reading CSV exports

def csv_cards(text):
    """Cards from a Google or Outlook export; None when it is neither."""
    first = text.split('\n', 1)[0]
    delimiter = ';' if first.count(';') > first.count(',') else ','
    csv.field_size_limit(IMPORT_LIMIT)
    rows = csv.reader(io.StringIO(text), delimiter=delimiter)
    header = [cell.strip().casefold() for cell in next(rows, [])]
    if not CSV_KEYS & set(header):
        return None
    found = []
    for row in rows:
        if not any(cell.strip() for cell in row):
            continue
        try:
            found.append(row_card(dict(zip(header, row))))
        except (ValueError, LookupError):
            found.append(None)
        if len(found) > IMPORT_COUNT:
            break
    return found


def multi(value):
    """Google puts several values in one cell, between :::."""
    return [part.strip() for part in re.split(r'\s*:::\s*', value or '') if part.strip()]


def row_card(fields):
    def cell(*names):
        for name in names:
            values = multi(fields.get(name))
            if values:
                return values[0]
        return ''

    def numbered(prefix):
        pattern = re.compile(re.escape(prefix) + r' (\d+) - .+')
        return sorted({int(match.group(1)) for match in map(pattern.fullmatch, fields) if match})

    card = blank()
    outlook = bool({'e-mail address', 'mobile phone', 'business phone', 'job title'} & set(fields))
    card['fn'] = cell('name', 'display name')
    card['given'] = cell('first name', 'given name')
    card['middle'] = cell('middle name', 'additional name')
    card['family'] = cell('last name', 'family name')
    # Outlook's Title column is Mr. or Dr.; the job is Job Title.
    card['prefix'] = cell('name prefix', 'title') if outlook else cell('name prefix')
    card['suffix'] = cell('name suffix', 'suffix')
    card['nickname'] = cell('nickname')
    card['org'] = cell('organization name', 'organization 1 - name', 'company')
    department = cell('organization department', 'organization 1 - department', 'department')
    card['units'] = [one_line(department)] if department else []
    card['title'] = cell('organization title', 'organization 1 - title', 'job title')
    for key in ('fn', *NAME_KEYS):
        card[key] = one_line(card[key])
    card['birthday'] = birthday(cell('birthday'))
    card['note'] = lines_of(fields.get('notes') or '').strip()
    labels = multi(fields.get('labels') or fields.get('group membership'))
    labels += [part.strip() for part in (fields.get('categories') or '').split(';')]
    for label in labels:
        if label.lstrip('* ').casefold() == 'starred':
            card['favorite'] = True
        elif label and not label.startswith('*') and label not in card['categories']:
            # "* myContacts" and the like are Google's own groups.
            card['categories'].append(label)

    for n in numbered('phone'):
        kind = kind_of(words(cell(f'phone {n} - label', f'phone {n} - type')), PHONE_WORDS) or 'other'
        for value in multi(fields.get(f'phone {n} - value')):
            add_phone(card, kind, value)
    for column, kind in OUTLOOK_PHONES.items():
        for value in multi(fields.get(column)):
            add_phone(card, kind, value)
    for n in numbered('e-mail'):
        kind = kind_of(words(cell(f'e-mail {n} - label', f'e-mail {n} - type')), PLACE_WORDS) or 'other'
        for value in multi(fields.get(f'e-mail {n} - value')):
            add_email(card, kind, value)
    for column in ('e-mail address', 'e-mail 2 address', 'e-mail 3 address'):
        for value in multi(fields.get(column)):
            add_email(card, 'other', value)
    for n in numbered('address'):
        kind = kind_of(words(cell(f'address {n} - label', f'address {n} - type')), PLACE_WORDS) or 'other'
        parts = {name: multi(fields.get(f'address {n} - {name}')) for name in (
            'po box', 'extended address', 'street', 'city', 'region', 'postal code', 'country', 'formatted')}
        for index in range(max(len(values) for values in parts.values())):
            pick = {name: values[index] if index < len(values) else '' for name, values in parts.items()}
            address = {
                'type': kind,
                'street': '\n'.join(filter(None, (pick['po box'], pick['extended address'], pick['street']))),
                'city': pick['city'], 'region': pick['region'],
                'postcode': pick['postal code'], 'country': pick['country'],
            }
            if not any(address[key] for key in ADDRESS_KEYS):
                address['street'] = pick['formatted']
            add_address(card, address)
    for prefix, kind in (('business', 'work'), ('home', 'home'), ('other', 'other')):
        street = [cell(f'{prefix} po box'), cell(f'{prefix} street'),
                  cell(f'{prefix} street 2'), cell(f'{prefix} street 3')]
        add_address(card, {
            'type': kind, 'street': '\n'.join(filter(None, street)),
            'city': cell(f'{prefix} city'), 'region': cell(f'{prefix} state'),
            'postcode': cell(f'{prefix} postal code'),
            'country': cell(f'{prefix} country/region', f'{prefix} country'),
        })
    for n in numbered('website'):
        for value in multi(fields.get(f'website {n} - value')):
            add_url(card, value)
    for column in ('web page', 'personal web page'):
        for value in multi(fields.get(column)):
            add_url(card, value)
    return fit(card)


def read_cards(data):
    """The cards in a .vcf or .csv file, told apart by content."""
    if data.startswith((codecs.BOM_UTF16_LE, codecs.BOM_UTF16_BE)):
        data = data.decode('utf-16').encode()
    data = data.removeprefix(codecs.BOM_UTF8)
    if re.match(rb'\s*BEGIN:VCARD', data, re.I):
        cards = vcards(data)
    else:
        cards = csv_cards(decode_bytes(data))
        if cards is None and re.search(rb'BEGIN:VCARD', data, re.I):
            cards = vcards(data)
    if cards is None:
        raise ValueError('That is not a contacts file')
    if not cards:
        raise ValueError('No contacts found in that file')
    return cards


# Writing vCards

def tel_line(item):
    return f"TEL;TYPE={TEL_TYPES.get(item['type'], 'VOICE')}:{escape(item['value'])}"


def email_line(item):
    kind = {'home': ',HOME', 'work': ',WORK'}.get(item['type'], '')
    return f"EMAIL;TYPE=INTERNET{kind}:{escape(item['value'])}"


def adr_line(item):
    kind = {'home': ';TYPE=HOME', 'work': ';TYPE=WORK'}.get(item['type'], '')
    return f'ADR{kind}:;;' + ';'.join(escape(item[key]) for key in ADDRESS_KEYS)


def url_line(url):
    return 'URL:' + url


def fold(line):
    """Lines of at most 75 octets, never splitting a UTF-8 sequence."""
    if line.isascii():
        # Photos are long ASCII lines; slicing keeps them quick.
        return '\r\n '.join([line[:75]] + [line[i:i + 74] for i in range(75, len(line), 74)])
    out, current, size = [], [], 0
    for char in line:
        width = len(char.encode())
        if size + width > 75:
            out.append(''.join(current))
            current, size = [' '], 1
        current.append(char)
        size += width
    out.append(''.join(current))
    return '\r\n'.join(out)


def serialize(card):
    lines = ['BEGIN:VCARD', 'VERSION:3.0', 'PRODID:' + PRODID, 'UID:' + card['uid'],
             'FN:' + escape(display(card)),
             'N:' + ';'.join(escape(card[key]) for key in N_KEYS)]
    if card['nickname']:
        lines.append('NICKNAME:' + escape(card['nickname']))
    if card['org'] or card['units']:
        lines.append('ORG:' + ';'.join(escape(part) for part in [card['org'], *card['units']]))
    if card['title']:
        lines.append('TITLE:' + escape(card['title']))
    lines += [tel_line(item) for item in card['phones']]
    lines += [email_line(item) for item in card['emails']]
    lines += [adr_line(item) for item in card['addresses']]
    lines += [url_line(url) for url in card['urls']]
    if card['birthday']:
        lines.append('BDAY:' + card['birthday'])
    if card['note']:
        lines.append('NOTE:' + escape(card['note']))
    categories = card['categories'] + (['starred'] if card['favorite'] else [])
    if categories:
        lines.append('CATEGORIES:' + ','.join(escape(part) for part in categories))
    kind = image_kind(card['photo'])
    if kind:
        lines.append(f"PHOTO;ENCODING=b;TYPE={'JPEG' if kind == 'jpg' else 'PNG'}:"
                     + base64.b64encode(card['photo']).decode())
    lines += card['extra']
    if card['rev']:
        lines.append('REV:' + card['rev'])
    lines.append('END:VCARD')
    return ''.join(fold(line) + '\r\n' for line in lines)


# The store

def load(uid):
    if not SAFE_UID.fullmatch(uid or ''):
        raise ValueError('No such contact')
    try:
        data = contact_path(uid).read_bytes()
    except FileNotFoundError:
        raise ValueError('No such contact') from None
    cards = [card for card in vcards(data) if card]
    if not cards:
        raise ValueError('That contact cannot be read')
    cards[0]['uid'] = uid
    return cards[0]


def load_all():
    cards = []
    try:
        entries = list(os.scandir(contacts_dir()))
    except FileNotFoundError:
        return cards
    for entry in entries:
        if entry.name.endswith('.vcf') and SAFE_UID.fullmatch(entry.name[:-4]):
            try:
                cards.append(load(entry.name[:-4]))
            except (OSError, ValueError):
                continue
    cards.sort(key=lambda card: (sort_key(card), display(card).casefold(), card['uid']))
    return cards


def forget_index():
    try:
        index_path().unlink()
    except FileNotFoundError:
        pass


def store(card):
    card['rev'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
    write_private(contact_path(card['uid']), serialize(card).encode())
    forget_index()


def index_key():
    try:
        stamp = contacts_dir().stat().st_mtime_ns
        count = sum(1 for entry in os.scandir(contacts_dir()) if entry.name.endswith('.vcf'))
    except FileNotFoundError:
        return [0, 0]
    return [stamp, count]


def index():
    """{digits: summary} for every number. Kept in the cache until the folder
    changes; this helper's own writes drop it at once."""
    key = index_key()
    try:
        cached = json.loads(index_path().read_text())
        if cached.get('key') == key and isinstance(cached.get('numbers'), dict):
            return cached['numbers']
    except (OSError, ValueError, AttributeError):
        pass
    numbers = {}
    for card in load_all():
        item = summary(card)
        for phone in card['phones']:
            digits = digits_key(phone['value'])
            if digits:
                numbers.setdefault(digits, item)
    try:
        write_private(index_path(), json.dumps({'key': key, 'numbers': numbers}).encode())
    except OSError:
        pass
    return numbers


def lookup(number):
    """The summary of the contact with this number, or None."""
    digits = digits_key(number)
    if not digits:
        return None
    item = index().get(digits)
    if item and item.get('photo') and not os.path.exists(item['photo']):
        try:
            item = summary(load(item['uid']))
        except (OSError, ValueError):
            pass
    return item


# Commands

def text_field(data, key, limit=FIELD_LIMIT, multiline=False):
    value = data.get(key)
    if value is None:
        return ''
    if not isinstance(value, str):
        raise ValueError(f'{key} must be text')
    value = clean(value).strip() if multiline else one_line(clean(value))
    if len(value) > limit:
        raise ValueError(f'{key} is longer than {limit} characters')
    return value


def entries(data, key):
    value = data.get(key)
    if value is None:
        return []
    if not isinstance(value, list):
        raise ValueError(f'{key} must be a list')
    if len(value) > ITEM_LIMIT:
        raise ValueError(f'A contact has at most {ITEM_LIMIT} {key}')
    for item in value:
        if not isinstance(item, dict if key != 'urls' else str):
            raise ValueError(f'Each of {key} has the wrong type')
    return value


def entry_kind(item, kinds):
    kind = item.get('type') or 'other'
    if kind not in kinds:
        raise ValueError(f'Unknown type: {kind}')
    return kind


def from_json(data, old):
    if not isinstance(data, dict):
        raise ValueError('A contact is a JSON object')
    card = blank()
    for key in NAME_KEYS:
        card[key] = text_field(data, key)
    for item in entries(data, 'phones'):
        value = text_field(item, 'value', 40)
        if value and not PHONE.fullmatch(value):
            raise ValueError(f'Not a phone number: {value}')
        if value:
            card['phones'].append({'type': entry_kind(item, PHONE_KINDS), 'value': value})
    for item in entries(data, 'emails'):
        value = text_field(item, 'value', 254)
        if value and not good_email(value):
            raise ValueError(f'Not an email address: {value}')
        if value:
            card['emails'].append({'type': entry_kind(item, PLACE_KINDS), 'value': value})
    for item in entries(data, 'addresses'):
        address = {'type': entry_kind(item, PLACE_KINDS)}
        for key in ADDRESS_KEYS:
            address[key] = text_field(item, key, multiline=key == 'street')
        add_address(card, address)
    for url in entries(data, 'urls'):
        url = text_field({'url': url}, 'url', URL_LIMIT)
        if url:
            card['urls'].append(url)
    card['birthday'] = text_field(data, 'birthday', 10)
    if card['birthday'] and birthday(card['birthday']) != card['birthday']:
        raise ValueError('Birthday must be YYYY-MM-DD or --MM-DD')
    card['note'] = text_field(data, 'note', NOTE_LIMIT, multiline=True)
    card['favorite'] = data.get('favorite', old['favorite'] if old else False)
    if not isinstance(card['favorite'], bool):
        raise ValueError('favorite must be true or false')
    name = text_field(data, 'name')
    if not (name or identified(card)):
        raise ValueError('A contact needs a name, a company, a phone number or an email address')
    # A shown name that was only made from the parts follows them when they change.
    if old and name == display(old) and old['fn'] in ('', derived(old)) and derived(card):
        name = ''
    card['fn'] = name
    return card


def save(data):
    uid = data.get('uid') if isinstance(data, dict) else None
    if uid is not None and not isinstance(uid, str):
        raise ValueError('uid must be text')
    old = load(uid) if uid else None
    card = from_json(data, old)
    card['uid'] = uid or str(uuid.uuid4())
    if old:
        card['photo'], card['extra'], card['categories'] = old['photo'], old['extra'], old['categories']
        card['units'] = old['units'] if card['org'] == old['org'] else []
    store(card)
    return full(card)


def delete(uid):
    if not SAFE_UID.fullmatch(uid or '') or not contact_path(uid).is_file():
        raise ValueError('No such contact')
    contact_path(uid).unlink()
    drop_photos(uid)
    forget_index()


def set_favorite(uid, on):
    card = load(uid)
    card['favorite'] = on
    store(card)
    return full(card)


def is_photo_line(line):
    return re.match(r'(?:[^:;]*\.)?PHOTO[;:]', line, re.I) is not None


def set_photo(uid, raw):
    card = load(uid)
    path = home_path(raw)
    if not path.is_file():
        raise ValueError('That photo is not a file')
    if path.stat().st_size > PHOTO_LIMIT:
        raise ValueError('The photo is larger than 5 MB')
    data = path.read_bytes()
    if not image_kind(data):
        raise ValueError('The photo must be a JPEG or PNG image')
    card['photo'] = data
    card['extra'] = [line for line in card['extra'] if not is_photo_line(line)]
    store(card)
    return full(card)


def clear_photo(uid):
    card = load(uid)
    card['photo'] = b''
    card['extra'] = [line for line in card['extra'] if not is_photo_line(line)]
    store(card)
    drop_photos(uid)
    return full(card)


def import_file(raw):
    path = home_path(raw)
    if not path.is_file():
        raise ValueError('That is not a file')
    if path.stat().st_size > IMPORT_LIMIT:
        raise ValueError('The file is larger than 20 MB')
    cards = read_cards(path.read_bytes())
    if len(cards) > IMPORT_COUNT:
        raise ValueError(f'The file has more than {IMPORT_COUNT} contacts')
    seen = {fingerprint(card) for card in load_all()}
    added = skipped = failed = 0
    for card in cards:
        if card is None or not identified(card):
            failed += 1
            continue
        mark = fingerprint(card)
        if card['group'] or mark in seen:
            skipped += 1
            continue
        seen.add(mark)
        if not SAFE_UID.fullmatch(card['uid']) or contact_path(card['uid']).exists():
            card['uid'] = str(uuid.uuid4())
        store(card)
        added += 1
    return {'ok': True, 'added': added, 'skipped': skipped, 'failed': failed}


def export(raw, uids):
    path = home_path(raw)
    if path.is_dir():
        raise IsADirectoryError(str(path))
    if path.exists() and path.suffix.lower() != '.vcf':
        raise FileExistsError('Only a .vcf file can be replaced')
    cards = [load(uid) for uid in uids] if uids else load_all()
    if not cards:
        raise ValueError('There are no contacts to export')
    write_private(path, ''.join(serialize(card) for card in cards).encode())
    return {'ok': True, 'count': len(cards), 'path': str(path)}


def importable():
    home = os.path.realpath(Path.home())
    found = {}
    for folder in (Path(home) / 'Downloads', Path(home) / 'Documents', Path(home)):
        try:
            listing = list(os.scandir(folder))
        except OSError:
            continue
        for entry in listing:
            if entry.name.startswith('.') or not entry.name.lower().endswith(('.vcf', '.csv')):
                continue
            real = os.path.realpath(entry.path)
            if os.path.commonpath([home, real]) != home or not os.path.isfile(real):
                continue
            stat = os.stat(real)
            found[real] = (stat.st_mtime, {
                'path': real,
                'name': entry.name,
                'size': label_size(stat.st_size),
                'modified': time.strftime('%d %b %Y', time.localtime(stat.st_mtime)),
            })
    newest = sorted(found.values(), key=lambda pair: -pair[0])
    return [item for _, item in newest[:100]]


def read_input():
    text = sys.stdin.read(1_000_001)
    if len(text) > 1_000_000:
        raise ValueError('The contact is too large')
    return json.loads(text)


def main(args):
    action = args[0] if args else ''
    try:
        require_grant(action)
        if action == 'list' and len(args) == 1:
            result = {'ok': True, 'contacts': [summary(card) for card in load_all()]}
        elif action == 'get' and len(args) == 2:
            result = {'ok': True, 'contact': full(load(args[1]))}
        elif action == 'save' and len(args) == 1:
            result = {'ok': True, 'contact': save(read_input())}
        elif action == 'delete' and len(args) == 2:
            delete(args[1])
            result = {'ok': True}
        elif action == 'favorite' and len(args) == 3 and args[2] in ('on', 'off'):
            result = {'ok': True, 'contact': set_favorite(args[1], args[2] == 'on')}
        elif action == 'set-photo' and len(args) == 3:
            result = {'ok': True, 'contact': set_photo(args[1], args[2])}
        elif action == 'clear-photo' and len(args) == 2:
            result = {'ok': True, 'contact': clear_photo(args[1])}
        elif action == 'import' and len(args) == 2:
            result = import_file(args[1])
        elif action == 'export' and len(args) >= 2:
            result = export(args[1], args[2:])
        elif action == 'importable' and len(args) == 1:
            result = {'ok': True, 'files': importable()}
        elif action == 'lookup' and len(args) == 2:
            result = {'ok': True, 'contact': lookup(args[1])}
        else:
            print(__doc__.strip(), file=sys.stderr)
            return 2
    except (OSError, ValueError, csv.Error) as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1
    print(json.dumps(result))
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
