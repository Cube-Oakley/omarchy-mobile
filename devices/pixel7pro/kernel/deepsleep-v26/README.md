# V26: deep sleep for daily use

`deepsleep-v26.patch` is a complete patch against the pinned base in
`kernel-base.txt`, as v25's is. It differs from v25 in one file,
`drivers/pmdomain/samsung/gs201-g3d-pd.c`: the GPU's S2MPU (0x20080000,
"always-on" in the stock DT) comes back from SYS_SLEEP with CTRL0 = 1, which
blocks the GPU's memory accesses; the MCU then fails to boot ("FW slow reset
failed" every second) and the desktop stops rendering. The GPU domain driver
now reads CTRL0 before powering the top domain off and writes it back after
powering it on, when the domain is up and the register is safe to touch.
CTRL0 is the only S2MPU register read or written: others have reset the SoC.

`kernel.config` is image `deepsleep-image-2`'s configuration with the
workspace path replaced by `@PIXEL_ROOT@`. Against v25 it adds ramoops
(pstore console) and a 256 MB default CMA area below 4 GiB
(`build-pixel-shell.py --cma 256M`), room for the camera's packed RAW10
buffers next to the display's.

The image's initramfs carries the current modules: `pixel-ufs` with the UFS
link-off path SYS_SLEEP needs (kernel/storage), `pixel-pd-off` with the camera
domains' power-on path (kernel/pd) and the touch driver's sleep mode. Built
with `scripts/build-pixel-shell.py --persistent-root --seconds 0 --cma 256M`.

Validated by RAM boot on October 1 with the modem running and the cable in:
13 deep sleeps through the daily suspend adapter (`pixel-suspend`), woken by
the modem and the RTC; UFS, USB, Wi-Fi (reloaded after alarm wakes), the GPU,
the display, the cameras, touch and the modem (IMS registered) all came back.
