# Waydroid: Android apps on the Pixel

Waydroid 1.6.3 (Arch Linux ARM package) runs LineageOS 20 (Android 13,
GAPPS system with Google Play, MAINLINE vendor, 2026-09-27 images) in an LXC
container. The
Android UI renders with the GPU through Mesa (Panthor) and shows as Wayland
windows in Hyprland.

## Status (October 5, 2026)

Working on kernel v39:
- Android boots in about a minute (`sys.boot_completed` 1, BOOT_COMPLETED
  sent).
- GPU rendering: RenderEngine reports "Mali-G610 MC7 (Panfrost)" (the G710
  under the patched model entry, below).
- Networking: Android gets 192.168.240.x from the host's dnsmasq on
  `waydroid0`, NATed to Wi-Fi or cellular; ping and DNS work from Android.

Android apps as phone apps:
- Each Android app is its own window, with app ID `waydroid.PACKAGE`, so it
  gets a switcher card and closes like the phone's own apps; it never shares
  the screen with another app. There is no Android home screen.
- Single-window mode (October 6; `persist.waydroid.multi_windows` false,
  set by `waydroid-container watch`): only the app in front of Android is
  live. The others keep a picture of their last frame and are in Android's
  background, as on a phone, so they notify. Bringing an app's window
  forward (focus) brings it to the front of Android (the hwcomposer's
  `setFocusedTask`). In multi-window mode (October 5) every Android window
  stayed "on screen" to Android, so WhatsApp parked with a chat open neither
  notified nor held back read receipts.
- Android's screen follows the shell (October 6): it is on only while an
  Android app is in front of a lit, unlocked screen, and for 10 s after the
  shell launches one (Android draws nothing while asleep, so the app would
  never get a window). The shell writes "awake" or "asleep" to the FIFO
  `$XDG_RUNTIME_DIR/omarchy-mobile/android-power` (going to sleep waits
  1.5 s); the watcher (root) sends Android's WAKEUP or SLEEP key. While
  asleep every Android app is stopped (WhatsApp's activity `STOPPED`).
  Waking takes under half a second.
- Android's screen-off lays a black layer (ColorFade, level 0) over the
  display, and single-window mode draws every layer into the window of the
  app in front of Android, so that app's window went black: in the
  switcher, and when it came back until Android drew again. The
  ColorFade can only be turned off as a "low-RAM device". Before the SLEEP
  key the watcher now starts the keyboard bridge's blank `SleepActivity`
  (version 6): Waydroid freezes the app's window at its last frame, as for
  any app no longer in front, and the black goes into the blank screen's
  window, which `hypr-mobile.lua` keeps on the hidden special workspace.
  It finishes when anything covers it; the shell closes its leftover window
  3 s after Android wakes. When it maps, Hyprland focused the window focused
  before (the parked app, so its workspace became active); the shell puts
  workspace 1 back in front.
- When Android itself shows its (hidden) home screen, as when the app in
  front exits on Back, Waydroid closes every Android window: the cards go,
  while the apps keep running in the background and reopen at once from the
  drawer.
- The watcher keeps Android's density at the shell's scale (480 dpi) and
  its time zone at the phone's.
- Notifications (October 6): Waydroid's bridge (WayDroidService in
  LineageOS's platform library) posts each Android notification with the
  desktop entry `waydroid.PACKAGE`, a "default" action for its tap intent
  and its own actions as plain buttons. Tapping a notification in the shade
  or its banner invokes "default" (Android opens the conversation), wakes
  Android and brings the app's window forward (`openNotification` in the
  shell). A reply action came through as a plain "Reply" button that sent no
  text; the shade now shows its own Reply for it, and the text goes through
  the watcher's FIFO ("reply PACKAGE TITLE TEXT", base64) to the keyboard
  bridge's `ReplyService`, a notification listener that fills in the
  notification's RemoteInput and fires the action, as a watch does. It
  answers the newest notification of that app with the same title, or a
  lone one; otherwise nothing (never a guess between conversations). Its
  receiver needs WRITE_SECURE_SETTINGS, so other Android apps cannot use it.
- Push (October 6): microG instead of Google Play Services. The VANILLA
  image's LineageOS spoofs microG's signature for microG's own key only (its
  services.jar has the `fake-signature` hook; microG's Self-Check ticks
  "System spoofs signature"). GmsCore 0.3.17.252432 and the Companion
  (com.android.vending 0.3.17.40226) come from microG's repository
  (microg.org/fdroid/repo), signed with microG's published key, SHA-256
  `9bd06727e62796c0130eb6dab39b73157451582cbd138e86c468acc395d14165`
  ("NOGAPPS Project"); they are installed as ordinary apps (`pm install`),
  copies in `~cube/.cache/omarchy-mobile/apks/`. In microG Settings:
  Google device registration and Cloud Messaging on (no Google account).
  microG then holds one connection to Google's push server (port 5228) for
  every app, the watcher exempts it from Android's battery saving, and
  WhatsApp registered for push at its next start (it dropped its own chat
  connection). Signal uses it once registered. Google sees which apps get
  pushes and when, not their content.
- The phone's photos and videos (October 7): Android apps' pickers
  (WhatsApp, Signal) show them under Pictures/Phone and Movies/Phone. The
  watcher hard-links every file of `~/Pictures` and `~/Videos` into
  Android's storage. That is the same file system, so nothing is copied,
  and links whose file was deleted go too. Android's media index then scans
  (`content call … scan_volume`), at Android's start and when an Android
  app comes to the front, at most once a minute. All 35 photos were indexed.
  A read-only bind mount there was invisible to Android: its media provider
  works in a mount namespace of its own.
- Cameras (October 7): not yet. Android now gets the camera bridge's two
  webcams (`overlay/mobile/camera-bridge`, `/dev/video20` and `21`), and
  only those. `install.sh` adds them to `lxc.py`'s nodes (the running
  install had its `config_nodes` extended by hand), and the container
  manager's `chmod 777` now covers only them. It used to cover every
  `/dev/video*`, the raw sensors and the video decoder included. Waydroid's
  external camera HAL (provider 2.7) still found 0 cameras: it takes MJPEG
  only ("input format list is empty"), and the bridge writes YUYV for the
  browser. It also reports such cameras as external, not front or back,
  which apps that ask for a front or back camera (CameraX) skip.
- Media controls (October 6, bridge v7): what an Android app plays shows on
  the shade's and the lock screen's media card, with its buttons.
  - **The bridge's MediaBridge** follows Android's media sessions, through
    the notification access ReplyService already has. It picks the one that
    plays, else the most recent, and logs its state as JSON under
    `OmarchyMedia` whenever it changes: app, title, artist, album, duration,
    position, actions and an artwork version. The artwork (at most 512 px)
    goes to the bridge's `files/media-art.png`.
  - **The watcher** keeps the newest line in
    `$XDG_RUNTIME_DIR/omarchy-mobile/android-media.json` and copies the
    artwork out as the session's user.
  - **The phone's side:** `omarchy-mobile-android-media`
    (`overlay/mobile/android-media.py`, started by the shell) publishes it
    as the MPRIS player `org.mpris.MediaPlayer2.android` while there is a
    track. The player's buttons go back as "media CMD [POSITION_MS]" on the
    FIFO, then to the bridge's MediaReceiver, which calls the session's
    transport controls.
  - **Test without an app or sound:** "media test" on the FIFO starts a
    session of the bridge's own (TestSession), and "media test-end" ends it.
    Through MPRIS, play/pause, next, seek and play all reached it, and the
    player went away with it.
- Push in deep sleep (October 6, `scripts/pixel-suspend.py`): deep sleep
  unloads Wi-Fi, so the push connection runs over mobile data, which stays
  up in sleep. The modem wakes the phone when a packet arrives.
  - **The move:** the container sits behind NAT and never sees Wi-Fi go, so
    its sockets would keep a dead route until microG's next heartbeat. After
    unloading Wi-Fi, the adapter closes them in Android's network namespace
    (`nsenter … ss -K`, every connection but loopback), as Android closes a
    lost network's sockets. It then waits up to 20 s for microG's new
    connection to port 5228; microG took 5.3 s. The apps' own connections
    move too. `/run/pixel-push-uplink` records where they went.
  - **While asleep:** a modem wake that brought packets on `rmnet0` ends the
    sleep as a `network` wake. The shell's policy then keeps the phone up
    15 s, for the messenger to fetch its message and post it. Modem wakes
    with no packet sleep again at once, as before.
  - **Keepalive:** Android's alarms (microG's heartbeat, which keeps the
    carrier's NAT from dropping the connection) cannot wake the phone from
    the container. While the push connection is up, the adapter wakes every
    10 minutes and stays up 5 s for whatever came due.
  - **Back on Wi-Fi:** NetworkManager's dispatcher
    (`scripts/pixel-android-uplink-dispatcher.sh`, installed as
    `53-pixel-android-uplink`) closes them again when the default route
    changes interface while the phone is awake: Wi-Fi back after a sleep, or
    lost on the way out. It leaves them alone while the adapter runs
    (`/run/pixel-suspend.active`).

  Tested with a server trickling bytes over mobile data: the phone slept
  31.7 s, woke as `network` on the first byte, and after Wi-Fi came back
  microG was on Wi-Fi again. A real message in deep sleep is still to be
  tried. Web apps (Discord, Telegram, Snapchat) do not get this: their
  WebSockets need the page awake every minute or so, so their messages
  arrive at the next wake. Their Android apps get push through microG.
- Android's status and navigation bars are hidden (`install.sh` makes
  `waydroid app launch` set `immersive.full`, as in multi-window mode).
- An Android app is in the app drawer only once chosen in Settings > Apps
  (`omarchy-mobile-app android`, `android-show PACKAGE`, `android-hide
  PACKAGE`): that marks Waydroid's desktop entry with the
  `X-Omarchy-Mobile-Shown` category, which Waydroid keeps when it updates the
  entry. Android's own apps (Calendar, Camera...) are listed as built in.
- The boot script starts the container manager; tapping an Android app
  starts or unfreezes the session (`waydroid app launch`).
- F-Droid is installed (signature checked against F-Droid's key).
- Google Play (October 5): the GAPPS system image, checked against the OTA
  hash, replaced VANILLA (kept as `images/system.img.vanilla`), with fresh
  Android data (the old data is in `~cube/.local/share/waydroid/data.vanilla-20261005`).
  `waydroid.cfg` follows the GAPPS channel and sets
  `persist.waydroid.multi_windows=true` and `ro.sf.lcd_density=480` from
  the first boot. Play Store needs the device registered once, by its owner,
  at google.com/android/uncertified with the GSF android_id
  (`gservices.db`, table `main`).

- No caption bar (October 5): Android draws back, minimize, maximize and
  close buttons on top of every freeform window. `no-caption/` is a static
  overlay on Android's framework resources that makes that bar zero height
  with zero-size buttons, so apps get the whole window and no touch at
  their top is taken for a button. It goes in Waydroid's vendor overlay.
- Back: the shell's left-edge swipe sends the evdev Back key (158) when an
  Android window is in front (`requestBack` in `overlay/mobile/shell.qml`,
  through `omarchy-mobile-vkbd`). Android's `Generic.kl` maps it to BACK.
  The key is held 80 ms: a press and release sent together did nothing,
  and Escape (Android falls back from it to BACK) is kept by some apps
  (F-Droid's app pages).
- No "Configure physical keyboard" notice: `keyboard-layout.sh` gives
  Waydroid's keyboard the English (US) layout (see the script for why
  Waydroid's own choice fails).
- Typing (October 5): the phone's own keyboard. `keyboard-bridge/` is
  Android's input method: when a text field takes focus Android shows it,
  an empty view as tall as the phone's keyboard (global setting
  `omarchy_keyboard_height`, in dp, which the watcher sets from the
  keyboard settings), so the app lays itself out above the keys. It logs
  "shown" and "hidden" (tag `OmarchyKeyboard`, version 3); the watcher
  writes that to `$XDG_RUNTIME_DIR/omarchy-mobile/android-keyboard`, and the
  shell shows the phone's keyboard while it says shown and an Android app is
  in front (`androidWantsKeyboard`). (In multi-window mode it was a window of
  its own, `waydroid.InputMethod`, which `hypr-mobile.lua` still hides.)
  Over an Android app the keyboard reserves no space, so the app's window
  never changes size. Keys arrive as hardware keys through Waydroid's
  Wayland keyboard. Android's LatinIME stays installed but unused; it had
  opened as a second window that split the app's tile.
  Until October 6 the bridge's view filled the whole display (Android's
  `setInputView` replaces the view's layout parameters, and a plain View
  measured WRAP_CONTENT takes all the height offered), so Android took the
  keyboard to cover everything: WhatsApp's phone number screen shrank to
  nothing, and apps that pan moved off the top. The view now measures to the
  set height (version 2). The keyboard's hide handle over an Android app sends
  Back, which closes Android's input method and the phone's keyboard with it;
  hiding only the phone's keyboard had left Android's space empty.
  An Android app that leaves the screen with its keyboard up has it closed
  first (version 5's `HideReceiver`, through the watcher's FIFO, at the
  start of the home or switcher gesture, else before Android sleeps), so
  the app lays itself out at full height again: the switcher's card showed
  it squeezed above an empty keyboard space (Settings search: content 1926
  px tall with the keyboard, 2970 after). The switcher keeps the card's
  picture live for 0.8 s meanwhile (`holdLive`).
- A second launch of an Android app brings its window forward (`focusApp`
  in the shell), as for web apps; launching it through Waydroid again had
  left it tiled beside the app in front.

Not done yet: camera, audio routing and sensors; non-ASCII text from the
keyboard.

Hotplugs (October 5): Waydroid's hwcomposer reports a display hotplug on
every configure of any Android window (`xdg_toplevel_handle_configure` calls
`do_hotplug`), focus and state changes included, and it sizes Android's one
display from that window. Android's graphics side failed four times between
11:32 and 12:49, mostly while I was testing:
- system_server's watchdog fired with WindowManager waiting on a hotplug's
  `getDynamicDisplayInfo` (12:37);
- surfaceflinger crashed with a null pointer in `onComposerHalVsync`
  (12:49), likely a vsync racing a hotplug;
- the hwcomposer lost its Wayland connection ("socket error: Invalid
  argument", in `data/waydroid_hwc_wayland_error.txt`) and aborted (11:32,
  12:39). The cause of these two is not known; five Back keys in a row did
  not reproduce it.
Each one restarts Android's system_server and closes every Android window,
and once Android came back with no default network until the next session
restart. Two Android windows on one workspace make it worse: they get
different sizes, the display flips between them, and touches land in
whichever task Android has on top. The launcher gives each Android app its
own workspace, which avoids that.

`patch-hwc-hotplug.py` first patched the vendor hwcomposer to hotplug only
when the configured size differs from the display's (details in the script). Three
switches between F-Droid and Calculator launched over each other went from 47
display reconfigurations to 12 (the 12 are real size changes from that
tiling); the same switches through the home gesture caused none, and typing
with the keyboard none. A shrunk display still leaves freeform tasks shrunk
when it grows back (Android clamps them and never regrows them), so the
watcher reads the display size at each new task, and the keyboard no longer
resizes Android windows.

October 6: the size changes alone still hung Android. WhatsApp and Signal
ended up tiled on workspace 1 (Waydroid opens a task's window by itself at
times, on the workspace in front), and a minute later system_server's
watchdog fired: the android.display thread held the display manager's lock
while it waited in `getDynamicDisplayInfo` (SurfaceFlinger) for a hotplug,
and the power and window managers waited on that lock. No Android window
closed (Waydroid's close asks the stuck system_server to remove the task),
and the watchdog's restart of system_server left Android with no default
network until the session restarted. The patch now keeps the display at the
size of the first window (466x990 dp here) for the whole session: two
Android windows held tiled for 10 s and swapped three times left
`wm size` at 1398x2970 with no watchdog, and both closed. A tiled Android
window only shows cut off now, and the shell keeps Android windows apart
anyway: one that opens or is moved beside another window goes to a
workspace of its own (the app just launched stays in front), and the
switcher does not tile Android cards. A change to the work area (a
different bar height) needs a session restart.

A tap on an Android app with the session stopped (the first after a phone
boot) starts it and shows the app in about 13 s. A launch sent a few seconds
after a separately started session reached `sys.boot_completed` was dropped
once (F-Droid's activity never started); the next tap worked.

## What it needed

| Problem | Fix |
|---|---|
| No bridge, veth, overlay, fuse or NAT modules (the kernel had none built) | `pack-modules.sh` / `install.sh`: modules in `/lib/modules/RELEASE` |
| `kernel.modprobe` points at the initramfs helper, so nothing loads on demand | `waydroid-container.sh` loads every packed module first |
| LXC: "Failed to initialize cgroups" | cgroup2 mounted on `/sys/fs/cgroup` |
| Images truncated by the 01:08 brownout reset (ext4 delayed allocation) | downloaded again on the laptop, checked against the OTA hashes, pushed with `sync` |
| lmkd exited four times, so Android init rebooted the container | kernel v39: `CONFIG_PSI` |
| zygote: "Not allowlisted: /run/arch/apex/..." | the container manager runs in a mount namespace whose root is the Arch root (`waydroid-container.sh`) |
| surfaceflinger aborted: gralloc could not allocate (no G710 in upstream Mesa) | `patch-mesa-g710.py`, via the Waydroid vendor overlay |
| netd: iptables tables, matches and targets missing | kernel v39: the Android netfilter set as modules |
| Android had an address but no route ("ip rule" not supported) | kernel v39: `IP_MULTIPLE_TABLES`, `IPV6_MULTIPLE_TABLES` and the other Android routing options |

`install.sh` also patches Waydroid's `user_manager.py`: a session started by
`waydroid app launch` ran that launch at every "Android is ready", so after
each Android crash the first app of the session opened again by itself over
whatever was in front (F-Droid on October 5, which also took the shell down
once); now only the first ready launches it. And it keeps the container away
from hardware only our drivers should drive: it patches Waydroid's `lxc.py` so `/dev/video*` (pixel-camera's
four lenses), `/dev/fb*` and `/sys/kernel/debug` are not bound into the
container. Run `waydroid upgrade -o` afterwards to regenerate `config_nodes`.

## The namespace

The initramfs mounts the Arch root at `/run/arch` and chroots into it, so the
kernel's root mount is still the initramfs. Inside the chroot `mountinfo`
shows ext4 at `/`, LXC pivots, and Android's paths come out under `/run/arch`.
`waydroid-container.sh start` runs the manager as
`busybox chroot /proc/1/root busybox unshare -m sh -c 'mount --make-rprivate /;
cd /run/arch; mount --move . /; busybox chroot . waydroid container start'`:
the switch_root move, in a private namespace, so the phone's own mounts stay
as they are. A boot that used switch_root itself would not need this.

## Use

On the phone as root, once per kernel:
```
/root/waydroid/install.sh /root/waydroid/waydroid-modules-$(uname -r).tar.gz
waydroid upgrade -o
```
Then, each time:
```
waydroid-container start          # /usr/local/sbin, as root
waydroid session start            # as cube, in the desktop session
waydroid show-full-ui             # as cube
```
Waydroid froze the container while none of its windows showed
(`suspend_action = freeze`), so messaging apps received nothing in the
background. Since October 6 the phone runs with `suspend_action = none`
(`install.sh` patches `hardware_manager.py` to accept it): Android keeps
running, and its notifications reach the shell's shade through Waydroid's
notification bridge (`WayDroidService` in Android, `waydroidnotifications`
in the session). Settled, Android with Google Play services used under 1 %
of a core with the screen off; for the first minutes after a start Play
services take most of a core. `waydroid container unfreeze` still wakes a
frozen one.

**Google-free by default (October 6).** The user wants Android without
Google for now (Signal and WhatsApp work without it; Snapchat, Discord and
the like become web apps), with Google as an option. The phone runs the
vanilla LineageOS 20 image: `images/system.img` is VANILLA, the GApps image
stays as `images/system.img.gapps` and its Android data as
`~cube/.local/share/waydroid/data.gapps-20261006`; `system_ota` points at
VANILLA.json. Switching is: stop the session and `waydroid-container`,
swap `system.img` and the `data` directory, set `system_ota`, run
`keyboard-layout.sh` for that data (each data set needs it, or Android posts
"Configure physical keyboard" again), start again.

The first vanilla session after the switch went wrong once: SystemUI
crash-looped (172 times in a minute, a null list in
`AppOpsControllerImpl.setListening`), system_server restarted, and its
Ethernet service then waited for ever for the network stack's IpClient
(`awaitIpClientStart`), so Android had no network ("WhatsApp needs an
internet connection") and windows would not close. A session restart was
clean. If an app reports no connection, check `dumpsys connectivity` for
"Active default network: none" and restart the session.
Signal and WhatsApp came from their own sites (`apksigner verify` matched
Signal's published certificate, and WhatsApp LLC's key rotated from its
original WhatsApp Inc. one), installed with `waydroid app install`, then
shown in the drawer with `omarchy-mobile-app android-show PACKAGE`; the
keyboard bridge was installed again and set with `ime enable`/`ime set`.

**Binder after a reboot (October 6).** `waydroid-container.sh` mounts
binderfs in the phone's own namespace and now also makes the `/dev/binder`,
`/dev/vndbinder` and `/dev/hwbinder` links. Without them (a fresh `/dev`
every boot), the first container start mounted a binderfs of its own in the
manager's namespace: Android ran there, while the session's tools waited
for "binder Service Manager" on the phone's, so no app opened (the Play
Store "crashed", F-Droid never came up) and a waiting `waydroid app launch`
spun at 100 % CPU.

From the laptop, after a kernel build: `pack-modules.sh`, then copy the tarball
and `install.sh` to `/root/waydroid/`. The Mesa libraries:
`patch-mesa-g710.py vendor.img OUT`, then `OUT/{lib64,lib}/libgallium_dri.so`
to `/var/lib/waydroid/overlay/vendor/{lib64,lib}/`.

The caption overlay: `no-caption/build.sh FRAMEWORK_RES_APK BUILD_TOOLS_DIR`
with the image's `/system/framework/framework-res.apk` and Android
build-tools 34 (aapt2, apksigner; Java), then `OmarchyNoCaption.apk` to
`/var/lib/waydroid/overlay/vendor/overlay/` and restart the session. `cmd
overlay list android` inside Android shows it enabled. Rebuild it after a
system image update (it links against that framework-res's ids).

The hwcomposer: `patch-hwc-hotplug.py hwcomposer.waydroid.so OUT` with the
vendor image's `/lib64/hw/hwcomposer.waydroid.so`, then OUT to
`/var/lib/waydroid/overlay/vendor/lib64/hw/hwcomposer.waydroid.so`; it
refuses any other build, so a vendor update needs the patch redone. The
October 5 build (hotplug on a size change only) is kept on the phone as
`/root/hwcomposer.waydroid.so.size-change-only`.

The keyboard bridge: `keyboard-bridge/build.sh ANDROID_JAR BUILD_TOOLS_DIR
ECJ_JAR` (API 33 `android.jar`, build-tools 34, the Eclipse compiler
`ecj` on a plain Java runtime), then inside Android `pm install -r` the
APK (copied to the data directory's `local/tmp`), `ime enable
org.omarchy.mobile.keyboard/.BridgeIme` and `ime set` the same.

## Power

The first boot at full clocks drew at most 1.07 A from the battery, with
brownout protection (`kernel/bcl`) loaded.

Idle, screen off, awake (on USB, so no deep sleep), ODPM energy over 60 s
windows, two rounds:

| Session | Total | Little cores | Wi-Fi/BT |
|---|---|---|---|
| none (container manager only) | 341 / 344 mW | 90 / 88 mW | 21 / 23 mW |
| running, frozen (Waydroid's state with no window) | 356 / 358 mW | 91 / 96 mW | 24 / 21 mW |

So a frozen session costs about 15 mW awake. The container manager alone
costs nothing measurable (no CPU time or wakeups in a minute). Deep sleep
with a frozen session is still to be measured unplugged.
