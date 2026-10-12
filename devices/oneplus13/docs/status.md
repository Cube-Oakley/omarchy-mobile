# OnePlus 13 status — October 12, 2026

## Latest checkpoint

The installed native kernel now boots directly from `boot_a` into the ext4 Arch
root, Adreno desktop and shared mobile shell. Repeated normal boots and clean
return to fastboot passed. Full boot partition readback matches the tested
image; seven other active boot-related partitions match the original baseline.
The native boot-success helper updated only `boot_a`'s success bit in both GPT
copies and their dependent CRCs. Fastboot independently reports slot A
successful, bootable, with six retries remaining after installing Bluetooth.
Clean power-off followed by physical Power-button boot also passed with Wi-Fi
enabled: the owner confirmed the desktop and idle/wake response, and automatic
reconnect plus HTTPS passed in the new boot.
Optical display checks and full hardware parity remain pending.

Normal boot initially failed because the USB2-only profile did not select
UTMI as the controller's PIPE clock. Fastboot's retained clock state masked
that omission during RAM tests. Early persistent logs proved the kernel had
started, mounted the native root and initialized touch/display/ADSP, then
failed the session's USB-network prerequisite after a DWC3 reset timeout.
`qcom,select-utmi-as-pipe-clk` fixes USB startup; USB SSH now starts independently
of the local desktop. No AVB verification-policy change was needed. A proposed
VBMeta flag test was rejected by automatic review, later explicitly approved
by the owner, and abandoned after the USB diagnosis; it was never flashed.

Startup also waits for all six UFS LUNs before setting disks and partitions
read-only, then opens only the native userdata root for writes. It mounts the
verified root before display startup and saves early kernel/startup logs.
Actual protection flags and saved logs were checked on-device. The installed
image is `out/native-persistent22-audio/boot-persistent.img`, SHA256
`31c8b49bf190b10c5f3b3fcf33814f88e6037fa12deaee5a60601c94daf12c11`
(audio, panel control, Bluetooth in the base tree); see [recovery](recovery.md)
for the earlier images.
The earlier 120 Hz image remains a verified rollback artifact (see recovery).
The diagnostic and USB revisions retain the probe 20 module ABI.

Do not format data in Android recovery: userdata contains the installed native
root. No physical reset is currently required. See [recovery](recovery.md) for
stock restoration artifacts and the current boot procedure.

Additional verified progress:

- Audio (October 12, image `native-persistent22-audio`, installed and slot A
  marked successful): AudioReach on the stock ADSP, WCD9395 microphones over
  SoundWire, two AW88461 smart amps (new driver) on secondary MI2S. PipeWire
  starts with the session and offers Speakers and Internal microphone through
  the board's UCM profile. Owner heard left (earpiece) and right (bottom) test
  tones and PipeWire playback; capture verified. No speaker protection yet, so
  the amps stay 6 dB under stock. See [audio](audio.md).

- Panel control over DSI0 command DMA (October 12, image
  `native-persistent21-panel`, installed and slot A marked successful):
  `/sys/class/backlight/oneplus13-panel` drives DCS brightness from the shell's
  slider, and screen off now puts the panel to sleep (display off + sleep in,
  TE stops), waking with the stock 120 Hz on sequence. DCS reads work (power
  mode, brightness). Thirteen off/on cycles, zero faults; owner confirmed the
  picture after wake. Rail power-down, AOD and HBM remain. See [panel](panel.md).

- Vibration motor (October 12): PMIH0108 HAP530_HV haptics as an FF_RUMBLE
  input device (`kernel/haptics/oneplus13-haptics.c`), loaded at session
  start from `/opt/oneplus13-haptics`. The bootloader leaves the block enabled
  and tuned (Vmax 2.4 V, LRA period, brake); the driver only uses Qualcomm's
  direct-play sequence (clear faults, ignore SoundWire, amplitude, play with
  brake, wait for brake completion) and stops any effect after 5 s. Registers
  showed a clean play (power interface active, no faults); the owner felt short,
  medium and long pulses at normal strength. FIFO/pattern effects, closed-loop
  calibration and boost tuning from the PMIC's SDAM are not used.

- Experimental Bluetooth now starts on normal boot using the exact stock
  Ganges patch/NVM, independent votes on shared Wi-Fi rails and a 3 Mbaud UART.
  LE discovery found 27 nearby advertisers; BlueZ/mobile-shell scanning and
  three off/on cycles passed with Wi-Fi HTTPS. The complete Bluetooth stack
  was rebuilt with LE/RFCOMM/BNEP enabled. Startup validates hashes and retains
  a failure marker if interrupted. Pairing, accessory data/audio, range and
  suspend remain pending; a private test address is used until the factory
  retrieval path is implemented. See [Bluetooth bring-up](bluetooth.md).

- The installed PCIe profile powers the stock-matched six Peach rails
  and enumerates Wi-Fi `17cb:110e` on a Gen1 x2 link. DWC internal MSI routing
  resolves the GIC ITS firmware-handshake timeout. Stock firmware reaches MHI
  mission mode and QMI capabilities, accepts the exact US board data, regulatory
  data, PHY/AUX firmware and QDSS configuration, and signals firmware ready.
  Stock calibration completes in about 8.4 seconds; an MHI restart retaining
  its RAM buffers allows station initialization. Passive scanning sees 2.4,
  5 and 6 GHz access points. 5 GHz WPA2, DHCP, DNS, HTTPS, reconnect and
  screen-off/wake traffic passed. A 32 MiB transfer each way verified both
  hashes at approximately 93/82 Mbps on the test path. Normal boot now starts
  Wi-Fi and reconnects the owner's saved network automatically. Startup
  explicitly creates the system bus before asynchronous hardware helpers.
  A fresh boot has only the expected out-of-tree module taint, with no kernel
  WARN/OOPS. Regulatory update timeouts remain, with the stock US domain intact.
  The firmware-only removal path now skips uninitialized data-path cleanup and
  powers down MHI even before CE initialization. Two firmware-ready/unload cycles
  passed without stale devices or new faults. Connected station removal also
  passed. Native startup verifies module/firmware hashes and skips a previous
  interrupted initialization. Factory MAC retrieval, 2.4/6 GHz association,
  AP mode, suspend, power saving and sustained reliability remain unvalidated.
  See [wireless bring-up](wireless.md).

- Owner verified responsive desktop and power-key wake while unplugged. The
  local log captured 113 seconds with USB offline, one continuous boot ID,
  approximately 5 V after reconnection, and then battery status Charging.
  USB SSH reconnects. This validates transitions, not a full charging cycle.

- The installed compositor presents at approximately 120.7 fps after a normal
  boot (median interval 8,276 µs). Early notification after TE/address latch
  overlaps rendering with the current DMA, while both framebuffers stay held
  through hardware completion. The owner confirms significantly smoother
  interaction and slight flicker on portrait/landscape changes. Four rotations
  and three screen-off/wake cycles passed with zero transfer faults. The
  earlier 60 fps image remains saved for rollback. See [display timing](display-timing.md).

- The owner confirmed both volume indicators and power-key screen-off/wake
  with the CRT animation. Alert slider and suspend wake remain unvalidated.

- The PMK8550 RTC counter advanced by 110 seconds across the physical power
  cycle, matching host elapsed time. `rtc-clock.py` restores wall time at
  session startup from a native-root anchor; another normal reboot confirmed
  the correct date before desktop startup. Hardware RTC time, alarms and
  calibration partitions are unchanged. See [RTC setup](rtc.md).

- Probe 16: all eight cores enter CPU retention, with zero rejected entries.
  The final 10-second sample had 58–99% per-core retention residency. Cluster
  and APSS power collapse remain disabled; this is not suspend support.
- Probe 17: screen-off DMA selects a dedicated black buffer instead of the old
  boot image; enable/disable/enable passed with zero transfer failures. True
  panel power-down and optical validation remain pending.
- The CPH2655 battery profile exposes only verified read-only telemetry.
  Unreliable optional power/time/full-capacity fields and charge setters are
  excluded. USB telemetry works; a charging cycle has not been validated.
- Stock-matched ADSP FastRPC context 5, its IOMMU streams, dedicated remote
  heap and read-only HexagonRPC file service work. Probe 18 sampled 130
  accelerometer values (mean approximately -0.183, 0.184, 9.821 m/s²), 68 gyro
  values, 16 compass values and NEAR/FAR proximity states. Ambient light
  discovery fails. Controlled portrait and landscape measurements now establish
  the sensor-proxy mount matrix (-X, -Y, +Z). The owner confirmed the shared
  rotate button turns the desktop upright and preserves touch alignment.
  The optional sensor service starts automatically after the desktop; another
  normal boot verified accelerometer readiness and D-Bus availability.
  Auto-brightness and suspend/resume remain unavailable.

Evidence is private under `out/native-probe16`, `out/native-probe17`,
`out/native-persistent18` and `out/native-persistent19`. Sensor details are in
[the userspace notes](../adapter/userspace/hexagonrpc/README.md).

Probe 20's size-oriented, Qualcomm-only build uses an uncompressed kernel,
with DSP firmware loaded from the installed root. Its approximately 29 MB
payload fits below the stock AVB boundary. The installed revision has a correct
unsigned AVB hash, packaged with `pack-persistent.py --metadata unsigned`.
The separate stock VBMeta partition remains intact. Embedded files, executable
modes and 23 matching modules were checked. Artifacts and the resumption record
are in ignored `out/native-persistent20-usb` and `out/NATIVE-CHECKPOINT.json`.

## Verified stock baseline

- ADB authorized; identity CPH2655 / OP5D55L1 / SM8750, platform `sun`.
- OxygenOS `CPH2655_16.0.10.501(EX01)`, Android 16, security patch 2026-08-01.
- Linux `6.6.118-android15-8-g2e6b9c3812c5-ab15114928-4k`.
- Initial state: slot `_a`, bootloader locked, verified boot green. After the
  owner's unlock confirmation and factory reset, ADB confirms unlocked/orange.
- Region marker `US`, hardware project `23893`; model and installed OxygenOS
  build are unchanged by the unlock/reset.
- Thirteen full-resolution camera originals (60,496,792 bytes), including the
  owner's completed zoom sequence, copied and independently SHA256-verified
  against the phone. Pictures/thumbnails are also saved.
- 1,921,766,070 bytes of readable camera tuning/models, camera libraries and
  stock camera APK copied locally. Local SHA256 inventory retained.
- Stock kernel config and camera, input, sensor, display, battery, thermal,
  service and partition-name inventories saved. Some kernel, audio, module and
  firmware probes are partial because production Android denies access.
- Matching stock boot/init_boot/vendor_boot/recovery/DTBO and three VBMeta
  images have been reconstructed from official OTAs, with stock-key signature
  verification and all 64 intermediate/final partition hashes passing. See
  [recovery preparation](recovery.md) for provenance and exact final hashes.
- Rooted capture now preserves the live device tree, memory map, persist files,
  and 130 hash-verified raw partitions (3,275,976,704 bytes), including persist,
  modem NV and both slots’ boot/recovery/firmware. All eight active boot-related
  readbacks match their expected stock/patched image (zero padding accounted
  for on VBMeta). Stock super (13,329,498,112 bytes) is independently verified;
  both GPT copies on six UFS logical units pass CRC checks. This is not an
  atomic whole-device backup.

All raw evidence is under ignored `out/stock-*`. The host snapshots have UTC
timestamps; the phone's camera clock reports January 2026, so photo dates are
not bring-up dates. Use the host snapshot manifest for provenance.

Stock input enumerates touchpanel, alert slider, magnetic cover, fingerprint,
TOF8801, Qualcomm haptics, power and volume keys. Sensors include Bosch BMI26x,
TCS3720 proximity and front light, and TCS3449 rear light. These are stock
observations, not working Linux drivers.

## Next milestones and acceptance evidence

1. **Unlock and recovery:** unlock and matching stock boot images are verified.
   Exact stock init_boot restoration and return to Android are now tested.
   Calibration/radio NV and the stock super partition are preserved. Avoid firmware downgrades
   and boot-chain replacement; rollback compatibility remains unverified.
2. **First Linux boot:** native mainline RAM boot, USB root shell and timed
   return to Android are verified. All eight CPUs and six UFS logical units
   enumerate; eight read-only boot partition hashes match the saved baseline.
   Installed root and automatic desktop passed under a RAM-loaded kernel;
   standalone persistent boot remains pending. Device-specific boot images
   must remain separate from the Pixel and OnePlus 7 Pro.
3. **Desktop:** storage reads, native display, touch, Adreno acceleration and
   the shared shell. Require visible correctness and measured 120 Hz frame
   delivery, including repeated screen-off/wake and cold boot.
4. **Daily hardware:** Wi-Fi, Bluetooth, audio with speaker protection, sensors,
   keys, haptics, battery, charging and thermals. Validate unplugged suspend,
   wake sources and measured drain. Keep charging limits tied to stock evidence.
5. **Communications:** SIM registration, data, SMS, incoming/outgoing calls,
   audio routing, IMS and wake from sleep; GNSS cold/warm fixes. The owner will
   need to provide a test SIM/carrier when this phase is reached.
6. **Cameras and remaining hardware:** all four cameras, autofocus/OIS, exposure,
   color, video, zoom quality, fingerprint, NFC, IR, USB host/display output,
   wired/wireless fast charging and Android apps. Track unsupported functions
   explicitly; do not equate driver probe success with usable hardware.

Native mainline Linux is now RAM-tested with a working root shell over USB
ACM, all eight CPUs online, 15,218,088 KiB RAM, and no block filesystems mounted.
Probe 3 reports Linux 7.3.0-rc2-omarchy-oneplus13-probe3. Its three-minute timer
returned to Android: USB disconnected at 182.3 s, Android USB appeared at
196.3 s and boot completion was verified at 199.3 s. Probe 1 also returned on
its timer; probe 2 had a non-executable init packaging error and returned early.

Probe 5 verifies all six UFS logical units and eight active boot partition
hashes against Android backups after setting the block devices read-only.
Probe 6 provides USB Ethernet plus a root serial console. Its first display
test had no framebuffer device because DRM was configured as a module; the
owner still saw the retained fastboot screen. This is not visible Linux output.

The shared Arch/mobile rootfs was transferred over USB in 45.5 seconds,
SHA256-verified and extracted into RAM. SSH confirms Arch Linux ARM, systemd
262 binaries, and a tmpfs root under the native kernel (BusyBox remains PID1).
Hyprland reports its version when supplied the required XDG runtime directory;
this proves userspace execution, not a working compositor or visible desktop.

Probe 7 builds DRM/simpledrm into the kernel. The owner confirmed color bars
and corner markers, then the shared Omarchy desktop. Hyprland 0.56.2 uses
llvmpipe software rendering at 1440 x 3168, scale 3; its 60 Hz mode is a
simpledrm software mode, not a measured panel refresh rate. The current
`overlay/mobile` source and OnePlus RAM adapter were installed. Touch was absent in probe 7. Probe 8 adds a bounded native S3910 TouchComm v1
driver: controller identity/build match Android, application geometry is
14399 x 31679 with ten slots, and temporary coordinate reporting works. The
owner confirmed touchscreen interaction, with a very laggy software-rendered
desktop. A saved input trace contains nine contact starts and nine releases.
This establishes basic touch only; multitouch, edge accuracy, latency and
suspend/wake remain pending. Persistent boot, native panel
initialization, radio, camera and power management remain unvalidated.
Full Pixel parity and stock-quality 120x zoom are goals, not established support.

## Bootloader unlock checkpoint

After the completed photo backup, the owner authorized unlocking and asked for
the physical confirmation instructions. `adb reboot bootloader` succeeded.
Fastboot reported product `sun`, slot `a`, `unlocked: no`, and
`get_unlock_ability: 1`. `fastboot flashing unlock` returned OKAY, and the owner
was instructed to select **Unlock the bootloader** with volume keys and confirm
with Power. The device then disappeared from fastboot during the factory reset.

The owner completed setup and re-authorized ADB. Post-reset readings confirm
`ro.boot.flash.locked=0`, `ro.boot.vbmeta.device_state=unlocked` and
`ro.boot.verifiedbootstate=orange`. Slot A, CPH2655/OP5D55L1, kernel and full
OxygenOS build identifiers match the pre-reset capture. Region marker is `US`
and boot project is `23893`, distinct from the PJZ110/23821 community survey.
Raw evidence is saved locally in `out/post-unlock-getprop.txt` and
`out/post-unlock-state.json`.

The owner observed brief Chinese text and a progress bar during the reset. Its
exact content is unknown. The successful return to the same stock build and
US identity supplies no evidence of a region conversion or failed unlock.
After explicit owner authorization, Magisk-patched `init_boot_a` was flashed
and Android booted successfully. Magisk 30.7 is running and the separately
authorized ADB Shell grant provides `uid=0`. The first native Linux RAM probe now reaches userspace and USB ACM;
the root shell, eight CPU cores and timed Android return are verified.
The stock kernel, vendor boot, recovery and VBMeta remain unchanged.


## Adreno 830 RAM desktop checkpoint

Probe 10 adds Qualcomm's A830 kernel patch series, GPU clocks/IOMMU nodes,
the exact stock ZAP reservation, and captured stock GPU firmware. Render-only
MSM DRM needs `msm.separate_gpu_kms=1` alongside simpledrm. Eight active raw
boot partitions still hash-match the Android readback baseline.

Mesa 26.2.3 initially rejected the speed-bin-bearing revision-1 chip ID. An
isolated build extends its existing A830v2 entry with the standard wildcard
speed-bin match. Two hardware-only GLES shader tests returned the expected
RGBA 64,128,191,255 and no GL error; renderer is `Adreno (TM) 830`.
Hyprland/Aquamarine selected renderD128, and compositor/shell processes loaded
the isolated libraries. A desktop screenshot was captured. The owner confirmed
it is a huge improvement, though still less smooth than Android.

GPU OPPs are temporarily capped at 342 MHz. Display output remains inherited
1440 x 3168 simpledrm with an advertised 60 Hz mode, not validated physical
refresh. Native DSI/panel control and 120 Hz remain work in progress. Vulkan,
conformance, sustained load and suspend have not been validated. Temperature
readings around the first GPU test were 37–43 C; cooling behavior is unproven.

Loading the matching upstream CPUCP mailbox module succeeded: SCMI v2.0 from
Qualcomm firmware 0x20000 reports two performance domains. CPU frequency
scaling is present in probe 11, with powersave initially selected. After capping
the little cluster at 1,996,800 kHz and big cluster at 1,958,400 kHz, schedutil
was enabled. Frequency readings changed; controlled load/idle validation is
still pending. CPU idle remains disabled. Maximum observed TSENS reading in
this session was 51.3 C; this is not a battery or case temperature measurement.

## Display timing and recovery checkpoint

Probe 10 successfully rebooted directly into fastboot using upstream PMIC
reboot-mode support. Fastboot reported unlocked, product sun and slot a; a
subsequent normal reboot completed Android startup. No persistent boot image
was changed by this test.

Probe 11 read the panel's GPIO86 TE signal without changing its pinmux and
measured 120.848 rising edges/s. This measures panel timing, not rendered frames.
Hyprland initially advertised 60 Hz. A temporary custom modeline was accepted
at 119.838 Hz, but a bounded Wayland presentation test reported sequence zero
and completion intervals faster than the physical panel. Simpledrm's synthetic
completion is therefore unsuitable as evidence of synchronized 120 Hz delivery.
The test was volatile and has not been installed as a default.

A direct, unpowered display-register read then interrupted the RAM session;
Android boot completion was verified again. No register output or pstore was
retained, so the precise failing access/reset cause is unknown. That method was
retired. Future access must acquire the display power/clock resources first.

The exact stock AA569 panel has DSC 1.2, 10 bpc, source-side SPR and native 4:2:2
compression. Its hash-verified SPR filter XML and power-managed stock register
reads are saved under ignored `out/panel-survey/`. Upstream DPU lacks the SPR
stage and the DSI host currently forces RGB/pre-SCR parameters. Native takeover
must account for both; a higher advertised refresh alone does not solve this.

Probe 12's managed clock/power diagnostic successfully reads the inherited
DPU and verifies the physical-address display DMA bypass. The two VIG pipes
and SPR stages match the expected split-panel layout. Nevertheless, external
TE and internal frame-done events stopped after clock takeover; this is a
failed retained-display handoff, not native scanout support. The session
remained accessible over USB. A fresh RAM test will check clock registration
without temporarily parking its active roots.

A three-second core-6 load test in Arch confirms big-cluster schedutil changes
from 1,017,600 kHz to the 1,958,400 kHz cap and returns to 1,017,600 kHz when the
load ends. The little cluster remained at its cap during this test; no idle
claim is made for it. A first BusyBox-only attempt lacked `taskset` and is not
counted as load validation. CPU idle, energy use and thermal throttling remain
unvalidated.

Probe 13 preserves the AHB/ESC0/MDP clock roots at registration. External TE
remains approximately 120.8 Hz before/after DISPCC and powered diagnostic
binding. Real display interrupts counted 243 TE events and 122 PP0 completions
in 2.012 seconds. Thus the inherited display transport currently delivers
approximately 61 frames/s, even though TE is approximately 121 Hz. Native DMA
buffer flipping and increasing the transport cadence remain separate steps.


Probe 13's first DMA flip timed out after issuing CTL_START while automatic
refresh was active. The uncertain buffer was pinned until reboot; Android
recovery and a 100% battery at 31.2 C were verified. OnePlus's published SDE
command driver explicitly omits CTL_START in automatic-refresh mode.

Probe 13b omits that command and passes two independent new-buffer/restore
cycles, each with 128 real PP0 completions and both flush bits cleared. The
upstream sequence to disconnect TE, drain/disable automatic refresh, and
reconnect TE also passes. In on-demand mode, 120 alternating DMA transfers
completed in 999,415 microseconds with zero reported failures; the original
buffer and automatic refresh were restored. This establishes approximately
120 transfers/s for the retained panel configuration, not yet compositor or
optically verified 120 fps. The new CPU provisioning helper also ran successfully.

The native controls profile maps Power to PMK8550 PON, Volume Up to RESIN,
and Volume Down to PM8550 GPIO6, matching the captured stock DT. The profile
compiles; physical key-event and wake validation remains pending.

Stock PCI enumeration identifies Wi-Fi as 17cb:110e, using the peach firmware
family. The current upstream ath12k PCI table supports WCN7850 at 17cb:1107,
so that driver cannot simply be enabled under a WCN7850 assumption. Battery
and gauge telemetry are provided through OnePlus's ADSP PMIC GLINK protocol;
native ADSP and protocol compatibility must precede meaningful battery support.


Probe 14 registers the three native physical-key input devices and explicitly
enables the touchscreen's volatile reporting. Its 128 MiB CMA pool supports
four full-resolution framebuffers. All eight raw boot partition hashes still
match the baseline. The experimental `oneplus13-scanout` DRM driver switches
from simpledrm to on-demand DMA, preserves panel/DSI/DSC/SPR state, and pins
potentially active buffers on failure. It is deliberately not unloadable.

A ten-second DRM test completed 1,209 page flips (120.841/s), restored the
original framebuffer, and released its allocations, with zero driver failures.
Adreno rendered red, green and blue into display-owned DMA buffers; every
pixel matched in all three CPU/readback checks. The existing isolated Mesa
build's generic KMSRO path works without another Mesa patch.

Hyprland and the shared mobile shell started on DSI-1 at 1440 x 3168 @ 120 Hz,
using Adreno. A screenshot confirms rendered desktop content, but does not
verify the physical panel. Wayland presentation feedback has real sequence
numbers and hardware flags, with median 16,551 us between frames (about
60 fps). Thus 120 Hz modesetting/direct DRM works, while the tested compositor
workload still ran at 60 fps. The next driver version separates TE/vblank from
PP0 transfer completion, matching upstream DPU command-mode IRQ semantics.
Android recovery again succeeded; battery was 100% at 31.9 C.

All 58 ADSP firmware components (40,840,848 bytes) from the local capture now
independently hash-match fresh Android reads. ELF relocation metadata requires
exactly the captured 0x5080000 ADSP reservation; the upstream default is too
small. The optional DT-preparation path verifies both ADSP reservations and
phandles, rejects wrong sizes/references, and leaves GPU-only output byte-identical.
The experimental battery profile uses common read properties and deliberately
omits the incompatible charging-control properties. ADSP/battery runtime tests
are still pending.


Probe 15 starts the stock ADSP through the mainline PAS driver, with both
firmware reservations and all 58 components verified. The remote processor
reports running, PMIC GLINK binds, and the shared mobile battery service reads
100%, 29.2–29.4 C, good health and an online USB SDP connection. The protocol's
optional power/time/capacity fields include inconsistent or placeholder values;
the next profile exports only the useful common read-only telemetry. Charge
limits, fast charging, unplug/replug and full charge-cycle validation remain
pending. No charging-control setters are exposed.

QRTR discovery finds the SSC sensor service (400) on the ADSP. That establishes
transport availability, not yet sensor measurements. The native Power and both
Volume input devices remain present; physical-event tests still need the owner.

Continuous TE vblank is now separate from PP0 transfer completion. No DMA
failures were reported, but feedback-driven and low-latency Wayland test modes
both still measure a median 16,553 us between presented frames. Holding the GPU
at its existing 342 MHz cap and switching the compositor's render scheduler did
not change that result. Kernel scanout throughput and compositor throughput
therefore remain distinct validation items. Probe 16 tests CPU retention with
cluster and APSS collapse states removed from its device tree.
