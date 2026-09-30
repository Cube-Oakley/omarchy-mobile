# Sensors

The Pixel 7 Pro's sensors are wired to the AoC's own I2C/I3C buses, not to
Linux:

| Sensor | Part |
| --- | --- |
| Accelerometer and gyroscope | LSM6DSV (I3C) |
| Magnetometers | two MMC5633 |
| Barometer | ICP20100 |
| Proximity and ambient light | TMD3719 |
| Rear light, flicker and spectral | VD6282 |

The AoC firmware runs their drivers and Google's fusion sensors under USF
(Unified Sensor Framework). Linux talks to it over the `com.google.usf` AoC
service.

## Files

| File | What it does |
| --- | --- |
| `usf.py` | USF client. Each message on `/dev/acd-com.google.usf` is one FlatBuffer (request, response, fragment, batch or compact sample batch). The layouts were recovered from the stock `libusf.so` ([USF-PROTOCOL.md](USF-PROTOCOL.md)). `python3 usf.py --type 1` lists the sensors and samples the accelerometer. |
| `pixel-sensor-proxy.py` | Serves iio-sensor-proxy's system bus API (`net.hadess.SensorProxy`: accelerometer orientation and tilt, ambient light in lux, proximity, and the compass heading). The shell's rotation, automatic brightness and in-call proximity helpers use it through `monitor-sensor`, as on the OnePlus. A sensor samples only while a client holds a claim. |
| `usf_backend.py` | Its USF backend: accelerometer at 15 Hz; light from Google's auto-brightness sensor once a second; proximity on change; the compass from the AoC's fused orientation sensor (azimuth) at 5 Hz. |

Install iio-sensor-proxy's package for `monitor-sensor` only: its own daemon
would compete for the bus name.

The UI subscriptions use USF's non-waking delivery. Wake-up delivery at the
accelerometer's 15 Hz continually renews the channel driver's 200 ms wake
lock, preventing system suspend. Rotation, light, proximity and compass
updates do not need to wake a sleeping AP; the phone stays awake during calls.

## Bringing the sensors up

1. The AoC runs ([aoc](../kernel/aoc/README.md)), with `aoc_channel_dev`
   serving the USF and CHRE services. `aoc_channel_dev` multiplexes a client
   channel per open file: it prefixes each message with the channel number.
   If the catch-all `aoc_char_dev` takes these services instead, the AoC logs
   "Message on unopened channel".
2. The sensor rails are on ([aoc-power](../kernel/aoc-power/README.md)).
3. The AoC's sensor registry is loaded. Android's sensor HAL does this once
   per AoC boot, and `pixel-sensor-proxy` does it on start when `"/"` is not
   `loaded`:
   - the stock scripts (`/vendor/etc/sensors/registry`, installed as
     `/usr/share/omarchy-mobile/sensors/registry`);
   - this phone's factory calibration from `persist:/sensors/registry`, read
     with debugfs, read-only;
   - sent as RegistryLoadScript with the board's CDT (`/chosen/plat`: product
     3, stage 6, so 0x36100 selects `cheetah_dvt.reg`);
   - then `"/" loaded=1`.

   Only the AoC's RAM changes. The AoC then probes its sensors
   asynchronously, which takes about a second.

If the rails were off when the registry loaded, the physical sensors stay
missing until the AoC restarts, which currently means a reboot. Only the
virtual (fusion) sensors appear.

Validated on 2026-09-29, phone flat on a table in the dark:
- the accelerometer reads (−0.12, 0.31, 9.79) m/s² at about 60 Hz;
- light 1.7 lux, proximity far, pressure 1010.5 hPa, gyroscope about 0;
- `monitor-sensor` reports the accelerometer (tilt face-up), light,
  proximity and the compass through `pixel-sensor-proxy`.

The orientation mapping (Android frame to iio-sensor-proxy's, by negating
x, y, z) is right: the shell's rotate button turns the screen upright both
ways, and proximity reports near and far (checked by hand on 2026-09-29).

## Ambient light

The TMD3719 sits under the panel. The AoC gives its light in three ways:

| Sensor | Behaviour |
| --- | --- |
| TMD3719 Ambient Light (type 9), on change | Missed a fall from 60 to 30 lux. |
| TMD3719 Ambient Light (type 9), continuous | Every conversion, about 27 a second, whatever period is asked for. |
| Auto Brightness (type 48, Google) | One reading a second. `vals[1]` follows the room within seconds. `vals[0]` is slower and holds its level when the room darkens. |

The proxy serves `vals[1]` of Auto Brightness as `LightLevel`. That worked
until about 16:50 on 2026-09-29. Since then the TMD3719 has converted nothing,
for light or proximity (see the [status](../docs/status.md)). The AoC logs a
watchdog (`status:0`) and "Sync delay shouldn't be less than 0 after
adjustment" whenever it is enabled. The stock registry syncs its readings to
the panel (`te2_alignment`, `sync_delay_ns`, EM cycles, `min_fps=30`). None of them
sees the panel's own light: 1% and full brightness read the same 61.6 lux.
The Pixel's device profile therefore sets `panelLux` to 0, where the OnePlus's
is 190.
