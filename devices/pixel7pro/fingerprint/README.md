# Fingerprint (Goodix under Trusty)

The sensor's trusted app, Goodix's `g7.app`, runs in Trusty, Google's secure
OS. Trusty already runs under our kernel (`trusty-core`, `-log`, `-ipc`,
`-virtio` and `gsa`, loaded at boot), with `/dev/trusty-ipc-dev0`.

## Lab

`run-fp-lab.sh [WIFI-ADDRESS]` (from the host) copies a RAM-only runtime
(`out/fp-work/fp-runtime.tar`, proprietary, never committed) and runs stock's
`trusty_apploader g7.app` in an unshared chroot that sees only the Trusty IPC
node, the system DMA heap and the null, zero and urandom nodes
(`fp-lab.sh`). `fp-sample.py` prints where the loader waits. Nothing
persistent is written, and a reboot unloads anything loaded.

## Findings (October 1)

- **Trusty IPC works.** Connect-only probes (TIPC_IOC_CONNECT, no message)
  answer within 1 ms. A nonexistent port is refused; the apploader, keymaster
  and gatekeeper ports accept. Trusty's doorbell interrupt fires.
- **The loader hangs waiting for the apploader's reply.** It sits in
  `read()` (`tipc_read_iter`) for its whole 60 s timeout: the load request,
  with the app's dma-buf shared, was sent, and Trusty never answers. Trusty's
  log shows nothing.
- **Stock loads the app only once Trusty's secure storage is ready**
  (vendor `fingerprint-goodix.rc`):
  `on property:ro.vendor.trusty.storage.fs_ready=1 && property:ro.vendor.fingerprint.ta.name=*`.
  That storage is served by `storageproxyd` (vendor `init.gs201.rc`):
  `storageproxyd -d /dev/trusty-ipc-dev0 -r /dev/sg1 -p /data/vendor/ss -t ufs`.
  Without it the apploader waits forever.
- **The RPMB unit is there, without a node to reach it.** The UFS RPMB
  well-known LUN is SCSI 0:0:0:49476 (0xC144). `storageproxyd -t ufs` sends
  RPMB frames through SCSI generic (`/dev/sgN`). Our kernel has
  `CONFIG_CHR_DEV_SG` off, and `/dev/bsg` has no node for that LUN.

## Secure storage and the app loaded (October 1, 23:25)

The user approved Trusty writing its secure storage. `fp-lab.sh --storage`:
- loads `sg.ko` (`kernel/scsi-sg`); the RPMB LUN becomes `sg1`, as on stock;
- runs stock's `storageproxyd` as system (1000), without capabilities
  (`ENTER_UID`/`ENTER_GID` in `gnss/enter-gnss.c`; two Binder thread-pool
  stubs added to the shim). Its storage:
  - RPMB;
  - `/var/lib/omarchy-mobile/trusty-ss` for stock's `/data/vendor/ss`;
  - the persist partition's `ss` directory, mounted read-write inside the
    lab's mount namespace while its block-layer guard is lifted. A full
    image of persist is in `/root/backups/persist-20261001.img` (64 MB).

Trusty's log then showed its four filesystems:
- **tp** (RPMB) and **tdp** (persist) loaded from stock's data;
- **td** (the old userdata) was gone, and Trusty created it empty;
- **nsp** loaded.

The apploader's policy passed (`passed policy: 8db26df6-…`), and the app
registered `com.goodix.fingerprint.test`, which accepts connections.
- **The load came from an earlier run's request.** It had waited inside
  Trusty for storage; its reply failed (-15) because that loader was gone.
- **This run's own request** failed with "Port ... already registered".
- **The first `--storage` run left persist writable.** Its exit trap ran
  under `set -e` and stopped at a failing umount. That was fixed and the
  guard restored by hand.

## What stock's fingerprint service needs (October 1, from its libraries)

`android.hardware.biometrics.fingerprint-service.goodix` is a thin AIDL front
end. Goodix's `libgf_hal.so` does the work, with `fp_utils.so`:
- **The trusted app,** on `com.goodix.fingerprint.test` (`/dev/trusty-ipc-dev0`),
  the port the loaded app registers.
- **`/dev/goodix_fp`,** the sensor's kernel driver, with `GF_IOC_INIT`, `EXIT`,
  `ENABLE_POWER`, `DISABLE_POWER`, `ENABLE_IRQ`, `DISABLE_IRQ` and `REMOVE`.
  Google did not publish the driver.
- **Factory calibration** in `/dev/block/by-name/mfg_data` (read).
- **The panel's local high-brightness spot (LHBM)** during capture, through
  Google's display HAL (`com.google.hardware.pixel.display`) and
  `/sys/class/backlight/panel0-backlight/{hbm_mode,local_hbm_max_timeout,state}`.
- **Other nodes:** `/dev/fth_fd` (`fp_utils`), and Android's binder services
  for thermal, power and the sensor NDK bridge.

## Sensor driver (October 2)

[`kernel/fingerprint`](../kernel/fingerprint/README.md) gives the HAL its
`/dev/goodix_fp` (the ioctl numbers read from `libgf_hal.so`'s calls) and the
interrupt as netlink events. Reset, the interrupt and the sensor's supply
(LDO15S, off as the bootloader leaves it; switched by the power ioctls, user
approved) work: powered, the sensor answers each reset with interrupts.

Closing `/dev/goodix_fp` disables the interrupt and powers the sensor off, so
a HAL that dies mid-capture does not leave it powered.

## The HAL in the lab (October 2)

v36 ([`kernel/fp-v36`](../kernel/fp-v36/README.md)) has binder, and the
LHBM circle as `pixel_scanout.lhbm`. `fp-hal-lab.sh RUNTIME.tar SHIM.so
[sm|hal|full] [SECONDS]` (on the phone) runs stock's fingerprint stack in a
RAM-only chroot. The runtime, `out/fp-work/fp-hal-runtime.tar`, is
`fp-runtime.tar` plus `servicemanager`, `vndservicemanager`, the HAL service,
Trusty's KeyMint and gatekeeper HALs and their libraries from stock's system
and vendor images (proprietary, never committed). The lab:
- mounts a binderfs of its own (nodes 0666: `storageproxyd` runs as system);
- starts `servicemanager` on `/dev/binder` and `vndservicemanager` on
  `/dev/vndbinder`, as init.rc does;
- in `full`, runs `storageproxyd` and the apploader as `fp-lab.sh --storage`
  does, and keeps `storageproxyd` running alongside the HAL;
- starts `android.hardware.biometrics.fingerprint-service.goodix`;
- gives it a small `/sys` of its own: the device tree and the CPUs read-only,
  and stand-ins for stock's panel sysfs (the panel name `sdc-s6e3hc4` selects
  the HAL's configuration), a thermal zone and Trusty's module parameters;
- creates the HAL's data directories (RAM-only).

`mfg_data` (factory calibration) is a read-only node; the HAL opens it
writable and gets EACCES, by design.

**`fp-shim.c`**, preloaded into every program, adds to the modem shim:
- **Logging:** liblog's entry points go to stderr. Without logd an abort said
  nothing.
- **SELinux:** there is none here. `servicemanager` aborts when
  `selinux_status_open()` fails, so the shim answers its checks with fixed
  contexts, every access allowed. The lab's limits are the chroot and running
  without capabilities.
- **Caller contexts:** `servicemanager` asks binder for each caller's
  security context (`BINDER_SET_CONTEXT_MGR_EXT`). Without an LSM, every
  transaction to it then failed with -EOPNOTSUPP
  (`security_secid_to_secctx`). The shim refuses that ioctl; libbinder falls
  back to the plain context manager and asks `getpidcon()`.
- **Wake locks:** no-ops. `storageproxyd` takes one around every RPMB request.
- **Power HAL lookups** fail at once. The HAL looks the power HAL up around
  every operation with `AServiceManager_getService()`, which waits up to 5 s
  for a missing service: four waits made each operation take about 20 s.
- **Properties through libc** (`__system_property_find` and
  `_read_callback`) come from the files in `/properties`. KeyMint's HAL
  reads the OS version and patch levels this way.
- **The sensor service** (`ISensorManager`) is absent as well. At every
  touch the HAL reads ambient light through `libsensorndkbridge`, whose
  `AServiceManager_waitForService()` never gives up, so no capture finished.
  The lab also sets stock's `persist.vendor.udfps.als_feed_forward_supported`
  to false.
- Built with `-mno-outline-atomics`: GCC's outlined atomics call libgcc
  helpers, which Bionic programs do not have.

**Result.** The HAL opens `/dev/goodix_fp`, connects to the trusted app,
reads the board (platform 3, stage 6, rev 1.0) and its OTP
(`cali_expo` 66.05), and registers its AIDL service. It reports the sensor at
(717, 2360), radius 121 px, which is where the LHBM circle is lit. Gaps it
logs and survives:
- no power HAL (`android.hardware.power.IPower`);
- no property service for `vendor.gf.*`;
- no previous statistics or enrollments (first run);
- `mfg_data` read-only, so the factory calibration is not injected into
  Trusty's storage (`injectMtCali`). Stock's storage, which Trusty loaded,
  may already hold it.

## The client, and enrollment's auth token (October 2)

**`fp-client.c`** plays FingerprintService and SystemUI against the HAL. It is
plain C on `libbinder_ndk`, with the parcels written as the NDK backend writes
them and the transaction numbers taken from the frozen V3 API (its hash
matches the phone's library). It is a Bionic executable without Android's
build system: its `_start` calls `__libc_init()` as Bionic's crtbegin does.
The build command is in its header.

`fp-hal-exec.sh /system/bin/fp-client COMMAND` runs it inside a running lab,
in its namespaces and chroot. `nsenter` needs `-r`: after `switch_root`, a
mount namespace's own root is the initramfs. Every program in the lab links
the vendor libbinder, whose default is `/dev/vndbinder`. The HALs and the
client meet at `vndservicemanager`.

Commands:
- `props`: the sensor's properties. One under-display optical sensor (ID 0),
  strong, 4 fingers per user, at (717, 2360) radius 121. It has
  `halHandlesDisplayTouches` and `halControlsIllumination` false, so touches
  and the circle are the client's job, as on stock.
- `list`: the enrolled fingers and the authenticator ID. None for user 0;
  stock's went with the old userdata.
- `auth` and `enroll`: touches come from the touchscreen (`/dev/input/event1`,
  in panel pixels like the sensor's location). A touch on the circle:
  1. calls `onPointerDownWithContext`;
  2. lights LHBM (`pixel_scanout.lhbm`, bound into the lab's `/sys`);
  3. waits a frame at the panel's 10 Hz idle (120 ms, `LHBM_SETTLE_MS`);
  4. calls `onUiReady`.
  Lifting the finger reverses that.
- `secret`: the shared secret (below).
- `remove ID`: removes an enrolled finger.

**The auth token.** `enroll` needs a HardwareAuthToken signed with the key
Trusty's KeyMint shares with gatekeeper and the fingerprint app. Until that
key is agreed, the app refuses every token
(`keymaster_get_auth_token_key: auth token key wrong length`). On Android,
keystore agrees it at boot, and LockSettingsService gets the token from
gatekeeper after checking the user's password. The lab now runs stock's
KeyMint HAL (as nobody/drmrpc, with stock's version properties: 15,
2025-02-05) and gatekeeper HAL (as system). `fp-client enroll`:
1. computes the shared secret with KeyMint alone (stock adds the Titan M2's
   StrongBox);
2. enrolls a lab password for user 0 with gatekeeper, once. The handle is
   kept in `/var/lib/omarchy-mobile/fp-lab`, a secure-storage record that
   `deleteUser` removes;
3. verifies it for the HAL's challenge;
4. passes the signed token to `enroll`.

The app accepted it and waited for touches.

**The app pins itself to CPUs.** `setTaCpuAffinity` puts it on CPU 4 when
idle and CPU 6 during operations. CPUs 4-7 are offline while the screen is
dark, and our Trusty driver lost each CPU's nop thread binding over a hotplug
cycle, so the app never ran again. v37 fixes the driver
([`kernel/trusty-v37`](../kernel/trusty-v37/README.md)). Now a call made
while those CPUs are offline waits until they come back (the screen lighting,
or the lab bringing them online).

## The first finger (October 2)

The user enrolled a thumb with `fp-client enroll`: 23 presses, every good one
counting 2 down from 40, until "0 to go" for finger 1380025933. It is kept in
Trusty's secure storage and was still listed after the HAL restarted.

`fp-client auth` then matched it. Two presses landed off-centre (PARTIAL);
the third, after one retry capture, returned the finger with a signed auth
token (type FINGERPRINT, the gatekeeper user's secure ID, the finger set's
authenticator ID).

- **The HAL allows one session.** A second `createSession` while one is open
  fails a CHECK and the HAL aborts. `fp-client` now takes a lock and refuses
  to run twice.
- **Touches also reach the shell,** so a press can open the app drawer or
  the notification bar. The lock screen and the enrollment page in Settings
  must own the touches over the circle.

## The service (October 2)

The stack now runs from boot, as an opt-in bundle like the modem's, in
`/var/lib/omarchy-mobile/fingerprint` (never in git):
- `runtime.tar` and `fp-shim.so`;
- `fp-client`, `fp-hal-lab.sh` and `fp-hal-exec.sh`;
- `pixel-goodix-fp.ko` and `sg.ko`;
- the `enabled` marker.

The boot script starts `fp-hal-lab.sh … serve` after the cameras (it needs
their system dma-buf heap), and again 30 s after it ends while `enabled`
exists. The user approved persist staying writable while it runs: Goodix's
app updates its file there (`ss/nsp`) during normal use, as on stock. It
changed on the first match.

`serve` is `full` plus `fp-client serve /run/omarchy-fingerprint/fingerprint.sock`,
until `fp-hal-exec.sh stop` or the service ends. The service keeps one HAL
session open and speaks a line protocol to the shell: commands in, JSON
events out (the list is in `fp-client.c`). For example:
- `status`: the credential, the fingers, the sensor and the panel;
- `auth`, then `down X Y`/`up` for touches on the circle (the shell owns the
  touches; the service lights the circle and tells the HAL);
- `enroll SECRET`: progress events carry the good taps left (`left`), from
  the HAL's remaining count and its step;
- `remove ID`, `verify SECRET`, `setcred KIND OLD NEW`, `cancel`.

**The credential** (`STATE`, `/var/lib/omarchy-mobile/fingerprint/state`)
is user 0's PIN or password, enrolled with Trusty's gatekeeper. One file
holds its kind, a PIN's length (for the keypad) and the gatekeeper handle,
replaced atomically. Setting it verifies the old one, so the secure user ID
stays the same and enrolled fingers stay valid. Before one is set, the lab's
fixed stand-in is enrolled, so enrollment can get a token.

**CPUs 4 and 6.** While an operation runs, the service prints `@hold`. The
script then writes `/run/pixel-cores-hold` and brings the two CPUs online,
and the display hook leaves held CPUs online while dark. On `@release` it
lets the hook take them offline again.

Checked over the socket: status; a temporary test PIN set with the same
secure ID, a wrong PIN refused and the right one verified; enroll refused
with the wrong PIN, started with the right one and cancelled. After a
reboot, the service came up by itself with the finger and the PIN.

## Next

1. **The lock screen** (shared shell): the session-lock protocol, a PIN pad
   or password field, the circle with pass/fail and optional vibration,
   owning the touches over the sensor.
2. **Settings → Security:** set or change the PIN or password, enroll with
   guidance (taps left, then the tip and the edges), a finger list, and
   switches for vibration, the animation and the sensor in AOD.
3. **Animations** around the circle: our own, tinted by the theme, and packs
   the user adds.
4. **The sensor in AOD** (opt-in): the touch controller's low-power
   detection, measured first.
5. **Lockout:** reset the fingerprint lockout after a correct PIN
   (`resetLockout` with a gatekeeper token).

## Earlier plan (done above)

1. SCSI generic as a module (`drivers/scsi/sg.c` built out of tree for our
   kernel), for a `/dev/sgN` on the RPMB LUN.
2. `storageproxyd` from stock in the lab chroot. Its file-backed storage
   would live in a persistent directory on our root, for example
   `/var/lib/omarchy-mobile/trusty-ss`. The phone's `userdata` is our root
   now, so stock's `/data/vendor/ss` is already gone.
3. Then the loader again, then the fingerprint HAL (binder, LHBM, the
   gatekeeper HAT).

`storageproxyd` lets Trusty read and write its secure storage: RPMB, which is
replay protected and authenticated with Trusty's own key, and the files
above. Nothing runs it without the user's go-ahead. On October 1, building
the SCSI generic module was refused by the automatic safety check as
credential exploration, and is left to the user.
