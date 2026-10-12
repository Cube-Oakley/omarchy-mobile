import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('pixel_suspend',
    Path(__file__).resolve().parents[1] / 'devices/pixel7pro/scripts/pixel-suspend.py')
sleep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sleep)


class AlarmTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / 'since_epoch').write_text('1000')
        self.alarm = self.root / 'wakealarm'
        self.alarm.write_text('')
        self.override = patch.object(sleep, 'RTC', self.root)
        self.override.start()

    def tearDown(self):
        self.override.stop()
        self.temp.cleanup()

    def test_existing_earlier_alarm_is_untouched(self):
        self.alarm.write_text('1020')
        alarm = sleep.Alarm(60)
        self.assertEqual(self.alarm.read_text(), '1020')
        alarm.restore()
        self.assertEqual(self.alarm.read_text(), '1020')

    def test_later_alarm_is_restored_after_our_wake(self):
        self.alarm.write_text('1200')
        alarm = sleep.Alarm(60)
        self.assertEqual(self.alarm.read_text().strip(), '1060')
        self.alarm.write_text('')  # hardware fired our alarm
        (self.root / 'since_epoch').write_text('1060')
        alarm.restore()
        self.assertEqual(self.alarm.read_text().strip(), '1200')

    def test_new_alarm_from_another_owner_is_preserved(self):
        alarm = sleep.Alarm(60)
        self.alarm.write_text('1090')
        alarm.restore()
        self.assertEqual(self.alarm.read_text(), '1090')


class TransitionTests(unittest.TestCase):
    def setUp(self):
        # No Android and no mobile data unless a test says so.
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        for name, value in (('SUSPEND_ACTIVE', Path(temp.name) / 'active'),
                            ('push_over_data', unittest.mock.Mock(return_value=None)),
                            ('data_packets', unittest.mock.Mock(return_value=None))):
            override = patch.object(sleep, name, value)
            override.start()
            self.addCleanup(override.stop)

    def test_live_call_or_audio_refuses_sleep(self):
        for calls, audio in (([dict(state='ringing-in')], False), ([], True)):
            with patch.object(sleep, 'request', return_value=dict(
                    modem=dict(ready=True), calls=calls, audio=dict(running=audio))):
                with self.assertRaises(RuntimeError):
                    sleep.idle_modem()

    def test_terminated_call_does_not_inhibit(self):
        with patch.object(sleep, 'request', return_value=dict(modem=dict(ready=True),
                calls=[dict(state='terminated')], audio=dict(running=False))):
            sleep.idle_modem()

    def test_pending_wake_restores_alarm_modem_and_display(self):
        with patch.object(sleep, 'prerequisites'), patch.object(sleep, 'display') as display, \
             patch.object(sleep, 'request') as request, patch.object(sleep.os, 'sync'), \
             patch.object(sleep, 'Alarm') as alarm, patch.object(sleep, 'sensors') as sensors, \
             patch.object(sleep, 'wake_count', side_effect=TimeoutError('active')):
            with self.assertRaises(TimeoutError):
                sleep.suspend(60, False, True)
            alarm.return_value.restore.assert_called_once()
            self.assertEqual([c.args for c in sensors.call_args_list], [(False,), (True,)])
            self.assertEqual([c.args for c in display.call_args_list], [(False,), (True,)])
            self.assertEqual([c.kwargs for c in request.call_args_list if c.kwargs],
                             [dict(on=False), dict(on=True)])

    def test_partial_modem_failure_attempts_foreground_restore(self):
        with patch.object(sleep, 'prerequisites'), patch.object(sleep, 'display'), \
             patch.object(sleep, 'sensors') as sensors, \
             patch.object(sleep, 'request', side_effect=[dict(screen_on=True), TimeoutError(), {}]) as request:
            with self.assertRaises(TimeoutError):
                sleep.suspend(60, True, True)
            self.assertEqual([c.kwargs for c in request.call_args_list if c.kwargs],
                             [dict(on=False), dict(on=True)])
            sensors.assert_not_called()

    def test_quiet_modem_wakes_sleep_again_and_report_the_deep_current(self):
        sleeps = [dict(woke='modem', slept=50.0, sleep='deep', used_nah=1_000_000),
                  dict(woke='modem', slept=0.0, sleep='deep-early'),
                  dict(woke='alarm', slept=40.0, sleep='deep', used_nah=500_000)]
        with patch.object(sleep, 'prerequisites'), patch.object(sleep, 'request', return_value={}), \
             patch.object(sleep, 'deep_ready', return_value=True), patch.object(sleep, 'deep_watchdog'), \
             patch.object(sleep, 'wifi_off'), patch.object(sleep, 'bluetooth_off') as bluetooth_off, \
             patch.object(sleep, 'bluetooth_stays', return_value=False), patch.object(sleep, 'bluetooth_wake'), \
             patch.object(sleep, 'wifi_on') as wifi_on, patch.object(sleep.os, 'sync'), \
             patch.object(sleep, 'Alarm'), patch.object(sleep, 'modem_events', return_value=3), \
             patch.object(sleep, 'sensors'), \
             patch.object(sleep, 'deep_sleep', side_effect=sleeps), \
             patch.object(sleep, 'modem_brought_something', return_value=False):
            result = sleep.suspend(900, True, False)
        bluetooth_off.assert_called_once()
        wifi_on.assert_called_once()
        self.assertEqual(result, dict(woke='alarm', slept=90.0, sleep='deep', quiet_modem_wakes=2, deep_ma=60.0,
                                      bluetooth='unloaded'))


    def test_a_packet_on_mobile_data_ends_the_sleep_as_a_network_wake(self):
        sleeps = [dict(woke='modem', slept=300.0, sleep='deep', used_nah=1_000_000)]
        with patch.object(sleep, 'prerequisites'), patch.object(sleep, 'request', return_value={}), \
             patch.object(sleep, 'deep_ready', return_value=True), patch.object(sleep, 'deep_watchdog'), \
             patch.object(sleep, 'wifi_off'), patch.object(sleep, 'bluetooth_off'), \
             patch.object(sleep, 'bluetooth_stays', return_value=False), patch.object(sleep, 'bluetooth_wake'), \
             patch.object(sleep, 'wifi_on') as wifi_on, patch.object(sleep.os, 'sync'), \
             patch.object(sleep, 'Alarm'), patch.object(sleep, 'modem_events', return_value=3), \
             patch.object(sleep, 'sensors'), patch.object(sleep, 'deep_sleep', side_effect=sleeps), \
             patch.object(sleep, 'modem_brought_something', return_value=False), \
             patch.object(sleep, 'data_packets', side_effect=[10, 12]):
            result = sleep.suspend(900, True, False)
        wifi_on.assert_not_called()
        self.assertEqual(result['woke'], 'network')
        self.assertEqual(result['quiet_modem_wakes'], 0)

    def test_without_a_modem_session_it_sleeps_and_says_cellular_stopped(self):
        sleeps = [dict(woke='alarm', slept=600.0, sleep='deep')]
        with patch.object(sleep, 'prerequisites', return_value=False), \
             patch.object(sleep, 'request', side_effect=FileNotFoundError('no telephony socket')) as request, \
             patch.object(sleep, 'deep_ready', return_value=True), patch.object(sleep, 'deep_watchdog'), \
             patch.object(sleep, 'wifi_off'), patch.object(sleep, 'bluetooth_off'), \
             patch.object(sleep, 'bluetooth_stays', return_value=False), patch.object(sleep, 'bluetooth_wake'), \
             patch.object(sleep, 'wifi_on'), patch.object(sleep.os, 'sync'), \
             patch.object(sleep, 'Alarm'), patch.object(sleep, 'modem_events') as modem_events, \
             patch.object(sleep, 'sensors'), patch.object(sleep, 'deep_sleep', side_effect=sleeps) as deep_sleep:
            result = sleep.suspend(900, True, False)
        request.assert_not_called()
        modem_events.assert_not_called()
        deep_sleep.assert_called_once_with(False)
        self.assertEqual(result['cellular'], 'stopped')
        self.assertEqual(result['woke'], 'alarm')

    def test_sleeps_that_end_at_once_pause_and_stop_after_three(self):
        sleeps = [dict(woke='modem', slept=0.0, sleep='deep-early')] * 5
        with patch.object(sleep, 'prerequisites'), patch.object(sleep, 'request', return_value={}), \
             patch.object(sleep, 'deep_ready', return_value=True), patch.object(sleep, 'deep_watchdog'), \
             patch.object(sleep, 'wifi_off'), patch.object(sleep, 'bluetooth_off'), \
             patch.object(sleep, 'bluetooth_stays', return_value=False), patch.object(sleep, 'bluetooth_wake'), \
             patch.object(sleep, 'wifi_on'), patch.object(sleep.os, 'sync'), \
             patch.object(sleep, 'Alarm'), patch.object(sleep, 'modem_events', return_value=3), \
             patch.object(sleep, 'sensors'), patch.object(sleep, 'deep_sleep', side_effect=sleeps) as deep, \
             patch.object(sleep, 'modem_brought_something', return_value=False), \
             patch.object(sleep.time, 'sleep') as pause:
            result = sleep.suspend(900, True, False)
        self.assertEqual(deep.call_count, 3)
        self.assertEqual([call.args[0] for call in pause.call_args_list], [2, 2])
        self.assertEqual((result['woke'], result['early_wakes']), ('modem', 3))

    def test_push_keepalives_wake_hold_and_sleep_again(self):
        clock = [1000.0]

        def deep_sleep(modem=True):
            clock[0] += alarm.call_args.args[0]
            return dict(woke='alarm', slept=alarm.call_args.args[0], sleep='deep')
        with patch.object(sleep, 'prerequisites'), patch.object(sleep, 'request', return_value={}), \
             patch.object(sleep, 'deep_ready', return_value=True), patch.object(sleep, 'deep_watchdog'), \
             patch.object(sleep, 'wifi_off'), patch.object(sleep, 'bluetooth_off'), \
             patch.object(sleep, 'bluetooth_stays', return_value=False), patch.object(sleep, 'bluetooth_wake'), \
             patch.object(sleep, 'wifi_on'), patch.object(sleep.os, 'sync'), \
             patch.object(sleep, 'Alarm') as alarm, patch.object(sleep, 'modem_events', return_value=3), \
             patch.object(sleep, 'sensors'), patch.object(sleep, 'deep_sleep', side_effect=deep_sleep), \
             patch.object(sleep, 'push_over_data', return_value='moved'), \
             patch.object(sleep, 'push_connected', return_value=True), \
             patch.object(sleep.time, 'sleep') as hold, \
             patch.object(sleep.time, 'time', side_effect=lambda: clock[0]):
            result = sleep.suspend(1300, True, False)
        self.assertEqual([call.args[0] for call in alarm.call_args_list], [600, 600, 100])
        self.assertEqual([call.args[0] for call in hold.call_args_list], [5, 5])
        self.assertEqual((result['woke'], result['push'], result['push_keepalives']), ('alarm', 'moved', 2))

    def test_the_wake_counts_from_the_call_not_from_getting_ready(self):
        clock = [1000.0]

        def push_over_data(wifi_went):
            clock[0] += 40
            return 'moved'

        def deep_sleep(modem=True):
            clock[0] += alarm.call_args.args[0]
            return dict(woke='alarm', slept=alarm.call_args.args[0], sleep='deep')
        with patch.object(sleep, 'prerequisites'), patch.object(sleep, 'request', return_value={}), \
             patch.object(sleep, 'deep_ready', return_value=True), patch.object(sleep, 'deep_watchdog'), \
             patch.object(sleep, 'wifi_off'), patch.object(sleep, 'bluetooth_off'), \
             patch.object(sleep, 'bluetooth_stays', return_value=False), patch.object(sleep, 'bluetooth_wake'), \
             patch.object(sleep, 'wifi_on'), patch.object(sleep.os, 'sync'), \
             patch.object(sleep, 'Alarm') as alarm, patch.object(sleep, 'modem_events', return_value=3), \
             patch.object(sleep, 'sensors'), patch.object(sleep, 'deep_sleep', side_effect=deep_sleep), \
             patch.object(sleep, 'push_over_data', side_effect=push_over_data), \
             patch.object(sleep, 'push_connected', return_value=True), \
             patch.object(sleep.time, 'time', side_effect=lambda: clock[0]):
            result = sleep.suspend(100, True, False)
        self.assertEqual([call.args[0] for call in alarm.call_args_list], [60])
        self.assertEqual((result['woke'], clock[0]), ('alarm', 1100.0))

    def test_a_mobile_data_check_leaves_wifi_off(self):
        sleeps = [dict(woke='alarm', slept=900.0, sleep='deep', used_nah=10_000_000)]
        with patch.object(sleep, 'prerequisites'), patch.object(sleep, 'request', return_value={}), \
             patch.object(sleep, 'deep_ready', return_value=True), patch.object(sleep, 'deep_watchdog'), \
             patch.object(sleep, 'wifi_off'), patch.object(sleep, 'bluetooth_off'), \
             patch.object(sleep, 'bluetooth_stays', return_value=False), patch.object(sleep, 'bluetooth_wake'), \
             patch.object(sleep, 'wifi_on') as wifi_on, patch.object(sleep.os, 'sync'), \
             patch.object(sleep, 'Alarm'), patch.object(sleep, 'modem_events', return_value=3), \
             patch.object(sleep, 'sensors'), patch.object(sleep, 'deep_sleep', side_effect=list(sleeps)), \
             patch.dict(sleep.os.environ, {'OMARCHY_MOBILE_CHECK_NETWORK': 'data'}):
            sleep.suspend(900, True, False)
        wifi_on.assert_not_called()


    def test_a_screen_lit_while_it_got_ready_gets_wifi_and_bluetooth_back(self):
        for lit, marker in ((True, True), (True, False), (False, True)):
            sleeps = [dict(woke='screen', slept=0.0, sleep='deep-early')]
            with tempfile.TemporaryDirectory() as root, \
                 patch.object(sleep, 'prerequisites'), patch.object(sleep, 'request', return_value={}), \
                 patch.object(sleep, 'deep_ready', return_value=True), patch.object(sleep, 'deep_watchdog'), \
                 patch.object(sleep, 'wifi_off'), patch.object(sleep, 'bluetooth_off'), \
                 patch.object(sleep, 'bluetooth_stays', return_value=False), patch.object(sleep, 'bluetooth_wake'), \
                 patch.object(sleep, 'wifi_on') as wifi_on, patch.object(sleep.os, 'sync'), \
                 patch.object(sleep, 'Alarm'), patch.object(sleep, 'modem_events', return_value=3), \
                 patch.object(sleep, 'sensors'), patch.object(sleep, 'deep_sleep', side_effect=list(sleeps)), \
                 patch.object(sleep, 'screen_lit', return_value=lit), \
                 patch.object(sleep, 'BT_SLEEPING', Path(root) / 'marker'), \
                 patch.object(sleep.subprocess, 'run') as run:
                if marker:
                    (Path(root) / 'marker').touch()
                result = sleep.suspend(900, True, False)
            self.assertEqual(result['woke'], 'screen')
            self.assertEqual(wifi_on.called, lit)
            restarts = [call for call in run.call_args_list if call.args[0] == [sleep.BT_RESTART]]
            self.assertEqual(len(restarts), int(lit and marker))

    def test_a_clock_minute_wake_leaves_wifi_off(self):
        sleeps = [dict(woke='alarm', slept=55.0, sleep='deep', used_nah=1_000_000)]
        with patch.object(sleep, 'prerequisites'), patch.object(sleep, 'request', return_value={}), \
             patch.object(sleep, 'deep_ready', return_value=True), patch.object(sleep, 'deep_watchdog'), \
             patch.object(sleep, 'wifi_off'), patch.object(sleep, 'bluetooth_off'), \
             patch.object(sleep, 'bluetooth_stays', return_value=False), patch.object(sleep, 'bluetooth_wake'), \
             patch.object(sleep, 'wifi_on') as wifi_on, patch.object(sleep.os, 'sync'), \
             patch.object(sleep, 'Alarm'), patch.object(sleep, 'modem_events', return_value=3), \
             patch.object(sleep, 'sensors'), patch.object(sleep, 'deep_sleep', side_effect=list(sleeps)), \
             patch.dict(sleep.os.environ, {'OMARCHY_MOBILE_WAKE_PURPOSE': 'ambient'}):
            sleep.suspend(55, True, False)
        wifi_on.assert_not_called()


    def test_a_finger_outranks_the_clock_and_the_modem(self):
        before = dict.fromkeys(('power-key', 'touch', 'alarm', 'modem'), 0)
        both = dict(before, touch=1, alarm=1, modem=2)
        self.assertEqual(sleep.deep_wake_reason(before, both), 'touch')
        self.assertEqual(sleep.deep_wake_reason(before, dict(before, alarm=1)), 'alarm')
        self.assertEqual(sleep.deep_wake_reason(both, dict(both, **{'power-key': 1})), 'power-key')

    def test_bluetooth_traffic_outranks_the_clock_but_not_a_finger(self):
        before = dict.fromkeys(('power-key', 'touch', 'bluetooth', 'alarm', 'modem'), 0)
        self.assertEqual(sleep.deep_wake_reason(before, dict(before, bluetooth=1, alarm=1, modem=1)),
                         'bluetooth')
        self.assertEqual(sleep.deep_wake_reason(before, dict(before, bluetooth=1, touch=1)), 'touch')

    def test_bluetooth_stays_through_deep_sleep_when_its_low_power_mode_is_wired(self):
        sleeps = [dict(woke='bluetooth', slept=300.0, sleep='deep', used_nah=1_000_000)]
        with patch.object(sleep, 'prerequisites'), patch.object(sleep, 'request', return_value={}), \
             patch.object(sleep, 'deep_ready', return_value=True), patch.object(sleep, 'deep_watchdog'), \
             patch.object(sleep, 'wifi_off'), patch.object(sleep, 'bluetooth_off') as bluetooth_off, \
             patch.object(sleep, 'bluetooth_stays', return_value=True), \
             patch.object(sleep, 'bluetooth_wake') as bluetooth_wake, \
             patch.object(sleep, 'wifi_on') as wifi_on, patch.object(sleep.os, 'sync'), \
             patch.object(sleep, 'Alarm'), patch.object(sleep, 'modem_events', return_value=3), \
             patch.object(sleep, 'sensors'), patch.object(sleep, 'deep_sleep', side_effect=sleeps):
            result = sleep.suspend(900, True, False)
        bluetooth_off.assert_not_called()
        bluetooth_wake.assert_called_once_with(True)
        wifi_on.assert_not_called()
        self.assertEqual((result['woke'], result['bluetooth']), ('bluetooth', 'kept'))

    def test_the_host_wake_is_added_to_the_wake_list_and_taken_out_again(self):
        with tempfile.TemporaryDirectory() as d, patch.object(sleep, 'SLEEP_PARAMS', Path(d)):
            wake = Path(d) / 'eint_wake'
            wake.write_text('43,44,62,60\n')
            sleep.bluetooth_wake(True)
            self.assertEqual(wake.read_text(), '43,44,62,60,51\n')
            sleep.bluetooth_wake(True)
            self.assertEqual(wake.read_text(), '43,44,62,60,51\n')
            sleep.bluetooth_wake(False)
            self.assertEqual(wake.read_text(), '43,44,62,60\n')

    def test_bluetooth_is_in_use_with_a_connection_or_a_registered_app(self):
        with tempfile.TemporaryDirectory() as d:
            runtime = Path(d)
            listed = type('Run', (), dict(returncode=0, stdout=''))
            with patch.object(sleep, 'session_runtime', return_value=runtime), \
                 patch.object(sleep.subprocess, 'run', return_value=listed):
                self.assertFalse(sleep.bluetooth_in_use())
                listed.stdout = 'Device 00:11:22:33:44:55 watch\n'
                self.assertTrue(sleep.bluetooth_in_use())
                listed.stdout = ''
                (runtime / sleep.BT_SLEEP_APPS).mkdir()
                self.assertFalse(sleep.bluetooth_in_use())
                (runtime / sleep.BT_SLEEP_APPS / 'key').touch()
                self.assertTrue(sleep.bluetooth_in_use())
            with patch.object(sleep, 'session_runtime', return_value=runtime / 'none'), \
                 patch.object(sleep.subprocess, 'run', side_effect=subprocess.TimeoutExpired('bluetoothctl', 5)):
                # Unknown: keep what may be a link.
                self.assertTrue(sleep.bluetooth_in_use())

    def test_bluetooth_stays_only_when_on_and_wired(self):
        with tempfile.TemporaryDirectory() as d:
            params = Path(d)
            (params / 'sleep_ready').write_text('Y\n')
            interrupts = params / 'interrupts'
            interrupts.write_text('           CPU0\n 236:          4  gs201-weint   6 Edge      host_wake\n')
            powered = type('Run', (), dict(returncode=0, stdout='   variant       boolean true\n'))
            real_path = sleep.Path
            def path(name, *rest):
                return interrupts if name == '/proc/interrupts' else real_path(name, *rest)
            flag = params / 'bluetooth-unload-in-sleep'
            in_use = [True]
            with patch.object(sleep, 'BT_PARAMS', params), patch.object(sleep, 'Path', side_effect=path), \
                 patch.object(sleep, 'BT_UNLOAD_FLAG', flag), \
                 patch.object(sleep, 'bluetooth_in_use', side_effect=lambda: in_use[0]), \
                 patch.object(sleep.subprocess, 'run', return_value=powered):
                # Kept while in use; the opt-out flag unloads it for deep sleep.
                self.assertTrue(sleep.bluetooth_stays())
                in_use[0] = False
                self.assertFalse(sleep.bluetooth_stays())
                in_use[0] = True
                flag.touch()
                self.assertFalse(sleep.bluetooth_stays())
                flag.unlink()
                self.assertTrue(sleep.bluetooth_stays())
                powered.stdout = '   variant       boolean false\n'
                self.assertFalse(sleep.bluetooth_stays())
                powered.stdout = '   variant       boolean true\n'
                interrupts.write_text('           CPU0\n')
                self.assertFalse(sleep.bluetooth_stays())
                (params / 'sleep_ready').write_text('N\n')
                self.assertFalse(sleep.bluetooth_stays())

    def test_files_in_the_users_directory_are_written_only_when_plain(self):
        with tempfile.TemporaryDirectory() as d:
            folder = Path(d)
            plain = folder / 'tick'
            sleep.write_user_file(plain, '1\n')
            self.assertEqual(plain.read_text(), '1\n')
            target = folder / 'roots-file'
            target.write_text('keep\n')
            (folder / 'link').symlink_to(target)
            sleep.write_user_file(folder / 'link', 'x\n')
            (folder / 'hard').hardlink_to(target)
            sleep.write_user_file(folder / 'hard', 'x\n')
            self.assertEqual(target.read_text(), 'keep\n')

    def test_under_the_clock_each_minute_redraws_and_sleeps_again(self):
        clock = [1000 * 60 + 10.0]

        def deep_sleep(modem=True):
            clock[0] = (clock[0] // 60 + 1) * 60 + 1  # A second past the next minute.
            return dict(woke='alarm', slept=50.0, sleep='deep', used_nah=1_000_000)
        with patch.object(sleep, 'prerequisites'), patch.object(sleep, 'request', return_value={}), \
             patch.object(sleep, 'deep_ready', return_value=True), patch.object(sleep, 'deep_watchdog'), \
             patch.object(sleep, 'wifi_off'), patch.object(sleep, 'bluetooth_off'), \
             patch.object(sleep, 'bluetooth_stays', return_value=False), patch.object(sleep, 'bluetooth_wake'), \
             patch.object(sleep, 'wifi_on') as wifi_on, patch.object(sleep.os, 'sync'), \
             patch.object(sleep, 'Alarm') as alarm, patch.object(sleep, 'modem_events', return_value=3), \
             patch.object(sleep, 'sensors') as sensors, patch.object(sleep, 'deep_sleep', side_effect=deep_sleep), \
             patch.object(sleep, 'minute_tick') as tick, patch.object(sleep.time, 'time', side_effect=lambda: clock[0]), \
             patch.object(sleep, 'session_runtime', return_value=Path('/run/user/1001')), \
             patch.dict(sleep.os.environ, {'OMARCHY_MOBILE_MINUTE_TICK': '/etc/shadow',
                                           'OMARCHY_MOBILE_WAKE_PURPOSE': 'check'}):
            # Three minutes on, a second past the minute: the background check.
            result = sleep.suspend(171, True, False)
        # The file is the shell's by its fixed name in the session's runtime
        # directory; the caller's path is never used (this runs as root).
        self.assertEqual(tick.call_args_list,
                         [unittest.mock.call(Path('/run/user/1001/omarchy-mobile-minute-tick'))] * 2)
        self.assertEqual([call.args[0] for call in alarm.call_args_list], [51, 60, 60])
        # Nothing is put back between minutes: one pause, one resume.
        self.assertEqual([call.args[0] for call in sensors.call_args_list], [False, True])
        wifi_on.assert_called_once()
        self.assertEqual(result, dict(woke='alarm', slept=150.0, sleep='deep', quiet_modem_wakes=0,
                                      bluetooth='unloaded', minute_ticks=2, deep_ma=72.0))

    def test_a_minute_is_drawn_once_the_display_has_sent_it_and_powered_off(self):
        stats = [dict(completed_transfers=5, display_powered_off=1),
                 dict(completed_transfers=5, display_powered_off=1),
                 dict(completed_transfers=6, display_powered_off=0),
                 dict(completed_transfers=6, display_powered_off=1)]
        with tempfile.TemporaryDirectory() as root, patch.object(sleep, 'SCANOUT_STATS', Path(root)), \
             patch.object(sleep, 'scanout', side_effect=stats) as scanout, patch.object(sleep.time, 'sleep'):
            sleep.minute_tick(Path(root) / 'tick')
            self.assertTrue((Path(root) / 'tick').read_text().strip().isdigit())
        self.assertEqual(scanout.call_count, 4)

    def test_scanout_counters_are_read_from_debugfs(self):
        with tempfile.TemporaryDirectory() as root:
            stats = Path(root) / 'pixel_scanout'
            stats.write_text('completed_transfers 389\nfailures 0\ndisplay_powered_off 1\nrefresh_hz 60 Hz\n')
            with patch.object(sleep, 'SCANOUT_STATS', stats):
                self.assertEqual(sleep.scanout(), dict(completed_transfers=389, failures=0, display_powered_off=1))
            stats.write_text('failures 0\n')
            with patch.object(sleep, 'SCANOUT_STATS', stats):
                self.assertIsNone(sleep.scanout())


    def test_a_minute_that_comes_due_during_a_quiet_modem_wake_is_still_drawn(self):
        clock = [1000 * 60 + 10.0]
        wakes = iter([('modem', 1000 * 60 + 59.5), ('alarm', 1002 * 60 + 1.5), ('alarm', 1003 * 60 + 1)])

        def deep_sleep(modem=True):
            reason, clock[0] = next(wakes)
            return dict(woke=reason, slept=40.0, sleep='deep')

        def looked_at(*_):
            clock[0] += 1.0  # The minute's alarm fires while awake.
            return False
        with patch.object(sleep, 'prerequisites'), patch.object(sleep, 'request', return_value={}), \
             patch.object(sleep, 'deep_ready', return_value=True), patch.object(sleep, 'deep_watchdog'), \
             patch.object(sleep, 'wifi_off'), patch.object(sleep, 'bluetooth_off'), patch.object(sleep, 'wifi_on'), \
             patch.object(sleep, 'bluetooth_stays', return_value=False), patch.object(sleep, 'bluetooth_wake'), \
             patch.object(sleep.os, 'sync'), patch.object(sleep, 'Alarm') as alarm, \
             patch.object(sleep, 'modem_events', return_value=3), patch.object(sleep, 'sensors'), \
             patch.object(sleep, 'deep_sleep', side_effect=deep_sleep), \
             patch.object(sleep, 'modem_brought_something', side_effect=looked_at), \
             patch.object(sleep, 'minute_tick') as tick, patch.object(sleep.time, 'time', side_effect=lambda: clock[0]), \
             patch.dict(sleep.os.environ, {'OMARCHY_MOBILE_MINUTE_TICK': '/run/tick'}):
            result = sleep.suspend(171, True, False)
        # :00.5 after the modem: drawn before sleeping, the next alarm a minute on.
        self.assertEqual(tick.call_count, 2)
        self.assertEqual([call.args[0] for call in alarm.call_args_list], [51, 61, 60])
        self.assertEqual((result['woke'], result['quiet_modem_wakes'], result['minute_ticks']), ('alarm', 1, 2))


    def test_wake_sources_still_active_are_a_wake_not_a_refusal(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            for name, value in (('refused', '0'), ('early_count', '0'), ('budget', '0'), ('deep_link_off', '0'),
                                ('mem_sleep', 's2idle'), ('wakeup_count', '7'), ('state', '')):
                (root / name).write_text(value + '\n')
            counts = iter([dict(alarm=0, modem=4, **{'power-key': 0}), dict(alarm=0, modem=5, **{'power-key': 0})])
            with patch.object(sleep, 'SLEEP_PARAMS', root), patch.object(sleep, 'UFS_PARAMS', root), \
                 patch.object(sleep, 'POWER', root), patch.object(sleep, 'irq_counts', side_effect=lambda: next(counts)), \
                 patch.object(sleep, 'wake_count', side_effect=TimeoutError('Wake sources are still active')):
                result = sleep.deep_sleep()
            self.assertEqual(result, dict(woke='modem', slept=0.0, sleep='deep-early'))
            self.assertEqual((root / 'state').read_text(), '\n')
            self.assertEqual((root / 'mem_sleep').read_text(), 's2idle\n')

    def test_a_try_waits_for_the_modem_link_to_go_down(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            link = root / 'modem_link'
            states = iter(['1\n', '1\n', '0\n'])
            with patch.object(sleep, 'SLEEP_PARAMS', root), patch.object(sleep.time, 'sleep') as pause:
                def read_link(path):
                    return next(states).strip() if Path(path) == link else ''
                with patch.object(sleep, 'read', side_effect=read_link):
                    link.write_text('1\n')
                    self.assertTrue(sleep.modem_link_down())
                self.assertEqual(pause.call_count, 2)
                # A link that stays up: the try goes ahead after the wait.
                link.write_text('1\n')
                with patch.object(sleep.time, 'monotonic', side_effect=[0.0, 1.0, 2.0, 3.5]):
                    self.assertFalse(sleep.modem_link_down(3))
                # A pixel-sleep without the parameter: no wait.
                link.unlink()
                self.assertTrue(sleep.modem_link_down())

    def test_a_screen_lit_during_the_call_is_not_slept_under(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            for name, value in (('refused', '0'), ('early_count', '0'), ('budget', '0'), ('deep_link_off', '0'),
                                ('mem_sleep', 's2idle'), ('wakeup_count', '7'), ('state', '')):
                (root / name).write_text(value + '\n')
            (root / 'omarchy-mobile').mkdir()
            (root / 'omarchy-mobile/screen').write_text('on\n')
            with patch.object(sleep, 'SLEEP_PARAMS', root), patch.object(sleep, 'UFS_PARAMS', root), \
                 patch.object(sleep, 'POWER', root), patch.object(sleep, 'session_runtime', return_value=root), \
                 patch.object(sleep, 'irq_counts', return_value={}), patch.object(sleep, 'wake_count') as count:
                result = sleep.deep_sleep()
                self.assertEqual(result, dict(woke='screen', slept=0.0, sleep='deep-early'))
                count.assert_not_called()
                self.assertEqual((root / 'state').read_text(), '\n')
                (root / 'omarchy-mobile/screen').write_text('ambient\n')
                self.assertFalse(sleep.screen_lit())

    def test_a_freeze_stopped_by_a_wake_event_is_retried_not_refused(self):
        for modem_after, expected, retries in ((4, 'unknown', sleep.DEEP_TRIES - 1), (5, 'modem', 0)):
            with tempfile.TemporaryDirectory() as root:
                root = Path(root)
                for name, value in (('refused', '0'), ('early_count', '0'), ('budget', '0'), ('deep_link_off', '0'),
                                    ('mem_sleep', 's2idle'), ('wakeup_count', '7')):
                    (root / name).write_text(value + '\n')
                (root / 'state').mkdir()        # the state write fails, as an aborted freeze does
                (root / 'suspend_stats').mkdir()
                (root / 'suspend_stats/last_failed_step').write_text('freeze\n')
                (root / 'suspend_stats/last_failed_dev').write_text('\n')
                quiet = dict(alarm=0, modem=4, **{'power-key': 0})
                # At the start, then per attempt: before the state write, and
                # after it failed (the modem's interrupt, or nothing).
                counts = [quiet] + [quiet, dict(quiet, modem=modem_after)] * sleep.DEEP_TRIES
                with patch.object(sleep, 'SLEEP_PARAMS', root), patch.object(sleep, 'UFS_PARAMS', root), \
                     patch.object(sleep, 'POWER', root), patch.object(sleep, 'irq_counts', side_effect=counts), \
                     patch.object(sleep, 'wake_count', return_value='7'), patch.object(sleep, 'idle_modem'), \
                     patch.object(sleep, 'coulomb', return_value=None), \
                     patch.object(sleep.time, 'sleep') as pause:
                    result = sleep.deep_sleep()
                self.assertEqual(result, dict(woke=expected, slept=0.0, sleep='deep-early'))
                self.assertEqual(pause.call_count, retries)


class ModemWakeTests(unittest.TestCase):
    def check(self, events=3, calls=(), marker='same', wakes=None):
        with patch.object(sleep, 'wake_count', side_effect=wakes), patch.object(sleep.time, 'sleep'), \
             patch.object(sleep, 'message_marker', return_value=marker), \
             patch.object(sleep, 'modem_events', return_value=events), \
             patch.object(sleep, 'request', return_value=dict(calls=list(calls))):
            return sleep.modem_brought_something('same', 3)

    def test_a_quiet_wake_brings_nothing(self):
        self.assertFalse(self.check())
        self.assertFalse(self.check(calls=[dict(state='terminated')]))

    def test_a_text_or_call_is_something(self):
        self.assertTrue(self.check(events=4))
        self.assertTrue(self.check(marker='changed'))
        self.assertTrue(self.check(calls=[dict(state='ringing-in')]))

    def test_a_modem_still_holding_the_phone_awake_is_something(self):
        self.assertTrue(self.check(wakes=TimeoutError('active')))

    def test_service_counts_only_texts_and_calls(self):
        with patch.object(sleep, 'request', return_value=dict(indications={'0906': 40, '0d1e': 2, '0d01': 1})):
            self.assertEqual(sleep.modem_events(), 3)


class WatchdogTests(unittest.TestCase):
    """The watchdog swap for deep sleep never leaves the phone without one."""

    def modules(self, loaded):
        real = Path.exists

        def exists(path):
            text = str(path)
            if text == '/sys/module/pixel_wdt_pm':
                return 'old' in loaded or 'deep' in loaded
            if text == '/sys/module/pixel_wdt_pm/parameters/deep_suspends':
                return 'deep' in loaded
            return real(path)
        return patch.object(Path, 'exists', exists)

    def test_a_failed_swap_puts_the_modem_copy_back(self):
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            if command[0] == 'insmod' and str(sleep.SUSPEND_MODULES) in command[1]:
                raise subprocess.CalledProcessError(1, command)
            return subprocess.CompletedProcess(command, 0)
        with self.modules({'old'}), patch.object(sleep.subprocess, 'run', side_effect=run):
            with self.assertRaises(subprocess.CalledProcessError):
                sleep.deep_watchdog()
        self.assertEqual(calls[0], ['rmmod', 'pixel_wdt_pm'])
        self.assertEqual(calls[-1], ['insmod', str(sleep.MODEM_WATCHDOG)])

    def test_the_check_loads_a_lost_watchdog_instead_of_refusing(self):
        calls = []
        with self.modules(set()), patch.object(sleep.subprocess, 'run',
                                                side_effect=lambda c, **k: calls.append(c)), \
                patch.object(sleep, 'read', side_effect=lambda p: {'mem_sleep': '[s2idle] deep', 'pm_test': '[none]',
                                                                    'capacity': '80', 'status': 'Discharging'}
                             .get(Path(p).name, 'enabled')), \
                patch.object(Path, 'read_bytes', return_value=b'google,GS201 CHEETAH'):
            # Later checks need the phone (the modem manager); only the
            # watchdog matters here.
            try:
                sleep.prerequisites(allow_charging=True)
            except (RuntimeError, OSError) as error:
                self.assertNotIn('Watchdog', str(error))
        self.assertIn(['insmod', str(sleep.SUSPEND_MODULES / 'pixel-wdt-pm.ko')], calls)
        self.assertNotIn(['rmmod', 'pixel_wdt_pm'], calls)


if __name__ == '__main__':
    unittest.main()


class ModemSessionTests(unittest.TestCase):
    def manager(self, pids, state):
        fake = unittest.mock.Mock()
        fake.recorded_pid.side_effect = lambda name: pids
        fake.cp_state.return_value = state
        return fake

    def test_no_session_with_cp_off_sleeps_without_the_modem(self):
        with patch.object(sleep, 'idle_modem') as idle:
            self.assertFalse(sleep.modem_session(self.manager(None, 0)))
        idle.assert_not_called()

    def test_no_session_with_cp_still_up_refuses(self):
        for state in (4, 5):
            with self.assertRaisesRegex(RuntimeError, 'mobile network stopped'):
                sleep.modem_session(self.manager(None, state))

    def test_a_session_is_idled_and_sleeps_with_the_phone(self):
        with patch.object(sleep, 'idle_modem') as idle:
            self.assertTrue(sleep.modem_session(self.manager(1234, 4)))
        idle.assert_called_once()
        with self.assertRaisesRegex(RuntimeError, 'not online'):
            sleep.modem_session(self.manager(1234, 0))
