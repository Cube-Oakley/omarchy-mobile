# Webcam nodes for the camera bridge

The phone's cameras are raw sensors that only libcamera turns into
pictures. The browser (getUserMedia) and Android apps (Waydroid's external
camera HAL) only know plain V4L2 webcams. `overlay/mobile/camera-bridge`
writes libcamera's frames, upright and as YUYV, into two
[v4l2loopback](https://github.com/umlaeute/v4l2loopback) nodes:
`/dev/video20` (front) and `/dev/video21` (back). Each bridge turns its
camera on only while an app captures, which v4l2loopback signals to the
writer (its client-usage event, 0.13 and later).

## Build

Upstream v0.15.1 with `0001-kernel-7.3-file-based-v4l2-fh.patch`:
```sh
git clone --depth 1 --branch v0.15.1 https://github.com/umlaeute/v4l2loopback.git
cd v4l2loopback && git apply …/0001-kernel-7.3-file-based-v4l2-fh.patch
make -C devices/pixel7pro/mainline/linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=$PWD modules
```
The module goes to `/usr/local/lib/omarchy-mobile/camera/v4l2loopback.ko`.
The boot script's `start_cameras` loads it after the camera driver:
`devices=2 video_nr=20,21 card_label="Front camera,Back camera"
exclusive_caps=1,1 max_buffers=2`. With exclusive capabilities the browser
lists a node once its bridge has set the format, as a capture device only.

## The patch

Kernel 7.3 changed two things v4l2loopback 0.15.1 predates:
- `v4l2_fh_add` and `v4l2_fh_del` take the file. The compiler found this.
- The ioctl handlers get `NULL` where the file handle used to be. The
  compiler did not find this: v4l2loopback's `fh_to_opener(fh)` turned NULL
  into a pointer just below it, and the first `VIDIOC_QUERYCAP` (udev's
  probe) oopsed. The phone panicked and rebooted, with the log kept
  (October 7). Every lookup now goes through `file_to_v4l2_fh(file)`.
