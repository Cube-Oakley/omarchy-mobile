import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch, Mock

ROOT=Path(__file__).resolve().parents[1]/'overlay/mobile'
def module(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/(name+'.py'))
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

class BackendTests(unittest.TestCase):
    def test_battery_units_missing_fields_and_charger_disagreement(self):
        m=module('battery')
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);(root/'bat').mkdir();(root/'usb').mkdir()
            (root/'bat/uevent').write_text('POWER_SUPPLY_TYPE=Battery\nPOWER_SUPPLY_CAPACITY=84\nPOWER_SUPPLY_STATUS=Not charging\nPOWER_SUPPLY_CURRENT_NOW=-123000\nPOWER_SUPPLY_VOLTAGE_NOW=4192000\nPOWER_SUPPLY_TEMP=283\nPOWER_SUPPLY_CHARGE_NOW=3571000\n')
            (root/'usb/uevent').write_text('POWER_SUPPLY_TYPE=USB\nPOWER_SUPPLY_ONLINE=1\nPOWER_SUPPLY_STATUS=Full\nPOWER_SUPPLY_CURRENT_MAX=500000\n')
            r=m.read_battery(root)
            self.assertEqual((r['current_ma'],r['voltage_v'],r['temperature_c'],r['charge_mah']),(-123,4.192,28.3,3571))
            self.assertIsNone(r['full_mah']);self.assertFalse(r['charging'])
            self.assertEqual(r['chargers'][0]['status'],'Full')
            self.assertEqual(r['chargers'][0]['input_limit_ma'],500)

    def test_battery_usage_by_screen_state_on_battery_only(self):
        m=module('battery')
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            h=[dict(time=1000,screen='on',capacity=100,charge=5000000,plugged=True),
               dict(time=1600,screen='on',capacity=100,charge=5000000,plugged=False),
               dict(time=2200,screen='ambient',capacity=99,charge=4900000,plugged=False),
               dict(time=5800,screen='ambient',capacity=97,charge=4800000,plugged=False,event='wake:touch')]
            (root/'battery-history.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in h)+'not json\n')
            (root/'sleep.log').write_text(json.dumps(dict(time=5800,woke='touch',slept=3500,deep_ma=60.0,minute_ticks=58))+'\n')
            u=m.usage(hours=2,now=6000,root=root)
            # Charging time counts, its charge does not.
            self.assertEqual(u['states']['on'],{'seconds':1200.0,'mah':100.0})
            self.assertEqual(u['states']['ambient'],{'seconds':3600.0,'mah':100.0})
            self.assertEqual((u['deep']['seconds'],u['deep']['average_ma'],u['wakes'],u['minute_ticks']),(3500,60.0,{'touch':1},58))
            self.assertEqual((u['since_unplugged']['percent'],u['since_unplugged']['mah']),(3,200.0))

    def test_battery_details_by_component_and_process(self):
        m=module('battery')
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            h=[dict(time=1000,screen='ambient',capacity=90,charge=4500000,voltage=3900000,plugged=False,radios={'wifi':True}),
               dict(time=4600,screen='on',capacity=88,charge=4400000,voltage=3900000,plugged=False,metered=3600.0,
                    rails={'VSYS_DISPLAY':36000000,'S4M_VDD_CPUCL0':18000000,'VSYS_MODEM':7200000,'VSYS_RFFE':3600000},
                    cpu={'shell':12.5,'manager.py':3.0},wakeups={'cp2ap_wakeup':[40,1200]},irqs={'pixel-touch-attn':5},
                    net={'wlp1s0':[1000,200]},radios={'wifi':False}),
               dict(time=4660,screen='on',capacity=88,charge=4399000,plugged=False,metered=60.0,
                    rails={'VSYS_DISPLAY':6000000},cpu={'shell':1.5})]
            (root/'battery-history.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in h))
            r=m.details(hours=2,now=5000,root=root)
            parts={c['name']:c for c in r['components']}
            # 36 J over the always-on hour is 10 mWh; 6 J on screen 1.67 mWh.
            self.assertEqual(parts['Display']['mwh'],11.67)
            self.assertEqual(parts['Display']['by_screen'],{'on':1.67,'ambient':10.0,'off':0.0})
            self.assertEqual(parts['Cellular radio']['mwh'],3.0)
            self.assertTrue(parts['CPU, little cores']['floor'])
            self.assertEqual(r['components'][0]['name'],'Display')
            self.assertEqual(r['battery_mwh']['ambient'],390.0)
            self.assertEqual(r['cpu'],[['shell',14.0],['manager.py',3.0]])
            self.assertEqual(r['wakeups'],[['cp2ap_wakeup',40,1.2]])
            self.assertEqual((r['net'],r['radio_seconds']['wifi']),({'wlp1s0':[1000,200]},3600))

    def test_nmcli_escaping_and_deduplication(self):
        m=module('wifi')
        sample='*:aa\\:bb\\:cc\\:dd\\:ee\\:ff:My\\:Net\\\\name:45:WPA2\n:11\\:22\\:33\\:44\\:55\\:66:My\\:Net\\\\name:90:WPA2\n'
        with patch.object(m,'nmcli',return_value=sample):
            rows=m.networks('wlan0')
        self.assertEqual(len(rows),1);self.assertEqual(rows[0]['ssid'],'My:Net\\name')
        self.assertTrue(rows[0]['active'])
        self.assertEqual(m.split_fields('IP6.ADDRESS[1]:fe80\\:\\:1/64'),['IP6.ADDRESS[1]','fe80::1/64'])

    def test_secret_only_stdin_and_no_shell(self):
        m=module('wifi')
        with patch.object(m,'interfaces',return_value=['wlan0']), patch.object(m.subprocess,'run',return_value=Mock(stdout='')) as run:
            r=m.action('connect',{'interface':'wlan0','bssid':'aa:bb:cc:dd:ee:ff','password':'x$`secret'})
            self.assertTrue(r['ok'])
            args,kwargs=run.call_args
            self.assertNotIn('x$`secret',' '.join(args[0]))
            self.assertEqual(kwargs['input'],'x$`secret\n')
            self.assertNotIn('shell',kwargs)

    def test_invalid_interface_and_multiline_password_never_connect(self):
        m=module('wifi')
        with patch.object(m,'interfaces',return_value=['wlan0']),patch.object(m,'nmcli') as run:
            for request in ({'interface':'eth0'},{'interface':'wlan0','bssid':'bad'}, {'interface':'wlan0','bssid':'aa:bb:cc:dd:ee:ff','password':'x\ny'}):
                with self.assertRaises(ValueError):m.action('connect',request)
            run.assert_not_called()

    def test_weather_opt_in_cache_and_offline_data(self):
        m=module('weather')
        with tempfile.TemporaryDirectory() as d, patch.object(m.sys,'argv',['weather']):
            m.CONFIG=Path(d)/'config';m.CACHE=Path(d)/'cache'
            with patch.object(m,'get') as get:
                self.assertEqual(m.main(),{'configured':False});get.assert_not_called()
            config={'name':'Test','latitude':1,'longitude':2};m.save(m.CONFIG,config)
            cached={'configured':True,'available':True,'location':config,'fetched':m.time.time(),'current':{},'hourly':{}}
            m.save(m.CACHE,cached)
            with patch.object(m,'get') as get:
                result=m.main();get.assert_not_called()
            self.assertTrue(result['configured'])
            self.assertEqual(result['kind'],'unknown')
            self.assertEqual(result['daily_forecast'],[])
            cached['fetched']=0;m.save(m.CACHE,cached)
            with patch.object(m,'get',side_effect=OSError('offline')):
                result=m.main();self.assertTrue(result['stale']);self.assertTrue(result['available'])

    def test_weather_forecast_weekdays_and_kinds(self):
        m=module('weather')
        from datetime import date
        rows=m.forecast_rows({
            'time':['2026-09-18','2026-09-19','2026-09-20'],
            'temperature_2m_min':[58,60,55],
            'temperature_2m_max':[72,68,64],
            'weather_code':[0,61,95],
        }, today=date(2026,9,18))
        self.assertEqual([row['label'] for row in rows],['Today','Tomorrow','Sunday'])
        self.assertEqual([row['weekday'] for row in rows],['Friday','Saturday','Sunday'])
        self.assertEqual([row['kind'] for row in rows],['clear','rain','thunder'])
        self.assertEqual(m.condition_name(2),'Partly cloudy')
        self.assertEqual(m.condition_kind(45),'fog')

    def test_weather_hourly_starts_at_the_local_hour(self):
        m=module('weather')
        from datetime import datetime, timezone
        hours=[f'2026-09-27T{h:02d}:00' for h in range(24)]+[f'2026-09-28T{h:02d}:00' for h in range(24)]
        result={'utc_offset_seconds':-7*3600,
                'hourly':{'time':hours,'temperature_2m':list(range(48)),'weather_code':[61]*48,
                          'precipitation_probability':[40]*48,'is_day':[0]*7+[1]*13+[0]*28},'current':{'weather_code':0,'is_day':0}}
        # 22:10 UTC is 15:10 at UTC-7: the cached current time (09:45) is not used.
        enriched=m.enrich(result,now=datetime(2026,9,27,22,10,tzinfo=timezone.utc))
        rows=enriched['hourly_forecast']
        self.assertEqual(enriched['kind'],'clear_night')
        self.assertEqual(m.after_dark('rain',0),'rain')
        self.assertEqual(len(rows),24)
        self.assertEqual((rows[0]['label'],rows[0]['temperature']),('Now',15))
        self.assertEqual((rows[1]['label'],rows[1]['kind'],rows[1]['precipitation']),('4 PM','rain',40))
        self.assertEqual(rows[-1]['time'],'2026-09-28T14:00')
        # 20:00 is after dark here; rain keeps its own icon.
        self.assertEqual(rows[5]['kind'],'rain')
        self.assertEqual(m.enrich({'current':{}})['hourly_forecast'],[])

    def test_dns_parse_modify_and_forget_guard(self):
        m=module('wifi')
        self.assertEqual(m.parse_dns('1.1.1.1, 8.8.8.8'),['1.1.1.1','8.8.8.8'])
        with self.assertRaises(ValueError):
            m.parse_dns(['not-an-ip'])
        with self.assertRaises(ValueError):
            m.parse_dns(['1.1.1.1']*5)
        uuid='aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee'
        with patch.object(m,'wifi_uuids',return_value=[uuid]), patch.object(m,'nmcli') as run:
            self.assertTrue(m.action('dns',{'uuid':uuid,'auto':False,'servers':['1.1.1.1']})['ok'])
            self.assertEqual(run.call_args_list[0][0][:3],('connection','modify',uuid))
            self.assertEqual(run.call_args_list[1][0][:3],('connection','up',uuid))
            run.reset_mock()
            self.assertTrue(m.action('forget',{'uuid':uuid})['ok'])
            run.assert_called_with('connection','delete',uuid,wait=10)
        with patch.object(m,'wifi_uuids',return_value=[uuid]), patch.object(m,'nmcli') as run:
            with self.assertRaises(ValueError):
                m.action('forget',{'uuid':'00000000-0000-0000-0000-000000000000'})
            run.assert_not_called()

    def test_charge_limit_applies_saves_and_restores(self):
        m=module('battery')
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/'ps'; state=Path(d)/'state'
            for name,props in (('bq27541-0',{'POWER_SUPPLY_TYPE':'Battery','POWER_SUPPLY_CAPACITY':'87'}),
                               ('pm8150b-charger',{'POWER_SUPPLY_TYPE':'USB','POWER_SUPPLY_ONLINE':'1'})):
                (root/name).mkdir(parents=True)
                (root/name/'uevent').write_text(''.join(f'{k}={v}\n' for k,v in props.items()))
            (root/'pm8150b-charger'/'charge_control_end_threshold').write_text('100\n')
            with patch.dict(os.environ,{'XDG_STATE_HOME':str(state)}):
                self.assertEqual(m.read_battery(root)['limit'],100)
                self.assertTrue(m.read_battery(root)['limit_supported'])
                self.assertEqual(m.set_limit(80,root)['applied'],['pm8150b-charger'])
                self.assertEqual((root/'pm8150b-charger'/'charge_control_end_threshold').read_text(),'80\n')
                self.assertEqual(m.read_battery(root)['limit'],80)
                (root/'pm8150b-charger'/'charge_control_end_threshold').write_text('100\n')
                self.assertEqual(m.saved_limit(),80)
                m.apply_limit(m.saved_limit(),root)
                self.assertEqual((root/'pm8150b-charger'/'charge_control_end_threshold').read_text(),'80\n')
                with self.assertRaises(ValueError):
                    m.set_limit(20,root)

    def test_static_ipv4_validation(self):
        m=module('wifi')
        self.assertEqual(m.static_ipv4('192.168.1.50','192.168.1.1'),('192.168.1.50/24','192.168.1.1'))
        self.assertEqual(m.static_ipv4(' 10.0.5.7/16 ','10.0.0.1'),('10.0.5.7/16','10.0.0.1'))
        for address,gateway in [('192.168.1.0','192.168.1.1'),('192.168.1.255','192.168.1.1'),
                                ('192.168.1.50','192.168.2.1'),('192.168.1.50','192.168.1.50'),
                                ('192.168.1.50/31','192.168.1.51'),('fe80::1','fe80::2'),('nope','192.168.1.1')]:
            with self.assertRaises(ValueError,msg=(address,gateway)):
                m.static_ipv4(address,gateway)

    def test_ipv4_static_and_automatic(self):
        m=module('wifi')
        uuid='22222222-2222-2222-2222-222222222222'
        with patch.object(m,'wifi_uuids',return_value=[uuid]), patch.object(m,'nmcli') as run:
            self.assertEqual(m.action('ipv4',{'uuid':uuid,'auto':False,'address':'192.168.1.50',
                                              'gateway':'192.168.1.1','dns':'192.168.1.1 1.1.1.1'}),{'ok':True})
            run.assert_any_call('connection','modify',uuid,'ipv4.method','manual','ipv4.addresses','192.168.1.50/24',
                                'ipv4.gateway','192.168.1.1','ipv4.dns','192.168.1.1 1.1.1.1',
                                'ipv4.ignore-auto-dns','yes',wait=10)
            run.assert_called_with('connection','up',uuid,wait=30)
            m.action('ipv4',{'uuid':uuid,'auto':True})
            run.assert_any_call('connection','modify',uuid,'ipv4.method','auto','ipv4.addresses','',
                                'ipv4.gateway','',wait=10)
            with self.assertRaises(ValueError):
                m.action('ipv4',{'uuid':uuid,'auto':False,'address':'192.168.1.50','gateway':'192.168.1.1','dns':''})
            with self.assertRaises(ValueError):
                m.action('ipv4',{'uuid':'unknown','auto':True})

    def test_wifi_radio_toggle_and_disabled_status(self):
        m=module('wifi')
        with patch.object(m,'nmcli') as run, patch.object(m,'interfaces',return_value=[]):
            run.return_value='disabled\n'
            status=m.status()
            self.assertTrue(status['available'])
            self.assertFalse(status['radio'])
            self.assertFalse(status['connected'])
            run.return_value=''
            self.assertTrue(m.action('radio',{'enabled':False})['ok'])
            run.assert_called_with('radio','wifi','off',wait=10)
        self.assertTrue(m.radio_enabled('WIFI:enabled\n'))
        self.assertFalse(m.radio_enabled('disabled'))

    def test_prefs_dnd_round_trip_and_unknown_keys(self):
        m=module('prefs')
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'prefs.json'
            self.assertEqual(m.main([],path=path),{'dnd':False,'fontFamily':'JetBrainsMono Nerd Font','corners':'','alwaysOn':False,
                                                   'sleep':True,'sleepCheck':15,'sleepCheckNetwork':'wifi','screenTimeout':60,
                                                   'fingerprintUnlock':True,'fingerprintHaptics':True,'fingerprintAmbient':False,
                                                   'ambientWeather':False})
            result=m.main(['set'],stdin=__import__('io').StringIO('{"fingerprintAmbient":true,"ambientWeather":true}'),path=path)
            self.assertTrue(result['fingerprintAmbient'])
            self.assertTrue(result['ambientWeather'])
            result=m.main(['set'],stdin=__import__('io').StringIO('{"dnd":false}'),path=path)
            self.assertTrue(json.loads(path.read_text())['fingerprintAmbient'])
            with self.assertRaises(ValueError):
                m.main(['set'],stdin=__import__('io').StringIO('{"fingerprintHaptics":"on"}'),path=path)
            result=m.main(['set'],stdin=__import__('io').StringIO('{"dnd":true}'),path=path)
            self.assertTrue(result['dnd'])
            self.assertEqual(json.loads(path.read_text())['dnd'], True)
            result=m.main(['set'],stdin=__import__('io').StringIO('{"fontFamily":"CaskaydiaCove Nerd Font"}'),path=path)
            self.assertEqual(result['fontFamily'],'CaskaydiaCove Nerd Font')
            with self.assertRaises(ValueError):
                m.main(['set'],stdin=__import__('io').StringIO('{"secret":1}'),path=path)
            with self.assertRaises(ValueError):
                m.main(['set'],stdin=__import__('io').StringIO('{"fontFamily":"bad\\nfont"}'),path=path)
            result=m.main(['set'],stdin=__import__('io').StringIO('{"alwaysOn":true}'),path=path)
            self.assertTrue(result['alwaysOn'])
            self.assertTrue(json.loads(path.read_text())['alwaysOn'])
            self.assertTrue(result['dnd'])
            with self.assertRaises(ValueError):
                m.main(['set'],stdin=__import__('io').StringIO('{"alwaysOn":"yes"}'),path=path)

    def test_prefs_sleep_choices(self):
        m=module('prefs')
        io=__import__('io')
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'prefs.json'
            result=m.main(['set'],stdin=io.StringIO('{"sleep":false,"sleepCheck":30,"screenTimeout":120}'),path=path)
            self.assertEqual((result['sleep'],result['sleepCheck'],result['screenTimeout']),(False,30,120))
            self.assertEqual(m.read(path)['screenTimeout'],120)
            for bad in ('{"sleep":1}','{"sleepCheck":7}','{"screenTimeout":true}','{"screenTimeout":"60"}'):
                with self.assertRaises(ValueError):
                    m.main(['set'],stdin=io.StringIO(bad),path=path)
            path.write_text('{"sleepCheck":999,"screenTimeout":-1}')
            self.assertEqual((m.read(path)['sleepCheck'],m.read(path)['screenTimeout'],m.read(path)['sleep']),(15,60,True))

    def test_prefs_background_check_network(self):
        m=module('prefs')
        io=__import__('io')
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'prefs.json'
            self.assertEqual(m.read(path)['sleepCheckNetwork'],'wifi')
            result=m.main(['set'],stdin=io.StringIO('{"sleepCheckNetwork":"data"}'),path=path)
            self.assertEqual(result['sleepCheckNetwork'],'data')
            self.assertEqual(m.read(path)['sleepCheckNetwork'],'data')
            with self.assertRaises(ValueError):
                m.main(['set'],stdin=io.StringIO('{"sleepCheckNetwork":"lte"}'),path=path)
            path.write_text('{"sleepCheckNetwork":7}')
            self.assertEqual(m.read(path)['sleepCheckNetwork'],'wifi')

if __name__=='__main__': unittest.main()
