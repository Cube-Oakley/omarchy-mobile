#!/usr/bin/env python3
"""Keep Waydroid's display at the size it starts with.

xdg_toplevel_handle_configure (hardware/waydroid, wayland-hwc.cpp) sizes
Android's one display from every configure of every Android window and calls
do_hotplug(), and a compositor sends configures for focus and state changes
too. Each hotplug makes Android reread its display. On October 5 that preceded
a WindowManager watchdog and a surfaceflinger crash in onComposerHalVsync, and
each such crash restarts system_server and closes every Android window.

A first version hotplugged only when the configured size changed. That still
hung Android on October 6: two Android windows tiled on one workspace shrank
the display and grew it back, and system_server's android.display thread
waited forever in getDynamicDisplayInfo (SurfaceFlinger) under the display
manager's lock. After that no Android window closed, and the watchdog's
restart of system_server left Android without a default network.

Now the display keeps the size of the first window's configure (the
calibration in create_window, with choose_width_height's
persist.waydroid.width/height overrides; this phone sets neither). Later
configures still record the requested size, but once the display has a size,
the handler returns before it stores a new one, sets the window geometry or
hotplugs. A window the compositor makes smaller (tiled) shows the display at
its full size; the shell keeps Android windows from sharing the screen. A
new work area (a different bar height) needs an Android session restart.
xdg_surface's configure (which marks a new window configured) is a separate
handler and keeps working.

    patch-hwc-hotplug.py HWCOMPOSER_SO OUT_SO

HWCOMPOSER_SO is the vendor image's /lib64/hw/hwcomposer.waydroid.so; OUT_SO
goes in /var/lib/waydroid/overlay/vendor/lib64/hw/ on the phone. Checked
against lineage-20.0-20260927-MAINLINE-waydroid_arm64-vendor (build ID
8937ba8f2556cc3e0050372676e63c9d); it refuses any other library.
"""
import hashlib
import sys
from pathlib import Path

ORIGINAL_SHA256 = 'c5847fd6b1228afb803f36a0818d037ce65369e6b95e96d85666b67f2158463f'
OFFSET = 0x42da4  # .text is mapped at its file offset
OLD = bytes.fromhex(
    '00e4006f'   # movi v0.2d, #0         (property buffer for property_get)
    'a0feffb0'   # adrp x0, ...           ("persist.waydroid.width")
    '00e03291'   # add  x0, x0, #0xcb8
    'e1030091'   # mov  x1, sp
    'e2031faa')  # mov  x2, xzr
NEW = bytes.fromhex(
    '28000014'   # b    0x42e44           the display has a size: return
    '1f2003d5'   # nop
    '1f2003d5'   # nop
    '1f2003d5'   # nop
    '1f2003d5')  # nop


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.split('\n\n')[4])
    data = bytearray(Path(sys.argv[1]).read_bytes())
    digest = hashlib.sha256(data).hexdigest()
    if digest != ORIGINAL_SHA256:
        raise SystemExit(f'not the library this patch was made for (sha256 {digest})')
    if data[OFFSET:OFFSET + len(OLD)] != OLD:
        raise SystemExit('unexpected code at the patch site')
    data[OFFSET:OFFSET + len(NEW)] = NEW
    Path(sys.argv[2]).write_bytes(data)
    print(f'{sys.argv[2]}: sha256 {hashlib.sha256(data).hexdigest()}')


if __name__ == '__main__':
    main()
