# v40: thermal limits in stock's range, and a skin guard (2026-10-05)

v39 with the thermal policy moved toward stock's. The user asked to expand
the thermals if safe: scrolling and animations were losing frames once the
phone was warm, because the GPU dropped to 603 MHz at 60 C and 302 MHz at
65 C, and the CPUs were capped from 75 C.

## What stock does

From the stock device tree (`vendor_kernel_boot`) and the vendor partition's
`/etc/thermal_info_config.json`:
- **SoC zones** (BIG, MID, LITTLE, G3D, TPU): power allocator on from 75 C,
  control targets 95 and 98 C, hardware DFS trips 102, 105 and 115 C, hot
  120 C.
- **The real limit is the skin**: the thermal HAL's VIRTUAL-SKIN, the hottest
  of four weighted pairs of the S2MPG13 board thermistors, throttles CPUs
  and GPU with a PID at 46.5 C (lighter levels from 37–41 C).

## Changes (`0001-thermal-stock-control-range.patch`)

- **`pixel_acpm.c` fast CPU cap:** stock rates below 85 C, then 2048/1999 MHz
  (BIG/MID), 1582/1663 from 91 C and 1106/1197 from 95 C (was 75/85/92 C).
  It polls every 20 ms from 70 C (was 60 C).
- **`pixel_gpu.c` GPU cap:** uncapped below 85 C, then 750, 603, 470 and
  302 MHz from 85, 90, 94 and 97 C, each step clearing 5 C lower (was 603
  from 60 C, 302 from 65 C). New `skin_max_khz` parameter: the skin guard's
  limit, applied with the chip's (the lower wins, only while the GPU is
  powered).
- **`pixel-acpm-overlay.dts`:** CPU zones throttle from 88 C (was 75) and
  reboot at 99 C (was 95); G3D, ISP, TPU and AUR reboot at 99 C (was 80).
- **`kernel/odpm`:** the quiet, qi, display and GNSS thermistors (S2MPG13
  channels 1, 2, 5, 7) are on with the port's three, and named in
  `parameters/temps`.
- **`thermal/pixel-skin-guard.py`** (started by the boot script after
  pixel-odpm): stock's VIRTUAL-SKIN formula every 3 s; limits from 41 C
  skin (BIG 2.4 GHz), 43 (BIG 2.05, GPU 750), 45 (BIG 1.75, GPU 603), 46.5
  (BIG 1.43, GPU 470), 48 and 52 C; a level falls after 10 s a degree below.
  CPU limits go to `scaling_max_freq`, the GPU's to `skin_max_khz`.

## Validated by RAM boot

200 s of eight busy CPU loops plus a full-screen WebGL shader, plugged in:
- chip held at 86–87 C by the fast cap (2048/1582 MHz steps), no reboot, no
  brownout;
- G3D peaked at 79 C; the GPU stayed at 885 MHz for 70 s (v39 would have
  dropped to 302 MHz after about 7 s), then the skin guard took it to 750
  and 603 MHz;
- skin estimate 36.7 → 45.2 C, guard levels 1, 2, 3 at 41.2, 43.1, 45.0 C.

Wi-Fi, cellular data, audio, sensors and Bluetooth came up as on v39.
