# v45: a 384 MB area for the video codec (2026-10-08)

v44 (`kernel/scanout-stats-v44`) with `pixel_mfc_cma=384M` in place of 128M on
the command line, which is built into the kernel (`CONFIG_CMDLINE`), hence a
new build ID. No source change: `scripts/build-pixel-shell.py --persistent-root
--seconds 0 --cma 256M --mfc-cma 384M` (`out/mfc-pool-v45`, build ID
`30230bc2…`, image `ad6bba60…`); the configuration differs from v44's only
in the command line and the initramfs path.

**Why.** Chromium asks for 16 CAPTURE buffers per decoder (about 48 MB at
1080p, about 55 MB with the codec's and the input buffers), allows three MFC
decoders (QtWebEngine 0008), and a seek briefly holds two sets: 128 MB fit
two at most, and on October 8 X ran it dry (the chain is in
`kernel/mfc/README.md`, "Out of memory, cleanly"). The area is CMA: ordinary
movable pages use it while the codec does not.

**Room.** `/proc/pagetypeinfo` (2 MB pageblocks): the DMA zone (below 4 GiB)
has 2 GB present, 1.48 GB managed, 1.42 GB free before; its CMA went from
384 MB (192 blocks) to 640 MB (320 blocks). CmaTotal 1356 to 1612 MB.

**Validated.** RAM-booted (after a USB replug: the first `fastboot boot`
failed mid-transfer and fastboot stopped answering), the codec probed with its
8 MiB block, `mfc-pressure-test.sh mfcerr`: three videos on the MFC, two in
FFmpeg, no failed allocation, no codec error. **Installed in boot_a**
(readback matched; previous image v44 `29bf2acf…`) and an orderly reboot came
up on it: modem data after about 40 s, Wi-Fi, `/dev/video-dec0`, and the
installed browser library passed the same test.
