# Pixel modem service and runtime laboratory

On September 29, 2026 the Pixel 7 Pro reached **ONLINE** under native Linux
with the stock signed firmware and RAM copies of its own NV. The fixed AT
smoke test returned:

```
AT          -> OK
AT+CGMR     -> g5300q-240919-241106-B-12612898, OK
AT+CFUN?    -> +CFUN: 0, OK
```

This passes the full-boot gate G2 and the radio-off IPC gate G3. On September
29, 2026 the physical Tello SIM passed **outgoing SMS delivery, incoming SMS
storage/ACK, and outgoing call ringing/answer/hangup**, confirmed by the user.
Both outgoing and incoming calls passed **two-way voice audio**, confirmed by the user,
with the call continuing beyond 21 seconds. A subsequent +6 dB microphone
comparison was confirmed louder and undistorted by the user, suitable for
normal calls. This is now the audio helper default; speakerphone gain still
needs separate tuning and validation.
`call_test.py` hangs up on answer by default; `--audio` opens temporary AoC
routes and permits45 seconds of conversation after answer.

Cellular DNS and certificate-verified HTTPS (HTTP 200) passed 14 consecutive
LTE tests. After traffic stopped, RAT changed to NR21 and both tests failed.
Changing the stock one-byte NR mode from3 to0 returned the modem to LTE14 and
recovered DNS/HTTPS without reboot or bearer recreation. Repeated interface
restarts and simultaneous IMS registration then passed. The original mode is
saved privately in RAM; 5G and long-duration reliability remain unresolved.

Internet CID1 and IPv6 IMS CID2 coexist. The modem reports IMS registered
state2 with voice|SMS feature3. An IMS SMS submission returned cause0 and the
recipient confirmed delivery; the reply was durably stored before IMS ACK.
Earlier failed/unknown SMS attempts remain in private phone records.

Outgoing test SMS requires an explicit listener option and a private approved
recipient file. Each transport has a durable attempt ledger; uncertain outcomes
block automatic retries. No PIN commands, APDUs, eUICC operations, or Google NV
commands are used.

## Persistent startup

The automatic sequence reached LTE/IMS registration and app readiness on a
fresh RAM boot and a normal installed boot. Cellular HTTPS returned HTTP 200
on both; Wi-Fi-off ordinary DNS/HTTPS also passed. A second normal boot again
registered both services and passed cellular HTTPS, without manual modem
commands. A direct PID1 reboot saved confirmation that CP, RFS, app/network
workers and the watchdog stopped cleanly before restarting. The user then
confirmed the Messages text and Phone call test worked after reboot.

The opt-in `manager.py` service removes the laboratory deadline and owns the
startup/shutdown order. `prepare` verifies a private, kernel-pinned bundle,
arms the watchdog, and loads `pixel-gpio modem=1` before audio. `run` loads the
PCIe/CPIF modules, creates a fresh RAM sandbox from the original seed, and starts
RFS/CBD with `runtime.py --continuous`. `app_service.py --bringup` waits for a
ready physical USIM, configures initial attach and the Tello profile, enables
the radio, waits for LTE, activates internet/IMS, and observes actual IMS voice
and SMS registration. The same IPC0 owner handles SMS throughout startup.
`data_network.py` supplies the ordinary cellular fallback once registration
completes. Read-only status timeouts are retried within a bounded window;
uncertain bearer setup and SMS sends are never blindly repeated.

The private installation is `/var/lib/omarchy-mobile/modem` (0700); source
helpers live in `/usr/local/lib/omarchy-mobile/modem`. `install-persistent.py`
copies the previously validated local modules, watchdog, and seed, pins their
checksums, backs up the boot/reboot scripts, and optionally creates `enabled`.
It expects a staged tree containing `modem/` and `scripts/`. No proprietary
asset or identity is included in the repository. The real modem/NV partitions
remain read-only; firmware runtime and all modem writes still use RAM copies.

A durable `starting.json` marker spans each enabled session. An unfinished
session skips modem startup on the next boot while allowing the desktop and
USB recovery. After inspecting the failure, stopping CP, and saving the logs,
move the marker aside and retry on a fresh full-phone boot. No automatic warm
CP restart is attempted: prior experiments did not establish reliable warm
recovery. An orderly `manager.py stop` ends calls, removes cellular networking,
stops CP before RFS, and disarms the watchdog. Both `pixel-reboot` and the
updated recovery PID1 call this before terminating the rest of userspace.
PID1 leaves recovery available if the modem cannot shut down safely.

Status-only logs are under `/run/pixel-modem`; call/SMS contents remain in the
existing private app databases. Removing `enabled` disables the next automatic
start. Speakerphone remains unavailable. The Pixel shell adapter can now request
guarded s2idle while unplugged; see the [suspend validation](../docs/suspend-20260929.md).
The private bundle includes `pixel-wdt-pm.ko`, loaded after the watchdog feeder
starts so the AP watchdog can pause/rearm during explicit suspend tests.
New installations use the validated CPIF and PCIe modules in
`/root/tools/suspend/`; older laboratory modules lack the suspend fixes.
The bundle also pins `brcmfmac.ko`, whose control-ring acknowledgement ordering
fix is needed for repeated Wi-Fi suspend/resume. The boot helper verifies the
bundle before loading that driver. With the modem bundle disabled it uses the
original in-image Wi-Fi driver, and the suspend helper refuses automatic sleep
without the running modem supervisor.

## Components

- `app_service.py`: single IPC0 owner for the native Phone and Messages apps.
  A mode-0600 Unix socket checks the client UID; the shared app helpers retain
  their app grants. Incoming SMS is durable before acknowledgment and imported
  into the existing conversation database. Outbound requests have durable IDs
  and uncertain outcomes cannot be blindly retried. Incoming calls ring through
  the existing shell UI and require the user's answer action. Audio lifetime
  follows the call; there is no laboratory conversation deadline. App texting,
  calling and mute have been confirmed by the user.
- `sms_codec.py`: ordinary GSM-7 and Unicode SMS encoding, multipart splitting
  and incoming reassembly. The Pixel's incoming SMS field includes an SMSC
  prefix; its outgoing TPDU field does not. Two real stored messages and the
  app's live reply validate receive framing. Multipart/Unicode cases also have
  host tests; carrier delivery of these variants needs a live test.
- `data_network.py`: owns CID1's IPv4 address and a metric-700 default route,
  behind Wi-Fi's metric 600. Uses carrier DNS while cellular is the default,
  restores the previous resolver when appropriate, and preserves a newer
  NetworkManager-written Wi-Fi resolver. No cellular address or resolver is
  committed. Wi-Fi-off ordinary DNS/HTTPS returned HTTP 200; Wi-Fi restoration
  returned default traffic to Wi-Fi. IPv6 internet routing remains unconfigured.
- `extract-nv.py`: extracts the three F2FS NV backup images using disposable
  copies and `dump.f2fs`. Handles the direct/inline layouts in this phone's
  snapshots and rejects unsupported layouts. Corrects the inline regular-file
  offset for extra inode attributes; the locally used dump.f2fs 1.16 exporter
  otherwise produces incorrect 32-byte checksum files. All three existing
  checksums must equal MD5(file bytes + `Samsung_SIT_RIL`); none are rewritten
  to force a match. Original backup SHA-256 values were rechecked afterward.
- `android-shim.c`: a small Bionic-loaded library supplying file-backed Android
  properties and filtered Android logging. No ioctl interception. Property
  files and all daemon writes stay inside the RAM root.
- `enter-runtime.c`: chroots, drops all capabilities and supplementary groups,
  sets `NO_NEW_PRIVS`, and invokes the stock Android linker. Run inside private
  mount, PID and network namespaces. It requires a correctly prepared root.
- `prepare-runtime.py`: runs on the phone, checks all nine protected partitions
  are block-layer read-only, creates a dedicated 512 MiB tmpfs, unpacks a private
  archive, validates stock NV checksums, supplies only null/zero/urandom device
  nodes, and copies the required live DT inputs without printing identities.
  It never starts a daemon or exposes real modem channels automatically.
- `at-lab.py`: fixed radio/status/APN actions, including the approved radio
  enable test. Radio changes require user authorization.
- `query-sit.py`: read-only physical-slot radio/PS registration and data-call
  count queries; suppresses unsolicited payloads and identifying fields.
- `sit.py`: shared single-owner IPC0 transport, with a fixed request allow-list,
  bounded framing and preservation of indications received during requests.
  Repeated physical-channel/signal snapshots are coalesced during long requests
  so they cannot fill the queue ahead of an SMS response.
- `runtime.py`: manually starts and supervises RAM-only CBD/RFS. SIGTERM,
  deadline or daemon failure powers CP off before stopping RFS. If power-off
  fails, it retains RFS and requires a manual reboot.
- `radio.py`: stock-derived SIT radio on/off/status; verifies the returned
  state. AT CFUN and SIT radio state disagreed during testing.
- `radio-mode.py`: temporary LTE preference with exact saved-mode restoration.
  Both set/readback and restoration passed, but RAT remained NR after a CP
  restart. This is not evidence of an actual LTE attachment.
- `sit-data.py`: fixed Tello APN/initial-attach and data-call requests derived
  from the matching stock SIT builders. Private IP configuration stays in RAM.
- `data_call.py`: separately owns internet CID1 and IMS CID2 and their PDU
  sessions. Records ownership before setup; an uncertain setup or failed
  release retains it. Refuses unknown bearers and duplicate PDU allocation.
  Disconnect requires the host interface down; IMS must first be unregistered.
- `ims.py`: minimal device/timer initialization and IPv6 IMS PDN notification
  using the returned address and P-CSCF servers. Registration indications retain
  only numeric state/features/status; private identities are never logged.
- `sms_test.py`: fixed ordinary GSM text to the privately approved recipient,
  through regular or IMS SMS. Disabled by default; durable attempt records
  prevent automatic retries after unknown outcomes.
- `radio_settings.py`: reversible NR mode and voice/data usage settings,
  retaining exact original values in RAM. `disable-nr` recovered LTE traffic;
  `restore-nr` restores the saved byte after testing.
- `call_test.py`: one bounded ordinary IMS call to the approved private
  recipient. Captures numeric call states and requires a release indication.
  The default signaling test hangs up on answer or after20 seconds; `--audio`
  allows45 seconds after answer, with a75-second total bound. If release
  cannot be confirmed, stops the owned runtime supervisor (CP before RFS).
  `--incoming` waits up to ten minutes for one ordinary call matching the
  private approved number, answers with audio, and applies the same conversation
  and release bounds. Other callers remain unanswered. Incoming voice passed
  a live test: answer accepted, active state observed, both audio directions
  confirmed by the user, release observed, routes restored and listener resumed.
  Audio calls default to the user-confirmed +6 dB hardware microphone boost
  and retain the existing boosted-call attempt record. `--mic-boost-db 0`
  selects the original stock gain for comparison. Both restore prior mixer
  settings on exit. Speakerphone gain has not been tuned.
- `monitor-sit.py` / `control.py`: persistent physical-slot SMS/call/IMS listener
  and fixed local control socket. Incoming GSM SMS is saved mode 0600 under
  `/root/.local/state/modem-lab/inbox` (directory 0700), fsynced, then ACKed.
  No sender or content is printed. An actual user reply exercised IMS incoming
  SMS storage and acknowledgment successfully.
- `voice_audio.py`: stock AoC PCM4 playback and PCM11 capture routes, using
  validated normal-call microphone gain (190 cB hardware, 0 dB record gain;
  stock hardware gain is 130 cB). Capture is
  prepared first; avoids duplicate modem-start commands from ALSA prepare.
  Snapshots/restores modified mixer controls and closes both PCMs on failures.
  Uses the existing DSP/CP voice path without a CPU mapping of AoC SRAM.
- `test-data.py`: temporary CID-1 IPv4 address and host routes for carrier DNS
  and HTTPS tests; removes its configuration afterward. No default route edits.
- `query-at.py`: fixed read-only `AT`, `AT+CGMR`, `AT+CFUN?` smoke test through
  `/dev/umts_router`; no arbitrary-command argument.

No proprietary executable, firmware, NV, identity, archive, or raw log belongs
in this directory or in git. Keep private working files under ignored `out/`
with mode 0700; keep transient phone state under `/run/modem-lab`. Received SMS
uses the persistent private inbox so an acknowledged message survives reboot.

## Build and seed preparation

Build the helpers on the host:

```sh
OUT=devices/pixel7pro/out/modem-stage4
mkdir -p "$OUT"
aarch64-linux-gnu-gcc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
  -fPIC -fno-stack-protector -shared -nostdlib \
  devices/pixel7pro/modem/android-shim.c -o "$OUT/pixel-android-shim.so"
aarch64-linux-gnu-gcc -O2 -Wall -Wextra -static \
  devices/pixel7pro/modem/enter-runtime.c -o "$OUT/enter-runtime"
```

The shim intentionally has no glibc dependency. Use `-std=gnu11`: newer GCC's
C23 defaults may introduce `__isoc23_strtol`, unavailable in the stock Bionic.
Stock linker64 takes the target executable directly; use `LD_LIBRARY_PATH`,
not a `--library-path` argument.

Extract from existing private backups, never live block devices:

```sh
python3 devices/pixel7pro/modem/extract-nv.py BACKUP_DIRECTORY NEW_PRIVATE_OUTPUT \
  --dump-f2fs /path/to/dump.f2fs
```

`dump.f2fs` can update filesystem metadata even while dumping, so this script
always operates on copies. Its executable basename must remain `dump.f2fs`
(the tool selects its mode from argv[0]). `persist/modem/cpsha` is extracted
separately with read-only debugfs from the existing persist backup.

Prepare an archive of regular files/directories, with these root-relative paths:

- `system/bin/{linker64,sh,toybox}` and their complete stock Bionic dependency
  closure in `system/lib64`; prefer the stock vendor_boot recovery runtime.
- `system/lib64/pixel-android-shim.so`, `enter-runtime`, `vendor/bin/{cbd,rfsd}`.
- `data/vendor/radio/image/modem.bin`, extracted from the matching factory radio.
- `mnt/vendor/{efs,efs_backup,modem_userdata,persist}` containing private copies.
- `properties/*`, using the initial values below.

Initial properties: `ro.vendor.cbd.modem_type=s5100sit`, `ro.bootmode=normal`,
`ro.vendor.build.type=user`, `ro.debuggable=0`, `persist.radio.multisim.config=dsds`,
`vendor.modem.bin.path=/data/vendor/radio/image/modem.bin`,
`persist.vendor.modem.recovery.enabled=0`, `persist.vendor.radio.no_modem_board=0`,
`vendor.cbd.modem_bin_type=0`, and `persist.vendor.modem.efs.clear.action=none`.
Copy `ro.boot.cdt_hwid` from this phone's private stock property capture. Set
`persist.vendor.modem.baseband.version` to the 31-byte, NUL-trimmed version at
INFO section +20 in this firmware's TOC.

An empty `efs.clear.action` makes stock RFS treat the environment as a factory
reset and clear RAM NV. A different baseband-version property can trigger the
image-change path. Validate both before connecting the modem channels.

The absent `/proc/device-tree/model_info-system_rev` is **not required** for
this boot: that cbd routine only publishes `vendor.cbd.dt_revision`. The full
boot succeeded without fabricating it. Stock hardware_config.json is absent
from this factory image; cbd uses its normal CDT/DT fallback, with the original
IMEIs and factory signature supplied privately.

## Manual run

Follow [CPIF's prerequisites](../kernel/cpif/README.md), arm the watchdog, stream
filtered kernel logs, and make `rmnet*` unmanaged. Modules stay under `/root/tools`;
the opt-in persistent manager uses its own pinned private bundle. Do not use
general module autoloading or mix the manual recipe with a running manager.

On the phone, `prepare-runtime.py PRIVATE_RUNTIME.tar` creates the RAM root and
checks the seed. Then run `runtime.py --seconds 3600` manually, with its output
redirected to a private status log. It creates the two modem channel nodes,
starts RFS before CBD, bounds daemon logs, and publishes `runtime.pid` inside
the RAM root. SIGTERM and the 30-second automatic deadline have both powered
CP off successfully on hardware. Stop the listener before the supervisor.
Never leave an independently timed RFS process to expire with CP online.

The firmware's `dev/block/by-name/modem` path resolves to a
regular read-only file inside this root, never a real block device. Add only
`umts_boot0` and `umts_rfs0` character nodes, using their live major/minor numbers,
after CPIF binds successfully. All daemon input/output remains isolated by:

The following commands describe the supervisor's internals; do not launch a
second RFS or CBD while `runtime.py` is running.

```sh
unshare --mount --net --pid --fork --kill-child=KILL \
  --mount-proc=/run/modem-lab/proc \
  /run/modem-lab/enter-runtime /run/modem-lab /vendor/bin/rfsd
```

Run RFS first, verify both NV files are reported valid and the RFS channel opens,
then use the same wrapper for:

```sh
/vendor/bin/cbd -t s5100sit -P by-name/modem -s 2 -o r
```

CBD runs once and exits after boot. The `-o r` option keeps its uid 0 inside the
capability-free root. RFS must continue running while CP is online. Bound test
processes with `timeout -k 2 SECONDS` and log-size limits; namespace PID 1 can
ignore TERM, so TERM-only timeouts are insufficient. Stop CP before RFS expires.
The RAM root provides `/vendor/bin/tar` as well as the shell's toybox commands,
so cbd can build REPLAY from the copied replay directory.

Verify the four initialization messages, `BOOTING -> ONLINE`, RFS ONLINE, and
run `query-at.py`. Radio-off power-down is `IOCTL_POWER_OFF` (0x6f20) on
`umts_boot0`; status ioctl 0x6f27 returned 4 (ONLINE), then 0 (OFFLINE).
Stop the watchdog before an orderly reboot. Reboot restores ordinary startup
and discards the laboratory; no protected partition changes are required.

## Physical Tello SIM and packet test

The September 29 test was explicitly authorized after the SIM was inserted.
`AT+CPIN?` reports READY. SIT `GET_PS_REG_STATE` (0x0701) reports registered
(state 1, reject 0, protocol RAT 21). AT `CEREG?`/`CGATT?` do not reflect that
state reliably; use the matching SIT response adapter rather than treating
those AT replies as proof that network registration failed.

On a fresh full-phone boot, wait for the USIM application to report READY
(type2/state5) before configuring initial attach and the default data profile:

```sh
python3 at-lab.py radio-off
python3 sit-data.py --apn wholesale --initial-attach
# The listener also exposes provision-profile for the default wholesale profile.
python3 at-lab.py register
# Wait for SIT PS registration state 1 (home) or 5 (roaming).
python3 query-sit.py
python3 sit-data.py --apn wholesale
python3 test-data.py
```

The above AT sequence records the original successful experiment. A subsequent
AT CFUN0 reported success while SIT still returned ON (10); `radio.py off`
returned OFF (0). A SIT off/on cycle then stalled packet registration, while
a full CP restart recovered it. Do not treat either AT state alone or a
successful setup response as proof that cellular traffic works.

For an observation window after registration, run `monitor-sit.py --seconds
900` and use `control.py status`, `connect-data`, or `disconnect-data`. Other
SIT command-line helpers refuse concurrent ownership. `test-data.py` is a host
network test and can run while the listener owns IPC0. It removes its addresses
and destination-only routes afterward. For ordinary default-route integration, the app service uses `data_network.py`. After registration, `control.py disable-nr` saves the original
NR mode and applies the verified LTE workaround; confirm actual RAT and traffic
before proceeding. The older preferred-network setter alone did not achieve
this result. Requests can defer indication handling for up to their timeout;
this is a laboratory listener, not a production SMS service.

`control.py start-ims` and `stop-ims` use the stock physical-slot stack lifecycle
messages (0d39/0d3a, payload SIM-ready=1, stack=1). Both were accepted. After
CPIN READY, `connect-ims` requests CID2, profile0, APN type1, APN `ims`, IPv6
and IPv6 P-CSCF discovery. The `ims`/IPv6 choice follows the T-Mobile entry in
[Google's APN configuration](https://android.googlesource.com/device/google/wahoo/+/android-8.1.0_r29/apns-full-conf.xml);
the fresh-boot test returned status0/active1 with IPv6 P-CSCF servers. Keep the
owned internet bearer active when adding IMS. `register-ims` sends minimal
hidden-menu configuration and ADD_PDN, then waits for the actual registration
indication. This has produced registered state2 with voice|SMS feature3.
`unregister-ims` removes that notification while keeping the bearer;
`disconnect-ims` subsequently releases CID2. Registration failed in the stalled NR data state, then succeeded after NR
mode0 restored working LTE.

For the user-authorized fixed outgoing test, start the listener with
`--allow-test-sms`. Supply a root-owned mode0600 `test-recipient.json` containing
`destination` in full +1 form under `/root/.local/state/modem-lab` (mode0700).
The destination must be explicitly approved by the user; never put it in git
or command output. `send-test-sms` and `send-test-ims-sms` each reserve a private
persistent ledger before writing the request. The IMS variant requires an
observed registration advertising SMS. A zero modem cause still requires
recipient confirmation. Keep unknown-outcome ledgers for investigation.

Host regression checks:

```sh
python3 -m unittest discover -s devices/pixel7pro/modem -p 'test_*.py'
```

Tello's [official APN guidance](https://tello.com/help_center/apn-settings/what-apn-settings-are-needed-with-tello)
provides `wholesale` for manual configuration, using IPv4/IPv6. The initial
attach request (0x0603, 251 bytes) is essential in the successful sequence:
without it, modern setup requests returned status 36 and no bearer for both
`wholesale` and `fast.t-mobile.com`. Mobile-data state and AllowData were
already enabled and did not resolve that failure on their own.

The working setup request uses the stock 983-byte version-2 layout, an
allocated PDU session, CID 1, and IPV4V6. It returned status 0, CID 1, raw
active state 2, PDP type 3, both addresses and MTU 1500. CID 1 maps to rmnet0.
A legacy 246-byte setup request was rejected with header error 2.

Three IPv4 pings and three IPv6 pings each passed with no loss. The DNS/HTTPS test then produced a
successful carrier-DNS answer and HTTP 200 with TLS certificate validation.
Interface totals afterward: 20 TX / 15 RX packets, zero errors or drops.
Addresses, resolver replies and HTTP response contents were not printed.
The scripts deliberately leave the modem bearer active but remove test host
configuration. Power CP off before stopping RFS; reboot discards all RAM NV.

## Current limits

### Native app integration

The Pixel adapter installs `telephony.json` selecting `pixel-sit`. Shared
`phone.py` and `messages.py` use `telephony.py` for that adapter and retain
ModemManager on other devices. The previous Pixel send failure was the missing
`mmcli` executable, rather than interference from a modem experiment.

After the manual modem sequence below reaches IMS registration and both
bearers are active, stop `monitor-sit.py` before starting `app_service.py`.
Start `data_network.py` separately for normal cellular fallback. Keep their
output in private RAM logs and keep the runtime/RFS supervisor alive. Do not
run standalone SIT tests or `test-data.py` concurrently with these services.
Both helpers accept SIGTERM for cleanup; end calls before stopping the service.
Restart existing app watchers after switching backends (the shell's original
watchers exit when ModemManager is absent).

Manual runs retain their configured deadline. The opt-in persistent manager
uses continuous supervision and runs this sequence on boot. The apps cannot
restart a stopped CP themselves. Speakerphone routing, call waiting,
emergency/short-code calling and MMS are not implemented. Keypad one-shot DTMF
uses the matching stock request layout and has host tests; audible/network DTMF
still requires a live test. The Speaker control is unavailable for this backend
rather than reporting a route change that never occurred.

The SMS codec follows [3GPP TS 23.038](https://www.etsi.org/deliver/etsi_ts/123000_123099/123038/16.00.00_60/ts_123038v160000p.pdf)
and [3GPP TS 23.040](https://etsi.org/deliver/etsi_ts/123000_123099/123040/16.00.00_60/ts_123040v160000p.pdf).
No proprietary implementation or private message is included in the source.

### Sleep with the modem online

The Pixel now has a guarded s2idle helper and shell adapter. An incoming SMS
woke the processor after 26.8 seconds asleep and appeared in Messages. With
the Wi-Fi resume fix loaded, an incoming call woke it after 57.6 seconds;
the user confirmed answering, two-way audio and End Call. Eight consecutive
RTC sleeps retained working Wi-Fi HTTPS. See [measurements and limitations](../docs/suspend-20260929.md).

The earlier freezer abort came from UI sensor subscriptions continually
renewing a wake lock. Those subscriptions are now non-wakeup. CPIF propagates
sleep refusals, its PCIe link retains private PM ownership, and the watchdog
has noirq pause/rearm support. Screen-off modem reporting suppresses optional
signal-strength traffic while retaining calls, SMS and essential state changes.

The shared shell owns screen and focus behavior. Calls bring Phone into the
foreground and wake the display; SMS wakes the processor for background
receipt without necessarily lighting the screen. The sleep helper refuses
while plugged in or during calls. Remaining power savings need deeper GS201
clock/power handling; ordinary s2idle does not yet deliver stock standby draw.

### Reproducing the successful service sequence

Use a fresh full-phone boot, the validated original RAM seed, the manual CPIF
prerequisites above, and the supervised runtime. Keep the protected partitions
read-only and the watchdog running. This remains a manually supervised recipe;
the persistent manager automates a fresh boot, but warm recovery remains unverified.

1. Wait for SIT SIM status to report USIM application type 2, state 5 (READY).
   The listener's startup status prints only application types/states. Stop it
   before using another SIT helper, because IPC0 has one owner.
2. Run `at-lab.py radio-off`, configure `sit-data.py --initial-attach`, and
   provision the default profile with `control.py provision-profile` while the
   listener owns IPC0. Then run `at-lab.py register`. Wait for actual SIT packet
   registration; AT replies alone do not establish success.
3. With the listener running, use `control.py disable-nr` once. Verify actual
   LTE RAT 14 in its status log. The saved original mode prevents accidental
   repeated experiments; do not delete it to bypass that guard.
4. Run `control.py connect-data` and `test-data.py`. Require successful carrier
   DNS and certificate-verified HTTPS before continuing. Keep CID1 active.
5. Run `control.py start-ims`, `control.py connect-ims`, then
   `control.py register-ims`. Require an actual registration indication with
   state 2 and voice/SMS feature bits 3. An accepted ADD_PDN request is only an
   intermediate result. Repeat `test-data.py` with both bearers active.
6. SMS tests use the listener and the private recipient/attempt records above.
   For a call, stop the listener and invoke `call_test.py --audio` (outgoing) or
   `call_test.py --incoming`. Both require an observed IMS voice registration.
   Keep private attempt records; inspect outcomes before intentionally testing
   again. Resume the listener after the helper exits if CP remains ONLINE.

The original hardware run applied the NR workaround after the first data
bearer was already active and recovered that bearer. The persistent startup
sequence has now also validated applying it before data setup, on fresh RAM
and installed boots. Do not reset a working phone during a user call test.

### Remaining validation

Packet queues and IPv4 traffic now work in this manual laboratory. Idle PCIe wake/retrain delivered the AT replies; system suspend and
crash recovery remain untested. Unopened OEM channels produce expected dropped
indication messages. CPU mapping of AoC voice SRAM remains disabled; the existing AoC/CP audio
path passed the outgoing two-way voice test without it. The kernel port
still has no general unload lifecycle; use reboot after manual testing.

### Boot-success flag

The bootloader reported slot A unsuccessful with one retry remaining. A
read-only inspection matched Google's [GS201 BootControl implementation](https://android.googlesource.com/device/google/gs-common/+/ec2a9a0c7ac5d981862093b47be2d24c36518ecd/bootctrl/aidl/BootControl.cpp)
and this phone's ABL: version 3.11 stores two slot words in `devinfo` at offset
48; the success flag is bit 9. The generic AOSP `misc` format did not match.

With the user's explicit approval, `scripts/pixel-boot-success.py` marked the
validated installed slot A successful. It checks the exact installed boot
SHA256, supported device/root/header, active A and bootable state; privately
backs up the complete partition on the phone; changes only byte 49's success
bit; verifies every other byte unchanged; and immediately restores block
read-only protection. The successful flag remained set after the new installed
image boot. No slot B, identity, calibration or rollback/OTP operation was
performed. This is a separately authorized boot repair, not a permission for
modem services to write `devinfo`; the runtime still requires all nine
protected partitions read-only. The helper defaults to a dry run and is not
called automatically on startup.
