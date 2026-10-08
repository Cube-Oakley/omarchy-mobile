# Stock's nested reserved memory (2026-10-06)

**The fault.** A file copied on the phone came out with sixteen bytes of
`f6 49` in it (October 5). A RAM watch
(`scripts/ui-tests/scribble-watch.py`: zeroed, locked memory scanned for
writes nobody made, with physical addresses) caught the writer within
seconds: physical page `0xd8100000`, 16-bit values at offsets `0x2a8` and
up, one per online CPU (four with the screen off, CPUs 4-7 offline; eight
with it on), every one to twelve seconds, the values changing like a
counter (`0x49f6`, `0x47f5`). No interrupt line followed the writes; they
went on with the display, GPU and AoC idle and with Wi-Fi off.

**The cause.** That address is the header of stock's debug-snapshot area.
Stock's device tree reserves it, but as `/reserved-memory/debug_snapshot/
header`, one level below `/reserved-memory`, with the parent's own cell
sizes; the vendor kernel's debug-snapshot driver reserves those itself.
Mainline's early scan only looks at direct children, so this kernel never
reserved it, and the 256 MB CMA pool at `0xd0000000` covers it: whichever
page-cache or program page landed there was written into by firmware the
bootloader set up (per-CPU state). The region list, from the stock DTB:
header `0xd8100000` (64 KiB), kevents, BCM log `0xdc210000` (4 MiB), s2d,
array dumps, SLC dumps and ITMON; the bootloader leaves only the header and
the BCM log enabled on this phone.

**The fix (v41).** `0001-of-reserve-nested-no-map-regions.patch` makes the
early scan reserve the available `no-map` regions one level down, read with
their parent's `#address-cells`/`#size-cells`, before the dynamic regions are
placed. The kernel is v40's otherwise: built with `scripts/build-pixel-shell.py
--persistent-root --seconds 0 --cma 256M` (`out/reserved-v41`, build ID
`a8fca6ba…`), same release string (the modem refuses to start when its
bundle's kernel release differs), same command line, configuration differing
only in the initramfs path. RAM-booted on October 6, then **installed in
boot_a** (`a72f2a8b…`, readback matched; the previous image was v40,
`e1eea487…`). On both boots `/proc/iomem` shows `d8100000-d810ffff` and
`dc210000-dc60ffff` reserved outside System RAM, `HardwareCorrupted` is 0,
and Wi-Fi, the modem and its data link, the desktop and the battery driver
came up.

**Before v41** the boot script (`scripts/pixel-persistent-session.sh`, early)
soft-offlines the header's 16 pages and the BCM log's 1024: the kernel moves
whatever is there and never hands those pages out again (they show as 4160
kB of `HardwareCorrupted` in `/proc/meminfo`). Applied by hand on October 6
at 02:52; the watch saw no more writes into memory in use.
