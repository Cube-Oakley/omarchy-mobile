import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
import unittest.mock

ROOT = Path(__file__).resolve().parents[1] / 'overlay/mobile'


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SettingsPageTests(unittest.TestCase):
    def test_storage_reports_one_volume_when_home_shares_the_disk(self):
        storage = load('storage')
        with tempfile.TemporaryDirectory() as directory:
            result = storage.snapshot(directory)
        self.assertTrue(result['ok'])
        self.assertGreaterEqual(len(result['volumes']), 1)
        self.assertIn('free', result['volumes'][0])

    def test_revoke_removes_only_the_named_grant(self):
        apps = load('app')
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            os.environ['HOME'] = str(home)
            os.environ['XDG_DATA_HOME'] = str(home / 'data')
            os.environ['XDG_STATE_HOME'] = str(home / 'state')
            manifest = home / 'data/omarchy-mobile/apps/files'
            manifest.mkdir(parents=True)
            (manifest / 'manifest.json').write_text(json.dumps({
                'id': 'files', 'name': 'Files', 'permissions': ['files.home'],
            }))
            apps.grant('files', 'files.home')
            self.assertTrue(apps.has_grant('files', 'files.home'))
            apps.revoke('files', 'files.home')
            self.assertFalse(apps.has_grant('files', 'files.home'))
            listed = apps.installed()
            self.assertEqual(listed['apps'][0]['name'], 'Files')
            self.assertFalse(listed['apps'][0]['permissions'][0]['granted'])

    def test_web_apps_are_listed_and_removed_with_their_data(self):
        with tempfile.TemporaryDirectory() as data, tempfile.TemporaryDirectory() as cache:
            app = load('app')
            share, cached = Path(data), Path(cache)
            (share / 'applications').mkdir()
            (share / 'applications/omarchy-mobile-webapp-example-com.desktop').write_text(
                '[Desktop Entry]\nType=Application\nName=Example\n'
                'Exec=/home/cube/.local/bin/omarchy-mobile-browser --app "https://example.com/"\n'
                'X-Omarchy-Mobile-WebApp=https://example.com/\n')
            (share / 'applications/kitty.desktop').write_text('[Desktop Entry]\nName=kitty\n')
            (share / 'omarchy-mobile/webapps').mkdir(parents=True)
            (share / 'omarchy-mobile/webapps/example-com.png').write_bytes(b'png')
            profiles = [root / 'omarchy-mobile-browser/QtWebEngine' for root in (share, cached)]
            for profile in profiles:
                (profile / 'webapp-example-com').mkdir(parents=True)
                (profile / 'webapp-example-com/Cookies').write_text('x')
                (profile / 'omarchy-mobile').mkdir()
            env = {'XDG_DATA_HOME': data, 'XDG_CACHE_HOME': cache}
            with unittest.mock.patch.dict(os.environ, env):
                self.assertEqual(app.webapps()['webapps'],
                                 [{'id': 'omarchy-mobile-webapp-example-com', 'name': 'Example',
                                   'url': 'https://example.com/'}])
                for bad in ('kitty', 'omarchy-mobile-webapp-../x', 'omarchy-mobile-webapp-'):
                    with self.assertRaises(ValueError):
                        app.remove_webapp(bad)
                self.assertEqual(app.remove_webapp('omarchy-mobile-webapp-example-com')['webapps'], [])
            self.assertTrue((share / 'applications/kitty.desktop').exists())
            self.assertFalse((share / 'omarchy-mobile/webapps/example-com.png').exists())
            for profile in profiles:
                # The web app's profile goes; the browser's own stays.
                self.assertFalse((profile / 'webapp-example-com').exists())
                self.assertTrue((profile / 'omarchy-mobile').exists())


if __name__ == '__main__':
    unittest.main()
