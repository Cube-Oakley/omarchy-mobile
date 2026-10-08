# Standard user session (October 2)

**Status (October 2, evening):** done through step 4, on the phone.
- The desktop, its helpers and PipeWire run as `cube` (display name Cube).
- The system services stay root's.
- Checked as `cube`: the lock screen with the fingerprint service, Phone and
  Messages (LTE and IMS registered), audio, Bluetooth, sensors, all four
  cameras, Wi-Fi control, brightness, the charge limit, screen changes and
  sleep through `sudo`.
- Waiting on the user's hands: touch, the power key, fingerprint unlock,
  calls.
- Left to do:
  - `sudo` with the lock screen's PIN or password, checked by the
    fingerprint service's Gatekeeper: built, to install (below).
  - Retiring the root session's leftovers.

What the boots taught:
- Under the boot script's `set -euo pipefail`, a missing optional file in a
  `$( … | … )` stops the whole boot.
- A shell function started with `&` runs in a subshell, so `$!` is not the
  program's PID.
- A service's socket is shared only after the service has replaced any stale
  one.
- Quickshell's Socket never retries a failed connection: a retry is a new
  Socket.
- `alarm` (uid 1000) was not deleted but became `android-system`: locked,
  with no home. The fingerprint service's Android programs run as uid 1000
  and Trusty's storage files belong to it, so no person's account may ever
  get that ID.

The Pixel's desktop runs as root: `/ourinit` runs `pixel-boot.sh` in the Arch
root, which starts the system services and then
`/root/pixel-gpu-session-start.sh`. That starts Hyprland
(`--i-am-really-stupid`), the session bus, PipeWire, the shell, the keyboard
and kitty, all as root. Every app, web page and script the user runs can
therefore write any file and any hardware register. This plan moves the
desktop to an ordinary account and leaves root only to the system services
and a few narrow entry points.

## What stays root

The boot script and the system services it starts keep running as root:
- modem (`manager.py`) and the fingerprint service;
- sensors (`pixel-sensor-proxy`), cameras (module loads only), audio
  bring-up, Wi-Fi and Bluetooth loads;
- `seatd`, `udevd`, the system bus, NetworkManager, BlueZ, sshd;
- polkitd (new: NetworkManager needs it to answer a user).

Root SSH stays as it is, for development and recovery: key only, USB and
Wi-Fi, `AllowUsers root`.

## What moves to the user

- Hyprland, through seatd (the `seat` group).
- The session bus and PipeWire, WirePlumber and pipewire-pulse.
- The shell and its helpers, the keyboard, kitty, and every app.

The home directory gets root's desktop data: `~/.config` (prefs, Hyprland,
quickshell, themes), `~/.local/share` (contacts, messages, calendars,
clock, app grants, themes), `~/.local/state`, and the parts of `~/.cache`
worth keeping. Root's copies stay as a backup until the switch is proven.

## Root entry points (sudo, no password, exact programs)

The device adapter is already a set of thin wrappers. Each becomes
`exec sudo -n /usr/local/sbin/<program> "$@"`. A sudoers drop-in allows
exactly these programs, and each checks its own arguments, as they do today:

| Wrapper | Program | Does |
|---|---|---|
| display-hook | pixel-display-hook | CPUs 4–7, touch modes, the panel's low-power mode, modem screen state |
| suspend | pixel-suspend | SYS_SLEEP (unloads Wi-Fi and Bluetooth, RTC alarm) |
| reboot | pixel-reboot | Restart, bootloader |
| (power menu) | poweroff, through pixel-reboot | Power off |

`pixel-suspend` and `pixel-display-hook` hard-code `/root` and
`/run/user/0` in places (the display script's path, the ambient-touch
flag). These become the user's paths, read from one file
(`/etc/omarchy-mobile/session`), since `sudo` resets the environment.

## sudo with the lock screen's PIN or password

Built on October 2 (not installed yet): `devices/pixel7pro/session/`.
- **The check:** `/etc/pam.d/sudo` (`pam-sudo`) first runs
  `lock-secret-auth.py` through `pam_exec`, with the answer on its standard
  input. For the session's person only, the helper asks the fingerprint
  service, which checks the answer with Trusty's Gatekeeper (`verify`) and
  throttles wrong guesses. No password hash is kept on the root.
- **Refusals:**
  - with no PIN or password set (the service would accept anything);
  - when the socket is not the service's: it must be in a root-owned folder
    nobody else can write, and its server must be root or uid 1000.
- **Fallback:** anything else, or a wrong answer, goes on to `system-auth`
  with the same answer. The account's password field must stay locked,
  because `pam_unix`'s `nullok` would take an empty one; the installer
  checks it.
- **Prompt:** the sudoers drop-in sets `[sudo] PIN or password for cube:`.

`install-lock-sudo.sh` (run from the host) installs the helper, the drop-in
and the PAM file, and keeps the old files in `/root/*.before-lock`. Root SSH
and the NOPASSWD entry points are unaffected.

## Permissions instead of root

- **Groups:** `video` (DRM, cameras, media), `render`, `audio`, `input`
  (haptics, the alert slider), `seat`, `wheel` (sudo).
- **udev rules:** the panel backlight's `brightness`, the torch LEDs, the
  battery's charge limit, and `/dev/dma_heap/system` (libcamera) writable
  by `video`.
- **Sockets:** the fingerprint service (`/run/omarchy-fingerprint`) and
  telephony (`/run/omarchy-mobile-telephony`) owned by the user's group,
  mode 0660. Both services still check what they are asked.
- **polkit:** NetworkManager's actions allowed for `wheel`, so Settings can
  scan and connect. BlueZ and the sensor service already allow any user.

## The switch

`/etc/omarchy-mobile/session` names the user. `pixel-gpu-session-start.sh`
does the following:
- Makes `/run/user/<uid>` (0700, the user's).
- Starts the session bus and PipeWire as the user.
- Starts Hyprland, the shell, the keyboard and kitty with `setpriv`, with
  the user's environment.

Without the file it starts the root session as today. That is a one-file
rollback over root SSH.

## Steps

1. **Prepare, nothing switched:** install `sudo`, create the user, set up
   groups, udev rules, polkit, sudoers, and copy the home data. The root
   session keeps running.
2. **Scripts:** user-aware paths in `pixel-suspend`, `pixel-display-hook`
   and the session start; the adapter wrappers through `sudo`.
3. **Switch and boot:** write the session file and reboot, with USB root SSH
   for recovery.
4. **Check everything:** display, touch, the lock and fingerprint, the power
   key, the always-on display, sleep and wake, alarms and reminders, calls
   and texts, audio, Bluetooth, Wi-Fi, cameras, rotation and brightness,
   charging, notifications, Settings pages.
5. **Clean up:** remove the `alarm` account (Arch ARM's default, with a
   known password), retire the root session's leftovers, and update the
   docs.
