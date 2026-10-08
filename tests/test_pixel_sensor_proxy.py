"""pixel-sensor-proxy's claims and the sleep pause, without D-Bus or sensors."""
import importlib.util
from pathlib import Path
import sys
import unittest

HERE = Path(__file__).resolve().parents[1] / 'devices/pixel7pro/sensors'
try:
    import gi  # noqa: F401
    sys.path.insert(0, str(HERE))
    spec = importlib.util.spec_from_file_location('pixel_sensor_proxy', HERE / 'pixel-sensor-proxy.py')
    proxy_module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(proxy_module)
except ImportError:
    proxy_module = None


class Backend:
    def __init__(self):
        self.calls = []

    def start(self, kind):
        self.calls.append(('start', kind))

    def stop(self, kind):
        self.calls.append(('stop', kind))


@unittest.skipIf(proxy_module is None, 'needs PyGObject')
class PauseTests(unittest.TestCase):
    def proxy(self):
        proxy = proxy_module.Proxy.__new__(proxy_module.Proxy)
        proxy.backend = Backend()
        proxy.claims = {'accel': set(), 'light': set(), 'proximity': set(), 'compass': set()}
        proxy.paused = False
        return proxy

    def test_pause_stops_claimed_sensors_and_resume_restarts_them(self):
        proxy = self.proxy()
        proxy._claim('accel', ':1.5')
        proxy._claim('light', ':1.6')
        proxy.backend.calls.clear()
        proxy.pause()
        proxy.pause()
        self.assertEqual(sorted(proxy.backend.calls), [('stop', 'accel'), ('stop', 'light')])
        proxy.backend.calls.clear()
        proxy.resume()
        proxy.resume()
        self.assertEqual(sorted(proxy.backend.calls), [('start', 'accel'), ('start', 'light')])

    def test_claims_while_paused_wait_for_resume(self):
        proxy = self.proxy()
        proxy.pause()
        proxy._claim('proximity', ':1.7')
        proxy._claim('accel', ':1.8')
        proxy._release('accel', ':1.8')
        self.assertEqual(proxy.backend.calls, [])
        proxy.resume()
        self.assertEqual(proxy.backend.calls, [('start', 'proximity')])


if __name__ == '__main__':
    unittest.main()
