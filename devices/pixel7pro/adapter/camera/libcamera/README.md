# libcamera for the Pixel 7 Pro

The Pixel uses the OnePlus package's libcamera 0.7.2 and its patches
(`devices/oneplus7pro/adapter/camera/libcamera/0001`–`0021`), plus two of its
own:

- `0022`: the simple pipeline runs the software ISP on the `pixel-camera`
  media device (`kernel/camera/pixel-camera.c`);
- `0023`: sensor helpers and properties for the IMX386 (ultrawide), S5K3J1
  (front), S5KGN1 (main) and S5KGM5 (telephoto): the Sony gain model
  1024 / (1024 − code), Samsung's code / 32, the 64-code black level and
  unit cell sizes.

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
(`CONFIG_DMABUF_HEAPS_SYSTEM=m`) so `/dev/dma_heap/system` exists.

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
