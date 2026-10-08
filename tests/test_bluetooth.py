import importlib.util
import io
import json
from pathlib import Path
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1] / 'overlay/mobile'


def load():
    spec = importlib.util.spec_from_file_location('bluetooth', ROOT / 'bluetooth.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def v(kind, data):
    return {'type': kind, 'data': data}


def device(address, name=None, paired=False, connected=False, rssi=None, battery=None):
    props = {'Address': v('s', address), 'Alias': v('s', name or address.replace(':', '-')),
             'Paired': v('b', paired), 'Trusted': v('b', paired), 'Connected': v('b', connected),
             'Icon': v('s', 'audio-headset')}
    if name:
        props['Name'] = v('s', name)
    if rssi is not None:
        props['RSSI'] = v('n', rssi)
    ifaces = {'org.bluez.Device1': props}
    if battery is not None:
        ifaces['org.bluez.Battery1'] = {'Percentage': v('y', battery)}
    return ifaces


OBJECTS = {
    '/org/bluez': {'org.bluez.AgentManager1': {}},
    '/org/bluez/hci0': {'org.bluez.Adapter1': {
        'Address': v('s', '02:00:00:00:00:01'), 'Name': v('s', 'BlueZ 5.87'),
        'Alias': v('s', 'Phone'), 'Powered': v('b', True), 'Discovering': v('b', False)}},
    '/org/bluez/hci0/dev_11_22_33_44_55_66': device('11:22:33:44:55:66', 'Headphones', True, True, battery=80),
    '/org/bluez/hci0/dev_22_22_33_44_55_66': device('22:22:33:44:55:66', 'Old speaker', True, False),
    '/org/bluez/hci0/dev_33_22_33_44_55_66': device('33:22:33:44:55:66', 'Far TV', rssi=-90),
    '/org/bluez/hci0/dev_44_22_33_44_55_66': device('44:22:33:44:55:66', 'Near watch', rssi=-40),
    '/org/bluez/hci0/dev_55_22_33_44_55_66': device('55:22:33:44:55:66', rssi=-30),
}


class BluetoothTests(unittest.TestCase):
    def test_snapshot_reads_adapter_and_devices(self):
        status = load().snapshot(OBJECTS)
        self.assertTrue(status['available'])
        self.assertEqual(status['name'], 'Phone')
        self.assertTrue(status['powered'])
        headphones = status['devices'][0]
        self.assertEqual((headphones['name'], headphones['connected'], headphones['battery']),
                         ('Headphones', True, 80))

    def test_devices_sort_connected_then_paired_then_signal(self):
        names = [d['name'] for d in load().snapshot(OBJECTS)['devices']]
        self.assertEqual(names[:2], ['Headphones', 'Old speaker'])
        self.assertLess(names.index('Near watch'), names.index('Far TV'))

    def test_nameless_devices_are_marked_for_hiding(self):
        nameless = [d for d in load().snapshot(OBJECTS)['devices'] if not d['named']]
        self.assertEqual([d['address'] for d in nameless], ['55:22:33:44:55:66'])

    def test_no_adapter_is_unavailable(self):
        status = load().snapshot({'/org/bluez': {'org.bluez.AgentManager1': {}}})
        self.assertFalse(status['available'])
        self.assertEqual(status['devices'], [])

    def test_rejects_bad_addresses_and_actions(self):
        bluetooth = load()
        with mock.patch.object(bluetooth, 'read_status', return_value=bluetooth.snapshot(OBJECTS)):
            for args in (['pair', 'not-an-address'], ['connect'], ['power', 'maybe'], ['explode']):
                with self.assertRaises(ValueError):
                    bluetooth.act(args)

    def test_failed_action_keeps_the_controller_available(self):
        bluetooth = load()
        out = io.StringIO()
        with mock.patch.object(bluetooth, 'read_status', return_value=bluetooth.snapshot(OBJECTS)), \
                mock.patch('sys.stdout', out):
            self.assertEqual(bluetooth.main(['pair', 'bad']), 1)
        result = json.loads(out.getvalue())
        self.assertTrue(result['available'])
        self.assertFalse(result['ok'])

    def test_power_on_runs_the_board_start_command_without_a_controller(self):
        bluetooth = load()
        down = bluetooth.unavailable('Bluetooth is not running.')
        up = bluetooth.snapshot(OBJECTS)
        with mock.patch.object(bluetooth, 'start_command', return_value='/usr/local/sbin/bt-start'), \
                mock.patch.object(bluetooth, 'read_status', side_effect=[down, up, up]), \
                mock.patch.object(bluetooth, 'ctl', return_value=(True, '')) as ctl, \
                mock.patch('subprocess.run') as run:
            status, ok, _ = bluetooth.power(True)
        run.assert_called_once()
        self.assertEqual(run.call_args[0][0], ['/usr/local/sbin/bt-start'])
        ctl.assert_called_once_with('power', 'on')
        self.assertTrue(ok and status['available'])


class FakeInvocation:
    def __init__(self):
        self.replies = []

    def return_value(self, value):
        self.replies.append(('ok', value))

    def return_dbus_error(self, name, message):
        self.replies.append(('error', name))


class FakeParams:
    def __init__(self, *args):
        self.args = args

    def unpack(self):
        return self.args


class FakeBus:
    def __init__(self):
        self.calls = []

    def call(self, service, path, interface, method, *rest):
        self.calls.append((path, method))


class FakeGio:
    class BusType:
        SYSTEM = 1

    class DBusCallFlags:
        NONE = 0

    bus = None

    @classmethod
    def bus_get_sync(cls, kind):
        return cls.bus


class FakeGLib:
    IO_IN = 1

    class MainLoop:
        pass

    @staticmethod
    def Variant(signature, value):
        return (signature, value)


class PairingTests(unittest.TestCase):
    def pairing(self):
        bluetooth = load()
        FakeGio.bus = FakeBus()
        pairing = bluetooth.Pairing('44:22:33:44:55:66', FakeGio, FakeGLib)
        pairing.device = '/org/bluez/hci0/dev_44_22_33_44_55_66'
        pairing.name = 'Near watch'
        events = []
        pairing.emit = lambda **event: events.append(event)
        return pairing, events

    def test_answers_follow_the_kind_of_request(self):
        answer = load().answer
        self.assertEqual(answer('confirm', 'yes\n'), ('ok', None))
        self.assertEqual(answer('confirm', 'no'), ('reject', None))
        self.assertEqual(answer('authorize', ''), ('reject', None))
        self.assertEqual(answer('passkey', '012345'), ('ok', 12345))
        self.assertEqual(answer('passkey', '1234567'), ('reject', None))
        self.assertEqual(answer('pin', '0000'), ('ok', '0000'))
        self.assertEqual(answer('pin', 'rm -rf'), ('reject', None))

    def test_a_code_to_compare_waits_for_the_users_yes(self):
        pairing, events = self.pairing()
        invocation = FakeInvocation()
        pairing.agent_call(None, None, None, None, 'RequestConfirmation',
                           FakeParams('/org/bluez/hci0/dev_44_22_33_44_55_66', 12345), invocation)
        self.assertEqual(events, [dict(event='request', kind='confirm', name='Near watch', code='012345')])
        self.assertEqual(invocation.replies, [])
        pairing.reply('yes')
        self.assertEqual(invocation.replies, [('ok', None)])

    def test_a_typed_passkey_is_sent_as_a_number(self):
        pairing, _ = self.pairing()
        invocation = FakeInvocation()
        pairing.agent_call(None, None, None, None, 'RequestPasskey', FakeParams('/dev'), invocation)
        pairing.reply('654321')
        self.assertEqual(invocation.replies, [('ok', ('(u)', (654321,)))])

    def test_digits_to_type_on_a_keyboard_are_shown_at_once(self):
        pairing, events = self.pairing()
        invocation = FakeInvocation()
        pairing.agent_call(None, None, None, None, 'DisplayPasskey', FakeParams('/dev', 42, 3), invocation)
        self.assertEqual(events, [dict(event='display', code='000042', entered=3)])
        self.assertEqual(invocation.replies, [('ok', None)])

    def test_cancel_declines_the_question_and_stops_the_pairing(self):
        pairing, _ = self.pairing()
        invocation = FakeInvocation()
        pairing.agent_call(None, None, None, None, 'RequestAuthorization', FakeParams('/dev'), invocation)
        pairing.reply('cancel')
        self.assertEqual(invocation.replies, [('error', 'org.bluez.Error.Canceled')])
        self.assertIn(('/org/bluez/hci0/dev_44_22_33_44_55_66', 'CancelPairing'), FakeGio.bus.calls)
        self.assertTrue(pairing.cancelled)

    def test_pairing_errors_read_as_sentences(self):
        error = type('Error', (), dict(message='GDBus.Error:org.bluez.Error.AuthenticationFailed: '
                                              'Authentication Failed'))()
        self.assertEqual(load().pair_error(error, 'Near watch'),
                         "The codes didn't match, or Near watch turned the pairing down.")
        odd = type('Error', (), dict(message='GDBus.Error:org.bluez.Error.Failed: Resource busy'))()
        self.assertEqual(load().pair_error(odd, 'Near watch'), 'Resource busy')


if __name__ == '__main__':
    unittest.main()
