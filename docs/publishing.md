# Review and publication

The public repository is `Cube-Oakley/omarchy-mobile`, which began as
`Cube-Oakley/omarchy-oneplus7pro` and was renamed when the Pixel 7 Pro work
joined it. Both devices publish through the one clean line described here.

The published line starts from the September 18 review snapshot: a clean source
tree with no parent commits from the private bring-up history. It is carried by
the local `public` branch, which is GitHub `main`; the original review branch was
merged into `main` and deleted. This keeps old versions of removed files and
identifiers from accompanying a later public push.

The original development history is preserved privately in the development
checkout and an ignored recovery bundle under `out/private/`. It is not the
publication branch. Do not publish all branches/tags or mirror that repository.

Reviewed distribution content includes source, experiment notes, theme assets
and six real screenshots. The personal file-backup helper is excluded. Handset
serials are local configuration; personal Wi-Fi names, LAN addresses and host
paths have been removed. Screenshot review hides Wi-Fi identity and notification
contents before capture, and avoids browser sessions or personal files.

`out/`, `.work/`, firmware extracts, SSH material and device backups stay local,
in each device directory. For the Pixel 7 Pro that also covers the factory images
(`parachute/`), the mainline tree (`mainline/linux/`), the Android property dump
(`docs/getprop.txt`) and the ramdisk tools built from their `.c` sources.
Keep upstream source attribution. The audio notes distinguish electrical tests
from user-confirmed sound and list the remaining protection/persistence checks.

The user approved merging the reviewed snapshot into local `main` and publishing
to `Cube-Oakley/omarchy-oneplus7pro` on September 18. Local `main` retains private
development ancestry; GitHub `main` starts from the clean review snapshot. These
branches have the same published file content but intentionally different history.
Future public updates must descend from the clean public branch. Never merge the
private development ancestry into it or mirror the private repository.

## Mechanics

Two branches, deliberately unrelated. `main` carries the private development
ancestry and pushes to the private development remote (`origin`). `public` descends
only from the clean snapshot and is what GitHub serves.

The `github` remote is pinned to one branch so an ordinary or mirrored push cannot
send private history:

```bash
git config remote.github.push '+refs/heads/public:refs/heads/main'
```

Public edits happen in the ignored `.work/public` worktree, never in the
development checkout:

```bash
cd .work/public
git checkout main -- <paths>       # or: git cherry-pick <sha>
git commit
git push github                    # GitHub main
git push origin public:refs/heads/public   # private-side mirror of the public line
```

The two trees stay byte-identical except for the Pages files below, so check that
diff after each public commit. When `main` moves or renames files, bring the whole
tree across (remove the public branch's tracked files except the Pages files, then
`git checkout main -- .`) rather than listing paths.

GitHub branch Pages can publish only `/` or `/docs`, so the site is served from the
public repository root: a root `index.html` redirects to `plans/`, and a root
`.nojekyll` keeps the hand-written HTML, CSS and JavaScript verbatim. Nothing on
the public branch may name or embed the private host — no LAN addresses, no
internal hostnames, no forge/gitea references.

GitHub redirects a renamed repository's web and git URLs but not its Pages site.
The `Cube-Oakley/Cube-Oakley.github.io` user site therefore serves the old
`/omarchy-oneplus7pro/` paths and redirects them here, mapping files that moved
into `devices/oneplus7pro/`. It is temporary: delete it once the old links no
longer matter. Never create a repository named `omarchy-oneplus7pro` while old
clone URLs may still be in use; that stops GitHub's rename redirect.

## Checks before each public sync

Scan the lines the sync adds (`git diff public main`, excluding the Pages files):

- IPv4 addresses: only public resolvers such as 1.1.1.1 and 8.8.8.8 may appear.
- MAC-style addresses: only obvious test fixtures.
- Digit runs of 14–20 (IMEI, IMSI, ICCID) and phone-number patterns: none.
- The private host, forge names, home paths and email addresses: none.
- The handset's own values, compared on the phone without printing them: the
  fastboot serial (`out/device.serial`), SoC serial, Wi-Fi connection names and
  SSID, network and Bluetooth controller addresses, paired device addresses.
  A paired device's product name, such as a headphone model, is acceptable.
- Binary files: none unless reviewed; camera frames and recordings stay in `out/`.
- Pixel 7 Pro: its serial (`devices/pixel7pro/out/device.serial`) appears nowhere;
  recorded kernel configs and Mesa build records name the workspace
  `@PIXEL_ROOT@`, never a home path; the USB link's 10.77.7.1/10.77.7.2 and its
  locally administered 02:70:07:… gadget addresses are the only addresses.
- Credentials, keys, tokens and PIN/PUK values: none.

## Publication log

- September 18: the reviewed snapshot became GitHub `main`.
- September 23: `public` synced to development `main` (Settings kit, audio and
  microphones, Bluetooth, camera, IPA, CPU scaling, 90 Hz, switcher smoothness)
  after the checks above found nothing private.
- September 23, later: synced again (Omarchy Camera's burst merge and photo
  renderer, libcamera-guacamole 0.7.2-7, the camera theme) after the same
  checks; the only address added is the USB link's 172.16.42.1.
- September 23, night: synced the brightness, vibration, flashlight and alert
  slider controls after the same checks; the only address added is the
  upstream author's, already public in the 7T Pro patches.
- September 24: synced kernel #192 (crash handling) after the same checks,
  which found nothing to remove.
- September 24, later: synced the sensor work (kernel #193, the SLPI
  modules, hexagonrpcd patches, the SEE client and checks) after the same
  checks, which found nothing to remove. No firmware, registry or persist
  data is included; those stay in `out/` and `.work/`.
- September 24, afternoon: synced the sensors' boot start, iio-sensor-proxy
  integration, automatic brightness and the rotate button after the same
  checks, which found nothing to remove.
- September 24, later: synced the proximity fix (a registry patch without
  per-device values) after the same checks, which found nothing to remove.
- September 24, evening: synced kernel #194 (the brightness flicker fix)
  and fading automatic brightness after the same checks, which found
  nothing to remove.
- September 24, night: synced the always-on display, the shell watchdog and
  the switcher and slider fixes after the same checks, which found nothing
  to remove.
- September 24, late: synced the telephoto camera, Omarchy Camera's lens
  switch and the steadier autofocus (libcamera-guacamole 0.7.2-10) after the
  same checks; the only address added is the upstream S5K3M5 driver author's,
  already public in linux-next. No camera frames or photos are included.
- September 25, evening: the repository was renamed from `omarchy-oneplus7pro`
  to `omarchy-mobile`, and `public` synced to development `main`: the device
  workspace layout, the imported Pixel 7 Pro bring-up and the multi-device plan
  pages, after the checks above. The only addresses added are the Pixel USB
  link's 10.77.7.1/10.77.7.2 and its locally administered gadget MACs; the only
  email is the maintainer line of the upstream GS101 ACPM binding, already
  public in mainline. Neither handset's serial and no home path is included.
  The old Pages paths redirect from the user site.

- September 25, later: publishing Pixel v19 CRT screen off/on, the PMIC power-key
  driver, and the in-progress persistent installation. UFS read hashes and an
  ext4 write/remount/readback check pass; root copying is still in progress and
  full suspend is explicitly unfinished. Source/privacy checks found no device
  identifiers, host paths or credentials. Only the documented Pixel USB link
  addresses are added. No firmware, boot images, root archives or raw logs are
  included.

- September 26: publishing the Pixel sparse-root installer, guarded boot_a
  installer, recovery-ADB result and first desktop from internal storage.
  Autonomous boot validation is still in progress at this checkpoint. Reviewed
  source contains no handset identifiers, home paths or credentials; only the
  documented USB link addresses remain. Root archives, font caches, images,
  screenshots and raw device logs stay local.

- September 26, follow-up: boot_a write and direct SHA256 readback passed.
  Published the subsequent restart stop honestly: console visible, USB absent,
  autonomous boot and cross-boot persistence still pending diagnosis. No new
  private data or binary artifacts are included.

- September 26, tracing checkpoint: full boot_a fetch matches G after reset;
  traced RAM boot succeeds and saved files persist. Published the guarded
  header-only tracing helper and this evidence. Traced normal boot still lacks
  USB, so autonomous boot remains unresolved. Device photos and logs stay local.

- September 26, command-line candidate: the new normal-boot photo has no tracing
  output. Published the candidate that embeds persistent-boot parameters before
  early parsing, explicitly marked not hardware-validated. No photo or image
  artifact is included.

- September 26, stopping checkpoint: H passes RAM validation and full boot_a
  write/readback. Its normal reboot is explicitly untested. The user sees the
  persistent-root terminal; cold Quickshell transparency remains unresolved.
  Published installer completion guard and forced-command-line tracing guard.
  No handset identifiers, home paths, credentials, photos or device artifacts
  are included.

- September 30: `public` synced to development `main` after four days of
  private-only work: the OnePlus cellular, SMS, VoLTE and phone apps, and the
  Pixel's native boot, smoothness, Wi-Fi, Bluetooth, AoC audio and sensors,
  modem, LTE data, texts and calls, guarded suspend and step charging, plus
  the updated plan pages. The checks above found only the documented Pixel USB
  link addresses and gadget MACs, example and test addresses, fictional
  555-01xx numbers and public carrier codes. The author address in the AoC and
  cpif patch headers was replaced with a no-reply one. No firmware, NV or
  partition backups, boot images, logs or handset identifiers are included.

- September 30, later: synced the Pixel's light and proximity fix (the USF
  DisplayInfo protocol, documented without the panel's serial) and the charge
  to 100 %, after the same checks, which found nothing to remove.

- September 30, evening: synced the Pixel GPS bring-up (the BCM4776 bridge
  driver, the Bionic runtime shim and the HAL-pipe client), the camera power
  and I2C modules with the first sensor ID, the deep-sleep stages 0-3 (CPU
  hotplug, MCT restart, the audit, the SYS_SLEEP sequence generated from
  Google's GPL flexpmu tables) and the PMIC opmode module, after the same
  checks. No vendor binaries, firmware, camera tables, logs or position data
  are included; the only address added is 127.0.0.1.

- September 30, night: synced the Pixel's real SYS_SLEEP work (storage, buses
  and the modem back after sleep: UFS VCC switching, the S2MPU and SMU
  restore, the USI and pin power-down restore, the display-domain test), the
  coulomb counter and charger-off current measurement, the display hook, and
  the camera capture path: the first raw frames from the ultrawide (with
  autofocus) and the front camera, plus the CSIS wiring and C-PHY status of
  the main and tele cameras. The checks above found nothing to remove. No
  camera tables, camera frames, vendor binaries or logs are included.

- October 1, night: synced the Pixel's camera stack (the V4L2 driver
  `pixel-camera`, the libcamera patches and build notes, the camera app's
  Pixel lens labels, camera domains powered on demand and the camera boot
  start), the touch controller's sleep at screen-off, the ODPM interval fix,
  the PMIC opmode test rails, the Waydroid plan and the updated plan pages,
  after the checks above. No camera tables, sensor firmware, camera frames,
  photos, vendor binaries or logs are included.

- October 1, later that night: synced the Pixel's libcamera tuning files
  (autofocus, colour matrices), the camera plan's status and the note on the
  USB sleep test, after the same checks, which found nothing to remove.


- October 1, afternoon: synced the Pixel's daily SYS_SLEEP:
  - kernel v26 with the GPU's S2MPU restore, pixel-sleep's MISC and CPUCL0
    S2MPU restore, and the touch SPI setup after sleep;
  - the suspend adapter's Wi-Fi- and Bluetooth-off-while-dark policy and its
    shorter wakes, with the shell's shorter background check;
  - the two unplugged standby measurements;
  - the main camera's full-resolution mode and 256 MB CMA;
  - the power and Bluetooth LE plan and roadmap sections.

  Run after the checks above: the only addresses added are the USB gadget's
  locally administered 02:70:07 addresses, and the only email address is an
  upstream maintainer's in the kernel patch. No camera tables, firmware,
  logs or vendor binaries are included.
