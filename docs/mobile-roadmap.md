# Mobile roadmap

Updated from the user's 2026-09-17 requirements. Shared shell, input and service
policy belongs in `overlay/mobile/`; board-specific kernel, firmware, audio
routing and charging support belongs in that phone's `devices/<device>/`
directory (`devices/oneplus7pro/`, `devices/pixel7pro/`).

## Current device work (September 26)

The project is consolidated into `omarchy-mobile`. On the connected Pixel, the
active implementation is persistent native Linux and power-button screen
sleep/wake using the shared CRT animation. The user permits replacing Android
and its userdata; bootloader and calibration partitions remain protected.
See the [Pixel implementation record](../devices/pixel7pro/docs/persistence-power-20260925.md).
CPU/thermal, Mali rendering and 120 Hz scanout already have working checkpoints.
Clean power-button CRT screen off/on is now physically confirmed. It leaves the
CPU awake; interrupt wake and full suspend are separate future milestones.
The Arch root is now installed on ext4 userdata and runs the shared desktop.
The native boot image is installed with direct checksum readback. The first
normal restart has not restored USB and is under diagnosis; autonomous boot
still needs validation. Recovery RAM boot verified that both saved test files
survive reset. Image H embeds the required boot parameters, passes RAM validation and is
installed with full checksum readback; its normal reboot is still untested.
Work stopped for the night with the persistent-root terminal visible. The cold
Quickshell layers remain transparent despite running processes; fix this before
claiming unattended desktop startup.
Storage still uses PWM gear 1: cold startup is slow, so full GS201 UFS support
is a priority alongside charging, suspend and standard SPI touch.
The priorities below continue to guide the OnePlus and shared software.

## Current order

1. Deeper suspend: measure SoC/CPU residency, identify blockers, test individual
   fixes with RTC recovery, then repeat battery measurements. Keep verified
   charging, physical wake and input/network recovery intact. Follow with idle
   policy, background wakes, CPU frequency scaling and true shutdown.
2. **Cellular foundations and audio are phone-critical.** Do not postpone modem
   discovery until a SIM is available: validate read-only QMI/QRTR, SIM detection,
   carrier-configuration prerequisites and the IPA data path first. Data, SMS
   and voice/IMS are separate milestones; registration and end-to-end tests need
   service. Do not equate a running remoteproc with a usable cellular modem.
3. **Audio is critical:** main loudspeaker, call earpiece and microphones.
   Bring up capture/playback and routing independently of Bluetooth or a SIM.
   Verify levels, mute, speaker/earpiece switching and later call audio.
   Add haptic motor support and consistent, configurable gesture feedback.
4. Notification/quick-settings shade in Quickshell: network picker and Wi-Fi
   status, a detailed power/battery widget, brightness and useful toggles.
   Report measured net battery mA, voltage, temperature and available charger
   information; distinguish input current from battery current and limits.
   Include actual notification history/actions, not only quick toggles. Separate
   notification presentation from background delivery/wake. Keep this as an early
   usable milestone rather than waiting for complete telephony or perfect sleep.
5. Input and daily usability: copy/paste, touch text selection/highlighting and
   selection handles, context menus, touch window resizing. Investigate a
   keyboard with swipe typing, autocorrect and suggestions, while retaining
   reliable manual show/hide and avoiding duplicate keyboard surfaces.
6. Bluetooth, GPS and later camera bring-up. Active cellular data, SMS and call
   validation proceeds when service is available, with audio as a call prerequisite.
7. USB-C docking: investigate native DisplayPort output, external monitor
   layout/scaling, USB host keyboard/mouse and USB Ethernet, including simultaneous
   charging. Test a Lenovo USB-C dock when its exact model is known. Verify
   the phone's Type-C/DP routing, role switching, PHY and kernel support first;
   USB peripherals and monitor output are separate milestones. Keep generic
   dock/session behavior in the shared shell and hardware enablement per device.

## Power: off unless needed (October 1)

The user's direction for both phones: keep the phone pleasant while it is in
use, but power down anything that isn't needed, completely. Done well, that can
beat Android, which doesn't always turn hardware off when it could. This is a
plan; each item says what exists today.

**Off means off.** Switching a radio off in the UI powers its chip down: the
driver is unloaded and the chip's enable line dropped, not just disconnected
(`nmcli radio wifi off` and `bluetoothctl power off` leave the firmware loaded
and the chip powered). The shell's toggles call a device hook, as the display
hook works today, and the device adapter does the hardware part. The Pixel
already does this for Wi-Fi and Bluetooth around deep sleep
([suspend notes](../devices/pixel7pro/docs/suspend-20260930.md)), so the
toggles reuse those paths. The same rule covers everything else:
- Sensors run only while something has claimed them; the shell already
  releases rotation and light when the screen goes dark.
- Cameras power on per stream.
- Touch is held in reset while the screen is dark.
- Unused blocks (TPU, codecs, GNSS) stay off.
- A power audit per state (in use, dark, asleep) shows what is still powered
  and what it costs, per rail where the hardware has meters (the Pixel's ODPM).

**Background work without polling.** Apps need to wake for work without each
one waking the phone on its own schedule:
- Apps ask for a wake time.
- The sleep policy (`overlay/mobile/sleep.py`) groups requests into shared
  windows, bringing Wi-Fi up once for all of them, like Android's alarm
  batching.
- An app holds the phone awake only while it works (the existing inhibitor
  files).
- Real-time delivery comes from one push connection shared by all apps
  (UnifiedPush, for example through ntfy), kept over mobile data, which stays
  up in deep sleep. The modem then wakes the phone only when something arrives.
  Whether an incoming mobile-data packet wakes the Pixel from SYS_SLEEP is
  still to be tested.
- The battery page attributes each wake and the time it held the phone up to
  its source.

The earlier
[background work plan](../devices/oneplus7pro/docs/background-wake-plan.md)
has the constraints.

**Always-on display at near-sleep cost.** Today the ambient clock keeps the
panel in its normal mode, and the phone can't sleep while it shows. The target
is how stock Android does it:
- The panel's low-power mode, where it refreshes slowly from its own memory.
- The SoC in deep sleep between clock updates, waking once a minute to draw
  the new time.
- Fully off face down or in a pocket, as now.

The Pixel's panel has this mode, and Google's GPL panel driver has its
commands; keeping the panel powered through SYS_SLEEP is the main work. The
OnePlus panel has no low-power mode (stock's commands for it are empty).

**Bluetooth Low Energy at the OS level.** Phone-as-key accessories (a car's
phone key, for example) need more than classic Bluetooth:
- LE scanning and connections.
- GATT client and server through BlueZ's D-Bus API.
- Advertising.
- LE Secure Connections pairing.
- Above all, BLE that keeps working while the phone sleeps: the chip stays
  powered in its own low-power mode, its host-wake line wakes the phone, and
  controller-side filters (BlueZ's advertisement monitors) wake it only for a
  known device. An app can then hold a connection to a paired accessory, with
  RSSI, through deep sleep.

On the Pixel today the chip is unloaded for deep sleep because SYS_SLEEP cuts
its power. Keeping it alive needs BT_REG_ON held in sleep, the device-wake and
host-wake GPIOs, and the chip's low-power mode. Apps that build on this live
outside this repository.

## Status bar and appearance

- Vertical battery icon, with the charging lightning bolt inside and percentage
  immediately alongside. Add a Wi-Fi connection/signal icon now.
- Later settings: show/hide battery percentage, optional thin battery bar
  along the top edge, and configurable status indicators. Avoid hardcoding
  these preferences into a OnePlus-only shell.
- Continue standard Omarchy theme repository compatibility, installation,
  selection, wallpaper and app-wide theme integration.
- Future fast charging needs the appropriate charger/cable and verified
  negotiation, temperature limits, taper and fault handling. Raising the
  existing conservative limits alone does not implement fast charging.

## Standard user session

Move the compositor, mobile shell and ordinary applications out of the root
bring-up session into a standard user account. Provide sudo for administrative
terminal work and narrowly scoped privileged services for hardware management.
Plan home/config/profile ownership migration, device/seat permissions, the user
D-Bus session, notifications and safe recovery access. Verify boot, networking,
charging, sleep/wake and touch before retiring the root desktop. The separate
`mobile-browser` account and display-socket ACL are a temporary Chromium bridge,
explicitly approved by the user on Sep 18, not the intended final session design.

## Phone/desktop integration

Build a paired service usable between Omarchy phones and Omarchy desktops:
theme selection/sync, clipboard copy/paste sync, phone notifications on the
desktop, and eventually SMS/call integration. Use explicit pairing and
per-feature controls; keep clipboard sharing opt-in. Separate the transport
and service from device adapters and Quickshell presentation. Evaluate existing
protocols/services before inventing a new one; preserve standard Omarchy theme
formats on both ends. Background delivery must fit the sleep/wake design.

See [next session](../devices/oneplus7pro/docs/next-session.md) for checkpoints and recovery instructions,
and [background wake design](../devices/oneplus7pro/docs/background-wake-plan.md) for notification delivery
while asleep. This is a backlog, not a claim of completed hardware support.

- Add a themed font picker to mobile settings; initial user choice is JetBrainsMono Nerd Font for the shell and Kitty. Preserve the choice across theme changes.
