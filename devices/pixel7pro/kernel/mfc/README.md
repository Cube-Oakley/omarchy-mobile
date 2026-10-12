# Video codec (Samsung MFC v15.6)

GS201's MFC encodes and decodes H.264, HEVC, VP8 and VP9. The research
(`.work/research/reports/Pixel 7 Pro video codec feasibility.md`) recommends a
GS201 variant of mainline's `s5p-mfc`, with the SysMMU left in bypass, if the
firmware starts and answers SYS_INIT without it.

## First light: it does (2026-10-06)

`pixel-mfc-light.c` is a test module, never loaded at boot.

```sh
make -C devices/pixel7pro/mainline/linux ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- M=<copy of this directory> modules
```

**What it needs:**
- **The domain on:** the boot powers the MFC domain off with the other unused
  blocks (`kernel/pd`). The test then ran with a boot flag (`mfc-dev`, since
  removed) that left it on as the bootloader left it. Now `echo mfc=1 >
  /sys/module/pixel_pd_off/parameters/power` powers it on. The module
  refuses to run unless the PMU says the domain is on.
- **The firmware:** `mfc_fw.bin` from the user's own vendor partition
  (`/firmware/mfc_fw.bin` in `vendor_a.img`, 716,196 bytes, version word
  0x44240614). It goes in `/usr/lib/firmware/omarchy-mobile/` on the phone,
  never in git.
- **A watchdog:** the modem's `wdtkick`, because a bad MMIO access hangs the
  SoC.

**Stage 1 (`stage=1`) only reads registers.** With the domain left on:

| Register | Value |
| --- | --- |
| PMU MFC_STATUS | 1 |
| CMU_MFC `MFC_USER` mux | 0x10 |
| NOCP divider | 3 |
| Every gate read (MFC aclk, LH AXI, SysMMU, SSMT) | 0x00200000 (on) |
| Both SysMMUs' CTRL | 0x24 (disabled, as for the camera's CSIS DMA) |
| Both S2MPUs' CTRL0 | 0 |

The MFC's 0xF000 registers (MFC_VERSION, FW_VERSION and the status word)
read as noise until the firmware runs: the firmware writes them.

**Stage 2 (`stage=2`) boots the firmware.** It follows the stock driver's
`mfc_core_run_init_hw`, polled instead of using interrupts:
1. Clear the registers and reset the MFC (0x1fff, then 0).
2. Put the firmware's base in `RISC_NONSECURE_BASE_ADDR` (0x68; the stock DT
   sets `security_ctrl`).
3. `MFC_OFF` 0, then `RISC_ON` 1. FW_STATUS_RET (12) comes back in about 2 ms.
4. SYS_INIT with a 30 KB device context. SYS_INIT_RET (1) comes back, error
   0, and the firmware reports `D` 2024-06-14 and MFC_VERSION 0x15060000.

**Lesson: buffers must sit above the firmware.** The first try put the
context in a CMA block of its own, below the firmware, and SYS_INIT failed
with ERR_RET (error 0x33). With the context in the same 2 MB block, after
the firmware's 1 MB, it worked. The stock driver keeps its firmware at the
lowest address too. So a driver needs one pool with the firmware at its
bottom, as mainline's FSD (MFC v12) has from a reserved 128 MB region.

Unloading stops the RISC (bus idle, `RISC_ON` 0, reset) before the memory is
freed.

## s5p-mfc decodes H.264 (2026-10-06)

The phone runs **kernel v42** (boot_a since 2026-10-06; the image is
`out/mfc-v42`, sha256 `85ff2edf…`, build ID `5c800a9c…`). It is v41 with the
codec's own CMA area: `0001-dma-pixel-mfc-cma-area.patch` adds
`pixel_mfc_cma=SIZE` (`scripts/build-pixel-shell.py --mfc-cma 128M`), a CMA area
below 4 GiB exported as `pixel_mfc_cma_area`. With it, CmaTotal grows by 128 MB.
The area is reusable: other allocations borrow it while the codec is idle.

Three modules run on top of it (`/root/tools/mfc` on the phone, test only,
loaded in this order):
1. `v4l2-mem2mem.ko`, built from the tree, since the camera's root copies lack
   it.
2. `pixel-mfc-dt.ko` (`pixel-mfc-dt.c`): it applies `pixel-mfc-overlay.dts`, a
   `google,gs201-mfc` node with the core's registers and SPI 504. The stock
   DT's MFC nodes point at SysMMUs, a power domain and a clock without
   drivers here.
3. `s5p-mfc.ko`, with `0002-s5p-mfc-gs201-variant.patch` applied. The patch:
   - **Version:** treats the MFC as v12.
   - **Memory:** gives the device `pixel_mfc_cma_area`, so the firmware's
     8 MiB block comes first, at the bottom, and every buffer lands above it.
   - **Firmware base:** writes it to `RISC_NONSECURE_BASE` (0x68), and clears
     `MFC_OFF` before `RISC_ON`.
   - **Commands:** polls `FIRMWARE_STATUS_INFO` before each one.
   - **Registers:** moves `D_DISPLAY_ASPECT_RATIO`, `D_DISPLAY_EXTENDED_AR`
     and `D_H264_INFO` up by 0x100.
   - **No clocks:** the boot leaves CMU_MFC on.
   - **No encoder yet:** v15 has decoder registers where v12's encoder writes
     its frame size, and the camera app would pick the encoder up for
     recording.

**What works.** The decoder is `/dev/video4` (H.264, H.264 MVC, H.263,
MPEG-1/2/4, Xvid, VC-1, VP8, HEVC, VP9 in; NV12, NV21, YUV420 and YVU420 out).
GStreamer's `v4l2h264dec` decoded 60 frames of 720p H.264 from `x264enc`;
the input needs `parsed=(boolean)true` in its caps, since the phone has no
`h264parse`. Luma PSNR against the source was 44 to 57 dB (57 at keyframes).
A first comparison read 24 dB because the reference had been generated as
BT.709 and the encoder's input as BT.601.

## Power: on only while decoding (2026-10-06)

The patch powers the MFC domain through `pixel-pd-off` (`kernel/pd`), which
has the stock save list, the context SMC and the S2MPU restore:
- **Probe:** off. The boot powers the domain off with its list saved;
  s5p-mfc then finds `pixel_pd_power` with `symbol_get`, so it loads without
  `pixel-pd-off`. In that case it refuses the codec while the PMU reports the
  domain off, since MMIO to it would hang the SoC.
- **First open, last close:** `s5p_mfc_power_on` powers the domain on before
  the firmware starts, and `s5p_mfc_power_off` powers it off after the
  firmware stops. Each open starts the firmware again (about 4 ms), as
  mainline does. udev and the browser open the node to read its formats,
  and each of those opens costs a full power cycle.
- **System sleep with a video open:** the deep sleep's PMIC modes drop the
  rails under the domain, which a powered display domain does not survive
  either. The suspend hook sends the firmware SLEEP, then powers the domain
  off, and refuses the sleep if it cannot. Resume powers it on and sends
  WAKEUP. The wakeup's RISC_ON clears `MFC_OFF` first, as init does.

**Tested:**
- Two 1080p H.264 runs of 120 frames each, with the domain going on and off
  around each.
- A live 2 fps decode that stayed open through a 19.6 s deep sleep
  (SYS_SLEEP). The domain went off at suspend and on at resume, and all 180
  frames came out.
- A boot that loads the codec by itself.

A first decode test failed with error 100 (`ERR_UNSUPPORTED_FEATURE`) and 0x0
dimensions. The power cycle was not the cause: without a format in the raw
caps, `x264enc` chose 4:4:4, which the MFC does not decode. Give the test
source `format=I420`.

## At boot

`scripts/pixel-persistent-session.sh` loads the three modules from
`/usr/local/lib/omarchy-mobile/mfc` after the cameras (it needs their
videobuf2 modules). udev's `v4l_id` opens the new node once, which starts
the firmware. While that runs, `/var/lib/omarchy-mobile/mfc-starting` marks
the attempt, and a boot that finds the marker left from the last one leaves
the codec off. `session/70-omarchy-mobile.rules` links the decoder as
`/dev/video-dec0`, the name the browser's decoder opens. The `mfc-dev` flag
is gone. `echo mfc=1 > /sys/module/pixel_pd_off/parameters/power` powers
the domain on for `pixel-mfc-light`.

Build (no `CONFIG_MODVERSIONS`; the tree's `Module.symvers` predates v42, so
`pixel_mfc_cma_area` comes from a one-line symvers file):
```sh
make -C devices/pixel7pro/mainline/linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    M=<dir with v4l2-core/v4l2-mem2mem.c and "obj-m += v4l2-mem2mem.o"> modules
make … M=<copy of drivers/media/platform/samsung/s5p-mfc> CONFIG_VIDEO_SAMSUNG_S5P_MFC=m \
    KBUILD_EXTRA_SYMBOLS="<pixel-mfc-cma.symvers> <mem2mem Module.symvers>" modules
make … M=<copy of this directory> modules        # pixel-mfc-dt.ko
```

## The browser decodes with it (2026-10-06)

Chromium's V4L2 stateful decoder (QtWebEngine patch
`overlay/mobile/browser/qtwebengine/0006`) needed three more changes here,
all in the patch:
- **The source-change event.** The stateful decoder interface signals the
  parsed header with `V4L2_EVENT_SOURCE_CHANGE`. s5p-mfc sent it only for a
  resolution change mid-stream; GStreamer works without it because `G_FMT`
  waits for the header. It is now queued when the first header is parsed
  (in `MFCINST_GOT_INST`).
- **Poll.** With OUTPUT streaming, nothing queued and CAPTURE not yet
  set up, poll returned `POLLERR` and hid the pending event. It now fails
  only when neither queue streams and no event waits.
- **More CAPTURE buffers.** The browser shows the decoder's own buffers and
  holds several for its playback queue, while `pb_count` must stay queued
  for the firmware to decode. GS201 allows `pb_count` + 12 (was + 5).

720p and 1080p H.264 play at 30 fps with no dropped frames, also through
Media Source Extensions. At 1080p the GPU process uses about 8 % of a CPU.
GStreamer still decodes.

## Out of memory, cleanly (2026-10-08)

On October 8 X, in the web app, ran the 128 MB area dry. Allocations from it
failed (`cma: pixel-mfc: alloc failed`, -EBUSY covers a full area too), and
dma-direct quietly gave the decoder ordinary pages instead: they came from
0x83200000, the area (and the firmware) at 0xeb200000. The firmware cannot
reach memory below its base; it failed with error 0x37 and the instance went
to `MFCINST_ERROR`. s5p-mfc then marked every CAPTURE buffer done with an
error and answered POLLIN for them while DQBUF failed with EIO, and Chromium
polled and dequeued in a loop (5.7 million suppressed messages in 7 minutes,
heat). `0003-s5p-mfc-no-busy-poll-or-unreachable-buffers.patch`:
- **poll:** EPOLLERR alone once the instance is in `MFCINST_ERROR`;
- **reach:** a vb2 buffer (`buf_init`, both queues) or a codec buffer
  (`s5p_mfc_alloc_generic_buf`) below the firmware's base is refused with
  -ENOMEM, so REQBUFS fails where the client sees it.

Chromium's side is QtWebEngine patch `0008` (failures fail the decode, and at
most three MFC decoders: three 2 MiB contexts fit the 8 MiB block beside the
firmware; three concurrent GStreamer decodes confirmed it). Kernel v45 makes
the area 384 MB (`--mfc-cma 384M`, `kernel/mfc-pool-v45`): Chromium asks for
16 CAPTURE buffers, about 48 MB at 1080p, about 55 MB per decoder with its
codec and input buffers, and a seek briefly holds two sets. The low zone had
1.42 GB free before; it now holds 640 MB of CMA (256 MB default + 384 MB).

`scripts/ui-tests/mfc-pressure-test.sh` (five looping 1080p videos, a scratch
profile, DevTools' Media domain via `cdp-media.py`): on the 128 MB area all
three hardware decoders failed to get buffers and fell back to FFmpeg, no
codec error, no busy loop; on 384 MB three decode on the MFC (no dropped
frames on two, the third shares the CPU with two FFmpeg decoders) with no
failed allocation in 70 s and three loops.

Installed October 8: s5p-mfc.ko `ca249846…` in `/usr/local/lib/omarchy-mobile/mfc`
(previous `/root/backups/s5p-mfc.ko.before-poll-error`), built as above from a
copy with this patch; reloaded with the boot script's marker.

## Next

1. **CMA size:** done October 8 (384 MB, three decoders; a fourth video
   decodes in software).
2. **DVFS:** the MFC clock is whatever CMU_TOP left (the stock table asks for
   134 MHz at 1080p30 and up to 666 MHz).
3. **Encoders:** v15's encoder registers (report §2.4) for the camera's
   recording.
