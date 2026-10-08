import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('pixel_time',
    Path(__file__).resolve().parents[1] / 'devices/pixel7pro/scripts/pixel-time.py')
pixel_time = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pixel_time)


class NtpTests(unittest.TestCase):
    def test_timestamps_round_trip(self):
        t = 1791430044.25
        data = struct.pack('!II', *pixel_time.to_ntp(t))
        self.assertAlmostEqual(pixel_time.from_ntp(data), t, places=6)


class SyncTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.state = Path(self.dir.name) / 'rtc-offset.json'
        patcher = patch.object(pixel_time, 'STATE', self.state)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.set = patch.object(pixel_time.time, 'clock_settime').start()
        self.addCleanup(patch.stopall)
        patch.object(pixel_time, 'rtc_edge', return_value=(1000, 1051.5)).start()

    def run_sync(self, answers, due=False):
        def query(server):
            answer = answers[server]
            if isinstance(answer, Exception):
                raise answer
            return answer
        with patch.object(pixel_time, 'query', side_effect=query):
            return pixel_time.sync(due)

    def test_agreeing_servers_set_the_clock_and_keep_the_offset(self):
        code = self.run_sync({'0.arch.pool.ntp.org': (-51.2, 0.04), '1.arch.pool.ntp.org': (-51.0, 0.02),
                              '2.arch.pool.ntp.org': OSError('unreachable')})
        self.assertEqual(code, 0)
        target = self.set.call_args[0][1]
        self.assertLess(target, pixel_time.time.time() - 50)    # the 20 ms answer
        self.assertEqual(pixel_time.load()['offset'], 51.5)
        self.assertEqual(pixel_time.load()['server'], '1.arch.pool.ntp.org')

    def test_one_server_alone_sets_nothing(self):
        code = self.run_sync({'0.arch.pool.ntp.org': (-3600.0, 0.01), '1.arch.pool.ntp.org': (2.0, 0.02),
                              '2.arch.pool.ntp.org': ValueError('kiss of death')})
        self.assertEqual(code, 1)
        self.set.assert_not_called()
        self.assertEqual(pixel_time.load(), {})

    def test_a_small_offset_leaves_the_clock(self):
        self.run_sync({'0.arch.pool.ntp.org': (0.01, 0.03), '1.arch.pool.ntp.org': (0.02, 0.03),
                       '2.arch.pool.ntp.org': (0.0, 0.05)})
        self.set.assert_not_called()
        self.assertIn('offset', pixel_time.load())

    def test_due_skips_a_recent_sync(self):
        pixel_time.save(51.0, 'x')
        with patch.object(pixel_time, 'query') as query:
            self.assertEqual(pixel_time.sync(True), 0)
        query.assert_not_called()


class RestoreTests(unittest.TestCase):
    def test_clock_follows_the_rtc_plus_the_kept_offset(self):
        with tempfile.TemporaryDirectory() as d, \
                patch.object(pixel_time, 'STATE', Path(d) / 'rtc-offset.json'), \
                patch.object(pixel_time, 'rtc_edge', return_value=(1000, 1051.0)), \
                patch.object(pixel_time.time, 'time', return_value=1051.2), \
                patch.object(pixel_time.time, 'clock_settime') as settime:
            pixel_time.save(-0.5, 'x')
            pixel_time.restore()
            settime.assert_called_once()
            self.assertAlmostEqual(settime.call_args[0][1], 999.5 + 0.2)

    def test_nothing_without_a_sync(self):
        with tempfile.TemporaryDirectory() as d, \
                patch.object(pixel_time, 'STATE', Path(d) / 'none.json'), \
                patch.object(pixel_time.time, 'clock_settime') as settime:
            self.assertEqual(pixel_time.restore(), 0)
            settime.assert_not_called()


if __name__ == '__main__':
    unittest.main()
