# AoC: the audio DSP and its drivers

The Pixel 7 Pro's audio runs on the AoC (Always-on Compute), a DSP subsystem.
The AoC owns:
- the digital microphones (PDM);
- the TDM link to the speaker amplifiers;
- the sensor buses.

Linux loads its firmware and talks to its services over mailboxes and shared
rings. This directory builds Google's own drivers for it, ported to this
kernel:

| Tree | Upstream | Modules |
| --- | --- | --- |
| `trusty/` | LineageOS `android_kernel_google_gs201` (lineage-21, 40ff9342), `drivers/trusty` | trusty-core, trusty-log, trusty-ipc, trusty-virtio |
| `gsa/` | same, `drivers/soc/google/gsa` | gsa |
| `aoc/`, `aoc-ipc/` | `kernel/google-modules/aoc` 222ba49 and `aoc-ipc` 61ee513 (android-gs-pantah-5.10-android14) | mailbox-wc, aoc_core, aoc_char_dev, aoc_channel_dev, aoc_control_dev |
| `aoc/alsa/` | same | aoc_alsa_dev_util (the AoC audio services), aoc_alsa_dev (the card) |
| `stubs/` | written here, plus Google's `modem_notifier.h` | none (headers) |

`sources.json` pins every upstream file by SHA-256. `assemble.py OUT` fetches
them into a content-addressed cache (`out/aoc-src-cache`), lays out the tree
and applies `patches/` in order. `scripts/build-pixel-shell.py` does this for
persistent images.

## How the AoC starts here

Stock runs pKVM and a vendor kernel; this kernel has neither. The chain is the
same as on stock:

1. Trusty (FF-A) comes up.
2. The GSA authenticates and loads the AoC image (mailbox command
   LOAD_AOC_FW_IMG).
3. Trusty's `hwmgr.aoc` starts the AoC.
4. The AoC reports its services: 82 with firmware 12255112-polygon.

It needed these changes:
- **Trusty load order.** trusty-ipc loads before trusty-virtio, so TIPC is
  probed before VIRTIO_START.
- **S2MPUs.** They reset to blocking. On stock, pKVM programs them when their
  power domain comes up. Here, `stubs/.../pkvm-s2mpu.h` disables the GSA's
  S2MPU instead (CTRL0 = 0) while the GSA is powered, and `aoc.c` does the
  same for the AoC's. Before this, GSA rejected the image with INVALID_ARGS.
- **Image header.** It is allocated below 4 GB through the AoC device; GSA
  rejects a header above 4 GB.
- **SysMMU.** A static SysMMU (`aoc_sysmmu.c`) maps the firmware's fixed
  windows, instead of the samsung-iommu driver. There are no physical dma-buf
  heaps.
- **Pin control.** The AoC's pin states get `pinctrl-use-default`, since there
  is no GS201 pin controller driver.
- **Endpoints.** `AOC_MAX_ENDPOINTS` is 200: this firmware has 82 services.
- **Vendor hooks.** Stand-ins cover coredump, ACPM reset, CPU PM, PMU, the
  debug snapshot and the modem notifier.
- **API.** The code is updated for the Linux 7.3 APIs.

The ALSA driver is also ported to the 7.3 ASoC API. On top of that:
- Links whose codec has no driver get the dummy codec, so the AoC endpoints
  work without them (the haptics link today).
- Codecs without slot controls get their TDM slots from `google,tdm-rx-slots`
  and `google,tdm-tx-slots` in their DT node.
- The card's driver name is `aoc-snd-card` (for UCM).

## Order

`scripts/pixel-persistent-session.sh` (`start_audio`) loads the modules in
this order, with a marker that skips audio on the next boot if a start never
finished:

1. Trusty, the GSA, the mailboxes, and the AoC core with
   `aoc_autoload_firmware=0`.
2. ALSA, and `aoc_alsa_dev_util` before the AoC firmware starts. The AoC bus
   binds each service to a driver whose name matches, else to `aoc_char_dev`,
   so `aoc_char_dev` loads last.
3. The microphone supplies and the speaker amplifiers
   ([audio](../audio/README.md)), then the card.
4. `echo aoc.bin > /sys/devices/platform/19000000.aoc/firmware`. It is online
   when `services` shows `Services : 82`; the `revision` file never says so.
   `aoc.bin` (21 MB) does not fit under the boot image's AVB boundary. It is
   on the Pixel root at `/usr/lib/firmware/omarchy-mobile`, and
   `firmware_class.path` points there: the kernel resolves firmware from
   PID 1's root, where the Pixel root is `/run/arch`.
5. The character devices (`aoc_char_dev`, `aoc_channel_dev`,
   `aoc_control_dev`).

Taking the AoC offline (unbinding the `aoc` driver, or shutdown) goes through
`aoc_take_offline` and returns cleanly (tested 2026-09-29).

The AoC starts only once per boot. A second start loads and authenticates the
image, but the AoC never answers ("aoc init no respond"). The driver's
recovery then resets the AoC through an ACPM callback, which the stand-in
does not provide (`AoC reset timeout ... ret=-19`), and gives up. Until the
ACPM AoC reset is implemented, restarting the AoC (or recovering from an AoC
crash) needs a reboot.

On stock, the reset works like this:
1. The driver arms the AoC's PCU watchdog.
2. The APM firmware resets the AoC and restores its TZPC.
3. The APM reports completion on ACPM channel 13 (the `aoc` node's
   `acpm-ipc-channel`). Google's ACPM driver delivers that message from its
   mailbox interrupt.

Mainline's ACPM driver reads a channel's ring only during its own
transactions, so the message is never delivered. A fix would drain channel
13's AP-bound ring in APM SRAM (initdata 0xA000) in the kernel, before the
AoC S2MPU is touched again. Reading that SRAM from userspace through
`/dev/mem` hung the SoC.
