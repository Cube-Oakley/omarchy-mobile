# libcamera for the Pixel 7 Pro

The Pixel uses the OnePlus package's libcamera 0.7.2 and its patches
(`devices/oneplus7pro/adapter/camera/libcamera/0001`–`0021`), plus four of
its own:

- `0022`: the simple pipeline runs the software ISP on the `pixel-camera`
  media device (`kernel/camera/pixel-camera.c`);
- `0023`: sensor helpers and properties for the IMX386 (ultrawide), S5K3J1
  (front), S5KGN1 (main) and S5KGM5 (telephoto): the Sony gain model
  1024 / (1024 − code), Samsung's code / 32, the 64-code black level and
  unit cell sizes.
- `0024`: low-light digital gain in the software ISP's AGC (October 1). Once
  the exposure and the analogue gain are at their limits, up to 4× more as
  digital gain. The ultra-wide and the telephoto sat at 16× analogue gain in
  a dim room and stayed darker than the main camera. It is generic, and could
  join the shared series.
- `0025`: the simple pipeline accepts a sensor configuration whose largest
  output is within 1% of the requested stream, then adjusts the stream to it
  (October 2). The debayer's 8-column margin kept the front camera's binned
  1920x1368 mode from covering a 1920x1080 video, so it ran the 3840x2736
  mode instead. Front video is now 1912x1080 from the binned mode, at half
  the software ISP time per frame; every other camera and role keeps its
  mode. Generic too.

Measuring frame rates over SSH needs the session's Mesa
(`__EGL_VENDOR_LIBRARY_FILENAMES=/opt/pixel-mesa/share/glvnd/egl_vendor.d/50_mesa.json`,
`GBM_BACKENDS_PATH=/opt/pixel-mesa/lib/gbm`, `LD_LIBRARY_PATH=/opt/pixel-mesa/lib`).
Without it the debayer falls back to llvmpipe on the CPU (`GL_RENDERER:
llvmpipe` in the eGL log) at about 43 ms a frame, and every camera reads
about 23 fps.

## Building on the phone

The image has gcc and pkg-config but no meson, ninja, patch or libyaml
headers, and its pacman mirror is a local proxy that is not running. What
worked on 2026-10-01:
1. `python3 -m venv /root/venv-build`, then pip install `meson ninja jinja2
   ply pyyaml`;
2. libyaml 0.2.5 from its release tarball into `/usr/local`, plus
   `/etc/ld.so.conf.d/usr-local.conf`;
3. libcamera v0.7.2 with all 23 patches applied on the host and the tree
   copied over, configured with the OnePlus PKGBUILD's meson options and
   `--prefix=/usr/local`.

The software ISP also needs a dma-buf heap: load `system_heap.ko`
(`CONFIG_DMABUF_HEAPS_SYSTEM=m`) so `/dev/dma_heap/system` exists. Do not load
`cma_heap.ko`: libcamera prefers a CMA heap for the ISP's output buffers
(50 MB each at 4080×3072), which then fills the CMA area the capture DMA needs
below 4 GiB, and the camera fails to start with ENOMEM.

## Tuning

`imx386.yaml`, `s5kgn1.yaml`, `s5kgm5.yaml` and `s5k3j1.yaml` go in
`/usr/local/share/libcamera/ipa/simple/`. Without them the simple IPA falls
back to `uncalibrated.yaml`, which has no autofocus and no colour matrix: the
lenses stay where the driver parks them and Omarchy Camera's multi-frame
stills get no matrix. The files follow the OnePlus ones: black level 64 (in
16-bit units), contrast 1.2 and saturation 1.1, autofocus on the ultrawide
and main, the IMX519 colour matrices for the Sony ultrawide and the identity
for the three Samsung sensors until they are calibrated.

## GPU debayer

The software ISP debayers on the GPU through EGL only with the Pixel's own
Mesa, the one the shell runs with: set `LD_LIBRARY_PATH=/opt/pixel-mesa/lib`
and `GBM_BACKENDS_PATH=/opt/pixel-mesa/lib/gbm`. With the system Mesa, EGL
fails ("failed to create dri2 screen") and the ISP falls back to the CPU at
about 5 fps.

With the GPU path, two things in `pixel-camera.c` set the frame rate
(2026-10-01, ultrawide at 2016×1508):
- **Line stride.** Mali's Mesa refuses to import a dma-buf whose stride is
  not a multiple of 64 bytes ("WSI pitch not properly aligned") and the
  frame is then uploaded by the CPU. The driver pads RAW10 lines to 64.
- **Cached buffers.** The ISP's statistics read every frame with the CPU.
  Through vb2-dma-contig's default uncached mapping that took 48 ms a frame
  (20 fps from a 60 fps sensor); with non-coherent (cached, synced on queue
  and dequeue) buffers it takes 3.8 ms and the debayer 5.5 ms, so the
  sensor's 60 fps (the main camera's 120) come through.

## Camera app

`overlay/mobile/camera` (the OmarchyCamera QML module) builds on the phone
with cmake from the same venv and
`PKG_CONFIG_PATH=/usr/local/lib/pkgconfig`. The app's lens buttons know the
Pixel's sensors (`imx386` 0.5×, `s5kgn1` 1×, `s5kgm5` 5×); it opens the
main camera first, as it sorts the back cameras by pixel count. Preview,
photos and the front camera worked on 2026-10-01.
