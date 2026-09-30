"""Safety checks for the explicit phone sleep command; never accesses hardware."""
import importlib.util
from pathlib import Path
import subprocess
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location('phone_suspend', Path(__file__).resolve().parents[1] / 'adapter/suspend.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SuspendPolicyTests(unittest.TestCase):
    def setUp(self):
        self.values = {
            '/sys/power/mem_sleep': '[s2idle]',
            '/sys/class/rtc/rtc0/device/power/wakeup': 'enabled',
            '/sys/bus/platform/devices/c440000.spmi:pmic@0:pon@800:pwrkey/power/wakeup': 'enabled',
            '/sys/class/power_supply/pm8150b-charger/online': '0',
            '/sys/class/power_supply/bq27541-0/capacity': '80',
            '/sys/class/rtc/rtc0/wakealarm': '',
        }

    def check(self):
        with patch.object(module.os, 'uname', return_value=SimpleNamespace(release='6.17.0-sm8150-codex-native5-test')), \
             patch.object(module, 'read', side_effect=self.values.__getitem__):
            module.prerequisites()

    def test_unplugged_ready(self):
        self.check()

    def test_plugged_in_refused(self):
        self.values['/sys/class/power_supply/pm8150b-charger/online'] = '1'
        with self.assertRaisesRegex(RuntimeError, 'Unplug'):
            self.check()

    def test_existing_alarm_preserved(self):
        self.values['/sys/class/rtc/rtc0/wakealarm'] = '123456'
        with self.assertRaisesRegex(RuntimeError, 'already scheduled'):
            self.check()

    def test_wake_source_required(self):
        self.values['/sys/bus/platform/devices/c440000.spmi:pmic@0:pon@800:pwrkey/power/wakeup'] = 'disabled'
        with self.assertRaisesRegex(RuntimeError, 'Power-button wake'):
            self.check()

    def test_display_recovery_after_multiple_cleanup_failures(self):
        commands = []
        def fake_run(*command):
            commands.append(command)
            if command[0] != 'display-helper':
                raise subprocess.TimeoutExpired(command, 30)
        fake_path = Mock()
        fake_path.write_text.side_effect = OSError('pm_async write failed')
        with patch.object(module, 'Path', return_value=fake_path), \
             patch.object(module, 'run', side_effect=fake_run):
            with self.assertRaisesRegex(RuntimeError, 'cleanup needs attention'):
                module.restore('1', True, 'display-helper')
        self.assertEqual(commands[-1], ('display-helper', 'on'))

    def test_keep_dark_leaves_the_display_alone(self):
        commands = []
        with patch.object(module, 'Path', return_value=Mock()), \
             patch.object(module, 'run', side_effect=lambda *command: commands.append(command)):
            module.restore('1', False, 'display-helper', keep_dark=True)
        self.assertNotIn('display-helper', [command[0] for command in commands])
        self.assertIn('trigger', commands[0])

    def reason(self, irq, modem_before=4, modem_after=4):
        interrupts = (' 152:  1  0 GICv3 pm8941_pwrkey Edge      pm8941_pwrkey\n'
                      ' 153:  2  0 GICv3 pm8xxx_rtc_alarm Edge      pm8xxx_rtc_alarm\n'
                      ' 160:  0  0 GICv3 343 Edge      ipa\n')
        def fake_read(path):
            if path.endswith('pm_wakeup_irq'):
                if irq is None:
                    raise OSError('No data available')
                return irq
            return str(modem_after)
        proc = Mock()
        proc.read_text.return_value = interrupts
        with patch.object(module, 'read', side_effect=fake_read), \
             patch.object(module, 'Path', return_value=proc):
            return module.wake_reason(modem_before)

    def test_wake_reasons(self):
        self.assertEqual(self.reason('152'), 'power-key')
        self.assertEqual(self.reason('153'), 'alarm')
        self.assertEqual(self.reason('160'), 'network')
        # A call or text raises a wakeup without an interrupt of its own.
        self.assertEqual(self.reason(None, modem_after=5), 'modem')
        self.assertEqual(self.reason(None), 'unknown')
        # The power key wins over a modem event in the same wake.
        self.assertEqual(self.reason('152', modem_after=6), 'power-key')


if __name__ == '__main__':
    unittest.main()
