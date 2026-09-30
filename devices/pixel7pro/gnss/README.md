# GNSS userspace (Broadcom BCM4776)

The receiver's position engine lives in Broadcom's closed `lhd` and `gpsd`
from the stock vendor image. They run unmodified in a RAM-only Bionic chroot,
like the modem's `cbd`/`rfsd` ([modem](../modem/README.md)), on top of the
[pixel-gnss](../kernel/gnss/README.md) kernel driver.

| File | What it does |
| --- | --- |
| `enter-gnss.c` | Enters the chroot, drops every capability and runs the stock linker, as `modem/enter-runtime.c` does, with `/vendor/lib64` on the library path. |
| `gnss-shim.c` | The modem shim's file-backed properties and filtered logging, plus the Android services the daemons reach for: wake locks (no-ops), the service manager (a wait that never returns, as on Android when a service never registers) and the Binder thread pool. Without them libbinder aborts: there is no Binder driver. |
| `gnss-lab.sh` | Test runner: unpacks a runtime archive into a tmpfs, bind-mounts only the driver's nodes (`/dev/ttyBCM`, `/dev/bbd_*`) and sysfs directory in a private mount and PID namespace, then starts `lhd` and `gpsd`. |

The runtime archive is assembled on the host from the stock images and never
tracked: `vendor/bin/hw/{lhd,gpsd}`, their library closure from `vendor_a.img`
and `system.img` (Bionic from the runtime APEX, the same build as the modem
runtime's), `vendor/firmware/SensorHub.patch`, and `vendor/etc/gnss/` with
these changes to the stock configuration:
- `GpioNStdbyPath=/sys/devices/platform/pixel-gnss/nstandby`;
- SUPL, LTO download, RTO/RTI, PPS and sensor aiding off (no network, no
  Android sensor service);
- logs in `/data/vendor/gps/`; the files must be owned by root, since the
  daemons run without capabilities.

## Running a session

`gpsd` only starts a position session when a client asks for one over its
HAL pipes (`.gps.interface.pipe.to_gpsd` / `.to_jni`), as Android's GNSS HAL
does. `brcm_gps_client.py` speaks that protocol (recovered from `gpsd` and
the stock `gps.default.so`): it initializes, sets the position mode
(standalone by default, which also keeps SUPL off), starts, turns on the NMEA
and satellite-status callbacks, and prints fixes, satellites and NMEA. It can
serve the NMEA on a pty (`--pty`) or TCP (`--tcp`) for Linux's gpsd or
geoclue. `gpsd` exits when its client disconnects, so `gnss-lab.sh` restarts
it.

Two shim details make `gpsd` usable without Android: the service-manager wait
returns "no service" on the main thread (blocking there froze `gpsd` before its
event loop), and the four SIT RIL calls it imports for modem-assisted GNSS
return success without a RIL.

The control pipe also works, for tests: `$pglirm,req_pos,<stream>,period,1000`
(lower-case prefix required) starts a request whose NMEA goes to the
`NmeaOutName` FIFO; `$pglirm,stop,<stream>` stops it.

## Status (September 30)

- `lhd` loads the patch into the chip (`ESW:READY`) and connects to `gpsd`.
- `gpsd` identifies the chip as `Broadcom,BCM4776,611184`, as stock Android
  did.
- Indoors, with no assistance data, a 90-second session tracked GPS
  satellites 2 and 3 at 29-31 dB-Hz and took UTC time from them; no position
  fix yet (it needs four or more satellites). A first fix needs a sky view.
- Not done: starting the stack at boot, powering the chip only while a client
  wants it, and a gpsd/geoclue source for the shell.
