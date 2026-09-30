# SPDX-License-Identifier: MIT
import unittest
from unittest.mock import Mock, patch
import bringup


class BringupTests(unittest.TestCase):
    def test_sim_requires_ready_usim_and_rejects_truncation(self):
        self.assertTrue(bringup.sim_ready(bytes([1, 0, 1, 2, 5]) + bytes(61)))
        self.assertFalse(bringup.sim_ready(bytes([1, 0, 1, 2, 2]) + bytes(61)))
        self.assertFalse(bringup.sim_ready(bytes([0, 0, 0])))
        with self.assertRaises(ValueError):
            bringup.sim_ready(bytes([1, 0, 1, 2, 5]))

    def test_actual_lte_registration_required(self):
        self.assertTrue(bringup.packet_registered(bytes([5, 0, 0, 14]), lte=True))
        self.assertFalse(bringup.packet_registered(bytes([1, 0, 0, 21]), lte=True))
        self.assertFalse(bringup.packet_registered(bytes([0, 0, 0, 14]), lte=True))

    def test_shutdown_cancels_wait_before_any_query(self):
        service = Mock()
        predicate = Mock()
        with self.assertRaises(InterruptedError):
            bringup.wait_for(service, predicate, 1, lambda: True)
        predicate.assert_not_called()

    def test_failed_data_setup_never_attempts_ims(self):
        service = Mock()
        with patch.object(bringup, 'wait_for'), patch.object(bringup, 'radio'), \
             patch.object(bringup, 'initial_attach'), patch.object(bringup, 'provision_default_profile'), \
             patch.object(bringup, 'nr_mode'), patch.object(bringup, 'activate', side_effect=TimeoutError), \
             patch.object(bringup, 'register') as register:
            with self.assertRaises(TimeoutError):
                bringup.start(service)
            register.assert_not_called()
            service.client.request.assert_not_called()

    def test_wait_keeps_handling_incoming_sms(self):
        service = Mock()
        service.client.next_indication.side_effect = [b'private-sms', None]
        with patch.object(bringup.time, 'monotonic', side_effect=[0, 0, 0, 0, 2, 2]):
            bringup.wait_for(service, Mock(side_effect=[False, True]), 10, lambda: False)
        service.event.assert_called_once_with(b'private-sms')

    def test_readonly_status_timeout_does_not_abort_startup(self):
        service = Mock()
        service.client.next_indication.return_value = None
        with patch.object(bringup.time, 'monotonic', side_effect=[0, 0, 0, 0, 2, 2]):
            bringup.wait_for(service, Mock(side_effect=[TimeoutError(), True]), 10, lambda: False)
