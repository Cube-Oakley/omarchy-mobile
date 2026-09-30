# SPDX-License-Identifier: MIT
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch
import manager


class ManagerTests(unittest.TestCase):
    def test_app_then_network_then_cp_shutdown(self):
        app, network, runtime = Mock(), Mock(), Mock()
        calls = []
        with patch.object(manager, 'terminate', side_effect=lambda p, _: calls.append(p)), \
             patch.object(manager, 'cp_state', return_value=0):
            manager.shutdown_children(app, network, runtime)
        self.assertEqual(calls, [app, network, runtime])

    def test_app_shutdown_failure_still_powers_off_cp(self):
        app, network, runtime = Mock(), Mock(), Mock()
        with patch.object(manager, 'terminate', side_effect=[subprocess.TimeoutExpired('app', 120), None, None]) as stop:
            with self.assertRaises(subprocess.TimeoutExpired):
                manager.shutdown_children(app, network, runtime)
        self.assertEqual([call.args[0] for call in stop.call_args_list], [app, network, runtime])

    def test_failed_cp_shutdown_does_not_stop_watchdog(self):
        with patch.object(manager, 'recorded_pid', return_value=None), \
             patch.object(manager, 'cp_state', return_value=4), patch.object(manager.os, 'kill') as kill:
            with self.assertRaises(RuntimeError):
                manager.stop()
            kill.assert_not_called()

    def test_previous_failed_boot_blocks_module_loading(self):
        with tempfile.TemporaryDirectory() as folder:
            guard = Path(folder) / 'starting.json'
            guard.write_text(json.dumps({'boot': 'old', 'stage': 'starting'}))
            with patch.object(manager, 'GUARD', guard), patch.object(manager, 'verify_bundle'), \
                 patch.object(manager, 'launch') as launch, patch.object(manager, 'command') as command:
                with self.assertRaises(RuntimeError):
                    manager.prepare()
                launch.assert_not_called()
                command.assert_not_called()
