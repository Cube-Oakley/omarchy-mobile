"""Sleep policy decisions and the adapter protocol, without sleeping anything."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('sleep_policy', Path(__file__).resolve().parents[1] / 'overlay/mobile/sleep.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PolicyTests(unittest.TestCase):
    def test_sleeps_only_after_the_grace_period_of_darkness(self):
        policy = module.Policy()
        self.assertFalse(policy.ready(100, dark=True, busy=False))
        self.assertFalse(policy.ready(100 + module.GRACE - 1, dark=True, busy=False))
        self.assertTrue(policy.ready(100 + module.GRACE, dark=True, busy=False))

    def test_light_restarts_the_count(self):
        policy = module.Policy()
        policy.ready(100, dark=True, busy=False)
        policy.ready(105, dark=False, busy=False)
        self.assertFalse(policy.ready(100 + module.GRACE, dark=True, busy=False))

    def test_a_call_or_audio_keeps_it_awake(self):
        policy = module.Policy()
        policy.ready(100, dark=True, busy=True)
        self.assertFalse(policy.ready(200, dark=True, busy=True))
        self.assertTrue(policy.ready(201, dark=True, busy=False))

    def test_each_wake_holds_the_phone_up_for_its_kind(self):
        policy = module.Policy()
        policy.woke(1000, 'alarm')
        policy.ready(1000, dark=True, busy=False)
        self.assertFalse(policy.ready(1000 + module.HOLD['alarm'] - 1, dark=True, busy=False))
        self.assertTrue(policy.ready(1000 + module.HOLD['alarm'], dark=True, busy=False))
        policy.woke(2000, 'something-new')
        policy.ready(2000, dark=True, busy=False)
        self.assertTrue(policy.ready(2000 + max(module.HOLD_OTHER, module.GRACE), dark=True, busy=False))

    def test_a_wake_that_leaves_the_screen_dark_skips_the_grace(self):
        policy = module.Policy()
        policy.ready(1000, dark=True, busy=False)
        policy.woke(2000, 'modem')
        # Only the modem's hold, not the hold plus the 10 s grace.
        self.assertTrue(policy.ready(2000 + module.HOLD['modem'], dark=True, busy=False))
        policy.woke(3000, 'touch')
        policy.ready(3000, dark=True, busy=False)
        self.assertFalse(policy.ready(3000 + module.GRACE - 1, dark=True, busy=False))

    def test_a_crashed_adapter_sleeps_again_within_seconds(self):
        # A crash (a signal) refused nothing: no grace, no refusal wait.
        policy = module.Policy()
        policy.ready(1000, dark=True, busy=False)
        policy.retry(1100)
        self.assertFalse(policy.ready(1100 + module.CRASH_RETRY - 1, dark=True, busy=False))
        self.assertTrue(policy.ready(1100 + module.CRASH_RETRY, dark=True, busy=False))
        self.assertLess(module.CRASH_RETRY, module.REFUSED_WAIT)

    def test_an_adapter_killed_by_a_signal_is_marked_crashed(self):
        killed = subprocess.CompletedProcess([], -11, stdout='', stderr='')
        refused = subprocess.CompletedProcess([], 1, stdout='{"woke": "error", "error": "Unplug"}', stderr='')
        with patch.object(module.subprocess, 'run', return_value=killed):
            self.assertTrue(module.sleep_once(15).get('crashed'))
        with patch.object(module.subprocess, 'run', return_value=refused):
            self.assertNotIn('crashed', module.sleep_once(15))

    def test_the_background_check_ends_soon_after_the_network_is_up(self):
        policy = module.Policy()
        policy.woke(1000, 'alarm')
        policy.ready(1000, dark=True, busy=False)
        policy.online(1012)
        self.assertFalse(policy.ready(1012 + module.ONLINE_HOLD - 1, dark=True, busy=False))
        self.assertTrue(policy.ready(1012 + module.ONLINE_HOLD, dark=True, busy=False))

    def test_the_background_check_is_announced_for_apps(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {'XDG_RUNTIME_DIR': tmp}):
            module.announce_check()
            self.assertTrue((Path(tmp) / 'omarchy-mobile-check').read_text().strip().isdigit())

    def test_the_background_check_runs_only_while_something_uses_it(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {'XDG_RUNTIME_DIR': tmp}):
            prefs = {'sleepCheck': 15, 'alwaysOn': True}
            self.assertEqual(module.check_minutes(prefs), 0)
            self.assertEqual(module.check_minutes(dict(prefs, ambientWeather=True)), 15)
            self.assertEqual(module.check_minutes(dict(prefs, ambientWeather=True, alwaysOn=False)), 0)
            (Path(tmp) / 'omarchy-mobile-check.d').mkdir()
            self.assertEqual(module.check_minutes(prefs), 0)
            (Path(tmp) / 'omarchy-mobile-check.d/mail').touch()
            self.assertEqual(module.check_minutes(prefs), 15)
            self.assertEqual(module.check_minutes(dict(prefs, sleepCheck=0)), 0)

    def test_the_network_never_lengthens_a_hold_or_shortens_other_wakes(self):
        policy = module.Policy()
        policy.woke(1000, 'alarm')
        policy.ready(1000, dark=True, busy=False)
        policy.online(1000 + module.HOLD['alarm'] - 1)
        self.assertTrue(policy.ready(1000 + module.HOLD['alarm'], dark=True, busy=False))
        policy.woke(2000, 'modem')
        policy.ready(2000, dark=True, busy=False)
        policy.online(2000)
        self.assertFalse(policy.ready(2000 + module.HOLD['modem'] - 1, dark=True, busy=False))

    def test_mobile_data_counts_as_online_with_a_default_route(self):
        for out, online in (('[{"dst":"default","dev":"rmnet0"}]', True), ('[]', False), ('', False)):
            done = subprocess.CompletedProcess([], 0, out, '')
            with patch.object(module.subprocess, 'run', return_value=done) as run:
                self.assertEqual(module.network_online('data'), online)
            self.assertEqual(run.call_args.args[0][:2], ['ip', '-j'])

    def test_network_state_comes_from_networkmanager(self):
        for state, online in (('connected', True), ('connecting', False), ('connected (site only)', False)):
            done = subprocess.CompletedProcess([], 0, state + '\n', '')
            with patch.object(module.subprocess, 'run', return_value=done):
                self.assertEqual(module.network_online(), online)
        with patch.object(module.subprocess, 'run', side_effect=FileNotFoundError()):
            self.assertFalse(module.network_online())

    def test_under_the_clock_it_wakes_each_minute_and_sleeps_again_quickly(self):
        self.assertEqual(module.seconds_to_minute(1000 * 60 + 10), 51)
        self.assertEqual(module.seconds_to_minute(1000 * 60 + 58), 63)
        policy = module.Policy()
        policy.last_check = 0
        self.assertEqual(policy.wake_plan(False, 15, wall=100), (None, 'check'))
        # The adapter ticks each minute; the call ends a second past the
        # minute the check is due (900 s after the last).
        self.assertEqual(policy.wake_plan(True, 15, wall=60 * 5 + 10), (591, 'check'))
        self.assertEqual(policy.wake_plan(True, 15, wall=60 * 15 + 10), (51, 'check'))
        self.assertEqual(policy.wake_plan(True, 15, wall=60 * 14 + 58), (63, 'check'))
        self.assertEqual(policy.wake_plan(True, 0, wall=60 * 99), (3541, 'ambient'))
        self.assertEqual(policy.wake_plan(True, 120, wall=60 * 5 + 10), (3591, 'check'))
        policy.woke(1000, 'alarm', 'ambient')
        self.assertTrue(policy.quick)
        self.assertFalse(policy.ready(1000 + module.AMBIENT_HOLD - 0.5, dark=True, busy=False))
        self.assertTrue(policy.ready(1000 + module.AMBIENT_HOLD, dark=True, busy=False))
        policy.woke(2000, 'alarm', 'check')
        self.assertFalse(policy.quick)
        self.assertFalse(policy.ready(2000 + module.GRACE, dark=True, busy=False))

    def test_battery_history_samples_rarely_unless_something_changes(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'battery').mkdir(); (root / 'usb').mkdir()
            for name, value in (('type', 'Battery'), ('capacity', '87'), ('charge_now', '4100000'),
                                ('current_now', '-250000'), ('voltage_now', '3950000'), ('status', 'Discharging')):
                (root / 'battery' / name).write_text(value + '\n')
            (root / 'usb' / 'type').write_text('USB\n'); (root / 'usb' / 'online').write_text('0\n')
            history = module.History(root / 'history.jsonl')
            real = module.battery_now
            with patch.object(module, 'battery_now', side_effect=lambda: real(root)):
                self.assertTrue(history.note('on', now=1000))
                self.assertFalse(history.note('on', now=1100))
                self.assertTrue(history.note('ambient', now=1150))
                self.assertTrue(history.note('ambient', event='wake:touch', now=1160))
                self.assertTrue(history.note('ambient', now=1160 + module.HISTORY_EVERY))
            lines = [json.loads(l) for l in (root / 'history.jsonl').read_text().splitlines()]
            self.assertEqual([l['screen'] for l in lines], ['on', 'ambient', 'ambient', 'ambient'])
            self.assertEqual(lines[0]['capacity'], 87)
            self.assertEqual((lines[0]['charge'], lines[0]['current'], lines[0]['plugged']), (4100000, -250000, False))
            self.assertEqual(lines[2]['event'], 'wake:touch')

    def test_rail_energy_reads_the_meter_and_skips_its_baseline(self):
        with tempfile.TemporaryDirectory() as d:
            meter = Path(d) / 'energy'
            meter.write_text('baseline 0\nbaseline 1\n')
            self.assertEqual(module.rail_energy(meter), {})
            meter.write_text('seconds 0 299.992\nS1M_VDD_MIF 1500000\nseconds 1 300.000\nVSYS_DISPLAY 9000000\n')
            self.assertEqual(module.rail_energy(meter),
                             {'rails': {'S1M_VDD_MIF': 1500000, 'VSYS_DISPLAY': 9000000}, 'metered': 299.992})
            self.assertEqual(module.rail_energy(Path(d) / 'missing'), {})

    def test_processes_are_named_for_what_they_are(self):
        with tempfile.TemporaryDirectory() as d:
            def named(comm, *args):
                folder = Path(d) / '42'
                folder.mkdir(exist_ok=True)
                (folder / 'cmdline').write_bytes(b'\0'.join(a.encode() for a in args) + b'\0')
                with patch.object(module, 'Path', side_effect=lambda p: Path(str(p).replace('/proc', d))):
                    return module.process_label('42', comm)
            self.assertEqual(named('quickshell', 'quickshell', '-n', '-d', '-p',
                                   '/home/cube/.config/quickshell/omarchy-mobile-apps/clock/shell.qml'), 'clock app')
            self.assertEqual(named('quickshell', 'quickshell', '-p', '/home/cube/.config/quickshell/omarchy-mobile/shell.qml'), 'shell')
            self.assertEqual(named('python3', 'python3', '-u', '/usr/local/lib/omarchy-mobile/modem/manager.py', 'run'), 'manager.py')
            self.assertEqual(module.process_label('7', 'kworker/u16:2'), 'kworker')
            self.assertEqual(module.process_label('7', 'Hyprland'), 'Hyprland')

    def test_top_keeps_the_largest_and_sums_the_rest(self):
        self.assertEqual(module.top({'a': 5, 'b': 1, 'c': 3, 'd': 1}, count=2), {'a': 5, 'c': 3, 'other': 2})

    def test_an_alarm_or_timer_ends_the_sleep(self):
        # Nothing due: the plan stands.
        self.assertEqual(module.clock_wake(None, 'check', 15, None, 1000), (None, 'check'))
        # Due before the background check: the sleep ends then.
        self.assertEqual(module.clock_wake(None, 'check', 15, 1000 + 300, 1000), (300, 'clock'))
        # Due after it: the check comes first.
        self.assertEqual(module.clock_wake(None, 'check', 15, 1000 + 1200, 1000), (None, 'check'))
        # Checks off: the adapter's own 900 s is the limit.
        self.assertEqual(module.clock_wake(None, 'check', 0, 1000 + 600, 1000), (600, 'clock'))
        # Under the clock: before the minute-aligned plan, at its own second.
        self.assertEqual(module.clock_wake(591, 'check', 15, 1000 + 125, 1000), (125, 'clock'))
        self.assertEqual(module.clock_wake(51, 'ambient', 0, 1000 + 125, 1000), (51, 'ambient'))

    def test_a_refusal_waits_and_is_logged_once(self):
        policy = module.Policy()
        self.assertTrue(policy.refused(100, 'Unplug before sleeping'))
        self.assertFalse(policy.refused(100 + module.REFUSED_WAIT, 'Unplug before sleeping'))
        policy.ready(100, dark=True, busy=False)
        self.assertFalse(policy.ready(100 + module.REFUSED_WAIT, dark=True, busy=False))
        self.assertTrue(policy.ready(100 + 2 * module.REFUSED_WAIT, dark=True, busy=False))


    def test_a_lasting_refusal_is_logged_again_and_told_once(self):
        policy = module.Policy()
        reason = 'Watchdog suspend support is unavailable'
        self.assertTrue(policy.refused(100, reason))
        self.assertFalse(policy.refused(160, reason))
        self.assertFalse(policy.alert_due(160, on_battery=True))
        self.assertTrue(policy.alert_due(100 + module.REFUSAL_ALERT, on_battery=True))
        self.assertFalse(policy.alert_due(100 + module.REFUSAL_ALERT + 60, on_battery=True))
        self.assertTrue(policy.refused(100 + module.REFUSAL_RELOG, reason))
        self.assertFalse(policy.refused(100 + module.REFUSAL_RELOG + 60, reason))

    def test_the_charger_or_a_plugged_phone_is_never_told(self):
        policy = module.Policy()
        policy.refused(100, 'Unplug before automatic sleep')
        self.assertFalse(policy.alert_due(100 + 2 * module.REFUSAL_ALERT, on_battery=True))
        policy.refused(5000, 'Waiting for modem registration')
        self.assertFalse(policy.alert_due(5000 + module.REFUSAL_ALERT, on_battery=False))
        self.assertTrue(policy.alert_due(5000 + module.REFUSAL_ALERT, on_battery=True))

    def test_inhibit_files_holding_long_on_the_battery_are_told_once(self):
        policy = module.Policy()
        self.assertFalse(policy.inhibit_alert_due(100, 'weather', on_battery=True))
        self.assertFalse(policy.inhibit_alert_due(100 + module.INHIBIT_ALERT - 1, 'weather', on_battery=True))
        self.assertTrue(policy.inhibit_alert_due(100 + module.INHIBIT_ALERT, 'weather', on_battery=True))
        self.assertFalse(policy.inhibit_alert_due(100 + 2 * module.INHIBIT_ALERT, 'weather', on_battery=True))
        # Released and held again: a new stretch.
        self.assertFalse(policy.inhibit_alert_due(9000, '', on_battery=False))
        self.assertFalse(policy.inhibit_alert_due(9002, 'weather', on_battery=True))
        self.assertTrue(policy.inhibit_alert_due(9002 + module.INHIBIT_ALERT, 'weather', on_battery=True))

    def test_inhibit_files_on_the_charger_are_never_told(self):
        policy = module.Policy()
        policy.inhibit_alert_due(100, 'pixel-update', on_battery=False)
        self.assertFalse(policy.inhibit_alert_due(100 + 2 * module.INHIBIT_ALERT, 'pixel-update', on_battery=False))


class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = patch.dict(os.environ, {'XDG_RUNTIME_DIR': str(self.root), 'XDG_CONFIG_HOME': str(self.root),
                                           'XDG_STATE_HOME': str(self.root)})
        self.env.start()
        self.addCleanup(self.env.stop)

    def test_a_programs_wake_request_counts_until_its_hold_runs_out(self):
        wake = self.root / 'omarchy-mobile-wake.d'
        self.assertEqual(module.wake_requests(1000), [])
        wake.mkdir()
        (wake / 'soon').write_text('1300\n')
        (wake / 'past').write_text('970')
        (wake / 'stale').write_text(str(1000 - module.WAKE_REQUEST_HOLD - 1))
        (wake / 'garbage').write_text('soon')
        self.assertEqual(sorted(module.wake_requests(1000)), [970.0, 1300.0])
        with patch.object(module, 'helper_module', return_value=None), patch.object(module.time, 'time', return_value=1000):
            (wake / 'past').unlink()
            self.assertEqual(module.clock_due(), 1300.0)

    def test_sleep_passes_the_background_interval_and_reads_the_wake(self):
        done = subprocess.CompletedProcess([], 0, 'noise\n{"woke": "modem", "slept": 812.4}\n', '')
        with patch.object(module.subprocess, 'run', return_value=done) as run:
            result = module.sleep_once(15)
        self.assertEqual(run.call_args.args[0][1:], ['--keep-dark', '--wake-after', '900'])
        self.assertEqual(result, {'woke': 'modem', 'slept': 812.4})

    def test_the_check_network_reaches_the_adapter(self):
        done = subprocess.CompletedProcess([], 0, '{"woke": "alarm", "slept": 900.0}\n', '')
        with patch.object(module.subprocess, 'run', return_value=done) as run:
            module.sleep_once(15, 'data')
        self.assertEqual(run.call_args.kwargs['env']['OMARCHY_MOBILE_CHECK_NETWORK'], 'data')
        with patch.object(module.subprocess, 'run', return_value=done) as run:
            module.sleep_once(15)
        self.assertEqual(run.call_args.kwargs['env']['OMARCHY_MOBILE_CHECK_NETWORK'], 'wifi')

    def test_a_minute_wake_is_passed_with_its_purpose(self):
        done = subprocess.CompletedProcess([], 0, '{"woke": "alarm", "slept": 50.0}\n', '')
        with patch.object(module.subprocess, 'run', return_value=done) as run:
            module.sleep_once(15, 'wifi', 51, 'ambient')
        self.assertEqual(run.call_args.args[0][1:], ['--keep-dark', '--wake-after', '51'])
        self.assertEqual(run.call_args.kwargs['env']['OMARCHY_MOBILE_WAKE_PURPOSE'], 'ambient')

    def test_the_clock_tick_file_reaches_the_adapter(self):
        done = subprocess.CompletedProcess([], 0, '{"woke": "alarm", "slept": 580.0, "minute_ticks": 9}\n', '')
        with patch.object(module.subprocess, 'run', return_value=done) as run:
            module.sleep_once(15, 'wifi', 591, 'check', module.minute_tick())
        self.assertEqual(run.call_args.kwargs['env']['OMARCHY_MOBILE_MINUTE_TICK'],
                         str(self.root / 'omarchy-mobile-minute-tick'))
        with patch.object(module.subprocess, 'run', return_value=done) as run:
            module.sleep_once(15)
        self.assertNotIn('OMARCHY_MOBILE_MINUTE_TICK', run.call_args.kwargs['env'])
        module.ambient_tick()
        self.assertTrue(module.minute_tick().read_text().strip().isdigit())

    def test_no_background_wake_when_turned_off(self):
        done = subprocess.CompletedProcess([], 0, '{"woke": "power-key", "slept": 3.0}\n', '')
        with patch.object(module.subprocess, 'run', return_value=done) as run:
            module.sleep_once(0)
        self.assertEqual(run.call_args.args[0][1:], ['--keep-dark'])

    def test_a_failed_sleep_is_reported(self):
        done = subprocess.CompletedProcess([], 1, '', 'A device failed to resume')
        with patch.object(module.subprocess, 'run', return_value=done):
            result = module.sleep_once(15)
        self.assertEqual(result['woke'], 'error')
        self.assertIn('failed to resume', result['error'])

    def test_calls_and_inhibitors_are_seen(self):
        self.assertFalse(module.in_call())
        phone = self.root / 'omarchy-mobile-phone'
        phone.mkdir()
        (phone / 'calls.json').write_text('{}')
        self.assertFalse(module.in_call())
        (phone / 'calls.json').write_text(json.dumps({'3': {'state': 'ringing-in'}}))
        self.assertTrue(module.in_call())
        self.assertEqual(module.inhibitors(), [])
        (self.root / 'omarchy-mobile-sleep-inhibit').mkdir()
        (self.root / 'omarchy-mobile-sleep-inhibit' / 'ssh').touch()
        self.assertEqual(module.inhibitors(), ['ssh'])
        # One that names its process holds only while that process lives.
        (self.root / 'omarchy-mobile-sleep-inhibit' / 'speaker').write_text(f'{os.getpid()}\n')
        self.assertEqual(module.inhibitors(), ['speaker', 'ssh'])
        dead = subprocess.Popen(['true']); dead.wait()
        (self.root / 'omarchy-mobile-sleep-inhibit' / 'speaker').write_text(f'{dead.pid}\n')
        self.assertEqual(module.inhibitors(), ['ssh'])
        self.assertFalse((self.root / 'omarchy-mobile-sleep-inhibit' / 'speaker').exists())

    def test_playing_audio_means_an_uncorked_stream(self):
        loopback = {'index': 3, 'corked': False, 'properties': {'node.virtual': 'true', 'media.role': 'Loopback'}}
        done = subprocess.CompletedProcess([], 0, json.dumps([loopback]), '')
        with patch.object(module.subprocess, 'run', return_value=done):
            self.assertFalse(module.playing_audio())
        streams = [{'index': 1, 'corked': True}, {'index': 2, 'corked': False}]
        done = subprocess.CompletedProcess([], 0, json.dumps(streams), '')
        with patch.object(module.subprocess, 'run', return_value=done):
            self.assertTrue(module.playing_audio())
        done = subprocess.CompletedProcess([], 0, json.dumps(streams[:1]), '')
        with patch.object(module.subprocess, 'run', return_value=done):
            self.assertFalse(module.playing_audio())

    def test_playing_audio_names_the_programs(self):
        streams = [{'corked': False, 'properties': {'application.process.binary': 'QtWebEngineProcess'}},
                   {'corked': False, 'properties': {'application.name': 'waydroid'}},
                   {'corked': True, 'properties': {'application.process.binary': 'mpv'}}]
        done = subprocess.CompletedProcess([], 0, json.dumps(streams), '')
        with patch.object(module.subprocess, 'run', return_value=done):
            self.assertEqual(module.playing_audio(), ['QtWebEngineProcess', 'waydroid'])


class AwakeReasonTests(unittest.TestCase):
    def test_a_dark_phone_says_why_it_is_not_asleep(self):
        policy = module.Policy()
        policy.ready(100, dark=True, busy=False)
        self.assertEqual(policy.waiting(105), 'grace')
        policy.woke(200, 'modem')
        self.assertEqual(policy.waiting(205), 'hold:modem')
        policy.refused(300, 'Unplug before automatic sleep')
        self.assertEqual(policy.waiting(310), 'refused:Unplug before automatic sleep')
        self.assertEqual(policy.waiting(400), 'checking')
        policy.woke(500, 'alarm', 'ambient')
        self.assertEqual(policy.waiting(500.5), 'hold:minute')

    def test_the_history_line_carries_the_awake_time_by_reason(self):
        with tempfile.TemporaryDirectory() as d:
            history = module.History(Path(d) / 'history.jsonl')
            with patch.object(module, 'battery_now', return_value={}), \
                    patch.object(module, 'rail_energy', return_value={}):
                history.awake_for('audio:QtWebEngineProcess', 120.4)
                history.awake_for('hold:modem', 15)
                history.awake_for('audio:QtWebEngineProcess', 30)
                history.awake_for(None, 50)
                history.note('off', now=1000)
                history.note('off', event='wake:alarm', now=1001)
            lines = [json.loads(l) for l in (Path(d) / 'history.jsonl').read_text().splitlines()]
            self.assertEqual(lines[0]['awake'], {'audio:QtWebEngineProcess': 150, 'hold:modem': 15})
            self.assertNotIn('awake', lines[1])


if __name__ == '__main__':
    unittest.main()


class CellularNoticeTests(unittest.TestCase):
    def test_the_notice_names_the_fix(self):
        with patch.object(module.subprocess, 'run') as run:
            module.notify_cellular_stopped()
        args = run.call_args.args[0]
        self.assertIn('Mobile network stopped', args)
        self.assertTrue(any('Restart the phone' in arg for arg in args))
