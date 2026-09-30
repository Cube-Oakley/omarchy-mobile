# Checkpoint — September 29, 2026 (audio and sensors)


**Cellular and app integration:** the native Phone and Messages apps now use
one Pixel SIT service. The user confirmed sending/receiving texts, calls with
two-way audio, and microphone mute. Normal calls use the validated +6 dB
microphone boost. Speakerphone routing remains unfinished.

Cellular IPv4 now supplies a metric-700 fallback default route. A Wi-Fi-off
check passed normal DNS and certificate-verified HTTPS with `rmnet0` as the
only default interface; reconnecting Wi-Fi restores its preference. Carrier
DNS is selected only while cellular is the default connection.

The opt-in persistent modem manager now prepares a fresh RAM runtime, loads
the pinned drivers, and brings up LTE and IMS automatically on boot. A RAM boot and
two normal installed boots reached app readiness and cellular HTTPS 200. The user confirmed app texting and calling after reboot. The
runtime has no test deadline. Ordered shutdown ends calls and host networking,
powers CP off before stopping RFS, then disarms the watchdog; the recovery PID1
uses this sequence before killing other processes. A durable failure marker
keeps a modem fault from looping across boots. Automatic warm CP recovery is
still unverified and disabled. All nine protected partitions remain read-only
while the modem runs. A separately user-approved one-bit boot-success repair
keeps Linux slot A from exhausting its retries; all other devinfo bytes were
verified unchanged and read-only protection restored. The LTE workaround disables NR while saving the original
mode; 5G is unresolved. A modem-online five-second RTC suspend attempt aborted
at the kernel freeze stage before entering sleep, with CP and LTE traffic still
working afterward. The user confirmed screen-off ringing, display wake and
answering; calls/SMS waking from true suspend are still unverified.
See [the modem integration notes](../modem/README.md) for setup and limits.

**Active work: audio and sensors.** The built-in microphones, both speakers
and the sensors work through the AoC, under Google's own drivers ported to
this kernel ([aoc](../kernel/aoc/README.md)).
- **AoC:**
  - Trusty, the GSA and the AoC core load the AoC firmware (12255112-polygon),
    which comes online with 82 services.
  - What it took without pKVM: opening the GSA's and AoC's S2MPUs where pKVM
    would program them, a static SysMMU for the AoC, and the right Trusty load
    order.
  - The AoC starts once per boot. Restarting it needs ACPM's asynchronous
    reset notification, which is not implemented.
- **Microphones:** their PMIC supplies were off. The stock DT keeps them
  always on; `pixel-aoc-power` switches them on through ACPM
  ([aoc-power](../kernel/aoc-power/README.md)).
- **Speakers:** two CS35L41 amplifiers on SPI7, driven by mainline `cs35l41`:
  - the SPI7 host is [spi](../kernel/spi/README.md), with reset and interrupt
    lines from [gpio](../kernel/gpio/README.md);
  - the stock protection firmware runs on both amplifiers, with the factory
    calibration read (read-only) from persist;
  - the left amplifier drives the top speaker and the right the bottom one,
    each on its own TDM slot.
- **PipeWire:** a UCM profile and a WirePlumber rule ([../audio](../audio))
  give "Built-in Audio Speakers" and "Built-in Audio Microphones". A tone
  played with `pw-play` shows up in `pw-record`.
- **Sensors:** accelerometer, gyroscope, magnetometers, barometer, proximity
  and light, all run by the AoC's sensor framework (USF). The protocol was
  recovered from the stock `libusf.so` ([sensors](../sensors/README.md)).
  - `pixel-sensor-proxy` serves iio-sensor-proxy's D-Bus API, so the shell's
    rotation, automatic brightness and proximity helpers use it unchanged.
  - The sensors need their PMIC rails on
    ([aoc-power](../kernel/aoc-power/README.md)) and the AoC's registry
    loaded, with this phone's factory calibration read from persist.
  - Rotation follows the phone both ways (checked by hand).
  - Light comes from Google's auto-brightness sensor, once a second. It sees
    none of the panel's own light, so the Pixel profile sets `panelLux` to 0.
  - **Fixed 2026-09-30:** from about 16:50 on 2026-09-29 the TMD3719 (light
    and proximity) did not convert. It times its conversions to the panel and
    needs the display state (on/off, DBV, refresh rate) that Android's sensor
    HAL sends the AoC as a DisplayInfo request. The proxy now sends it
    ([sensors](../sensors/README.md#the-display-state)); light and proximity
    report again after a clean reboot, including with the screen off.
    Automatic brightness and near/far still need a check by hand.
- **RTC alarm:** the S2MPG12's alarm 0 now wakes the phone from s2idle
  ([rtc](../kernel/rtc/README.md)), so `rtcwake` works and suspend can be
  tested unattended. Four suspend/wake cycles passed with the AoC, audio,
  sensors and Wi-Fi running.
- **Bluetooth:** works; pairing is still to be tested by hand
  ([bluetooth](../kernel/bluetooth/README.md)).
- **Speaker level:** the amplifiers ran at their reset gain of 0.5 dB, about
  17 dB below stock. The boot script now sets the rest of the stock default
  path: 17.5 dB of amplifier gain, DRE, and both protection inputs from each
  amplifier's own slot.
- **Touch in the shell:** fixed. The shell stopped taking taps after a touch
  with fingers on both the shell and an app, for example a grip on the
  screen's edge while tapping the rotate button. Hyprland 0.56 sends every
  finger's moves and lift to wherever the last finger landed. The shared
  shell now has a Hyprland plugin, `touch-fingers` (overlay/mobile), that
  sends each finger's moves and lift to the surface it went down on. The
  wallpaper also takes touches no other surface does.

The modem-persistence image (`out/modem-persistent/image`) is now in `boot_a`;
its direct readback matches SHA256
`15636c7fe93ebfc51b363ec9d85e478ee123ac9d8df32627a50710c7bc21ab2f`. It is image H
plus ordered modem shutdown in recovery PID1; the hardware kernel and bundled
audio/Bluetooth/RTC drivers are unchanged. The prior image H remains available
locally under `out/checkpoints/20260929-audio/image-h` with SHA256
`ebbcc45e97ac848cff8ec552a0be54bd25e63da45f09d3d50a3c8df9847d92bf`.
`aoc.bin` is on the Pixel root, because it does not fit under the boot
image's AVB boundary. The boot script now starts Bluetooth, the AoC with audio
and the sensors, and PipeWire.

Validated from normal boots of `boot_a` on 2026-09-29:
- the AoC comes online (82 services) and both amplifiers run protected;
- `pixel-sensor-proxy` finds the accelerometer, light and proximity sensors;
- PipeWire shows the speakers and microphones, and a tone played through it
  reaches the microphones;
- Bluetooth is powered;
- an `rtcwake` suspend wakes on the RTC alarm;
- by hand: the microphones record a voice, and each speaker plays on its own
  channel (top on the left, bottom on the right);
- by hand: proximity reports near and far, and rotation turns the screen both
  ways;
- after three reboots, the boot script applies the amplifier gain and the
  shell loads `touch-fingers`.

# Checkpoint — September 29, 2026 (sleep and Wi-Fi power)

**Active work: sleep.** Screen off, connected to Wi-Fi and idle, the phone
now draws 1.47 W at the USB input, down from 2.11 W:
- **Wi-Fi power:** about 0.10 W instead of 0.45 W.
  - Firmware deep sleep now runs, using the in-band device-wake handshake over
    control-ring mailbox messages.
  - The PCIe link enters L1.1/L1.2 once the firmware runs
    ([pcie](../kernel/pcie/README.md)).
  - Throughput is unchanged.
- **Panel sleep:** the panel sleeps while the screen is off
  (`pixel_scanout.panel_sleep`), saving 0.32 W. On wake the DSC configuration
  is sent again.
- **Keys:** power and volume use wake-up interrupts ([keys](../kernel/keys/README.md)).
- **System suspend:** `s2idle` is the default. Deep (PSCI SYSTEM_SUSPEND) needs
  the vendor's PMU preparation, which is not done yet.
  - Staged tests pass (freezer, devices ×6, platform ×2).
  - A real s2idle woke on the power key, with display, USB and Wi-Fi back.
  - Wi-Fi goes through D3 and back, and drops sleep requests while
    suspending, as bcmdhd does.
- **CPU idle:** the MCT is now the tick broadcast device, so every CPU can
  enter C2 at once; the kernel's hrtimer broadcast kept one CPU awake. The mid
  and big clusters power down when all their CPUs are idle
  ([cpupm](../kernel/cpupm/README.md)). Neither changes the input draw much.
  - The mid and big rails keep about 50–60 mW for their PLLs and clock
    trees, and only SICD (the SoC's clock-down idle) stops those.
  - The vendor's bus clock gating changed nothing measurable.
- **Touch:** the touch worker no longer counts as load (the load average went
  from 1.0 to about 0.2), and it freezes for suspend.
- **Brightness:** the panel's brightness register (DCS 0x51, DBV 4–2047) is
  now a backlight device, `pixel-panel`. The shell's brightness slider works,
  and the level is restored at session start.
- **Flashlight:** the LM3644 on hsi2c_15 ([torch](../kernel/torch/README.md)).
- **Vibration:** the CS40L26A's ROM effects on hsi2c_8
  ([haptics](../kernel/haptics/README.md)). Not yet felt by hand.
- **I2C buses:** hsi2c_15 and hsi2c_8, as the bootloader leaves them
  ([i2c](../kernel/i2c/README.md)). The battery EEPROM and the NFC/eSIM chip
  are guarded.
- **Wi-Fi MAC address:** stable per network (NetworkManager `stable`). The chip
  has no MAC address of its own.
- **Bluetooth (in progress):** the chip powers up and answers HCI over UART18,
  but it hangs at the first record of Google's patch firmware. On ROM firmware
  its radio finds nothing. It is not in the image
  ([bluetooth](../kernel/bluetooth/README.md)).

These changes are not yet in a kernel checkpoint. Image F is in `boot_a`. It
is image E (kernel v25) plus `pixel-mct.ko`, cluster power-down in
`pixel-cpupm.ko` and the boot script that loads them. Its readback matches
SHA256 `376b4d9271465f365966763a41b242ced47bc4074b4b83f89c90b3860807408c`.

# Checkpoint — September 28, 2026 (Wi-Fi)

**Active work: Wi-Fi.** The BCM4389 runs Google's stock firmware under
mainline brcmfmac (kernel [v24](../kernel/wifi-v24/README.md)), on PCIe
channel 1 from the [PCIe module](../kernel/pcie/README.md).
- **Starts at boot:** the boot script loads the link, then the Wi-Fi stack from
  the image, then the system bus and NetworkManager, which reconnects to saved
  networks. The shell's Wi-Fi panel lists and joins networks.
- **Speed:** 5 GHz at a 720–816 Mbit/s link rate; about 90 Mbit/s down and up,
  which is the home network's limit.
- **Power:** with the screen off it costs about 0.45 W (2.11 W against 1.65 W
  with the chip off). The link has no power states yet and firmware deep
  sleep is off.

Image Z is in `boot_a`. Its readback matches SHA256
`bcdc339ad0801df786379b0d43435894d1d1dd287e8497823ce52e5a7ce76a34`.

# Checkpoint — September 28, 2026 (idle power)

**Active work: power usage.** See the [power record](power-20260928.md).
Whole phone from USB, image W to image X: screen on 3.01 W to 2.09 W, screen
off 2.87 W to 1.65 W.
- **CPU idle:** the firmware rejected every C2 entry, so idle CPUs spun on
  SMCs. The [cpupm module](../kernel/cpupm/README.md) makes C2 work.
- **Power domains:** the unused camera pipeline, TPU, codecs, G2D, EH and AUR
  are powered off at boot ([pd](../kernel/pd/README.md)).
- **Memory clock:** MIF drops to 421 MHz while the screen is off (kernel
  [v23](../kernel/power-v23/README.md)).
- **Measuring:** the [ODPM module](../kernel/odpm/README.md) reads 24 PMIC
  rails; `scripts/pixel-usb-power.py` measures the USB input.

Image X was installed in `boot_a` (since replaced by image Z). Its readback
matched SHA256 `b6b5de2e51a8f508ba34692e88068918825aeb56e3035bbc46f9457661d3f0d1`.

# Checkpoint — September 28, 2026 (battery)

**Active work: battery and charging.** See the [battery module](../kernel/battery/README.md).
- **Battery state:** percentage, voltage, current, temperature and charge
  status now reach the shell. They come from the MAX77759 gauge and charger,
  over the bootloader's I2C bus 13.
- **Charging while in use:** the bootloader leaves the USB input at 500 mA, so
  with the screen on the phone drained about 90 mA while plugged in. The module
  allows 1.5 A from USB, as stock does on this computer's charging port. Measured:
  +0.7 to +1.0 A into the battery.
- **Charge limit:** the shell's limit works. At the limit the phone runs from
  USB and the battery rests. Charging also pauses at 45 °C.
- **Gauge caveat:** it has lost the stock battery model (POR). The percentage
  follows its default model and the capacity estimates are wrong until that
  model is restored.

Image W (V plus the battery module) was installed in `boot_a` (since replaced
by X). Its readback matched SHA256
`6aeb78276a390ade7b64ab0f3925378b76fa3388daef547bd73b943a036668cd`.

Wi-Fi is paused. A test PCIe host driver for channel 1
(`kernel/pcie/pixel-pcie.c`) hangs the SoC at its first sub-controller
register access, though every clock gate and Q-channel for the controller is
on. It is not in any image.

# Checkpoint — September 28, 2026

**Active work: UI smoothness.** See the [smoothness record](smoothness-20260928.md).
- **Display:** 120 Hz now works on a normal boot.
- **GPU:** scales 302–885 MHz with a thermal cap.
- **Touch:** runs on the SPI0 controller with its attention interrupt,
  at about 240 Hz with almost no CPU; it restarts the controller after errors.
  See the [SPI touch record](touch-spi-20260928.md).
- **CPUs:** full stock rates (to 2.85 GHz) under a 20 ms thermal cap that
  holds sustained load at 2.05 GHz, with a stock cross-cluster floor.
- **Switcher:** it now handles the session's terminal.
- **Result:** shell animations average 111–115 fps, up from 61.

Image V (U plus SPI0 and interrupt touch; U added the fast CPU thermal cap)
is in `boot_a`. Its readback matches SHA256
`1e9375e2850cef287e1555c2c4cf5f915e4baf5f6458dd06f376ccb1d376bbbe`.
The Pixel runs the current shared shell.
Kernel checkpoint: [v22](../kernel/smooth-v22/README.md).

# Checkpoint — September 27, 2026

**Active work: native boot without fastboot.** See the [native boot record](native-boot-20260927.md).
- **Storage:** UFS runs HS gear 4 rate B on two lanes (1.2–1.8 GB/s, up from
  0.57 MB/s), so the persistent desktop starts in seconds instead of about six
  minutes.
- **USB:** the kernel now powers up the USB 2 PHY that a normal boot leaves
  isolated.
- **Normal reboot of image K:** USB SSH returned after 15 seconds. Startup then
  stopped at CPU scaling, because the little cluster boots at 1598 MHz. The
  remaining steps, run by hand, started Hyprland and the shared shell on that
  normal boot.
- **Image L** accepts that rate, and the boot script no longer stops on a
  refused CPU-scaling request.
- **Normal reboot of image L:** after an orderly reboot, USB SSH returned in 18
  seconds, CPU scaling started, and the desktop finished starting by itself in
  under a minute.

- **Image M** resets the UFS device before the host enable, so the first probe
  succeeds on every boot (it used to fail once and retry).
- **Image N** reads the battery-backed PMIC RTC, so the clock is real time at
  boot.

Image N was then installed (since replaced by S), SHA256
`684f3704d8f7526f421d1ae2089f8a0bcdb85b3c9196fc27fd1671a8dfe2dbe1`.
`scripts/pixel-reboot.py fastboot|linux` switches between Linux and fastboot
without buttons, using a warm reset. Power+VolDown is only a fallback. Kernel checkpoint: [v20](../kernel/native-boot-v20/README.md).
Astra has stopped work on the Pixel. Linux-only installs to `boot_a` and userdata
are authorized. Never write the ST54J or the efs/persist partitions.

# Checkpoint — September 26, 2026

**Active work: persistent installation and power-button screen sleep/wake.**
The merged repository is the workspace for both devices. [V19 B](persistence-power-20260925.md)
now has physically verified power-key screen off/on with the shared CRT animation
and no console flash. The CPU stays awake; full suspend is not implemented.
The user authorizes replacing Android and userdata for a Linux-only install.
UFS enumerated the expected partitions and full boot-image reads matched the
saved hashes. Userdata is ext4 and passed a 16 MiB write/remount/readback check.
The complete Arch filesystem was installed through a checked sparse image;
image G mounts it and passes GPU shader readback. The shared desktop runs from
storage at 120 Hz after first-run font discovery and a desktop restart. Clock
setup now precedes Hyprland; cold reads at PWM gear 1 remain slow. The guarded
boot_a write and direct SHA256 readback passed. The following orderly restart
did not restore USB; the photo shows early kernel initialization. Recovery via
fastboot and a traced RAM boot worked. Both proof files survived, establishing
filesystem persistence across reset. Autonomous startup remains unverified.

**Stopping checkpoint:** image H embeds the required boot parameters with
`CONFIG_CMDLINE_FORCE`. RAM boot passed USB, UFS, saved-file and GPU checks;
the persistent bootstrap completed at 366.6 seconds. H is installed in boot_a
and its full direct readback matches SHA256
`ca77148f5fe6433a6625d8ca55a3b72e03bd13cc30ee11b5618de38b840742a3`.
A normal reboot of H has **not** been attempted. The phone is left in its RAM-booted
H session using the installed root, at the user's request to stop for the night.
The user sees the persistent-root terminal and prompt, but no top shell interface.
Quickshell is running with its layers at alpha 0; cold-start visibility is unresolved.
Setting the clock before Hyprland did not prevent this on H.
Overnight follow-up: the user reports the power button does not work in this
session. Remote display-off succeeded and DPMS reports off; Linux stays awake.
Next: fix cold-start shell visibility and the current physical-key path, then verify H through normal reboots,
saved-file readback and physical CRT power-key checks on the resulting desktop.
There is no automatic reboot timeout. USB serial works on the RAM recovery path.
Local SSH: `out/checkpoints/20260925-persistence-power/arch-session-f/ssh`.
Android recovery appeared after a native restart; recovery ADB successfully
returned it to the bootloader. Do not select Factory reset: userdata is Linux.

**Previous RAM milestone:** panel/bandwidth v18 image D, 120 Hz animation validated;
1800-second automatic return to stock slot A. No partitions were flashed.

**120 Hz milestone:** [Panel timing and bandwidth investigation](panel120-20260925.md)
now establishes clean native 120 Hz scanout and clean accelerated Hyprland
animation at approximately 120 fps. The user confirms both the native bars and
the final GPU triangle are clean. At the 421 MHz boot memory rate, GPU animation
was limited to about 60 fps and generated 600 observed DSI underruns in ten
seconds, producing full-width flashing lines. A stock-DT 1352 MHz MIF request
through CCF/ACPM, with unchanged GPU clocks, yields 119.6–120.2 fps and zero new
underruns. Image D integrates that bandwidth requirement with 120 Hz mode
ownership and detects underruns as transfer failures. Final D animation and
physical visual checks pass. Normal touch restoration remains under test: the
temporary GPIO driver initially stopped on a packet error. The vendor-style
bounded read-retry module stays loaded; physical response awaits the user's
retry. The normal shell is restored and its stale-kernel startup warning is
fixed. Earlier checkpoints remain intact.

**New hardware milestone:** [Thermal and CPU scaling](power-bringup-20260925.md)
work through standard Linux frameworks: seven thermal zones, three bounded
cpufreq policies, verified thermal cooling and schedutil. [GPU v15](gpu-bringup-20260925.md)
now powers the Mali-G710, starts CSF firmware and runs hardware GLES rendering.
An isolated Mesa build with the missing G710 model entry passes shader/readback
and resume tests. Hyprland selects Mali-G710 MC7 and renders the shared mobile
UI; a native screenshot is saved. The user confirms significantly faster
response, with remaining lag. Smooth 120 Hz is the target.
Full DPU/DSI display ownership and standard SPI touch remain unfinished.
The [v16 display investigation](display-pipeline-20260925.md) measured about
21 ms copying plus 12 ms refreshing each full-screen update. The user reports
horizontal tearing/glitchiness despite smooth-looking motion. Image B verifies
intermittent busy-at-update-entry and tests a bounded idle wait. Physical photos
also exposed blue backgrounds where software pixels are black; image C restores
the original ABL pixel/alpha layout. The user and physical photo now confirm
correct labeled red/green/blue bars, dark background and white checkerboard;
GPU animation still glitches, while the user confirms clean CPU-only motion.
A small display-owned GPU-buffer test reproduces stale CPU reads. Image D
uses standard shmem write-combined mappings: the identical three-color regression
now has zero mismatches (all 3,072 pixels failed on C). The user confirms GPU
animation edges are clean and the glitchy issue is fixed. Remaining pauses and
sub-60-fps performance persist; proper DPU/DSI scanout is still needed for 120 Hz.
The source/config, failed/passing regression and physical feedback are saved.

**Current milestone:** [Native Hyprland and shared mobile shell](hyprland-mobile-20260925.md)
now run on the Pixel. The user confirmed Hyprland's terminal and input proof.
The shared OnePlus mobile UI, Kitty and on-screen keyboard are captured in native
Wayland screenshots; Settings also maps and renders. Physical touch now reaches Hyprland and the shared UI through a temporary GPIO
SPI input driver; the user confirmed response, with severe latency. See the
[touch checkpoint](touch-bringup-20260925.md).
The older v11 EGL blocker was fixed with Mesa's `GBM_ALWAYS_SOFTWARE=1`.
The current v15 GPU launcher clears those software overrides and uses the
isolated hardware Mesa build; v11 remains an independent fallback.

**Previous stable 60 Hz image:** native DMA scanout v17 image D, SHA256
`8f8c6ae4d353452c6e76ecfc7ff086abd297cf81d844541fadddc83f8fef0fb0`.
The [direct scanout record](display-direct-20260925.md) preserves hardware IRQ
validation, read-only SysMMU capabilities, a physically confirmed Linux-owned
DMA test card and successful native DRM page flips. Both Mali shader and GPU
sharing regressions pass. The native display-only control reaches about 60 fps;
GPU desktop animation reaches 58–60 fps in several windows after lower startup
windows, with zero scanout failures. The user confirms correct visuals and much smoother motion. The mobile UI
was left running with bounded touch input. The later v18 work above adds 120 Hz
refresh control and the required memory bandwidth; full display power/PHY
ownership remains unfinished. The v16 D image remains the separate known-good visual fallback:
`b8479c90bfb58f9758f597393d56205794f0989fce1aa12466647b9c5480d2de`.
Sources, test results and replay are in the linked checkpoint. Earlier v11d
Arch/Hyprland/mobile shell and touch evidence remain preserved independently.
Earlier RAM milestones returned to Android. The current Linux-only installation
reuses userdata; recovery now means the bootloader and saved host images.

**Repository consolidation complete:** shared userspace lives in
`overlay/mobile/`; hardware-specific sources and evidence live in
`devices/<device>/`. The clean publication line is preserved separately, as
[publication instructions](../../../docs/publishing.md) describe.

## Display/Arch image

- Checkpoint: `out/checkpoints/20260925-drm-v11d/boot-pixel-shell.img`.
- SHA256: `ffcce07048c1924cd903d8550debd433d3a8baccb0f0eb777dd3820aa750ba95`.
- Kernel: `7.3.0-rc2-pixel-drm11-g5225b8eec4c9-dirty`.
- Display: `/dev/dri/card0`, DSI-1, fixed 1440×3120, CPU-copy XRGB → BGRA.
- Test: `pixel-kms-test 900` from the native serial shell.
- USB: composite serial + Ethernet, same link as v10 below.
- Local SSH wrapper while this boot is alive:
  `out/checkpoints/20260925-mobile-replay/arch-session/ssh`.
- Full patch/config: `devices/pixel7pro/kernel/native-bringup-v11.*`.

## Saved graphical userspace

The v11d directory contains a checked Weston rootfs archive, excluding temporary
SSH keys and runtime state. See [replay recipe and exact hashes](display-drm-20260925.md#saved-weston-rootfs-and-replay).
The Weston archive was successfully restored on a second RAM boot. Separate
Hyprland and mobile snapshots now preserve subsequent milestones; see the
[new replay notes](hyprland-mobile-20260925.md#checkpoints-and-replay).
The full mobile archive then passed a third-boot replay, including graphical
keyboard input. The RAM image reboots at 1800 seconds; that session has now ended.

## Network/Arch image

- Checkpoint: `out/checkpoints/20260925-network-v10b/boot-pixel-shell.img`.
- SHA256: `c67e891cb75b1297282cbde755f56018e95636e895baff01d9015ae257633dc5`.
- Composite USB `0525:a4aa`: CDC-ACM serial plus CDC-ECM Ethernet.
- Phone `10.77.7.1/30`, host `10.77.7.2/30`; no internet route/NAT.
- Twenty-minute automatic reboot; all phone userspace files disappear on reboot.
- See [exact boot/provisioning commands](native-arch-20260925.md#repeatable-use).

## Historical serial-only baseline (default helper image)

| Item | Value |
|---|---|
| Device | Pixel 7 Pro `cheetah`, serial kept locally in `out/device.serial` |
| Bootloader | `cloudripper-15.1-12292122`, unlocked, NOS production yes |
| Default OS | Factory Android 15 AP4A.250205.002 + Magisk 28.1 |
| Slot | A, successful; B is not a usable fallback |
| Native checkpoint | `out/checkpoints/20260925-shell-v9/` |
| Image | `boot-pixel-shell.img` |
| SHA256 | `88671219ba0d6835a7c6eee283948e38ec02f59522b44e6d760ddfb643493e05` |
| Native release | `7.3.0-rc2-pixel-shell9-g5225b8eec4c9-dirty` |
| USB | `0525:a4a7`, CDC-ACM; host `/dev/ttyACM*`, phone `/dev/ttyGS0` |
| Runtime | Default ten-minute automatic reboot; explicit `reboot` supported |
| Root filesystem | Embedded RAM-only initramfs; no phone partitions mounted |

## Use

```sh
python scripts/boot-pixel-shell.py
python scripts/pixel-shell.py
```

Run from `devices/pixel7pro/`. `Ctrl-]` leaves the connection; `exit` starts a fresh
shell. These are historical v9 commands; use the current installation state above.
ADB is an Android service and is not
present in the native image. The host helper identifies the Pixel gadget rather
than assuming that ttyACM0 is always its port.

## Milestones leading here

- September 24: recovered Android after identifying ABL command-line copying;
  factory boot restored exactly through fastbootd, Magisk root verified.
- Standalone framebuffer probe v2: screen photo and DECON frame-counter proof.
- Linux v4: screen console; MCT panic captured automatically in reserved RAM.
- Linux v5: GS201 MCT skip; eight CPUs, PID1 and planned 45-second reboot.
- USB v7: real DWC3 serial gadget enumerated; reset around 60 seconds.
- USB v8: stopped inherited watchdogs, completed 120 seconds and planned reboot.
- September 25: host udev rule installed, USB roundtrip verified, shell v9 tested.
- September 25 morning: USB Ethernet + serial, shared Arch base in RAM, key-only
  SSH and 8 MiB SFTP roundtrip verified; Android boot hashes unchanged afterward.

## Next work

1. Diagnose the traced normal-boot stop using the new console output and the
   successful RAM-boot trace. The original G boot image also matched a full
   fastboot fetch after the failed restart; image corruption was not observed.
2. Establish autonomous boot and improve UFS performance. File persistence
   across reset and a subsequent RAM recovery boot is already verified.
3. Extend charging and suspend/resume after persistent boot.
   Screen blanking does not establish CPU suspend or deep idle.
4. Replace temporary GPIO SPI touch with standard SPI/IRQ input. CPU scaling,
   thermal sensing, Mali rendering and 120 Hz scanout already have checkpoints.

## Recovery state

Userdata is now the Linux filesystem; Android is no longer the recovery OS.
Power + Volume Down reaches the verified bootloader. Preserve the host factory
images and follow the validated restoration procedure if needed; do not switch
to B or use the old restore scripts blindly. Exact recovery hashes and the ABL
flash-header caveat are in [restart notes](restart-20260924.md).
