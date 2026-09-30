import importlib.util
import math
import os
from pathlib import Path
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'overlay/mobile/controls.py'


def load(root):
    os.environ['OMARCHY_MOBILE_SYSFS'] = str(root / 'sys')
    os.environ['OMARCHY_MOBILE_DEV'] = str(root / 'dev')
    os.environ['XDG_STATE_HOME'] = str(root / 'state')
    os.environ['XDG_CONFIG_HOME'] = str(root / 'config')
    spec = importlib.util.spec_from_file_location('controls', SOURCE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


class ControlsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        sys = self.root / 'sys/class'
        write(sys / 'backlight/ae94000.dsi.0/brightness', '320\n')
        write(sys / 'backlight/ae94000.dsi.0/max_brightness', '1023\n')
        for name in ('white:torch', 'white:flash-1', 'mmc0::'):
            write(sys / 'leds' / name / 'brightness', '0\n')
            write(sys / 'leds' / name / 'max_brightness', '255\n')
        # FF_RUMBLE is bit 0x50: the second 64-bit word, bit 16.
        write(sys / 'input/event5/device/capabilities/ff', '10000 0\n')
        write(sys / 'input/event4/device/capabilities/abs', '400000000\n')
        self.controls = load(self.root)

    def tearDown(self):
        self.temp.cleanup()

    def brightness(self):
        return int((self.root / 'sys/class/backlight/ae94000.dsi.0/brightness').read_text())

    def test_status_finds_every_control(self):
        status = self.controls.status()
        self.assertEqual(status['backlight'], {'device': 'ae94000.dsi.0', 'percent': 56})
        self.assertEqual(status['torch'], {'available': True, 'on': False})
        self.assertTrue(status['haptics'])

    def test_brightness_follows_the_eye_and_is_remembered(self):
        self.controls.main(['brightness', '50'])
        self.assertEqual(self.brightness(), 256)
        self.controls.main(['brightness', '0'])
        self.assertEqual(self.brightness(), 10)
        self.controls.main(['brightness', '100'])
        self.assertEqual(self.brightness(), 1023)
        (self.root / 'sys/class/backlight/ae94000.dsi.0/brightness').write_text('320\n')
        self.controls.main(['restore'])
        self.assertEqual(self.brightness(), 1023)

    def test_serve_applies_each_level_and_remembers_the_last(self):
        import io
        import sys
        stdin = sys.stdin
        sys.stdin = io.StringIO('brightness 30\nnonsense\nbrightness 60\nremember 70\n')
        try:
            self.controls.serve()
        finally:
            sys.stdin = stdin
        self.assertEqual(self.brightness(), 501)
        self.assertEqual(self.controls.load_state(), {'brightness': 70})

    def test_torch_drives_every_flash_led_but_not_others(self):
        leds = self.root / 'sys/class/leds'
        self.controls.main(['torch', 'toggle'])
        self.assertEqual([(leds / n / 'brightness').read_text() for n in ('white:torch', 'white:flash-1', 'mmc0::')],
                         ['128', '128', '0\n'])
        self.assertTrue(self.controls.status()['torch']['on'])
        self.controls.main(['torch', 'toggle'])
        self.assertFalse(self.controls.status()['torch']['on'])

    def test_auto_brightness_starts_from_the_current_level(self):
        auto = self.controls.AutoBrightness()
        self.assertIsNone(auto.light(100.0, 0.09, 30, now=0.0))
        # The same room a moment later: nothing to change.
        self.assertIsNone(auto.light(100.0, 0.09, 30, now=5.0))

    def test_auto_brightness_follows_the_room_rarely(self):
        auto = self.controls.AutoBrightness()
        panel = [30]

        def run(lux, times):
            changes = []
            for t in times:
                level = auto.light(lux, (panel[0] / 100) ** 2, panel[0], now=t)
                if level is not None:
                    panel[0] = level
                    changes.append((t, level))
            return changes
        run(100.0, [0.0])
        # Much brighter (daylight): one higher level, not a ramp of small ones.
        bright = run(5000.0, [0.5, 1.0, 1.5, 2.0, 2.5])
        self.assertEqual(len(bright), 1)
        self.assertGreater(bright[0][1], 34)
        # Darker again: it comes down, never twice within the change interval.
        dark = run(1.0, [3.0 + i / 5 for i in range(200)])
        self.assertTrue(dark)
        self.assertLess(dark[-1][1], 30)
        gaps = [b[0] - a[0] for a, b in zip(bright + dark, (bright + dark)[1:])]
        self.assertTrue(all(gap >= 2.0 for gap in gaps))

    def test_auto_brightness_discounts_the_panel_light(self):
        dim, bright = (self.controls.AutoBrightness(offset=0, panel_lux=190.0) for _ in range(2))
        dim.light(113.0, 0.01, 10, now=0.0)
        bright.light(301.0, 1.0, 100, now=0.0)
        self.assertEqual(dim.target(), bright.target())

    def test_panel_light_comes_from_the_device_profile(self):
        self.assertEqual(self.controls.panel_lux(), 0.0)
        write(self.root / 'config/omarchy-mobile/device.json', '{"panelLux": 190}')
        self.assertEqual(self.controls.panel_lux(), 190.0)
        # A sensor that does not see the panel: the reading is the room.
        auto = self.controls.AutoBrightness(offset=0)
        auto.light(113.0, 1.0, 100, now=0.0)
        self.assertEqual(auto.target(), round(auto.base(math.log10(114.0))))

    def test_auto_brightness_learns_the_slider(self):
        auto = self.controls.AutoBrightness()
        auto.light(100.0, 0.09, 30, now=0.0)
        before = auto.target()
        # A drag in progress is left alone ...
        self.assertIsNone(auto.light(100.0, 0.36, 60, now=1.0))
        # ... and its final level becomes the preference in this light.
        auto.user_set(60)
        self.assertEqual(auto.target(), 60)
        self.assertGreater(auto.target(), before)

    def test_ramp_fades_a_point_at_a_time_within_a_second(self):
        ramp = self.controls.ramp
        self.assertEqual(ramp(30, 34), [31, 32, 33, 34])
        self.assertEqual(ramp(34, 30), [33, 32, 31, 30])
        long = ramp(10, 90)
        self.assertEqual(len(long), 25)  # a second at 40 ms a step
        self.assertEqual(long[-1], 90)
        self.assertEqual(ramp(50, 50), [50])

    def test_auto_toggle_is_remembered(self):
        self.assertFalse(self.controls.status()['auto'])
        self.assertTrue(self.controls.main(['auto', 'on'])['auto'])
        self.assertTrue(self.controls.load_state()['auto_brightness'])
        self.assertFalse(self.controls.main(['auto', 'off'])['auto'])

    def test_missing_hardware_is_reported_not_guessed(self):
        empty = tempfile.TemporaryDirectory()
        self.addCleanup(empty.cleanup)
        controls = load(Path(empty.name))
        status = controls.status()
        self.assertEqual((status['backlight'], status['torch']['available'], status['haptics'], status['slider']),
                         (None, False, False, None))
        with self.assertRaises(ValueError):
            controls.main(['torch', 'on'])


if __name__ == '__main__':
    unittest.main()
