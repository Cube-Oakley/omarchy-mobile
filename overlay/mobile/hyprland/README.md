# Hyprland patches

The phone runs Arch Linux ARM's `hyprland 0.56.2-3` with three patches, built
on the phone and installed over `/usr/bin/Hyprland` (Arch's binary is kept as
`/root/Hyprland.0.56.2-3.arch`). A `hyprland` package update replaces it
again; the patches are small enough to send upstream.

## 0001: draw a clipped texture only where it is damaged

`renderTextureInternal()` scissors a texture that has a clip box or a clip
region to the clip alone, instead of to the clip within the frame's damage
as `renderRect()` and the other clipped paths do. A surface's clip region is
its visible region from `hyprland_surface_v1`, which Quickshell sets for a
masked panel, so the shell's full-screen panels (shade, drawer, home,
back-gesture strip) were redrawn whole on every frame of anything on the
screen.

Found on October 7 with Panfrost's GPU trace (`MESA_GPU_TRACES=print
MESA_GPU_TRACEFILE=…` in Hyprland's environment): each frame is a pass into
Hyprland's work buffer and a copy to the output, and the first took 1.97 ms
of fragment work for an 8-pixel change, while the copy took 20 µs.
`devices/pixel7pro/scripts/ui-tests/damage/frame-cost.c` (Hyprland's frame
pattern on its own) cost 90 k cycles a frame, so the clear, the
depth/stencil attachment and the linear scanout buffer were not the cause.

Hyprland's GPU cycles per frame (Panthor fdinfo), the damage test page in
the browser with direct presentation, about 130 frames a second:

| animated area | before | after |
|---|---|---|
| 8 px square | 1522 k | 123 k |
| 57 px square | 1569 k | 162 k |
| 400 px square | 2378 k | 1575 k |
| whole page | 2490 k | 1754 k |

The 57-pixel animation's SoC and display rails went from 1315 to 898 mW
(GPU 351 to 77 mW, memory 168 to 94 mW); the still screen is 351 mW.

## 0002: draw a surface its opaque region covers without its alpha

A client's opaque region says its content is opaque there, but Hyprland
drew such a surface with its buffer's alpha. Chromium leaves an opaque
page's alpha at 0 in a buffer that has an alpha channel, and the browser's
pages are such buffers (Qt WebEngine patch 0007: a sub-surface below the
window, which must keep its alpha format). On screen nothing shows, since
nothing is drawn below an opaque surface and the output has no alpha, but
every picture of the window was transparent where the page was: the app
switcher's cards of web apps showed the wallpaper (October 7), and
screenshots of pages had no alpha there. A surface whose opaque region
covers it is now drawn with the RGBX shader, as an alpha-less buffer is.
(An alpha-less XBGR buffer for the page was tried instead: Hyprland 0.56
showed nothing for it below its parent.)

## 0003: touches in the edge strips reach the app until they swipe

The shell's gestures start in strips along the edges: back from the left
and right (layers `omarchy-mobile-back` and `omarchy-mobile-back-right`, 40
logical pixels), home and the switcher from the bottom
(`omarchy-mobile-navigation`, 24 pixels). They took every touch that landed
in them, so taps at the edges of apps often did nothing and scrolls starting
there were lost (reported October 7). Android watches such touches without
taking them and takes one over (`pilferPointers`, the app gets a cancel)
only once it moves like the gesture. This does the same: a touch in a
strip's input region, where that strip is the topmost overlay surface, goes
to the surface under the strip (the hit test skips the strip once). Once it
has moved 12 logical pixels inward, 1.2 times more than across, the app gets
`wl_touch.cancel` and the strip a touch down where the finger landed, then
the motion. 16 pixels across first leave it to the app; a second finger ends
the watch. A cancel also resets the seat's touch lock count.

Two things the first version got wrong:
- After the takeover every motion came back from Hyprland's event bus
  marked cancelled, and the strip saw the finger stand still. The bus's
  only touch listener is drag and drop, which never ran (checked), so the
  cause is not known; a taken-over touch's events now skip the bus.
- Apps leave a 7-pixel margin at the screen's edges, where the touch lands
  on the shell's wallpaper: cancelled there and pressed again in the strip,
  both in the shell, Qt mixed the two up (the right strip saw screen
  coordinates). Over a surface of the strip's own client, the strip now
  takes the touch itself.

Checked on October 7: 7 + 2 = typed on Calculator with "7" and "+" / "="
inside the left and right strips gave 9; back swipes from both edges,
starting in the margin and over Calculator, all went back; over F-Droid
(Android) back reached Android with no touch left down, a tap on its tab bar
inside the bottom strip switched tabs, and a swipe up from the bottom went
home. `/usr/bin/Hyprland` `725d0489…` (0001+0002 kept as
`/root/Hyprland.opaque-alpha`).

## Building and installing

On the phone, as the session user, from Hyprland's release tarball
(`source-v0.56.2.tar.gz`, which includes the submodules):

1. Build dependencies not in the image, from the official mirror with the
   existing package database (no `-Sy`, dry run with `--print` first):
   `pacman -S --needed --config /root/pacman-wifi.conf hyprwayland-scanner
   hyprland-protocols glaze`.
2. Apply the patches (the phone has no `patch`: patch on the laptop and copy
   `src/render/OpenGL.cpp`), and Arch's change for glaze 9:
   `sed -i -e '/find_package.glaze/s/7...<8 //' CMakeLists.txt
   hyprpm/CMakeLists.txt start/CMakeLists.txt`.
3. `cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
   -DCMAKE_SKIP_RPATH=ON -S . -B build` and `cmake --build build --target
   Hyprland -j6` (about 25 minutes from scratch, 5 for a change in one file).
   With the screen dark the display hook takes CPUs 4-7 offline: hold them
   with `echo "4 5 6 7" > /run/pixel-cores-hold; pixel-display-hook cores`
   and remove the file afterwards.
4. Install over `/usr/bin/Hyprland` and restart the session: stop Hyprland
   (TERM), the shell watchdog, and remove the old instance's directory in
   `/run/user/1001/hypr` (Hyprland leaves it behind), then run
   `/root/pixel-gpu-session-start.sh`. Apps close; Android keeps running in
   its container and comes back with the next Android app launched from the
   shell.
