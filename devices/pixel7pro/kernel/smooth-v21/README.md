# V21: 120 Hz, GPU and CPU scaling

`smooth-v21.patch` is a complete patch against the pinned base in
`kernel-base.txt`, with every added file; do not stack it on v20. It
reverse-applies cleanly to the working source (40 files). `kernel.config` is
image S's configuration with the workspace path replaced by `@PIXEL_ROOT@`.
The two overlay sources used by the build are copied here.

New since v20:

- `drivers/gpu/drm/sysfb/pixel_bandwidth.h`: 120 Hz also accepts a normal
  boot's 2028 MHz memory clock and holds it. Only the 421 MHz fastboot rate is
  raised to the 1352 MHz floor.
- `arch/arm64/kernel/pixel_gpu.c` and `pixel-gpu-overlay.dts`: Panthor devfreq
  scales the GPU top clock over the stock table (302, 470, 603, 750 and
  885 MHz). A clock notifier keeps the shader-stack clock at the stock pair,
  never above the top clock, and refuses any change while the GPU is
  power-gated. A thermal cap on the G3D zone limits it to 603 MHz from 60 C
  and 302 MHz from 65 C, releasing below 55 C.
- `arch/arm64/kernel/pixel_acpm.c` and `pixel-acpm-overlay.dts`: stock CPU
  rates up to LITTLE 1803, MID 1999 and BIG 2048 MHz. A LITTLE
  minimum-frequency request follows the stock `med_ank_perf` and `big_ank_perf`
  tables. The CPU thermal zones poll every 250 ms (100 ms while throttling),
  throttle from 75 C and reboot at 95 C.

Why the CPU stops short of stock: at 2850 MHz one BIG core's sensor went from
50 to 87 C within a second, and the polled policy's critical trip rebooted the
phone. Stock uses interrupt-driven sensors with control at 100 C and
emergency limits at 110 and 120 C. At the v21 caps, one BIG core peaks at
68 C, and all eight cores hold 74-75 C by throttling.

Image S SHA256: `e519c44ee982d6d066eeeb26526ae4225cb385d03713485738e23084888732c7`
(installed in `boot_a`). Evidence: [smoothness record](../../docs/smoothness-20260928.md).
