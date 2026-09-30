# Checkpoint — 2026-09-17

**September 27 sleep:** [sleep, charging and battery](sleep-20260927.md).
The phone charges to full on the stock profile (4.39 V; 2 A from a wall
charger) and can hold at 80 %. It sleeps (s2idle) 10 s after the screen goes
dark, with a screen timeout and a 15-minute background wake; incoming calls
and texts wake it through a QRTR wake filter. Suspend had been crashing the
modem, because genpd re-voted MSS at the top corner on resume (`mss_vote.ko`
now follows the domain), and the sensor DSP, because the freezer restarted a
FastRPC invoke (freezable wait). Text and missed-call notifications had never
posted (a gdbus argument bug); now fixed.

**September 27 calling:** [texts, calls and the phone apps](calling-20260927.md).
VoLTE calls work with audio both ways (earpiece, microphone, speaker, mute),
with the proximity sensor blanking the screen at the ear; SMS works both ways.
Omarchy-themed Phone, Messages and Contacts apps and a shell incoming-call
screen are installed. Call audio is sdm845's q6voice (VoiceMMode1) with four
patches; the uplink needed TX topology NONE (the default ECNS topology is
silent without calibration, and the network then drops the call on an RTCP
timeout at ~21 s). A SLIMbus enumeration race is fixed. USB does not come up
after a forced reset (cause still open), so SSH now also works over Wi-Fi
(static address, DHCP/Static in Settings) and the port-23 shell is USB-only.
Never dial or text a number the user has not approved.

**September 27 cellular:** [data and IMS](cellular-sim-20260926.md#third-round-data-and-ims-working).
The QLink assertion was the AP holding the modem rail (`mss.lvl`) at its top
corner (rpmhpd before `sync_state`); `mss_vote.ko` releases it. The
registration drop was the missing IPA data path; IPA now loads before the
modem. Data needed DPM Open Port, WDA QMAPv4 and an `ipv6.ko` built from the
#194 tree. ModemManager + NetworkManager now bring up mobile data at boot
(IPv6 and IPv4 HTTPS 200), and an IMS DCM server (`phone-qmi.py ims-dcm`)
lets the modem register IMS: SMS and voice at full service. Call audio next.

**September 26 SIM:** [first SIM](cellular-sim-20260926.md). The Tello card
reads (USIM and ISIM, no PIN, no SIM lock); opening the USIM session makes the
modem fetch and activate `Commercial-TMO` itself. Online, it registered on LTE
(310-260 "Tello", CS and PS attached) in three seconds, then asserted 103 s
later: `RFLM@qsf_hl_seq.c:118 (rflm_qlnk_ls_retry_cnt < 2)`, the 7T Pro's
QLink failure, now with the right profile active. Pins, TLMM routing, QLink
clkref, unused-resource cleanup, LDO modes and LLCC all match stock. The slot A
Android test is unsafe: the Arch root is `userdata`. The modem's own DIAG log
(diag-router at boot, opt-in, decoded with the image's qdb) shows QLink's
low-speed link start failing on the first RF wakeup after the modem has idled;
modem recovery panics the kernel and stays off. See the doc for the tools and
the incidents (a whole-SMEM read rebooted the phone).

**September 24 ultra-wide:** [the IMX481](camera-ultrawide-20260924.md) works
from runtime modules on the second camera control interface: full 4656x3496
frames at 30 fps, upright merged photos. Its AK7374 focus actuator, which the
7T Pro port left unpowered, now focuses it (tap to focus too). All three rear
cameras load together, and Omarchy Camera's lens buttons read 0.6×, 1× and
3× (libcamera-guacamole 0.7.2-13, whose autofocus now waits for exposure to
settle).

**September 24 cameras:** [the telephoto](camera-telephoto-20260924.md)
(Samsung S5K3M5) works from runtime modules: raw 4208x3120 frames, focus. One
overlay loads both rear cameras, libcamera lists both, and Omarchy Camera
switches between them with 1× and 3× buttons. Its lens now has the actuator's
whole range (a sweep put focus at 550-1023, beyond the 7T Pro's 400), its
stills merge bursts, and continuous focus holds steadier in the hand
(libcamera-guacamole 0.7.2-10).

**September 24 always-on display:** [the dim clock](always-on-display-20260924.md)
(Settings → Display): the CRT close, then the time, date, battery and
notification icons on black; power or a double tap wakes it through the CRT
open; face down or in a pocket the panel goes fully off. Shell work only: the
panel has no low-power mode.

**September 24 shell:** [fixes](shell-fixes-20260924.md). A watchdog starts
the shell again if it exits (Hyprland 0.56.2 disconnected it when an app's
window closed during a preview capture, leaving the phone looking dead);
swiping away the last app returns to the desktop; the brightness slider
follows the finger now that #194 stops the flicker.

**September 24 display:** [kernel #194](kernel194-20260924.md) (slot B,
rollback #193) keeps DSI commands out of command mode frames: the brightness
flicker is gone (10 DSI errors for a slider drag on #193, none on #194), and
automatic brightness now fades smoothly.

**September 24 sensors:** [SLPI](sensors-20260924.md). Kernel #193 (slot B,
rollback #192) moves the firmware carve-outs to the OEM map and adds a 16 MiB
FastRPC pool; everything else runs as on #192. A runtime module starts the
sensor DSP with this phone's own firmware: it publishes the Snapdragon Sensor
Core service over QRTR, `/dev/fastrpc-sdsp` exists, and it survives suspend.
With hexagonrpcd serving the stock configuration and a copy of the persist
registry, and OxygenOS 10's SLPI firmware (00083; the OxygenOS 12 image left
out the motion sensors, as on the 7T Pro), 42 sensor types register and the
accelerometer (axes checked by hand), gyroscope, magnetometer and light
sensor stream to a test client. Proximity is the light chip's, which reads
near once its threshold is lowered for the display in front of it (a
registry patch); factory calibration comes with the persist copy. It all
starts at boot after the radio, with Arch's
iio-sensor-proxy (libssc) on top: the shell has automatic brightness (the sun
icon by the slider) and Android's rotate button, both checked by hand.

**September 22 cellular groundwork:** [baseline](cellular-baseline-20260922.md).
Read-only PDC probe works: 25 resident EU profiles, active `Free-VoLTE`, no
T-Mobile resident; `Commercial-TMO` (PDC ID `cb45c810…`) is in the firmware
library. Kernel #189 (#188 plus the IPA DTB, embedded in the Image) is on
slot B; `ipa.ko` loaded by hand brings up `rmnet_ipa0` and an rmnet link.
The bottom speaker works after the user reseated the bottom board. A Tello
SIM (T-Mobile network) is on order.

**September 23 smoothness:** [CPU and GPU scaling](smoothness-20260923.md).
CPU frequency scaling was simply disabled in the boot DTB; a runtime overlay
enables it (schedutil, energy-aware scheduling, CPU thermal cooling) and the
big cores run 3.5x faster. GPU scaling already works. Kernel #191 (slot B,
rollback #190) adds 60/90 Hz panel modes and cpufreq in the DTB; the shell runs
at 90 Hz (89.8 Hz measured). The radio now finds its partitions by GPT name.

**September 23 cameras:** [main camera bring-up](camera-20260922.md). The main
IMX586 streams raw 4000x3000 frames over its C-PHY through CAMSS, all from
runtime modules; the first image shows the room (no focus yet). A new hold
module lets CAMCC bind without releasing the modem's power (MSS/MX/CX/MMCX
unchanged). Kernel #190 (on slot B) adds PM8150L's GPIO block and the Bluetooth
`hsuart0` alias; rollback is `scripts/flash_kernel190.sh rollback`.

**September 22 Bluetooth:** [bring-up](bluetooth-20260922.md). WCN3990 works
on #189 via runtime modules and a UART13 overlay: stock firmware, scanning,
pairing, and A2DP audio (aptX HD) to OnePlus Bullets, heard by the user. Fixed a
wake-IRQ/pinctrl self-deadlock and a missing serial alias (runtime shim until
the next DTB carries `hsuart0`). Settings pairs and connects; the shade has a
toggle; headphone buttons drive the volume overlay. Boot autostart is enabled
(not yet through a reboot).

**September 22 speakers:** [mic loopback measurements](speakers-20260922.md).
Both speakers work; the bottom one needed the user to reseat the bottom board.
The ADSP played the 24-bit path 48 dB low, which is why media was inaudible.
Output is now S16 in mono, capped at -18 dBFS (bottom) and -42 dBFS (earpiece),
behind a processing sink (400 Hz high-pass, leveler, limiter). An 8-period
buffer fixed silent and ticking PipeWire playback; YouTube is audible and clean.

**September 22 microphone session:** [microphone trial and boot-slot incident](microphone-20260922.md).
The internal microphone works: AMIC4 records through PipeWire and starts with
the audio stack; AMIC1 and AMIC3 are also real mics. A full regmap register
sweep crashed the phone (900e);
slot B then ran out of boot retries because Linux never marks a boot successful.
Recovered with `fastboot set_active b`. `guacamole-boot-slot` now marks each
boot successful after the desktop is up; verified that ABL keeps retry 6 on
boot `e8adeb46`. Never iterate over every regmap.

**September 22 read-only investigation:** [hardware handoff](hardware-plan-20260922.md)
and [cellular handoff](cellular-plan-20260922.md) record the next implementation
steps. Native5 modem and ADSP are running; SIM slots absent, DMS shutting-down,
no cellular netdev. ALSA exposes shared MultiMedia1 playback/capture; microphone
remains untested. Backlight exists at 320/1023; 25 thermal zones return plausible
temperatures, but brightness adjustment and throttling were not tested. No
Bluetooth controller, camera media/video, IIO or Type-C devices were listed;
no cpufreq policies were listed. No hardware configuration was changed.

**Current hardware checkpoint (Sep 18):** [Audio bring-up](audio-bringup-20260918.md).
ADSP, WCD9340 and both TFA9874 amps run; ALSA playback now **hw:0,3**.
Short quiet streams lock both amps' clocks and power them down afterward.
User heard one initial tone; individual output confirmation remains pending.
Fixed a live-OF/SPMI parent bug in the volume-key probe; clean boot verifies both
keys enumerate without a fault. Consolidated audio startup also passes a fresh
boot. Same #188 kernel; boot `3cf9dc5c-e395-4b01-bcae-62952ea39efc`.
PipeWire output has a fixed -36 dB cap pending speaker protection; themed volume
OSD installed. Opt-in automatic startup hook is installed and the live supervisor passes;
automatic reboot/startup and post-boot playback now pass. Audio suspend and
microphone remain untested. Latest boot `dcca1845-3070-43d3-9e21-85fe257b5c64`.
README/privacy/screenshots/local Git review follow audio; **no public push until
user approval**. Local private recovery bundle is in ignored `out/private/`.

**Latest (Sep 18):** [Keyboard and browser work](keyboard-browser-20260918.md).
Installed explicit-tap popup input and swipe-down keyboard handle; 52 UI tests
pass. User approved temporary browser account display access. Chromium and Grok
are now installed and verified on native Wayland as `mobile-browser` UID 1001.
Renderer seccomp/PID/network isolation verified; GPU opens renderD128. Grok
loads in its own app-mode window. Profile/downloads are in /home/mobile-browser;
standard-user session + sudo migration is on the roadmap. App-drawer launch failure
was traced to missing ~/.local/bin in Quickshell PATH; session entrypoint fixed
and both apps verified through the drawer’s own launch handler. No approval remains
pending and no fastboot is needed. Reboot persistence not yet tested.

**Current user priority: mobile shell.** [Notification shade + font](notification-shade-20260918.md)
installed on unchanged #188. Drag-down shade, themed detail popups, actual
notifications, battery metrics, Wi-Fi scan/connect/IP/DNS, calendar and opt-in
weather. JetBrainsMono Nerd Font installed for shell/Kitty/keyboard; font picker
on roadmap. User feedback fixes installed: no reserved bottom wallpaper band,
keyboard reaches bottom, fixed-height shade during popup focus changes, swipe
up across shade to close (notification scroll retains ownership until bottom).
41 touch tests + 5 backend tests pass; live notifications, Wi-Fi details/scan,
saved test-network activation/HTTPS and stable geometry verified. User physical
follow-up for the last gesture/jump fixes remains. Weather location unset;
new Wi-Fi password entry and session-bus startup after reboot not yet tested.
USB replug recovered same boot. No suspend test or diagnostic modules active.
Backup: `/root/.local/state/omarchy-mobile/backups/notification-shade-20260918/`.

**Latest completed test:** [MMCX SLEEP #188](mmcx-sleep-test-20260918.md)
passed 31.543660 seconds asleep with user-confirmed touch and no PM failures.
MMCX sleep/wake 0/6 at entry, 2/6 at exit; CX/MX 7/7, MSS ffffffff/9, XO 0/0
unchanged. The per-CPU cache captures do not establish when/who restored level 2.
All 283 clock records match, but one of 68 RPMh records differs; deepest counters
still zero. Modem healthy, Wi-Fi HTTPS 200 after an AP retry (~10.66s association).
Charger online/Full near 4.20 V ceiling, battery 84%/0 mA as before the trial.
Both controls 0, RTC cleared, pm_async 1, observers unloaded, no trial armed.
Boot `65942a28-ceec-430b-a072-05e594903681`, #188 remains installed. Frozen result
`out/checkpoints/20260918-mmcx-sleep-verified/`; #187 rollback retained. Next
investigate MX/CX dependency before further release; retain MSS and awake/wake.

**Latest completed test:** [combined CX + DSI sleep trial](cx-dsi-sleep-test-20260917.md)
passed **30.715147 seconds asleep**, user-confirmed touch, modem/Wi-Fi HTTPS and
charging recovery. CX SLEEP/WAKE 0/7 and XO 0/0 together; MX/MMCX/MSS unchanged.
All 283 clock and 68 RPMh records valid and equal at entry/exit. Same #187 boot,
suspend success 2, all failures 0. AOSD/CXSD/DDR remain 0; no battery-life claim.
CX restored to 0, RTC cleared, pm_async 1, observers unloaded, no trial armed.
Checkpoint `out/checkpoints/20260917-cx-dsi-sleep-verified/`. Next investigate
MX/MMCX dependencies while retaining MSS and awake/wake requirements.
Earlier test states below are historical; #187 remains the installed baseline.

**Latest verified fix:** [DSI PHY PM #187](dsi-phy-pm-test-20260917.md) passed
31.299355 seconds asleep with physical touch, modem, Wi-Fi HTTPS and charging
recovery; zero PM failures. Display AHB/XO prepare references reached 0 and XO
sleep/wake requests fell 3/3→0/0, returning to 3/3 awake. CX/MX/MMCX/MSS holds
unchanged, AOSD/CXSD/DDR still zero; no battery-life claim. Boot
`3c184f64-6dda-4509-99ec-ef0e77b1ff00`, kernel #187 remains installed, CX control 0.
All three observers unloaded, RTC cleared, no trial armed. Checkpoint
`out/checkpoints/20260917-dsi-phy-pm-verified/`; rollback to #186 available.
Next isolate remaining power-domain sleep holds while preserving MSS; the prior
CX-release test was on #186; the combined #187 result is recorded above.

**Latest completed test:** [CX sleep-only diagnostic](cx-sleep-test-20260917.md)
passed one 30.707757-second suspend with user-confirmed touch, zero PM failures,
running modem, Wi-Fi HTTPS 200 and charging recovery. Native5 #186, boot
`b64adda0-e729-4241-8be2-94b0cf53d04e`. CX sleep 0/wake 7 at recorded entry/exit;
MSS 9, MX 7/7, MMCX 6/6 and XO 3/3 held. AOSD/CXSD/DDR remained zero, so no
battery-life improvement established. CX control restored to 0 (normal startup policy),
RTC cleared, pm_async 1, trial exited, passive modules unloaded. No test armed.
Next investigate remaining MX/MMCX holds and XO clock consumers during suspend;
XO's request is clock prepare state, not the power-domain sync_state clamp.
Keep MSS and awake/wake votes intact. Result checkpoint:
`out/checkpoints/20260917-cx-sleep-verified/`.

**Latest modem/power investigation:** [modem foundations](modem-foundations-20260917.md).
Read-only kernel snapshot confirms QMP attached, handover issued and CX/MSS
proxy usage zero. Diagnostic unloaded; working modem hold remains intact.
Standard QMI/QRTR query tools installed, no ModemManager daemon. Basic modem/SIM
queries respond (both slots absent, stable pre-online `shutting-down` mode).
PDC platform inventory crashed qmicli; no core captured, no radio failure;
do not repeat it unchanged. Wi-Fi HTTPS still 200. No rail-changing trial armed.
Roadmap brings modem/audio foundations forward and retains an early notification
shade milestone. Xfinity physical SIM is offered subject to IMEI eligibility;
this phone/carrier/Linux voice compatibility remains unverified.

**Evening follow-up:** Same recovered boot passed eight ordinary suspend
cycles with zero failures, totaling 3h36m asleep. User reports 85%→78% over
~3h45m with ~20m screen use; treat as informal battery evidence. Portable
event-driven battery/Wi-Fi indicators installed to remove polling latency.
Physical follow-up confirmed prompt icons and a ninth successful suspend
(23.175s, zero failures). Charging current became positive ~2s after enable;
Wi-Fi associated ~5s after resume, with a later AP switch/primary-connection
gap still needing investigation. HTTPS passed after the test. Temporary timing
recorder stopped. See [evening notes](evening-observations-20260917.md).

**Current investigation:** [isolated MSS test](mss-handoff-test-20260917.md)
failed radio resume after 31.508 seconds asleep. Only MSS changed 9→0; CX/MX,
MMCX and XO stayed at their working corners. MPSS watchdog, Wi-Fi resume -110,
and failed_resume=1 reproduced without CAMCC/global handoff. MSS restoration
then timed out and blocked in RPMh; no restoration-success claim is valid.
The helper's `--mss-handoff` mode is retired and must not be repeated.
Recovery needed SysRq-u (confirmed ext4 read-only) then SysRq-b because both
normal remount and normal reboot were blocked. Automatic recovery boot passed:
`be4da80f-2acc-4b4b-bea0-7d4667d62123`, native5 #185, control 0, healthy modem,
Wi-Fi HTTPS 200, desktop/input, USB and guarded charging, fresh PM failures 0.
Retired helper installed and refusal checked; no test process remains.
Frozen native5 #184 remains available via
`scripts/flash_mss_handoff_test.sh rollback`. No new experiment is armed.

**Latest work:** [deeper suspend diagnostics](deeper-suspend-20260917.md)
confirmed 91.61 seconds asleep, working touch and network/charging recovery,
but zero SoC deep-state counts (five successful Linux suspend cycles).
A missing CAMCC driver blocked power-controller sync_state and held CX/MX
sleep votes at maximum. Loading the stock driver temporarily completed sync
and dropped those votes to zero; initial display/input/network checks passed.
**The subsequent suspend failed radio resume:** MPSS watchdog and Wi-Fi -110,
despite 91.17 seconds asleep and display/touch/USB/charging recovery. CAMCC
must not be promoted. Recovery reboot of unchanged native5 is verified:
`927a5e82-e6ae-4efc-9123-80844ecf186b`, running modem, Wi-Fi HTTPS 200,
desktop/input/charging auto-start. The approved key-ack Wi-Fi modules also
passed automatic boot activation. No sleep test is armed.
The [compact battery and Wi-Fi indicators](status-indicators-20260917.md)
are installed. [Updated mobile roadmap](../../../docs/mobile-roadmap.md) records critical
speaker/earpiece/microphone support, haptics, shade widgets, richer keyboard
and text selection, and Omarchy desktop integration.

**Earlier native5 #184 baseline (now rollback); automatic boot verified.** Eight
CPUs, native 60 Hz desktop, touchscreen and power-key registration, hidden
keyboard, test-network Wi-Fi, guarded charging and network time all started without
manual bootstrap or host clock injection. The RTC binds under its correct SPMI
parent; a five-second alarm fired while awake. Battery was 85%, 28.3 C, charging.
Evidence: `out/native5-reboot/`; frozen image: `out/checkpoints/20260917-native5-test/`.
Rollback via `scripts/flash_suspend_desktop.sh rollback` restores native4.

**Native5 unplugged suspend/wake passed.** The phone spent 12.15 seconds in
s2idle, returned before its 90-second fallback alarm, and the user reported the
physical test worked. All kernel suspend/resume failure counters stayed zero.
Touch and power key were rediscovered; Wi-Fi HTTPS returned 200 and charging
resumed on USB reconnection (85%, 28.6 C). Evidence/checkpoint:
`out/checkpoints/20260917-native5-suspend-verified/`.

**Power-button suspend/wake is physically verified.** Two unplugged button
cycles slept 10.32 and 7.10 seconds, with zero PM failures, working touch,
reconnected Wi-Fi/USB and no immediate re-sleep. Charging resumed after
reconnection (+79 mA in a follow-up gauge sample). The temporary 90-second
alarm override is removed; Power now sleeps until a wake event. Plugged-in
Power continues to blank/restore the display while preserving charging.
Checkpoint: `out/checkpoints/20260917-power-button-verified/`.
See [power-button policy and recovery](power-button-policy-20260917.md).
**Wi-Fi delay fix passed a five-minute suspend trial.** No key-removal timeout
occurred, all PM failure counters remain zero, Wi-Fi and charging recovered.
The user approved persistent installation; the tested pair is now installed
with the complete original module directory retained for rollback. Hashes and
current connectivity pass; next-boot activation remains to verify. See
[driver findings/installer](wifi-key-ack-20260917.md).

**First measured idle comparison:** 131 mA screen-off awake versus 83 mA across
a five-minute suspend interval, approximately 37% lower in this short trial.
Actual sleep: 301 seconds. Measurement resolution is about 12 mA; longer repeats
are required before predicting runtime. [Results](idle-measurement-20260917.md).
Automatic idle, background network wake, cellular service and true shutdown
remain unfinished.
See [suspend investigation](suspend-20260917.md).

**Previous verified charging checkpoint: native3; first automatic boot passed.** Native
desktop, eight CPUs, USB SSH, touch, virtual keyboard and power key registered.
Gauge and guarded charger started automatically; positive battery current and
charge accumulation observed. Independent PMIC readback confirms 500 mA FCC,
500 mA USB input and 4.20 V float. Driver unbind disables charging; rebind
resumes it. The user completed the unplug/use/power-button test and reconnected.
USB recovered without repair; logged charging stopped unplugged and resumed
on reconnect, with increasing stored charge and unchanged PMIC limits.
Repeat reboot, wall charging, taper, suspend and true shutdown remain unverified.
[Results and limits](charging-20260917.md#native3-first-boot-and-driver-cleanup-verified).
Verified checkpoint: `out/checkpoints/20260917-native3-verified/`; evidence:
`out/native3-runtime/`; flash/rollback: `scripts/flash_power_desktop.sh`.

The following power-investigation paragraphs describe the preceding native2 tests.

**Power investigation in progress:** a temporary overlay enables the previously
disabled SPMI/PON path. Linux and Hyprland now detect `pm8941_pwrkey`; USB,
eight CPUs and native 60 Hz display remain available. Two physical button
press/release pairs and an automated display off/on cycle passed. The user
confirmed button-controlled screen off/on with working touch. The shared
binding is saved; the kernel overlay still needs manual loading after reboot.
Suspend and real shutdown remain unverified. The boot image is unchanged.
The external TI bq27541 gauge now binds to the standard Linux power_supply
driver. Unplugged use verified live discharge/current/voltage readings, and
USB SSH reconnected without repair. A themed percentage indicator is installed.
The charger was disabled in PMIC configuration; a bounded 500 mA/4.20 V trial
produced about 470 mA of battery charging. Persistent charging is the next
priority, ahead of suspend. The timed test restored the original disabled
charging state; fix the charger's PM8150b encodings before enabling it normally.
[Charging findings](charging-20260917.md).
[Battery experiment](battery-gauge-20260917.md).
[Power findings and active test](power-investigation-20260917.md).

**Keyboard startup fixed:** missing `/dev/fd` broke the helper before USB SSH
setup. The helper now works without that link. Automatic startup and shell
show were verified after a full reboot, before running the USB setup helper;
the user also confirmed the keyboard/gestures work. [Evidence](keyboard-boot-fix-20260917.md).

**Theme wallpapers installed:** Appearance now previews and cycles standard
backgrounds, and selecting a theme applies its wallpaper. Per-theme choices
survive mobile shell restart; existing windows were preserved. 92 stock images
are available. [Wallpaper results and recovery](wallpaper-switching-20260917.md).

**Touch drawers installed:** finger-tracked sheets, tap-selected-workspace to
enter, preview swipe-to-close, and hold/drop workspace moves, tile swaps and
close targets are installed. Automated touch and live compositor tests pass;
the user confirmed workspace entry and moves. Follow-up fixes retain a large
translucent held preview, remove drawer remapping, and guard keyboard
construction against nested show requests. The user accepted those refinements.
A further update adds downward dismissal anywhere in the drawer when its list
is at the top, and from Overview previews; 38 Qt checks pass, and the user
confirmed this latest gesture works great. [Touch results and recovery](touch-drawers-20260917.md).

**Resume here:** [next-session plan](next-session.md) — power/shutdown/battery
and the hardware queue; the latest touch interactions are user-confirmed. Native 60 Hz,
ordinary pacman and theme wallpapers now work; 90 Hz remains future work.

**Previous verified rollback: native2 #179.** Native DSI
at 1440x3120@60 and the FD640-accelerated Hyprland/Quickshell desktop start
automatically. Eight CPUs, touch, hidden keyboard and USB SSH all start;
no manual display repair is needed. Ordinary pacman sync passes with Landlock.
Verified image/evidence: `out/checkpoints/20260917-native2-verified/`.
[Test evidence and rollback](native-display-work-20260917.md).
The keyboard focus-flash fix passes controlled tests and survives reboot;
the user confirmed the result looks good after reboot. [Keyboard fix](keyboard-focus-fix-20260917.md).

**Shutdown issue:** outer-initramfs power-off rebooted the phone, as observed
by the user. Filesystem sync/read-only remount succeeded; actual power-off did
not remain off. [Evidence and next investigation](shutdown-20260917.md).

**Latest interaction update:** the button dock is replaced by three bottom-edge
gesture zones, with a redesigned workspace/app overview. Kitty has an opt-in
native touch-scroll patch, confirmed by the user. App selection now switches
workspace and focuses the exact window. See [gesture results and recovery](mobile-gestures-20260917.md).

**Mobile userspace:** mobile scaling, Kitty + on-screen keyboard, launcher,
window cards/workspace moves, bottom-edge swipes, standard Omarchy palette
selection and branded Fastfetch are installed and tested live. The shared
shell is in `overlay/mobile/`; OnePlus setup is in `devices/oneplus7pro/`.
See [mobile results and recovery](mobile-work-20260917.md) and
[portable architecture/theme integration](../../../docs/mobile-architecture.md).
Full Omarchy/theme-repository installation remains future work. Keyboard
startup has now passed a full reboot test. Current live kernel is native2.

**Rollback: touch1 #175 was flashed and boot-verified on slot B.** All eight
cores, USB NCM, automatic Adreno Hyprland/Quickshell startup and automatic
S6SY761 touchscreen detection work. The saved fresh-boot logs contain both
`ADRENO_DESKTOP_STARTED` and `TOUCHSCREEN_READY`; both renderers report FD640.
The verified image/source bundle is `out/checkpoints/20260916-touch1/`.

USB internet sharing and public-key SSH are configured and verified live.
Run `bash scripts/phone-ssh.sh` from the host. After reboot, wait for the
desktop and run `bash scripts/phone-usb-up.sh` to restore routing, DNS, SSH
and the clock. Native1/native2 start SSH automatically; the touch1 rollback requires the helper.
See [USB networking and package management](usb-networking-20260917.md).

The entries below are historical checkpoints.

**Latest verified (~23:57 PDT): gpu2 #174 auto-boots the Adreno desktop.**
Hyprland/Quickshell report FD640; scanout matches the working wallpaper/bar.
Eight cores and USB remain working. **Touchscreen works live**, including
user-confirmed five-finger input, using the Samsung S6SY761 diagnostic modules
and a runtime DT overlay. Normal desktop is restored after the touch test.
**touch1 #175 is built but not flashed** to load touch automatically after GPU
startup. See [touchscreen results and next steps](touchscreen-work-20260916.md).

**Latest verified (~23:38 PDT):** gpu1 / **#173** is running. All eight cores
and USB pass; **Hyprland and Quickshell render on FD640**, and two exact-color
tests reached scanout memory. Clean compositor shutdown, no kernel warnings.
Wallpaper and existing Quickshell bar are running in the accelerated session.
**gpu2 / #174 is built, not flashed**: it makes this desktop start automatically
after Adreno initializes. Flash with `scripts/flash_gpu_desktop.sh` in fastboot.
Current gpu1 otherwise returns to software rendering on reboot. Details and
remaining display/touch limitations: [CPU/GPU work](cpu-gpu-work-20260916.md).

The entries below describe earlier checkpoints/experiments.

**Latest experiment (~23:20 PDT):** temporary GPU Hyprland selected FD640,
but hit a known imported dma-buf release bug and became stuck during exit;
software restoration failed. USB still responds. The upstream fix has been
built as **gpu1 (#173), not yet flashed**. Put the handset in fastboot and use
`scripts/flash_gpu_test.sh`; details in [CPU/GPU work](cpu-gpu-work-20260916.md).
The eight-core smp2 checkpoint remains available for rollback.

**Latest verified checkpoint — 2026-09-16 ~23:05 PDT:** Kernel **#172**
`6.17.0-sm8150-codex-smp2-g379d8fe35c7c-dirty` on slot B. **All eight cores
online and individually tested**, USB networking and persistent Arch working,
Adreno hardware shader/readback test passed. Hyprland/swaybg remain on software
rendering through simpledrm; native DSI/DSC is still unfinished. Frozen image pair:
`out/checkpoints/20260916-8cpu-gpu/{boot,dtbo}.img`. Reflash with
`bash scripts/flash_cpu_test.sh`. The current working build tree is
`.work/linux-sm8150-codex-cpu`; use `scripts/rebuild_cpu_test.sh`.
See [latest CPU/GPU results](cpu-gpu-work-20260916.md).
**The sections below are historical and include superseded CPU/UFS/GPU status.**

**2026-09-16 ~18:05 PDT:** Pausing DPU work. Restore flash: `boot-embed-hypr-pin.img` + `dtbo-filtered-gpu-sqe.img`. Writeup: `docs/progress.md`. Path 2 (DPU+DSC+panel) is the real Omarchy display stack.

**2026-09-16 ~18:00 PDT:** User: top-half orange/green glitch lines, wallpaper gone. INTF_1 TE was 0 (cmd-mode). Leftover FB painted green; dsi-fill magenta SETCRTC; DSI-1 enabled. Matches uncompressed RGB into a DSC cmd-mode AMOLED (SM8150 DSC still garbled upstream).

**2026-09-16 ~17:52 PDT:** Leftover-halt boot: **INTF_1 TE was already 0x0** (cmd-mode AMOLED, not video INTF). Painted leftover FB green, dsi-fill SETCRTC magenta, DSI-1 enabled, 0 Oops. USB live.

**2026-09-16 ~17:48 PDT:** Flashed leftover INTF halt + green leftover FB + magenta dsi-fill.

**2026-09-16 ~17:41 PDT:** Wallpaper+glitch+magenta flecks = leftover ABL INTF still scanning 0x9C000000 while KMS DSI-1 is enabled. Packed halt of INTF_1 TE then leftover FB green then dsi-fill magenta.

**2026-09-16 ~17:32 PDT:** GEM_GPUVA-only: **0 Oops**, `dsi-fill` SETCRTC **OK** `1440x3120 fb=91 magenta`, **DSI-1 enabled**. USB live. DPU mapped=0 MDSS mapped=1. First Linux DPU scanout.

**2026-09-16 ~17:29 PDT:** Flashed GEM_GPUVA-only DPU (17:25 image, no SMMU group join).

**2026-09-16 ~17:25 PDT:** GEM_GPUVA+iommu_group_add 900e'd (`05c6:900e`) at DPU time. Dropped SMMU group join; keep DRIVER_GEM_GPUVA only. Packed 17:25. Need hardware reset.

**2026-09-16 ~17:19 PDT:** Flashed GEM_GPUVA KMS + MDSS iommu-group DPU image.

**2026-09-16 ~17:17 PDT:** Packed `msm_kms_driver` **DRIVER_GEM_GPUVA** (KMS-only drm skipped gpuva list init → get_vma_locked oops). Also add DPU to MDSS iommu group. Waiting for START. Wallpaper still visible = leftover scanout, DSI never enabled.

**2026-09-16 ~16:58 PDT:** Scanout-fallback boot: USB live, 0 init Oops, card2 + DSI-1 connected. `kms iommu try DPU mapped=0, MDSS mapped=1` — VM created on MDSS. `dsi-fill` SETCRTC still Oops `get_vma_locked` (vma_new/list walk, x23=-16). DSI remains disabled. GEM pin on KMS VM is still the blocker.

**2026-09-16 ~16:54 PDT:** Flashed scanout fallback: NULL kms->vm no longer oopses; framebuffer prepare uses dma_map phys when IOMMU VM is missing; iommu retry on MDSS parent; dsi-fill waiter 180s.

**2026-09-16 ~16:47 PDT:** Magenta-fill boot: **0 Oops on DPU init**, fbdev skipped, `msm-kms card2`, DSI-1 connected 1440x3120 disabled. Init waiter missed card2 (90s). Live `dsi-fill` SETCRTC Oops in `get_vma_locked` (likely NULL kms->vm). USB live. Display GEM/SMMU is the remaining scanout blocker.

**2026-09-16 ~16:42 PDT:** Flashed magenta `dsi-fill` pair. After card2, kill Hyprland and SETCRTC DSI-1 to solid magenta.

**2026-09-16 ~16:39 PDT:** Packed `dsi-fill` (magenta dumb-buffer SETCRTC on card2/DSI-1) into `boot-embed-dpu.img`. Previous no-fbdev boot went **blank** (DPU took leftover scanout). Waiting for START to flash.

**2026-09-16 ~16:35 PDT:** Flashed no-fbdev DPU (`boot-embed-dpu.img` 16:33). Hyprland restarts onto card2 when DSI appears.

**2026-09-16 ~16:28 PDT:** DSI TX workaround: **msm-kms card2**, **DSI-1 connected 1440x3120@60**, then Oops in `msm_fbdev_driver_fbdev_probe` GEM pin (same get_vma_locked). Hyprland on card2 saw DSI-1 and allocated 1440x3120 XR24 dumb buffer; EGL still unmatched. Stuck Hyprland PIDs won't SIGKILL. Next: disable DRM fbdev so init doesn't oops.

**2026-09-16 ~16:22 PDT:** Flashed DSI TX dma_alloc workaround (`boot-embed-dpu.img` 16:19).

**2026-09-16 ~16:19 PDT:** Packed DSI TX `dma_alloc_coherent` workaround (skip GEM IOVA oops). Same dtbo. Restore: `boot-embed-hypr-pin.img`.

**2026-09-16 ~16:15 PDT:** Stub panel attached (`panel-simple-dsi ae94000.dsi.0`). DPU hw rev **0x50000001**, then Oops in `dsi_tx_buf_alloc_6g` → `msm_gem_get_and_pin_iova_range`/`get_vma_locked`. simpledrm `card0` gone; only GPU `card1`. USB live. Next: DSI TX GEM/SMMU.

**2026-09-16 ~16:10 PDT:** Flashed stub DSI panel pair. Expect wallpaper ~2 min, then DPU/panel attach; leftover scanout may die.

**2026-09-16 ~16:08 PDT:** Probe-order boot: 0 Oops, `msm_dpu`+`msm_dsi` bound, still no extra DRM card. msm_dsi only `component_add`s when a MIPI panel attaches. Packed stub `oneplus,guacamole-panel` (panel-simple DSI, 1440x3120 cmd, 4-lane) into `boot-embed-dpu.img` + `dtbo-filtered-dpu.img`.

**2026-09-16 ~16:00 PDT:** Flashed DPU probe-order fix (`boot-embed-dpu.img` 15:57 + `dtbo-filtered-dpu.img`). DSI/phy marked okay before MDSS; msm_drv NULL match → EPROBE_DEFER.

**2026-09-16 ~15:44 PDT:** DPU delay USB-live. dispcc/mdss/phy bound. DPU Oops in `component_master_add_with_match` (NULL match) because MDSS populate probes DPU before DSI. DSI0 `waiting_for_supplier=1`. DRM still card0 simpledrm + card1 GPU. Hyprland/swaybg still running. Next: mark DSI/phy okay before creating MDSS; add panel.

**2026-09-16 ~15:40 PDT:** Flashed delayed-DPU pair on slot B. Waiting for USB; DPU enable is ~120s after late_init.

**2026-09-16 ~15:37 PDT:** Packed delayed-DPU pair `out/pmos/boot-embed-dpu.img` + `out/dtbo-filtered-dpu.img`. dispcc clocks no longer reference disabled USB QMP PHY. Kernel enables dispcc/mdss/dsi0 at 120s (after USB+Hyprland+GPU). No panel node yet. Restore wallpaper desktop: `boot-embed-hypr-pin.img` + `dtbo-filtered-gpu-sqe.img`. Expect leftover scanout to die when dispcc reprograms.

**2026-09-16 ~15:28 PDT:** Tiny center mark is the pointer (`hyprctl cursorpos` 720,1560 on 1440x3120, `hardwareCursorsInUse: true`), not the logo (`disable_hyprland_logo` already true). Live `cursor.invisible = true`. Packed into hypr-pin ramdisk.

**2026-09-16 ~15:22 PDT:** Wallpaper visible (swaybg + wall0.png). Tiny Hyprland mark is the compositor overlay: `misc:disable_hyprland_logo` already true; `hyprctl eval` + `reload` also set `disable_splash_rendering`. GPU `card1` is render-only (no CRTC); hyprpaper still cannot PRIME. Next: DPU/DSI so GLES apps can use Freedreno with a real scanout.

**2026-09-16 ~15:18 PDT:** Hyprland painting (readable splash; lua error gone). GPU `card1` is up but unused for scanout. `hyprpaper` still asserts GBM/PRIME on simpledrm. Pushed `swaybg` to UFS; live PID 299 `swaybg -i /usr/share/hypr/wall0.png -m fill` on Unknown-1 after mounting `/dev/shm`. Init now auto-starts it next ramdisk. Clock is 1970 (no RTC) — that is the splash year.

**2026-09-16 ~15:04 PDT:** Fixed-Lua boot USB-live. Hyprland `-c /etc/hypr/hyprland.lua`, `hl.bind` present, **no lua errors in hyprland.log**. Still aquamarine EGL fail on simpledrm (96-line stall). If the panel still shows the previous red overlay, that is leftover scanout, not a new lua failure.

**2026-09-16 ~15:01 PDT:** Flashed hypr-pin with minimal 0.56 Lua (logo off, binds registered, no `hl.on`). Previous boot painted a new red overlay: lua error aborted before binds so `disable_hyprland_logo` never applied.

**2026-09-16 ~14:50 PDT:** Compositor-before-GPU boot is USB-live (Device 005, ping ~2ms, CPUs 0–3). Hyprland PID with `-c /etc/hypr/hyprland.lua`, simpledrm Unknown-1 **enabled** 1440x3120, `wayland-1` exists. Aquamarine still cannot create an EGL renderer on simpledrm (`eglQueryDeviceStringEXT` / no matching EGL_DRM_DEVICE_FILE). Multi-GPU `card0:card1` crashed (`GBM allocator failed`). GPU probed at ~90s as `card1`. Next: DPU/DSI so msm DRM has a CRTC and Freedreno can paint. Restore: `boot-embed-4cpu-hypr.img`.

**2026-09-16 ~14:42 PDT:** Repacked `boot-embed-hypr-pin.img`: Hyprland starts as soon as simpledrm `card0` exists; GPU delayed 90s so EGL can init with kms_swrast before Adreno `renderD128` appears. Lua config + `AQ_DRM_DEVICES=/dev/dri/card0`. 14:32 boot proved Aquamarine modesets Unknown-1 1440x3120 on simpledrm, then stalls in `eglQueryDeviceStringEXT` because msm renderD128 has no matching EGL device.

**2026-09-16 ~14:32 PDT:** Flashed first hypr-pin (GPU at 40s, init waited for renderD128). USB Device 126, ping ~2.5ms, `online=0-3`, Hyprland PID with `-c /etc/hypr/hyprland.lua`. Panel still frozen: EGL bound GPU render node. Restore: `boot-embed-4cpu-hypr.img`. GPU-only freeze pair: `boot-embed-gpu-sqe.img`.

**2026-09-16 ~14:18 PDT:** SQE ramdisk: USB Device 123, ping ~3ms, `online=0-3`. `loaded qcom/a630_sqe.fw`, `a640_gmu.bin`, GMU fw v2.0.261, `[drm] Initialized msm 1.13.0` `card1`/`renderD128`. Hyprland PID 173 `GALLIUM_DRIVER=freedreno`. `msm_gem_free_object` WARN. Panel showed frozen Hyprland logo + two top banners (old `.conf`; not `start-hyprland`). Restore: `boot-embed-4cpu-hypr.img`.

Phone serial `$PHONE_SERIAL`. Host at this checkpoint: `workstation`, gadget still enumerated as `1d6b:0104` with ping to `172.16.42.1`.

---

## What is running right now

| Item | Value |
|---|---|
| Kernel | `6.17.0-sm8150-g379d8fe35c7c-dirty` `#134 SMP PREEMPT Wed Sep 16 00:20:54 PDT 2026` |
| Tree | `.work/linux-sm8150` (sm8150-mainline, dirty local bring-up patches) |
| Boot path | **OOS12 ABL hdr2** + filtered 10-entry dtbo on **slot B**. Not kexec. Not `fastboot boot`. |
| Slot A | LineageOS 23.2 20260907 (expendable restore target) |
| Userspace | Busybox initramfs only (`rdinit=/init`). No Arch, no UFS, no GPU. |
| Panel | ABL leftover scanout at `0x9C000000` (1440×3120, stride 5760, a8r8g8b8). FB console crop ~y=1200 height=720, 4× VGA 8×8. Prints `guacamole-init: alive n=N UDC='a600000.usb'` every 10s. |
| USB | HS peripheral gadget `1d6b:0104` (`omarchy-oneplus7pro` / `guacamole-bringup` / serial `$PHONE_SERIAL`). NCM + ACM. UDC `a600000.usb` / `dwc3-gadget` / driver `g1`. |
| Phone IP | `172.16.42.1/24` on `usb0` |
| Host IP | `172.16.42.2/24` on `enp198s0f0u2` (NetworkManager “Wired connection 3”) |
| Shell | `nc 172.16.42.1 23` (busybox `nc -lk -p 23 -e /bin/sh`). Not SSH. ACM is `/dev/ttyACM0` on the host but no getty on `ttyGS0` in this ramdisk. |

Proof: `out/usb-reachability.log` (also `{SCRATCH}/usb-reachability.log`).

Flashed images (do not overwrite until you have a better one):

- `out/pmos/boot-embed-fbusb.img` (packed 2026-09-16 00:21, 96M, payload 30982144)
- `out/dtbo-filtered-fbusb.img` (same time, 10 entries, ~65KB each, KEEP 32–35 fragments)
- Frozen copies: `out/pmos/boot-embed-fbusb-20260916-usb.img` and `out/dtbo-filtered-fbusb-20260916-usb.img`

---

## Talk to it (host)

```bash
lsusb -d 1d6b:0104
# if the NCM iface has no IPv4:
nmcli connection modify "Wired connection 3" \
  ipv4.method manual ipv4.addresses 172.16.42.2/24 \
  ipv4.gateway "" ipv4.never-default yes ipv6.method link-local
nmcli connection up "Wired connection 3"

ping 172.16.42.1
nc 172.16.42.1 23
```

The iface name may change on replug (`enp198s0f0u2` this session). Match the new USB ethernet device, not `enp198s0f4u1u1` (that is something else).

If the phone is sitting on the alive console but the host sees nothing: unplug/replug USB only. Do not reboot unless you mean to.

---

## Reboot / reflash this known-good pair

Hardware: **Vol Up + Vol Down + Power** until the START menu, then plug in.

```bash
fastboot flash boot_b out/pmos/boot-embed-fbusb.img
fastboot flash dtbo_b out/dtbo-filtered-fbusb.img
fastboot set_active b
fastboot reboot
```

Lineage restore lives in `.work/firmware/payload-out/{boot,dtbo,vbmeta}.img`. Script: `scripts/restore_lineage_on_fastboot.sh`. Slot A is the Android parachute.

900e (`05c6:900e`) = Qualcomm crashdump, panel usually **black**. Sahara MEMORY_DEBUG reads of KMSG time out (`scripts/sahara_dump.py`). Hardware reset out of 900e; `scripts/sahara_reset.py` has not been reliable.

---

## Do not regress these

These are load-bearing. Several look optional and are not.

1. **Apps SMMU SID 0x140 on `usb@a600000`.** Keep `iommus = <&apps_smmu 0x140 0>` (do not `/delete-property/ iommus`). Probe without it creates a UDC; **writing UDC / first EP0 DMA 900e’s at ~48s**. Confirmed: configfs mkdir/link of NCM+ACM is safe; `echo $UDC > UDC` is the crash without SMMU.
2. **Do not set `iommu.passthrough=1`.** The working cmdline dropped it.
3. **Bake `clk_ignore_unused` / `pd_ignore_unused` / `regulator_ignore_unused` in C defaults** (`drivers/clk/clk.c`, `drivers/pmdomain/core.c`, `drivers/regulator/core.c`) **and** in DT `/chosen/bootargs`. ABL extra_cmdline never reaches this kernel (we scan the **embedded** DTB, not ABL’s FDT). Photo of the failure: `clk: Disabling unused clocks` then hang (`IMG_20260915_231940_262.jpg`).
4. **`CONFIG_INITRAMFS_SOURCE="bringup.cpio"`** in the kernel tree (`linux-sm8150/bringup.cpio`, built from `.work/initramfs-root/`). Do not copy ABL `linux,initrd-*` into the embedded DTB — that 900e’d at ~45s with a black screen.
5. **`DTC_FLAGS_sm8150-oneplus-guacamole := -@`** in `arch/arm64/boot/dts/qcom/Makefile`. Rebuild DTB without `-@` → filtered dtbo KEEP 0 (~300 byte entries) → ABL bounces to fastboot. Rebuild `embedded_dtb.o` **after** the DTB (parallel `make Image` can assemble the old embed).
6. **`&dispcc { status = "disabled"; }`.** `dispcc-sm8250` matches `qcom,sm8150-dispcc` and reprograms MDP/DSI PLLs → panel goes black. ABL leftover scanout is the console; do not let Linux take display clocks yet.
7. **Keep QUP (`qupv3_id_0/1`) disabled** — probing I2C/SPI killed leftover scanout.
8. **Keep `smem` and `ramoops` disabled** — both hung platform probe / `early_memremap`.
9. **HS-only USB.** `&usb_1_qmpphy { status = "disabled"; }`, `phys = <&usb_1_hsphy>`, `maximum-speed = "high-speed"`, `qcom,select-utmi-as-pipe-clk`. SuperSpeed/QMP 900e’d.
10. **`dwc3-qcom` skips `clk_get`** and maps qscratch itself; `ignore_clocks_and_resets` on `dwc3_core_probe`. ABL left USB clocks on; Linux must not gate them.
11. **Framebuffer DRAM `0x9C000000` is `memblock_reserve` + `nomap`** in `drivers/of/fdt.c`. Without that, the buddy allocator steals the scanout (brief magenta, then glitch).
12. **Hdr2 uncompressed `Image`**, `text_offset=0x80000`. Pack with `scripts/pack_hdr2_boot.sh`. OOS12 ABL ignores boot.img extra_cmdline (keep it short anyway).
13. **Filtered stock dtbo, 10 Qualcomm `dt_table` entries**, msm-id `<0x153 0x20000>`, board-id 18821. `scripts/build_filtered_dtbo.py`. Identity-only (KEEP 0) bounces.
14. **`maxcpus=0`** (CPU0 only). Fine for bring-up.
15. **Do not enable `&wdog`.**

---

## How we got USB (short)

Jump path is ABL hdr2 + matching msm-id + `-@` DTB + filtered dtbo. kexec from Lineage 4.14 always 900e’d; do not treat kexec as working.

On-screen log is the ABL framebuffer at `0x9C000000` (`arch/arm64/kernel/bringup_usb.c`, console `brfb`). That is how we saw initcall progress.

Sequence that mattered:

1. USB-off + dispcc off → kernel stays up, panel glitch/text.
2. `clk_ignore_unused` not in the real cmdline → `clk: Disabling unused clocks` hang. Fixed by baking flags in C + DT bootargs.
3. Embedded DTB dropped ABL initrd addrs → `VFS: Unable to mount root fs on unknown-block(0,0)` (`IMG_20260915_233932_539.jpg`). Fixed with `CONFIG_INITRAMFS_SOURCE`.
4. Built-in g_ether / `fw_devlink=off` / deleting USB interconnects / copying ABL initrd → black screen + 900e ~42–48s. Reverted.
5. Ramdisk **without** writing UDC: `IMG_20260916_000859_578.jpg` then `IMG_20260916_001828_054.jpg` — `UDC='a600000.usb'`, alive loop, panel up.
6. Writing UDC on that kernel without SMMU → 900e ~48s (first gadget DMA).
7. Restore `iommus` SID 0x140, drop `iommu.passthrough`, bind configfs NCM+ACM → **`1d6b:0104`**, ping, `nc :23`.

Photos of the ladder are `IMG_20260915_*.jpg` and `IMG_20260916_*.jpg` in the repo root.

---

## Kernel / ramdisk source to keep

| What | Where |
|---|---|
| DTS | `.work/linux-sm8150/arch/arm64/boot/dts/qcom/sm8150-oneplus-guacamole.dts` |
| Embedded DTB | `arch/arm64/kernel/embedded_dtb.S` (from guacamole.dtb); `setup.c` copies into `embed_dtb_copy` and **ignores ABL FDT** |
| FB console | `arch/arm64/kernel/bringup_usb.c` |
| DWC3 glue | `drivers/usb/dwc3/dwc3-qcom.c` (no clk_get, qscratch map, vbus override) |
| HS PHY | `drivers/phy/qualcomm/phy-qcom-snps-femto-v2.c` (clk/reset skipped, vregs optional) |
| Initramfs tree | `.work/initramfs-root/` → `linux-sm8150/bringup.cpio` |
| Init | `.work/initramfs-root/init` and `scripts/initramfs/init` (keep in sync) |
| Pack | `scripts/pack_hdr2_boot.sh` |
| DTBO filter | `scripts/build_filtered_dtbo.py` |
| USB watch | `scripts/watch_abl_usb.sh` (writes `{SCRATCH}/usb-reachability.log` on `1d6b:0104`) |
| Lineage images | `.work/firmware/payload-out/` |

Still disabled in DTS (on purpose): UFS, GPU, dispcc, QUP, SPMI, cpufreq, PCIe, wdog, remoteprocs, wifi, smem, ramoops, simplefb, USB SS PHY.

---

## Rebuild recipe (when you change kernel or init)

```bash
ROOT=$HOME/Projects/omarchy-oneplus7pro
SRC=$ROOT/.work/linux-sm8150

# ramdisk
( cd $ROOT/.work/initramfs-root && find . | cpio -o -H newc ) > $SRC/bringup.cpio

cd $SRC
make ARCH=arm64 LLVM=1 -j$(nproc) dtbs
# confirm __symbols__ and iommus SID 0x140 still present
rm -f arch/arm64/kernel/embedded_dtb.o arch/arm64/kernel/embedded_dtb.S
make ARCH=arm64 LLVM=1 -j$(nproc) Image

$ROOT/scripts/pack_hdr2_boot.sh \
  $SRC/arch/arm64/boot/Image \
  $ROOT/.work/initramfs.cpio.gz \
  $SRC/arch/arm64/boot/dts/qcom/sm8150-oneplus-guacamole.dtb \
  $ROOT/out/pmos/boot-embed-fbusb.img

python3 $ROOT/scripts/build_filtered_dtbo.py \
  --base $SRC/arch/arm64/boot/dts/qcom/sm8150-oneplus-guacamole.dtb \
  --lineage-dtbo $ROOT/.work/firmware/payload-out/dtbo.img \
  --out $ROOT/out/dtbo-filtered-fbusb.img
# abort if any dtbo entry size is ~300 bytes
```

Save a copy of a known-good `boot-embed-fbusb.img` / `dtbo-filtered-fbusb.img` before replacing them. The 00:21 pair is the USB-working one.

---

## Next (criterion 2): writable Arch + pacman

The ramdisk root is `bin dev etc init proc run sbin sys tmp` — not a distro. UFS nodes are **disabled** (`&ufs_mem_hc` / `&ufs_mem_phy`). `CONFIG_SCSI_UFS_QCOM=y` and `CONFIG_F2FS_FS=y` are already in `.config`.

Tomorrow, in order:

1. **Do not flash a kitchen-sink kernel.** Enable UFS the same way as USB: one change, keep FB console + gadget, photograph last lines if it dies.
2. Mount userdata (f2fs/ext4). There is a sparse `out/pmos/userdata-guacamole.simg` from earlier pmOS work — confirm what is actually on the UFS userdata partition before wiping it.
3. Put an aarch64 Arch (or Arch-compatible) rootfs there; keep this bring-up ramdisk as `/init` that mounts it, or `switch_root`.
4. Real SSH (dropbear/openssh) on `172.16.42.1`. `nc :23` is only bring-up.
5. Then Hyprland + Quickshell (criterion 3). Display is still ABL scanout; Linux dispcc/GPU are off. Headless session is allowed by the plan if processes run.

Do **not** treat Lineage still booting as success. Do **not** treat kexec as working.

---

## Hardware cheat sheet

- Reset: Vol Up + Vol Down + Power
- START menu = healthy ABL / fastboot
- “image destroyed” = ABL unbootable slots (restore Lineage boot/dtbo/vbmeta)
- Logo-only = wedged; key combo again
- Glitched scanlines = 6.17 with a bad panel mode, or leftover ABL scanout without our FB console
- Black + 900e = crashdump (often USB DMA / clocks)
- Magenta flash then glitch = FB DRAM stolen (reserve/nomap missing)

Background notification/call wake design and Android comparison: [plan](background-wake-plan.md).
