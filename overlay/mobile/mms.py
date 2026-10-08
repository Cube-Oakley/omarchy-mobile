#!/usr/bin/env python3
"""Picture messages (MMS): the messages themselves (OMA MMS encapsulation over
WSP) and the carrier's MMS server.

  decode FILE      a PDU's headers and parts, without their contents (debugging)

omarchy-mobile-messages uses this. An MMS arrives as a WAP push SMS holding an
M-Notification.ind; the message is fetched from the address in it, over mobile
data (the carrier's server is reachable only there), and the server is told
it came (M-NotifyResp.ind). Sending posts an M-Send.req to the server.

The device's telephony.json has the settings: "mms": {"interface": the mobile
data interface, "mmsc": the carrier's MMS server, "proxy": "host:port" or ""}.
Without an interface the system's routes are used.
"""
import http.client
import json
import os
from pathlib import Path
import secrets
import socket
import ssl
import sys
import time
import urllib.parse

MAX_SIZE = 10 * 1024 * 1024
# What Android's messaging sends: some servers tailor content to the profile.
USER_AGENT = 'Android-Mms/2.0'
UA_PROFILE = 'http://www.gstatic.com/android/sms/mms_ua_profile.xml'
MMS_TYPE = 'application/vnd.wap.mms-message'

TYPES = {0x80: 'send-req', 0x81: 'send-conf', 0x82: 'notification-ind', 0x83: 'notifyresp-ind',
         0x84: 'retrieve-conf', 0x85: 'acknowledge-ind', 0x86: 'delivery-ind', 0x87: 'read-rec-ind',
         0x88: 'read-orig-ind'}
TYPE_CODES = {name: code for code, name in TYPES.items()}
FIELDS = {0x01: 'bcc', 0x02: 'cc', 0x03: 'content-location', 0x04: 'content-type', 0x05: 'date',
          0x06: 'delivery-report', 0x07: 'delivery-time', 0x08: 'expiry', 0x09: 'from', 0x0A: 'message-class',
          0x0B: 'message-id', 0x0C: 'message-type', 0x0D: 'mms-version', 0x0E: 'message-size', 0x0F: 'priority',
          0x10: 'read-report', 0x11: 'report-allowed', 0x12: 'response-status', 0x13: 'response-text',
          0x14: 'sender-visibility', 0x15: 'status', 0x16: 'subject', 0x17: 'to', 0x18: 'transaction-id',
          0x19: 'retrieve-status', 0x1A: 'retrieve-text', 0x1B: 'read-status'}
CODES = {name: code for code, name in FIELDS.items()}
TEXT_FIELDS = {'content-location', 'message-id', 'transaction-id'}
ENCODED_FIELDS = {'subject', 'response-text', 'retrieve-text'}
ADDRESS_FIELDS = {'to', 'cc', 'bcc'}
INTEGER_FIELDS = {'date', 'message-size'}
TIME_FIELDS = {'expiry', 'delivery-time'}
CLASSES = {0x80: 'personal', 0x81: 'advertisement', 0x82: 'informational', 0x83: 'auto'}
STATUS = {'expired': 0x80, 'retrieved': 0x81, 'rejected': 0x82, 'deferred': 0x83, 'unrecognised': 0x84}
DELIVERY = {0x80: 'expired', 0x81: 'retrieved', 0x82: 'rejected', 0x83: 'deferred', 0x84: 'unrecognised',
            0x85: 'indeterminate', 0x86: 'forwarded', 0x87: 'unreachable'}
# WSP's assigned content types (WAP-230 table 40), the ones MMS meets.
MEDIA = {0x00: '*/*', 0x01: 'text/*', 0x02: 'text/html', 0x03: 'text/plain', 0x07: 'text/x-vcard',
         0x06: 'text/x-vcalendar', 0x0C: 'multipart/mixed', 0x0F: 'multipart/alternative', 0x1C: 'image/*',
         0x1D: 'image/gif', 0x1E: 'image/jpeg', 0x1F: 'image/tiff', 0x20: 'image/png', 0x21: 'image/vnd.wap.wbmp',
         0x22: 'application/vnd.wap.multipart.*', 0x23: 'application/vnd.wap.multipart.mixed',
         0x26: 'application/vnd.wap.multipart.alternative', 0x27: 'application/xml', 0x28: 'text/xml',
         0x33: 'application/vnd.wap.multipart.related', 0x3B: 'application/xhtml+xml', 0x3D: 'text/css',
         0x3E: MMS_TYPE}
MEDIA_CODES = {name: code for code, name in MEDIA.items()}
# WSP's well-known parameters (WAP-230 table 38); 1.4's text forms included.
PARAMETERS = {0x00: 'q', 0x01: 'charset', 0x02: 'level', 0x03: 'type', 0x05: 'name', 0x06: 'filename',
              0x09: 'type', 0x0A: 'start', 0x0B: 'start-info', 0x16: 'size', 0x17: 'name', 0x18: 'filename',
              0x19: 'start', 0x1A: 'start-info'}
# IANA MIBenum charsets.
CHARSETS = {0: 'utf-8', 3: 'ascii', 4: 'latin-1', 106: 'utf-8', 1000: 'utf-16-be', 1013: 'utf-16-be',
            1014: 'utf-16-le', 1015: 'utf-16'}
UTF8 = 106


class Uncertain(RuntimeError):
    """A message was sent but no answer came: it may have gone."""


class Reader:
    def __init__(self, data, pos=0):
        self.data, self.pos = data, pos

    def peek(self):
        if self.pos >= len(self.data):
            raise ValueError('The message ends early')
        return self.data[self.pos]

    def byte(self):
        value = self.peek()
        self.pos += 1
        return value

    def take(self, count):
        if count < 0 or self.pos + count > len(self.data):
            raise ValueError('The message ends early')
        chunk = self.data[self.pos:self.pos + count]
        self.pos += count
        return chunk

    def uintvar(self):
        value = 0
        for _ in range(5):
            octet = self.byte()
            value = value << 7 | octet & 0x7F
            if not octet & 0x80:
                return value
        raise ValueError('A length is malformed')

    def raw_text(self):
        """Text-string, Quoted-string or Token-text: up to its NUL."""
        if self.peek() in (0x7F, 0x22):
            self.pos += 1
        end = self.data.find(b'\0', self.pos)
        if end < 0:
            raise ValueError('A text ends early')
        chunk = self.data[self.pos:end]
        self.pos = end + 1
        return chunk

    def text(self):
        return self.raw_text().decode('utf-8', 'replace')

    def value_length(self):
        octet = self.byte()
        if octet <= 30:
            return octet
        if octet == 31:
            return self.uintvar()
        raise ValueError('A length is malformed')

    def long_integer(self):
        size = self.byte()
        if size > 30:
            raise ValueError('An integer is malformed')
        return int.from_bytes(self.take(size), 'big')

    def integer(self):
        """Integer-value: a short integer, else a long one."""
        if self.peek() & 0x80:
            return self.byte() & 0x7F
        return self.long_integer()

    def skip_value(self):
        """Any header value, by WSP's general rule for its first octet."""
        octet = self.peek()
        if octet <= 30:
            self.pos += 1
            self.take(octet)
        elif octet == 31:
            self.pos += 1
            self.take(self.uintvar())
        elif octet < 128:
            self.raw_text()
        else:
            self.pos += 1

    def encoded_string(self):
        """Encoded-string-value: text, or a length, charset and text."""
        if self.peek() > 31:
            return decode_text(self.raw_text(), UTF8)
        end = self.value_length() + self.pos
        charset = self.integer()
        chunk = self.data[self.pos:end]
        self.pos = end
        if chunk[:1] == b'\x7f':
            chunk = chunk[1:]
        return decode_text(chunk, charset).rstrip('\0')

    def media(self):
        octet = self.peek()
        if octet & 0x80:
            self.pos += 1
            return MEDIA.get(octet & 0x7F, 'application/octet-stream')
        if octet > 31:
            return self.text().lower()
        return MEDIA.get(self.long_integer(), 'application/octet-stream')

    def content_type(self):
        """Content-type: the media type and its parameters."""
        octet = self.peek()
        if octet > 31:
            return self.media(), {}
        end = self.value_length() + self.pos
        media = self.media()
        parameters = {}
        while self.pos < end:
            octet = self.peek()
            if octet & 0x80 or octet <= 30:
                name = PARAMETERS.get(self.integer())
                value = self.typed_value(name)
            else:
                name = self.text().lower()
                value = self.typed_value(name)
            if name:
                parameters[name] = value
        self.pos = end
        return media, parameters

    def typed_value(self, name):
        octet = self.peek()
        if name == 'type' and octet & 0x80:
            self.pos += 1
            return MEDIA.get(octet & 0x7F, '')
        if octet == 0:
            self.pos += 1
            return ''
        if 31 < octet < 128:
            return self.text()
        return self.integer()


def decode_text(chunk, charset):
    try:
        return chunk.decode(CHARSETS.get(charset, 'utf-8'), 'replace')
    except LookupError:
        return chunk.decode('utf-8', 'replace')


def address(value):
    """A phone number or e-mail address without MMS's /TYPE= suffix."""
    return value.split('/TYPE=', 1)[0].strip()


def decode(pdu):
    """A message's headers and, for a retrieved or sent one, its parts:
    {'message-type': ..., ..., 'parts': [{'type', 'parameters', 'name',
    'id', 'data'}]}."""
    reader = Reader(pdu)
    headers = {'parts': []}
    while reader.pos < len(pdu):
        octet = reader.byte()
        if not octet & 0x80:
            # An application header: a token and a text.
            reader.pos -= 1
            reader.raw_text()
            reader.raw_text()
            continue
        name = FIELDS.get(octet & 0x7F)
        if name == 'content-type':
            media, parameters = reader.content_type()
            headers['content-type'], headers['content-parameters'] = media, parameters
            if 'multipart' in media:
                headers['parts'] = parts(reader)
            elif reader.pos < len(pdu):
                headers['parts'] = [{'type': media, 'parameters': parameters, 'name': '', 'id': '',
                                     'data': pdu[reader.pos:]}]
            break
        if name == 'message-type':
            value = reader.byte()
            headers[name] = TYPES.get(value, hex(value))
        elif name in TEXT_FIELDS:
            headers[name] = reader.text()
        elif name in ENCODED_FIELDS:
            headers[name] = reader.encoded_string()
        elif name in ADDRESS_FIELDS:
            headers.setdefault(name, []).append(address(reader.encoded_string()))
        elif name in INTEGER_FIELDS:
            headers[name] = reader.integer()
        elif name in TIME_FIELDS:
            end = reader.value_length() + reader.pos
            token = reader.byte()
            seconds = reader.integer()
            reader.pos = end
            headers[name] = seconds if token == 0x80 else int(time.time()) + seconds
        elif name == 'from':
            end = reader.value_length() + reader.pos
            headers[name] = address(reader.encoded_string()) if reader.byte() == 0x80 else ''
            reader.pos = end
        elif name == 'message-class':
            headers[name] = CLASSES.get(reader.byte(), 'personal') if reader.peek() & 0x80 else reader.text()
        elif name == 'mms-version':
            value = reader.byte() & 0x7F
            headers[name] = f'{value >> 4}.{value & 15}'
        elif name in ('response-status', 'retrieve-status', 'status', 'report-allowed', 'delivery-report',
                      'read-report', 'priority', 'sender-visibility', 'read-status'):
            headers[name] = reader.byte() if reader.peek() & 0x80 else reader.integer()
        else:
            reader.skip_value()
    return headers


def parts(reader):
    """A multipart body (WAP-230 8.5)."""
    found = []
    for _ in range(reader.uintvar()):
        header_length = reader.uintvar()
        data_length = reader.uintvar()
        start = reader.pos
        media, parameters = reader.content_type()
        end = start + header_length
        part = {'type': media, 'parameters': parameters, 'name': '', 'id': ''}
        while reader.pos < end:
            octet = reader.byte()
            if octet & 0x80:
                field = octet & 0x7F
                if field == 0x0E:                   # Content-Location
                    part['name'] = reader.text()
                elif field == 0x40:                 # Content-ID
                    part['id'] = reader.text().strip('"<>')
                elif field in (0x2E, 0x45):         # Content-Disposition
                    stop = reader.value_length() + reader.pos
                    reader.byte()
                    while reader.pos < stop:
                        code = reader.peek()
                        name = PARAMETERS.get(reader.integer()) if code & 0x80 or code <= 30 else reader.text().lower()
                        value = reader.typed_value(name)
                        if name in ('filename', 'name') and isinstance(value, str) and value:
                            parameters.setdefault('name', value)
                    reader.pos = stop
                else:
                    reader.skip_value()
            else:
                reader.pos -= 1
                name = reader.text().lower()
                value = reader.text()
                if name == 'content-location':
                    part['name'] = value
                elif name == 'content-id':
                    part['id'] = value.strip('"<>')
        reader.pos = end
        part['data'] = reader.take(data_length)
        if not part['name']:
            part['name'] = str(parameters.get('name') or parameters.get('filename') or part['id'] or '')
        found.append(part)
    return found


def part_text(part):
    """A text part's text, in its charset."""
    charset = part['parameters'].get('charset', UTF8)
    return decode_text(part['data'], charset if isinstance(charset, int) else UTF8)


def push(data):
    """The MMS message in a WAP push (connectionless WSP), or None."""
    try:
        reader = Reader(data)
        reader.byte()                               # transaction id
        if reader.byte() not in (0x06, 0x07):       # Push, Confirmed push
            return None
        length = reader.uintvar()
        start = reader.pos
        media, _ = reader.content_type()
        if media != MMS_TYPE:
            return None
        return bytes(data[start + length:])
    except ValueError:
        return None


# ---- writing

class Writer:
    def __init__(self):
        self.out = bytearray()

    def uintvar(self, value):
        chunk = [value & 0x7F]
        value >>= 7
        while value:
            chunk.append(value & 0x7F | 0x80)
            value >>= 7
        self.out += bytes(reversed(chunk))

    def value_length(self, length):
        if length <= 30:
            self.out.append(length)
        else:
            self.out.append(31)
            self.uintvar(length)

    def text(self, value):
        data = value.encode()
        if data[:1] and data[0] >= 0x80:
            self.out.append(0x7F)
        self.out += data + b'\0'

    def field(self, name, value=None):
        self.out.append(0x80 | CODES[name])
        if value is not None:
            self.out.append(value)

    def encoded(self, value):
        """Encoded-string-value: plain when ASCII, else UTF-8 with its charset."""
        if value.isascii():
            self.text(value)
            return
        inner = Writer()
        inner.out.append(0x80 | UTF8)
        inner.text(value)
        self.value_length(len(inner.out))
        self.out += inner.out


def notify_response(transaction, status='retrieved', reports=True):
    """M-NotifyResp.ind: the message came (or not), for the server."""
    writer = Writer()
    writer.field('message-type', TYPE_CODES['notifyresp-ind'])
    writer.field('transaction-id')
    writer.text(transaction)
    writer.field('mms-version', 0x92)
    writer.field('status', STATUS[status])
    writer.field('report-allowed', 0x80 if reports else 0x81)
    return bytes(writer.out)


def content_type(writer, media, parameters=()):
    """A part's or message's content type, with parameters as (code, bytes)."""
    code = MEDIA_CODES.get(media)
    head = bytes([0x80 | code]) if code is not None else media.encode() + b'\0'
    if not parameters:
        writer.out += head
        return
    body = head + b''.join(bytes([0x80 | name]) + value for name, value in parameters)
    writer.value_length(len(body))
    writer.out += body


def safe_name(name, index):
    """An attachment's name as a part's location: ASCII, no paths."""
    stem = ''.join(c if c.isascii() and (c.isalnum() or c in '._-') else '_' for c in Path(name or '').name)
    return stem.strip('._') or f'part{index}'


SMIL = ('<smil><head><layout><root-layout/>'
        '<region id="Image" fit="meet" top="0" left="0" height="80%" width="100%"/>'
        '<region id="Text" top="80%" left="0" height="20%" width="100%"/>'
        '</layout></head><body>{}</body></smil>')


def send_request(recipients, text='', attachments=(), subject=''):
    """M-Send.req for recipients (phone numbers or e-mail addresses), with a
    text and attachments [(name, media type, bytes)]. Returns (transaction,
    message)."""
    if not recipients:
        raise ValueError('A message needs someone to send it to')
    if not text and not attachments:
        raise ValueError('The message is empty')
    transaction = 'T' + secrets.token_hex(8)
    writer = Writer()
    writer.field('message-type', 0x80)
    writer.field('transaction-id')
    writer.text(transaction)
    writer.field('mms-version', 0x92)
    writer.field('from')
    writer.out += bytes([1, 0x81])                  # the server fills in the sender
    for recipient in recipients:
        writer.field('to')
        writer.encoded(recipient if '@' in recipient else recipient + '/TYPE=PLMN')
    if subject:
        writer.field('subject')
        writer.encoded(subject)
    writer.field('message-class', 0x80)
    writer.field('delivery-report', 0x81)
    writer.field('read-report', 0x81)
    # The parts: a SMIL layout first, as Android sends, then the media and
    # the text, each named by Content-Location and Content-ID.
    entries, slides = [], []
    for index, (name, media, data) in enumerate(attachments):
        tag = {'image': 'img', 'video': 'video', 'audio': 'audio'}.get(media.split('/')[0], 'ref')
        location = safe_name(name, index)
        slides.append(f'<par dur="5000ms"><{tag} src="{location}" region="Image"/></par>')
        entries.append((media, [(0x05, location.encode() + b'\0')], location, location, data))
    if text:
        slides.append('<par dur="5000ms"><text src="text0.txt" region="Text"/></par>')
        entries.append(('text/plain', [(0x01, bytes([0x80 | UTF8]))], 'text0.txt', 'text0.txt', text.encode()))
    smil = SMIL.format(''.join(slides)).encode()
    entries.insert(0, ('application/smil', [], 'smil.xml', 'smil', smil))
    writer.field('content-type')
    content_type(writer, 'application/vnd.wap.multipart.related',
                 [(0x09, b'application/smil\0'), (0x0A, b'<smil>\0')])
    writer.uintvar(len(entries))
    for media, parameters, location, ident, data in entries:
        head = Writer()
        content_type(head, media, parameters)
        head.out.append(0x80 | 0x40)                # Content-ID, a quoted string
        head.out += b'"<' + ident.encode() + b'>\0'
        head.out.append(0x80 | 0x0E)                # Content-Location
        head.text(location)
        writer.uintvar(len(head.out))
        writer.uintvar(len(data))
        writer.out += head.out + data
    return transaction, bytes(writer.out)


# ---- the carrier's server

def settings():
    path = Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config') / 'omarchy-mobile/telephony.json'
    try:
        found = json.loads(path.read_text()).get('mms') or {}
    except (OSError, ValueError, AttributeError):
        found = {}
    return {'interface': str(found.get('interface') or ''), 'mmsc': str(found.get('mmsc') or ''),
            'proxy': str(found.get('proxy') or '')}


def connect(host, port, interface, timeout):
    """A TCP connection, over the interface when one is named (the carrier's
    server answers only on mobile data, also while Wi-Fi is the route)."""
    error = None
    for family, kind, protocol, _, target in socket.getaddrinfo(host, port, type=socket.SOCK_STREAM):
        sock = socket.socket(family, kind, protocol)
        try:
            if interface:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, interface.encode())
            sock.settimeout(timeout)
            sock.connect(target)
            return sock
        except OSError as failure:
            error = failure
            sock.close()
    raise error or OSError(f'{host} has no address')


class Connection(http.client.HTTPConnection):
    def __init__(self, host, port, interface, timeout):
        super().__init__(host, port, timeout=timeout)
        self.interface = interface

    def connect(self):
        self.sock = connect(self.host, self.port, self.interface, self.timeout)


class SecureConnection(http.client.HTTPSConnection):
    def __init__(self, host, port, interface, timeout):
        super().__init__(host, port, timeout=timeout, context=ssl.create_default_context())
        self.interface = interface

    def connect(self):
        sock = connect(self.host, self.port, self.interface, self.timeout)
        self.sock = self._context.wrap_socket(sock, server_hostname=self.host)


def transfer(url, body=None, config=None, timeout=60):
    """GET a message (body None) or POST one; the server's answer."""
    config = config or settings()
    target = urllib.parse.urlsplit(url)
    if target.scheme not in ('http', 'https') or not target.hostname:
        raise ValueError('The message address is not a web address')
    proxy = config.get('proxy', '')
    if proxy:
        host, _, port = proxy.rpartition(':') if ':' in proxy else (proxy, '', '80')
        connection = Connection(host, int(port or 80), config.get('interface'), timeout)
        path = url
    else:
        kind = SecureConnection if target.scheme == 'https' else Connection
        connection = kind(target.hostname, target.port or (443 if target.scheme == 'https' else 80),
                          config.get('interface'), timeout)
        path = (target.path or '/') + ('?' + target.query if target.query else '')
    headers = {'User-Agent': USER_AGENT, 'x-wap-profile': UA_PROFILE, 'Accept-Language': 'en-US, en',
               'Accept': f'*/*, {MMS_TYPE}, application/vnd.wap.sic'}
    if body is not None:
        headers['Content-Type'] = MMS_TYPE
    try:
        connection.connect()
        try:
            connection.request('GET' if body is None else 'POST', path, body=body, headers=headers)
            response = connection.getresponse()
            data = response.read(MAX_SIZE + 1)
        except (OSError, http.client.HTTPException) as error:
            # Once a message is on its way, a lost answer says nothing of it.
            if body is not None:
                raise Uncertain(f'No answer from the MMS server: {error}') from error
            raise OSError(f'The MMS server stopped answering: {error}') from error
    finally:
        connection.close()
    if response.status != 200:
        raise RuntimeError(f'The MMS server answered {response.status}')
    if len(data) > MAX_SIZE:
        raise RuntimeError('The message is too large')
    return data


def retrieve(location, config=None):
    """The message a notification points to, decoded."""
    message = decode(transfer(location, config=config))
    if message.get('message-type') != 'retrieve-conf':
        raise RuntimeError('The MMS server sent something else')
    status = message.get('retrieve-status', 0x80)
    if status != 0x80:
        raise RuntimeError(message.get('retrieve-text') or f'The MMS server refused the message ({status:#x})')
    return message


def acknowledge(transaction, config=None):
    """Tell the server the message came; without this some send it again."""
    config = config or settings()
    if config.get('mmsc'):
        transfer(config['mmsc'], notify_response(transaction), config)


def send(recipients, text='', attachments=(), subject='', config=None):
    """Send a message; the server's message id."""
    config = config or settings()
    if not config.get('mmsc'):
        raise RuntimeError('This phone has no MMS server set')
    transaction, message = send_request(recipients, text, attachments, subject)
    answer = decode(transfer(config['mmsc'], message, config))
    if answer.get('message-type') != 'send-conf':
        raise RuntimeError('The MMS server sent something else')
    status = answer.get('response-status', 0x80)
    if status != 0x80:
        raise RuntimeError(answer.get('response-text') or f'The MMS server refused the message ({status:#x})')
    return answer.get('message-id', transaction)


def main(argv):
    if len(argv) == 2 and argv[0] == 'decode':
        message = decode(Path(argv[1]).read_bytes())
        for part in message['parts']:
            part['size'] = len(part.pop('data'))
        print(json.dumps(message, indent=2, default=str))
        return 0
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
