# Next session — hardware and cellular handoffs

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

**September 22 planning checkpoint:** start with the
[remaining hardware implementation plan](hardware-plan-20260922.md) and
[cellular implementation plan](cellular-plan-20260922.md). These include fresh
read-only phone observations, pinned related-device sources, known failures,
implementation stages and physical acceptance tests. Suggested first hardware
work: microphone/application audio, brightness, haptics and Bluetooth, then
SLPI sensors and a rear camera. No implementation was deployed during planning.
Preserve the existing uncommitted shell work. Earlier session notes follow.

## Theme installer follow-up

**CRT screen on/off is in (2026-09-21).** `omarchy-mobile-display` plays a
close before DPMS off and an open after DPMS on. Power-button blank/restore
uses that path. Preview without blanking: `omarchy-mobile-display preview`.

**Theme installer is in.** Settings → Appearance → Install theme. Catalog
grid from https://omarchy.us/themes with search, confirm-to-install, and a
GitHub link that opens in the default browser. Pasted GitHub URLs use the
same helper. Pairing should call `omarchy-mobile-theme-install url|name`,
not a second clone. Ash was installed as a smoke test and the previous
theme (`vantablack`) was restored; Ash stays available in the picker.

Still out of scope: the pairing daemon itself, clipboard sync, and icon packs.

## Goal

An interactive theme installer in Settings → Appearance. Same
`MenuOverlay` language as Theme / Wallpaper / Font. Not a terminal flow.

## Install paths

1. **Catalog (easier on a phone).** Scrollable grid of themes from
   https://omarchy.us/themes. Name under each card. Search bar at the top
   filters by name.
2. **GitHub URL (same as desktop Omarchy).** Paste a repository link and
   install through Omarchy's real theme installer, not a second format.

## Card actions

Tap a catalog theme:

- Confirm dialog: install? Yes runs the standard installer and refreshes
  the Appearance picker.
- Separate control: open that theme's GitHub in the system default browser.

## Installer framework (required even if pairing is later)

One helper / one code path for every install source:

- catalog tap
- pasted GitHub URL
- future integration service ("desktop just installed theme X")

Do not build a mobile-only installer. Call Omarchy's actual theme
installation machinery, show progress/errors, then
`omarchy-mobile-theme sync` so the cover-flow picks up the new palette
and wallpapers.

Leave an explicit hook the pairing daemon can call later: install-by-name
or install-by-url, idempotent, no UI required. When a theme is installed
on the desktop, mobile should notice and run that same helper. Do not
invent a second sync protocol for themes.

Reuse conversion, staging, icon and wallpaper handling from Omarchy
rather than competing with it. See [architecture](../../../docs/mobile-architecture.md).

## Out of scope for the installer

Pairing daemon, clipboard sync, and actually talking to a desktop. Only
the installer UI plus the helper contract those later hooks will use.

---

## CRT screen on/off

**If we can, same morning.** Wake and sleep should not slam the panel
on or off. Opening (wake / display on) and closing (sleep / display off)
should play a CRT-style animation, then the hardware actually follows.

Today `omarchy-mobile-display` (`overlay/mobile/display-power.sh`) just
runs Hyprland DPMS. Power-button blank (plugged in) and real suspend
both go through that. Hook the animation there so every on/off path
gets it — button, idle, and resume — not only one gesture.

Close: play the shutdown (collapse / bright line / fade), *then* DPMS
off / suspend. Open: DPMS on, *then* play the power-up. Do not leave
the panel scanning during "off". Keep it in the portable shell; do not
bake it into the guacamole adapter. If a full shader CRT is too heavy
on this GPU, a simpler analog collapse is still better than instant.

Theme installer stays first. CRT is the second morning item.
---

# Next session — power and hardware

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
README, six screenshots and the clean source snapshot are published: the review
snapshot became the public branch, the user approved publication, and GitHub `main`
plus GitHub Pages now carry it. The review branch was merged into `main` and deleted.
Local private recovery bundle is in ignored `out/private/`. See
[publication](../../../docs/publishing.md).

**Latest user report:** YouTube playback is inaudible and video streaming is
choppy. Application sound is not yet acoustically verified. At the first follow-up
inspection Chromium was closed; PipeWire was healthy, unmuted at 70%, with no
stream and the PCM closed. Capture a playing browser stream next to distinguish
connection/routing failures from the fixed -36 dB attenuation. Do not assume the
volume limit is the cause. Investigate video decoding/acceleration separately;
choppiness alone does not identify a hardware-acceleration defect.

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

Agreed priorities after the 2026-09-17 overnight session. This is a plan,
not a claim that all hardware can be completed in one day.

Latest priorities and full feature wishlist: [mobile roadmap](../../../docs/mobile-roadmap.md).
Latest investigation: [modem foundations and power handoff](modem-foundations-20260917.md).
Handoff and QMP attachment are verified by a temporary read-only module (now
unloaded); do not repeat MSS release. Preserve modem hold while preparing
independent domain/completion diagnostics. Installed qmicli and QRTR libraries;
basic QMI replies, SIM slots empty. `shutting-down` is stable in this pre-online
setup, also documented by related reference. PDC platform query crashed qmicli;
no core captured, modem/Wi-Fi remained healthy. No Set Online/config writes,
carrier activation, or new suspend test. Cellular discovery/audio are brought
forward; the notification panel should not wait for complete telephony.
Latest user observations: [evening battery/latency](evening-observations-20260917.md).
Same recovered boot completed eight suspend cycles, zero PM failures, 3h36m
asleep. Informal 85%→78% over ~3h45m. Event-driven battery/Wi-Fi indicators now
installed and physically confirmed prompt. Ninth sleep/wake passed (23.175s,
zero failures), charging current positive ~2s after enable; Wi-Fi association
~5s after resume followed by a later AP switch/primary gap. Preserve distinction
between icon state and connectivity; investigate NM resume/roaming separately.
Trace saved under `out/evening-status/`; temporary recorder/children stopped.
USB-C dock/DisplayPort/host input/Ethernet added to future roadmap.
Latest test: [MSS-only diagnostic](mss-handoff-test-20260917.md), **failed**.
Releasing MSS alone reproduced modem watchdog/Wi-Fi resume -110 after 31.508s
asleep; CX/MX/MMCX/XO stayed held. Restore request timed out and its error
recovery blocked in RPMh. Do not repeat this mode or load CAMCC; CLI retired.
SysRq-u confirmed ext4 read-only, then SysRq-b was required after normal reboot
also blocked. Automatic recovery now verified: new boot
`be4da80f-2acc-4b4b-bea0-7d4667d62123`, #185 control 0, eight CPUs,
modem/Wi-Fi HTTPS/input/charging/NTP healthy; no trial or diagnostic modules
remain running. Retired CLI installed/refusal verified. Investigate firmware
handoff and RPMh completion before another rail release. Trial evidence:
`out/sleep-stats/mss-*`; checkpoint `out/checkpoints/20260917-mss-failure-recovered/`.
Rollback: `scripts/flash_mss_handoff_test.sh rollback` restores frozen #184.
Continue deeper suspend diagnostics, then the notification/quick-settings shade.
See [current residency investigation](deeper-suspend-20260917.md): the initial
91.61-second trial passed with physical touch/network/charging recovery, but
deep SoC counters stayed zero. A missing CAMCC driver was blocking power-domain
handoff; its temporary stock module now releases the maximum CX/MX sleep votes.
The CAMCC trial in `/root/suspend-test/camcc-residency.log` **failed radio
resume** (MPSS watchdog/Wi-Fi -110); do not promote CAMCC. Recovery reboot into
unchanged native5 is verified: modem, Wi-Fi HTTPS, desktop/input and charging
all auto-started; approved key-ack modules passed their first boot activation.
Investigate radio power handoff dependencies before another suspend experiment.
No persistent CAMCC install or new suspend trial is armed.
The compact battery and Wi-Fi
status indicators have been installed without a reboot.
Main speaker, call earpiece, microphones and haptics are explicit requirements;
audio should not wait for Bluetooth or cellular service. Keyboard improvements,
touch text selection and Omarchy phone/desktop integration are recorded there.

## Starting point

- **Baseline native5 #184 automatic boot verified (now rollback).** Desktop,
  eight CPUs, touch/power key, hidden keyboard, charging, test-network Wi-Fi and NTP
  start without bootstrap repair or host clock adjustment. RTC parent binding
  and a five-second awake alarm passed. Evidence: `out/native5-reboot/`.
  Frozen image: `out/checkpoints/20260917-native5-test/`; rollback through
  `scripts/flash_suspend_desktop.sh rollback` restores native4 on slot B.
  **Unplugged explicit sleep now passed:** 12.15 seconds in s2idle, user-reported
  successful physical wake, zero PM failures, input rediscovery, Wi-Fi HTTPS
  and charging on reconnection. Verified checkpoint:
  `out/checkpoints/20260917-native5-suspend-verified/`. The one-shot helper
  finished; no future unplug-triggered trial remains armed.
  **Power-button policy physically verified.** Two unplugged sleep/wake cycles
  passed (10.32 and 7.10 seconds), without PM failures or immediate re-sleep.
  Touch, Wi-Fi HTTPS, USB and charging recovered. The temporary backup alarm
  override has been removed and no RTC alarm is armed. Plugged-in Power only
  blanks/restores the display. Checkpoint:
  `out/checkpoints/20260917-power-button-verified/`.
  **Wi-Fi key-ack fix passed the full suspend test**, and remains live from
  `/root/wifi-key-ack-test/`. All PM failures remain zero; network, input and
  charging recover. Awake key teardown is 0.39 s versus 3.40 s originally.
  The user explicitly approved persistent promotion after the initial approval
  review block; `scripts/phone-install-wifi-key-ack.py` completed successfully.
  The tested pair is installed and checksummed; the complete original module
  directory is retained at the sibling `.before-key-ack` path. Next-boot
  activation was subsequently verified during the CAMCC trial recovery reboot;
  see `out/sleep-stats/recovered-network.log`.
  See [Wi-Fi findings](wifi-key-ack-20260917.md).
  **Battery comparison completed:** 131 mA screen-off awake, 83 mA in the
  suspend interval, 301 seconds actually asleep. Approx. 37% lower in one short
  trial, with ~12 mA gauge resolution. [Results](idle-measurement-20260917.md).
  Next: longer repeats, power-domain/clock work, and background wake design.
  Current suspend disconnects Wi-Fi; live push notifications while asleep are
  not implemented. Calls/SMS require separate modem/telephony integration.
  Check the persistent power-button policy after the next routine reboot.
  Automatic idle and true shutdown remain unverified.

- **Previous charging verification:** native3 was flashed and passed its first automatic
  boot, independent charger-register readback and driver stop/restart checks.
  Cable/power-button test completed; USB and charging resumed automatically,
  with charge accumulation and unchanged limits verified. The phone is plugged
  in and reachable over USB. Evidence is saved in `out/native3-runtime/` and
  `out/checkpoints/20260917-native3-verified/`. Onboard battery recording runs
  for 30 minutes in `/root/native3-validation/battery.log`.
  `scripts/flash_power_desktop.sh rollback` restores native2 on slot B.
  See [charging results](charging-20260917.md#native3-first-boot-and-driver-cleanup-verified).
  Agreed order: persistent charging, Wi-Fi, then deeper suspend/power management.

- Booted kernel: native5 #184, slot B (native4 retained as rollback). Automatic native DSI 60 Hz desktop,
  eight CPUs, Adreno rendering, touch,
  persistent Arch and USB SSH work. Slot A is unchanged by recent work.
- Shared mobile shell: `overlay/mobile/`; hardware adapter: `devices/oneplus7pro/`.
- Latest keyboard checkpoint: `out/checkpoints/20260917-keyboard-focus/`.
  Earlier startup fix: `out/checkpoints/20260917-keyboard-boot/`.
  Kitty patch/binary checkpoint: `out/checkpoints/20260917-gestures/`.
  Native boot/evidence: `out/checkpoints/20260917-native2-verified/`.
  Simpledrm rollback: `out/checkpoints/20260916-touch1/`.
- Start with the phone booted into Arch over USB. After a reboot, use
  `bash scripts/phone-ssh.sh` directly first. The USB bootstrap helper changes
  services/time and must not be used before assessing automatic startup.
  Request physical fastboot only once a concrete kernel test is built and ready.
- Confirm current configs and capture logs before changes. Preserve existing
  user windows/work and both recovery checkpoints.
- **Shutdown is not working:** the latest outer-initramfs `busybox poweroff -f`
  attempt rebooted the handset. Storage was synced and remounted read-only.
  USB disappearance alone must not be called successful power-off. See
  [shutdown failure](shutdown-20260917.md). Diagnose this early alongside the
  kernel work, and provide a phone-accessible shutdown command/UI once verified.
- **Keyboard boot regression fixed:** missing `/dev/fd` broke Bash process
  substitution before USB SSH setup. The helper no longer depends on it;
  automatic startup and shell show passed a full reboot test before host
  bootstrap. The user also confirmed the keyboard/gestures work. See
  [keyboard fix](keyboard-boot-fix-20260917.md).
- Linux USB intermittently failed to enumerate; fastboot and two subsequent
  Linux boots worked. Previous boot logs are saved, but the cause is unresolved.

## 1. Native display and frame pacing

Native2 #179 now boots the native DSI display and accelerated mobile desktop
automatically. The embedded panel graph repair works without live intervention.
One full boot passed with eight CPUs, USB SSH, touch and keyboard available.
[Panel audit, results and rollback](native-display-work-20260917.md).
The user cannot yet judge whether the UI is noticeably smoother; 90 Hz is not
implemented. Focus-change flashing is reproduced as keyboard auto hide/show
releasing and reclaiming its reserved space. The installed fix passes controlled
focus and visibility tests; the user confirmed the result looks good after reboot.

Read [timing measurements](display-performance-20260917.md),
[CPU/GPU history](cpu-gpu-work-20260916.md), `progress.md`, and the historical
DPU entries in `status.md`. Reconcile the current source with previous attempts
before building. Earlier DPU modesetting produced corruption; do not repeat
the incomplete uncompressed/stub-panel approach without resolving its cause.

Trace the actual guacamole panel initialization, command-mode DSI, DSC
configuration, TE/vblank timing and MSM display-buffer mapping. Recheck current
primary upstream/related-device work; shared SM8150 hardware does not make a
different phone's panel configuration interchangeable.

First target: clean native scanout at 60 Hz, reliable frame delivery and a
clean boot/restart while preserving USB, GPU, eight cores and touch. Then
validate and expose 90 Hz if the panel/driver path supports it correctly.
Measure frame pacing during real drawer gestures and terminal scrolling.
The existing synthetic 60 Hz mode and ~60 framebuffer updates/second do not
prove synchronized physical presentation.

Build isolated, recoverable tests; record each result and checkpoint progress.
If blocked, write down the precise failing stage and next experiment before
moving to independent userspace tasks.

## 2. Make ordinary pacman work

Native2 enables Landlock (ABI 7, active LSM verified). Ordinary pacman
repository sync to a temporary database passed without the sandbox bypass.
The check passed again after native2 boot; physical terminal usage is ready
for the user to try. Preserve
signature verification and download-user protections. The old touch1 rollback
still requires the documented filesystem-sandbox flag. USB SSH starts in the
native init; the host helper still restores routing/DNS and corrects the clock.

## 3. Theme wallpapers — implemented, full Omarchy integration remains

Appearance now previews/cycles the 92 bundled standard backgrounds and applies
wallpapers with theme changes. The bridge reads current and legacy Omarchy
state, supports user backgrounds, and stores minimal-Arch selections per theme.
Live switching and shell-restart persistence passed, with existing windows
preserved. See [wallpaper results](wallpaper-switching-20260917.md).
The user confirmed visible wallpapers and switching. Follow-up fixes remove
poll-driven button dimming and align the early boot wallpaper with the saved
choice; the updated shell restart test passes. Check those visually and on
the next routine phone reboot. Full theme repository installation,
GTK/icons and app-wide Omarchy integration remain separate work; retain the
standard format and Archwave compatibility target.

## 4. Directly manipulated drawers and overview refinement

Installed and ready for physical validation. See [touch drawer results](touch-drawers-20260917.md)
and `out/checkpoints/20260917-drawer-pull/` (latest) or
`out/checkpoints/20260917-touch-drawers/` (initial). The first implementation includes
finger tracking, release settling, header pull-down, selected-workspace tapping,
preview swipe-to-close, and preview hold/drop to move, swap or close. Automated
input tests and live compositor action tests passed. The user confirmed
workspace entry/moves; the follow-up keeps the held card large/translucent,
keeps the drawer surface mapped to avoid reopening flashes, and guards wvkbd
against nested show calls that can orphan a keyboard surface. The user accepted
those three refinements. The latest update adds downward dismissal from drawer
content at scroll-top and Overview previews; the user confirmed it works great.
Do not treat automated tests as a
substitute for physical gesture feedback. No reboot/flash was needed.


- Preserve bottom-left apps, bottom-center overview, bottom-right keyboard,
  and bottom-right tap to hide the keyboard.
- App drawer and overview position should track the finger throughout the
  drag. Releasing settles open or closed based on distance and velocity;
  support a short canceled pull, a deliberate slow drag and a quick fling.
  Allow dragging closed and interrupting/reversing an animation smoothly.
- Coordinate input capture with the changing visible area so touches do not
  accidentally activate apps behind a partially open drawer. Tune using the
  physical phone, including keyboard-visible cases.
- Remove **Go** from the workspace overview. Tapping an unselected workspace
  previews its apps; tapping that same, already-selected workspace enters it.
  This is two ordinary taps, not a time-sensitive double-tap gesture.
- Tapping an app preview still enters its workspace and focuses that exact
  window. Empty workspaces remain reachable through the selected-tile tap.
- Preserve standard theme colors, large touch targets and restrained motion.
  Smoothness is a measured goal, not a promise of zero dropped frames.

### Touch window management ideas to prototype

The user wants easier closing, rearranging and resizing without relying on
small buttons or a mouse. Explore these interactions after the drawer work:

- **Hold, then drag to arrange:** a deliberate press-and-hold enters window
  management, with clear visual feedback. Dragging can swap tiled windows or
  place a window into a supported tiling position. Show the proposed placement
  before release; determine the correct Hyprland move/swap behavior rather
  than assuming ordinary left-button dragging implements it.
- **Touch resizing:** provide an explicit resize affordance or mode after
  the hold, equivalent in purpose to the compositor's mouse resize action.
  The exact gesture is undecided; explore visible edge/corner handles or a
  move/resize choice. Preserve a separate way to invoke app context menus
  (right-click behavior), which is not the same as resizing a window.
- **Drag to close:** show a small trash icon at the physical bottom only while
  managing a window. Give it a generous invisible touch target, highlight it
  when armed, and close only on release inside it. Dragging away cancels the
  close. Use the compositor's normal close request so an app can present its
  unsaved-work dialog; do not force-kill the process or assume a shortcut.
- **Overview swipe-to-close:** dragging an app preview upward should follow
  the finger and close that window when released past a deliberate distance
  or velocity threshold. Short pulls snap back. Distinguish this vertical
  action from horizontal card browsing and ordinary tap-to-focus.
- **Gesture ownership:** do not globally steal app long-press, scrolling,
  terminal selection, context-menu or drag-and-drop interactions. Prototype
  hold-to-manage on a dedicated window affordance or in the overview first;
  evaluate any direct-on-window gesture on the phone before choosing it.
- Test cancellation, accidental closes, multiple tiled windows, empty spaces,
  keyboard-visible layouts, and closing the last window. Keep an accessible
  button/menu alternative while the gestures are being tuned.

The overview-based hold/drop and swipe-to-close subset is now installed.
Direct-on-window dragging, touch resizing, explicit tiling placement and
context-menu gestures remain proposals. Validate and refine the current
physical interactions before expanding gesture ownership.

## 5. Hardware queue after the display/userspace milestones

Current agreed order prioritizes power-button/shutdown/battery investigation
before Wi-Fi. See [active power test and findings](power-investigation-20260917.md):
the temporary power-key overlay is loaded on native2, and physical short-press
events are verified. Automated display off/on and the physical button test
passed; the user confirmed touch after return. The shared display binding is
saved without the test wake timer. The kernel overlay still needs loading
after reboot; no new image was flashed. Suspend and shutdown are not verified.
Latest steering: prioritize **persistent charging before further suspend or
unplugged testing**. Live battery readout and USB reconnect passed; a bounded
charging trial produces about 470 mA. Correct the PM8150b charger support and
validate startup/protection/reconnect behavior before treating it as unattended
charging support. The timed trial automatically restored disabled charging.
See [charger defects, evidence and next build](charging-20260917.md) and
[battery results](battery-gauge-20260917.md).
The broader hardware list below remains the backlog.

1. Wi-Fi: firmware/driver bring-up, connect/reconnect, package downloads and
   persistent network configuration. Keep USB recovery working.
   Add a themed Quickshell network picker to the future notification tray:
   scan, signal/security indicators, connect/password entry, disconnect,
   saved networks and clear connection/error feedback. Use NetworkManager
   underneath so the UI stays portable. User requested this after selecting
   their first Wi-Fi network during native4 testing.
2. Battery and charging: establish real capacity/status/voltage/current and
   charging behavior; then expose a truthful percentage/charging indicator.
   Future: investigate the handset's vendor fast-charge protocol and required
   compatible charger/cable/controller support. Keep the verified conservative
   limits until negotiation, temperature protection, battery limits, taper,
   watchdog/fault handling and disconnect fallback are validated. Do not enable
   fast charging merely by raising current limits.
   Future notification tray: show actual signed battery current in mA (net
   charging/discharging), voltage, temperature, charge/power and charger type
   where measured reliably. Distinguish battery current from USB input current
   and configured limits; label unavailable readings instead of estimating them
   as measurements. Keep this UI portable via standard power_supply properties.
3. Sleep/wake: distinguish display blanking from actual suspend. Bring up
   brightness, power-button wake, screen locking and repeated resume tests;
   check idle draw and restore display/touch/network after wake.
   A short power-button press should sleep/lock the phone and wake it again.
   Explore a themed power menu on a deliberate long press, with shutdown and
   restart actions; keep hardware emergency-reset behavior available. Confirm
   real key events and wake capability before choosing userspace bindings.
   Include real shutdown: trace PMIC/PS_HOLD/firmware handling and possible
   USB-powered restart behavior, then verify the phone stays off rather than
   merely losing USB temporarily. Keep shutdown distinct from suspend.
   **Battery life is a core requirement:** bring up CPU frequency scaling,
   CPU/cluster idle residency, GPU and peripheral runtime suspend, and suitable
   wake sources. Audit the current `clk_ignore_unused`, `pd_ignore_unused`,
   and `regulator_ignore_unused` bring-up settings and retire them individually
   after their dependencies work. The current kernel also defaults all three
   flags to true in `drivers/clk/clk.c`, `drivers/pmdomain/core.c`, and
   `drivers/regulator/core.c`; removing boot arguments alone is insufficient.
   Avoid polling/rendering when the screen is
   off. Measure unplugged screen-on and screen-off consumption and overnight
   drain, with repeated wake tests; do not infer good battery life from an
   idle governor name or a blank display. Keep thermal/charging protections.
   Baseline on touch1: `psci_idle` is registered and only `s2idle` is exposed;
   no CPU-frequency policies or power-supply devices were listed, and the only
   input device is the touchscreen (no power-key events yet). Evidence:
   `out/native-display-test/phone-baseline.log`.
   CPU0 exposes WFI and `cpu-sleep-0-0`, both with nonzero entry counts;
   that confirms some CPU idle activity, not deep system suspend or low draw.
4. Bluetooth and audio: pairing/reconnect, then usable audio routing. Speaker,
   earpiece and microphone support also matter for later calls.
5. GPS/location: identify the required firmware/services and verify a fix.
6. Cameras: separate sensor detection, ISP/media pipeline and image capture;
   handle the pop-up camera mechanism as its own device-specific task. Treat
   this as a larger investigation, not a quick package installation.
7. Cellular: bring up modem/services and inspect available capabilities.
   Live data, SMS and call validation waits for a suitable SIM/service; there
   is currently no service on this device. Data and voice are separate goals.

Before depending on desktop service management or robust suspend, address the
current BusyBox-initramfs/chroot boot arrangement and plan a normal persistent
Arch service lifecycle. Keep shared shell policy separate from board-specific
firmware, kernel, charging and radio work throughout.

## Carry-forward limitations

- Native panel control and automatic boot pass at 60 Hz; sustained frame pacing
  and repeated-boot reliability remain to be measured. 90 Hz is not implemented.
- Kitty scrolling uses a version-matched 0.48.2 Wayland patch; an upgrade will
  replace it. Preserve the patch/build recipe and plan proper packaging or
  upstream support.
- Full reboot keyboard startup and shell show now pass, with user-confirmed
  keyboard/gesture operation after the fix.
- Full Omarchy repository theme installation is not implemented yet.

See [background wake/Android comparison](background-wake-plan.md) for the user’s notification and cellular requirements.
