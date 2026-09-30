# V22: full CPU rates under a fast thermal cap

`smooth-v22.patch` is a complete patch against the pinned base in
`kernel-base.txt`, with every added file; do not stack it on v21. It
reverse-applies cleanly to the working source (40 files). `kernel.config` is
image U's configuration with the workspace path replaced by `@PIXEL_ROOT@`.

New since [v21](../smooth-v21/README.md): the CPU OPP tables again reach the
stock maxima (MID 2348 MHz, BIG 2850 MHz). `pixel_acpm.c` runs a
`pixel-cpu-thermal` thread at SCHED_FIFO priority that reads the BIG and MID
sensors every 20 ms and caps both clusters with PM QoS:

| Hottest of BIG/MID | BIG max | MID max |
|---|---|---|
| below 75 C | 2850 MHz | 2348 MHz |
| from 75 C | 2048 MHz | 1999 MHz |
| from 85 C | 1582 MHz | 1663 MHz |
| from 92 C | 1106 MHz | 1197 MHz |

A level clears 5 C below its threshold, but no sooner than a second after it
was raised. The sensor swings about 10 C within one poll, so without that
hold the cap changed every 20-40 ms. A failed read applies the sustained caps.
The zones' own trips stay behind it: throttling from 75 C, critical at 95 C.

Measured on image U:
- **One BIG core:** 750 ms at 2850 MHz from cool, then 2048 MHz at 61-69 C.
- **All eight cores:** held at the sustained caps plus zone throttling,
  75-81 C.
- **No thermal reboot** in either test.

Image U SHA256: `05f5888f4b29a870b69f6f74eef325561b7957076aa798ce8f94d78cf6eaa1ff`
(installed in `boot_a`).
