# Qt WebEngine at the panel's refresh rate

Chromium's frame clock follows the interval reported with each presented
frame and runs at 60 Hz when it is zero, which Qt WebEngine always reported:
flings, CSS and script animations and video ran at 60 Hz on the 120 Hz panel
(only finger drags reached 120, one frame per touch event).
`0002-chromium-frame-clock-at-display-rate.patch` reports the primary screen's
refresh interval (`QTWEBENGINE_DISPLAY_HZ` overrides it). `0001` is Arch Linux
ARM's own change to the package, so the rest matches what the phone ships.

`0003-frame-pacing-with-qt.patch` paces Chromium by Qt's frames. Without it
Chromium took the start of its own swaps as the vsync, so its 120 Hz clock
was in step with nothing: its frames doubled up or skipped against Qt's. It
also waited for Qt to take each frame before drawing the next, which left a
7 ms draw no slack. Now Qt's frame times set Chromium's clock (the browser
starts Chromium's frame 3.5 ms ahead, `QTWEBENGINE_FRAME_PHASE_US`), and
Chromium may run one frame ahead of Qt, the newer frame replacing an untaken
one. A window that is not drawn still stops Chromium after one frame: X with
its feed's video went from about 150 % CPU behind other apps to about 10 %.
`QTWEBENGINE_FRAME_STATS=1` prints where Chromium's frames land.

The library is cross-built on the laptop for the phone's `qt6-webengine
6.11.2-1` and replaces `/usr/lib/libQt6WebEngineCore.so.6.11.2` there.
**Installed on October 6 (early morning):** patches 0001 to 0005 with V8's write
barriers on (below), SHA-256 `2fadfb5b…`; the 0003 library it replaced is
`/root/libQt6WebEngineCore.so.6.11.2.0003` on the phone, Arch's own
`…arch-6.11.2-1`.

## Building (October 5, 2026)

All in `~/.cache/omarchy-webengine`:

1. **Sysroot** from the phone: `tar -C / -cf - usr/include usr/lib
   usr/share/pkgconfig usr/share/qt6 lib lib64` over SSH (without
   `usr/lib/{python3.14,kitty,guile,firmware,udev,modules}`), absolute
   symlinks made relative, and **`sysroot/include -> usr/include`**: GCC's
   headers come through `lib/gcc/…/../../../../include`, which the file system
   resolves through `lib -> usr/lib` but ninja shortens to `sysroot/include`.
   Without the link every ninja run found those headers missing and rebuilt
   almost everything.
2. **Toolchain**: clang/lld for `aarch64-unknown-linux-gnu` with
   `CMAKE_SYSROOT` (the file is `aarch64.toolchain.cmake` there); gperf and
   Python's html5lib in a private `tools/` and `venv/`.
3. **Source**: `qtwebengine-everywhere-src-6.11.2.tar.xz` with the patches
   (`patch -p1`).
4. **Configure** as Arch does (system ICU, re2, ffmpeg, openh264;
   proprietary codecs; PipeWire; Kerberos; Vulkan; no tests or examples)
   with `-DCMAKE_TOOLCHAIN_FILE=… -DQT_HOST_PATH=/usr`, **and
   `-DFEATURE_webengine_embedded_build=OFF -DFEATURE_webengine_printing_and_pdf=ON
   -DFEATURE_webengine_extensions=ON -DFEATURE_webengine_webrtc=ON`**: any
   cross build turns on Qt's "embedded build", which drops WebRTC,
   printing/PDF and extensions and sets `use_embedded_config`. The first
   library built that way lacked symbols the phone's own
   libQt6WebEngineQuick (extensions) and libQt6WebEngineWidgets (printing)
   need, so it would not load. The full option set is in
   `build/CMakeCache.txt`. Then `use_lld=true` in
   `build/src/core/Release/aarch64/args.gn` after every configure (Qt's GN
   arguments for the cross build keep the system linker otherwise).
   Add also **`v8_disable_write_barriers=false` and
   `v8_enable_single_generation=false`** there: QtWebEngine 6.11's copy of
   V8 sets the first true (upstream V8 14.0 has false), which compiles V8
   without write barriers, so without incremental marking, the memory
   reducer and the young generation. Garbage only Blink makes is then never
   collected: a self-hosted app's animated avatar grew its renderer by 5 to 8 MB a
   second, to 5 GB, on every library including Arch's (October 6). Only V8
   rebuilds (463 steps).
   Before installing, check that every binary on the phone linking
   libQt6WebEngineCore finds all its symbols in the new one.
5. **Build** in a memory-capped user unit, never from the terminal session
   (a 14-job build filled 27 GB of RAM and 27 GB of swap, and systemd-oomd
   killed the terminal). `cmake --build` starts Chromium's own ninja with
   no `-j` (all threads; ninja ignores `NINJAFLAGS`), so run that one
   directly with a job limit, then the outer build for the rest:
   `systemd-run --user -p MemoryHigh=15G -p MemoryMax=17G -p
   MemorySwapMax=4G …` with `TMPDIR` on disk and `NODEJS_EXECUTABLE` set,
   `ninja -C build/src/core/Release/aarch64 -j 4 QtWebEngineCore` from
   `build/src/core`, then `cmake --build build --target WebEngineCore`.
   The outer build runs syncqt first, which can leave Qt's own sources
   (`obj/QtWebEngineCore/*`) out of date and start the unlimited inner
   ninja after all: add `-j 4` to the `/usr/bin/ninja -C …/aarch64
   QtWebEngineCore` command in `build/build.ninja` (configure writes it
   again).
   V8's jumbo objects take up to about 4 GB each: unlimited, they reached
   the 15 GB cap and oomd killed the unit. About 18 000 steps.

## Installing

On the phone, as root, with the browser and web apps closed: keep
`/usr/lib/libQt6WebEngineCore.so.6.11.2` as a backup, put the new library in
place, `sync`. A `qt6-webengine` package update replaces it again.

Before that, `cmake -P` with `file(RPATH_REMOVE FILE …)` on a copy (the
build tree's RUNPATH is `:::::::`, empty entries that mean the current
directory) and `llvm-strip --strip-unneeded` (299 MB to 190 MB, the same
2349 exported symbols). It needs `libatomic.so.1`, which the phone has.

Installed October 5, 2026 (Arch's own library kept as
`/root/libQt6WebEngineCore.so.6.11.2.arch-6.11.2-1`). A fling on
Wikipedia's Linux article went from a 16.7 ms median frame interval
(about 89 moving frames) to 8.3 ms (111 to 133 frames), three runs each.

**0003, October 5.** Measured with `scroll-frames-test.py` (frames the panel
shows, from the scanout driver's counter) on the X web app, two flings per
variant, phone warm: refreshes without a new frame per 1.5 s fling averaged
56 with 0002 alone, 62 with 0003 at phase 0 and 39 at -3.5 ms; drags were
near perfect with all (0–8 of 84). The rest of X's misses follow its content
(a 2400×1700 feed video decoded in software, new images rastered as the
fling outruns them), not the pacing.

**0004, October 5: built, measured, then kept.** A queue of two
finished frames, shown in order one per Qt frame, instead of one frame a
newer one replaces (0003 showed about a third of X's frames replaced and
never shown). On X, with the phone hot (skin guard at level 4, big cores at
1.4 GHz), it was no better than 0003: 71 to 82 frames per fling against 69
to 74. Its statistics name the real limit: 90 % of presents found a frame
already waiting, so Qt in the browser takes only about 70 frames a second.
The next step is Qt's own frame rate in the browser process, measured with
the phone cool.

**Qt's frame rate, October 6.** Qt's render thread in the browser spends
each frame in its swap (sync and render about 0 ms, swap 7 to 16 ms), waiting
for Hyprland's frame callback, and a 16 ms swap is a refresh Hyprland
skipped. A swap interval of 0 (Qt's render thread not waiting in the swap)
was no better on X; neither was Hyprland at realtime priority. When warm the
frames fall into a steady 60 Hz lockstep: one stage takes just over 8.3 ms.

**0005, October 6: the trace category search.** A sampling profile of X
(`devices/pixel7pro/scripts/ui-tests/perf-sample.py` on the phone,
`perf-report.py` with the unstripped library on the laptop) put
`perfetto::internal::TrackEventCategoryRegistry::Find` at the top of every
Chromium thread: 46 % of Viz's thread, 39 % of the browser's main thread,
34 % of the renderer's compositor, 17 % of the renderer's main thread and
10 % of the GPU thread. Upstream Perfetto computes a trace event's category
index at compile time; this copy passed the lookup straight to the calls,
so every trace point, traced or not, searched 275 category names (twice per
scoped event). `0005-perfetto-compile-time-category-index.patch` restores
the constexpr index, and lets clang (which defines `__GNUC__`) use the
constexpr form of `TRACE_EVENT_CATEGORY_ENABLED` too. About 2700 steps
rebuild. The GPU thread's own next cost was Mesa (see
`devices/pixel7pro/adapter/userspace/mesa`, patch 0002).

Measured on X with the phone cool, alternating the library before and after
0005 (both with Mesa's 0002), CPU per frame while scrolling, three pairs:

| thread | before | 0005 |
|---|---|---|
| Viz compositor | 1.01 ms | 0.35 ms |
| renderer compositor | 2.29 ms | 0.86 ms |
| browser main (Qt's GUI thread) | 2.46 ms | 1.28 ms |
| renderer main | 4.22 ms | 3.29 ms |
| GPU thread | 0.46 ms | 0.36 ms |

About 38 % less CPU per frame in all; drags and flings were already whole
120 Hz runs with the phone cool, so the gain shows as heat, battery and
warm-phone frame rate. 0004 is in the build: with Qt as the limit it was
never worse than 0003.

Test traps: `scroll-frames-test.py` histograms count frame *gaps* in
refreshes, so `1x` is consecutive 120 Hz frames, not one missed; its fling
window (1.5 s from the lift) counts the stillness after a short fling as
"refreshes without a new frame", and anything else animating on screen as
frames (X's live-broadcast pill inflated October 5's fling counts). Read the
gap list. An `LD_LIBRARY_PATH` set for an A/B must keep
`/opt/pixel-mesa/lib`, or the page renders in software (and a software
buffer scanned out once froze the display until a power cycle). A blanked or
locked screen reads as zero frames.

**0006, October 6: hardware video decoding.** The Pixel 7 Pro's MFC
(`devices/pixel7pro/kernel/mfc`, s5p-mfc) decodes H.264, HEVC, VP8 and VP9
through Chromium's V4L2 stateful decoder. Configure it in `args.gn`:
`use_vaapi=false` (the two exclude each other on Linux) and
`use_v4l2_codec=true`. Rebuilding takes about 1700 steps. The patch:
- **On by default:** turns `AcceleratedVideoDecoder` on with V4L2, as it
  is with VA-API.
- **The device:** looks for the decoder at `/dev/video-dec0` (a udev link,
  `devices/pixel7pro/session/70-omarchy-mobile.rules`), so the camera
  nodes are never probed.
- **Whole access units:** queues the SPS, PPS and SEI with the next
  picture, in one buffer. The MFC finds no header in a buffer holding only
  an SPS (`ERR_HEADER_NOT_FOUND`, the instance lost), as Chromium's old
  V4L2 decoder knew.
- **The decoder's own buffers, shown directly:** the MFC runs without its
  IOMMU and writes only into its own CMA area, so the CAPTURE buffers are
  the driver's (MMAP), exported as DMA-bufs. Its two-buffer NV12 (`NM12`)
  is listed with NV12 as renderable, so there is no ImageProcessor copy.
  Six extra buffers cover the renderer's queue: with references + 2, the
  renderer held the spare frames and decoding stopped after 4.

The kernel side needed two fixes too: s5p-mfc now sends the source-change
event once it has parsed the first header, and its poll no longer reports
an error while that event is pending.

**Installed October 6, night** (SHA-256 `b16f3a85…`; the 0005 library is
`/root/libQt6WebEngineCore.so.6.11.2.0005`).
- **Tests:** a 720p H.264 MP4 and a 1080p fragmented MP4 through Media
  Source Extensions (as X plays) both run at 30 fps with no dropped frames.
  At 1080p the GPU process uses about 8 % of a CPU.
- **A hidden window:** with the window hidden (locked phone), the
  renderer's queue fills and decoding pauses until the window is drawn
  again, as the frame pacing of 0003 intends.

**0007, October 7: direct presentation and partial redraws.** A page that
changes a few pixels had the whole screen redrawn three times: by Chromium
into its buffer, by Qt Quick into the window, and by Hyprland into the
output. On a test page (`devices/pixel7pro/scripts/ui-tests/damage/`), a
57-pixel square spinning at 120 Hz cost as much as a full-screen colour
change: GPU and memory were about three quarters of its 1.2 W, and Hyprland
did half the GPU work. X's animated Spaces avatars are such an animation.
`0007-direct-presentation-and-partial-redraws.patch`:
- **Partial redraws in Chromium:** the output device reports partial swaps
  and, with each swap, what of the next buffer is older than the newest
  frame, so viz redraws only that. A skipped swap keeps the area (one line
  in viz).
- **Frames to Hyprland directly:** for a window with the dynamic property
  `_q_webengine_direct` and an alpha channel (the browser sets both,
  `OMARCHY_BROWSER_DIRECT=0` turns it off), each frame goes to the
  compositor as a `wl_subsurface` below the window, with the area that
  changed (`damage_buffer`) and a `wp_viewport` crop; Qt draws a transparent
  hole there (blending off), so menus, sheets and the bar still draw over
  the page. Qt draws the frames itself again while the page is faded,
  scaled, hidden, or pictured (`grabToImage`, the tab overview). A page
  hidden while its window is off screen (the browser hides its pages 1.5 s
  after, so Chromium treats them as background tabs) keeps its last frame
  in the sub-surface: Qt cannot draw an off-screen window, the frame it last
  drew has the hole, and the switcher's card (Hyprland's capture of the
  window) showed the wallpaper there until this (October 7). The kept frame
  goes with the window's first frame back if the page is still hidden. The
  libwayland-client calls are looked up at run time (the library does not
  link it); its objects have their own event queue and thread, which also
  waits for each frame's GPU fence. `QTWEBENGINE_DIRECT_DEBUG=1` logs
  entering, leaving and each attach.
- **Hyprland 0.56:** `place_below` the parent works, but a sub-surface
  without an alpha channel (XBGR) is drawn white there, so the buffers keep
  their alpha format; the opaque region says they are opaque.

Measured on the phone (GPU cycles per client from Panthor's fdinfo with
`/sys/devices/platform/28000000.gpu/profiling` on; ODPM rails, whose
`energy` file holds the energy *since the last read*):

| 57 px square at 120 Hz | Qt draws | direct |
|---|---|---|
| browser GPU | 400 Mcycles/s | 9.6 Mcycles/s |
| Hyprland GPU | 435 Mcycles/s | 171 Mcycles/s |
| GPU rail | 100 % | 42 % |
| memory rails | 100 % | 49 % |
| Qt's render thread | 11 % of a core | none |

A full-screen change costs what it did. X at rest (its loop paced to 10
frames a second by the browser, videos paused), SoC and display rails: the
0006 library 689 mW, 0007 with Qt drawing 640 mW, 0007 direct 566 mW. Flings
on X: 142 to 154 frames per 1.5 s, 28 to 36 refreshes without a new frame
(the 0006 library about 36 to 44 that day). Hyprland still spends about
1.3 Mcycles on a small change, and the panel still gets whole frames.

**Installed October 7** (SHA-256 `41bafa9e…` with the kept frame; the first
0007 build, `0f5eb671…`, is `/root/libQt6WebEngineCore.so.6.11.2.0007` and the
0006 library `/root/libQt6WebEngineCore.so.6.11.2.0006` on the phone). The
switcher's cards also need Hyprland patch 0002 (`overlay/mobile/hyprland`):
Chromium leaves an opaque page's alpha at 0. Rebuilding after
changing these files takes about 30 steps.

**0008, October 8: decoder failures fail the decode; three MFC instances.**
`0008-v4l2-decoder-failures-fail-the-decode.patch` (description in the patch).
That evening X ran the MFC's 128 MiB CMA area dry; a decoder carried on
without frame buffers until the firmware failed, then the GPU work called
DQBUF in a loop for minutes (heat, drain). With 0008, s5p-mfc's matching
fixes (`devices/pixel7pro/kernel/mfc/0003`) and the 384 MiB area of kernel
v45, `devices/pixel7pro/scripts/ui-tests/mfc-pressure-test.sh` (five looping
1080p videos on one page, a scratch profile, DevTools' Media domain via
`cdp-media.py`) shows three videos on the MFC with no dropped frames, two in
FFmpeg ("Too many decoder instances, max=3"), no failed allocation, no codec
error. On the 128 MiB area all three hardware decoders failed to get their
buffers and fell back to FFmpeg cleanly. Built with
`~/.cache/omarchy-webengine/build-mfcerr.sh` (two objects and the link, under
a minute).

**Installed October 8** (SHA-256 `a5faedc4…`; the 0007 library with the kept
frame, `41bafa9e…`, is `/root/libQt6WebEngineCore.so.6.11.2.0007-keepframe`).
