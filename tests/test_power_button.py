"""Power-key policy and wake-event races, without accessing phone hardware."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location('power_button', Path(__file__).resolve().parents[1] / 'overlay/mobile/power-button.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PowerButtonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = patch.dict(os.environ, {
            'XDG_RUNTIME_DIR': str(self.root), 'XDG_CONFIG_HOME': str(self.root),
            'XDG_STATE_HOME': str(self.root),
        })
        self.env.start()
        self.addCleanup(self.env.stop)
        self.button = module.PowerButton()
        self.button.display_on = Mock(return_value=True)
        self.button.set_display = Mock()
        self.button.start_hold = Mock()
        self.button.show_menu = Mock()
        # The screen's close animation starts as its own process.
        self.button.start_display = Mock(return_value=Mock(poll=Mock(return_value=0), wait=Mock()))
        self.patch_run = patch.object(module.subprocess, 'run')
        self.run = self.patch_run.start()
        self.addCleanup(self.patch_run.stop)

    def tap(self):
        self.button.event('press')
        self.button.event('release')

    def test_lit_screen_goes_dark_without_sleeping(self):
        # The sleep policy suspends later; the key only darkens the screen.
        self.tap()
        self.button.start_display.assert_called_once_with('off')
        self.button.set_display.assert_not_called()
        self.run.assert_not_called()

    def test_dark_screen_lights(self):
        self.button.display_on.return_value = False
        self.tap()
        self.button.set_display.assert_called_once_with('on')
        self.button.start_display.assert_not_called()

    def test_wake_press_after_sleep_lights_the_screen(self):
        # The press that woke the phone arrives once it is awake, on a dark screen.
        self.tap()
        self.button.display_on.return_value = False
        with patch.object(module.time, 'monotonic', return_value=module.time.monotonic() + 600):
            self.tap()
        self.button.set_display.assert_called_once_with('on')

    def test_release_that_starts_before_its_press_still_acts(self):
        # After a wake both processes start at once; the release may win the lock.
        other = module.PowerButton()
        other.start_hold = Mock()
        with patch.object(module.time, 'sleep', side_effect=lambda seconds: other.event('press')):
            self.button.event('release')
        self.button.start_display.assert_called_once_with('off')

    def test_release_without_press_does_nothing(self):
        with patch.object(module.time, 'sleep'):
            self.button.event('release')
        self.button.start_display.assert_not_called()
        self.button.set_display.assert_not_called()

    def test_always_on_shows_ambient(self):
        self.button.prefs.parent.mkdir(parents=True, exist_ok=True)
        self.button.prefs.write_text('{"alwaysOn": true}')
        self.tap()
        self.button.start_display.assert_not_called()
        self.button.set_display.assert_called_once_with('ambient')

    def test_press_in_ambient_wakes(self):
        self.button.prefs.parent.mkdir(parents=True, exist_ok=True)
        self.button.prefs.write_text('{"alwaysOn": true}')
        self.button.ambient.touch()
        self.tap()
        self.button.set_display.assert_called_once_with('on')

    def test_hold_shows_menu_before_release_and_release_does_not_blank(self):
        with patch.object(module.time, 'monotonic', return_value=100):
            self.button.event('press')
        with patch.object(module.time, 'sleep'), patch.object(module.time, 'monotonic', return_value=102.1):
            self.button.hold(100)
            self.button.event('release')
        self.button.show_menu.assert_called_once()
        self.button.start_display.assert_not_called()
        self.button.set_display.assert_not_called()

    def test_old_hold_timer_cannot_act_on_released_or_new_press(self):
        self.tap()
        self.button.save({'pressed': 200})
        with patch.object(module.time, 'sleep'), patch.object(module.time, 'monotonic', return_value=202.1):
            self.button.hold(100)
        self.button.show_menu.assert_not_called()
        self.assertEqual(self.button.load(), {'pressed': 200})

    def test_delayed_timer_and_key_repeats_still_show_only_one_menu(self):
        with patch.object(module.time, 'monotonic', return_value=100):
            self.button.event('press')
            self.button.event('press')
        self.button.start_hold.assert_called_once_with(100)
        with patch.object(module.time, 'monotonic', return_value=103):
            self.button.event('release')
        with patch.object(module.time, 'sleep'), patch.object(module.time, 'monotonic', return_value=104):
            self.button.hold(100)
        self.button.show_menu.assert_called_once()
        self.button.start_display.assert_not_called()

if __name__ == '__main__':
    unittest.main()
