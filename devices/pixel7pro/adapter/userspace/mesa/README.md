# Experimental Mali-G710 Mesa enablement

Tested with Mesa 26.2.3 on the Pixel 7 Pro's native v15 B2 kernel. The two-line
model addition enables the real G710 architecture10.8/product2/variant0; it does
not spoof the device as a G610. Core parameters match the G610's shared shader
architecture. No GLES conformance claim is made.

Hardware acceptance: a GLES shader returned RGBA64,128,191,255, no GL error;
renderer `Mali-G710 MC7 (Panfrost)`. A second run after runtime suspend passed via
the display-only card0/kmsro route. Hyprland/Aquamarine selected renderD128 and
produced a captured shared mobile desktop through the retained-display bridge.
See [the GPU record](../../../docs/gpu-bringup-20260925.md).

## Inputs and build

Work directory: `out/checkpoints/20260925-gpu-v15/`.

- Official source: https://archive.mesa3d.org/mesa-26.2.3.tar.xz
- Source SHA256: `1628058a8d2c0615975de5a15ab7bbb9638c50000b5bed9456ff423ea034a81f`.
- Patch: `0001-panfrost-add-g710-model.patch` (apply with `patch -p1`).
- Patch: `0002-panfrost-keep-cached-bo-mappings.patch` (2026-10-06). Every
  GL batch takes its pool BOs from panfrost's BO cache, and Mesa unmapped
  each BO as it went back to the cache, then mapped it again for the next
  batch, and set a kernel debug label at each move (one ioctl each). In
  Chromium's GPU thread scrolling X at 120 Hz that was about 14 % of the
  thread (mmap, munmap and their page-table work), and its time per frame
  fell from about 5.4 to 4.6 ms with the patch. A cached BO now keeps its
  mapping until it is freed (Panthor never evicts cached BOs; on Panfrost a
  purged BO is freed, not reused), and kernel labels are set only with
  `PAN_MESA_DEBUG`. The build directory still names the repository's old
  path (`~/Projects/omarchy-pixel7pro`, now `devices/pixel7pro`); a
  temporary symlink there lets `ninja` rebuild in place. Installed on the
  phone on October 6 (`/opt/pixel-mesa/lib/libgallium-26.2.3.so`, SHA-256
  `33851d9f…`; the previous one is `/root/libgallium-26.2.3.so.before-bo-cache`):
  apps started since use it, Hyprland and the shell from the next boot.
- ARM sysroot: `usr/include`, `usr/lib`, `usr/share/pkgconfig` and
  `usr/share/wayland-protocols` from the unchanged mobile Arch archive, SHA256
  `9b204df8aff3f54625f347d11d4e1370b44a83aef12e6371d588c8581ba54e57`.
- Cross toolchain: aarch64-linux-gnu GCC16.1.0, binutils2.47.
- Isolated Python environment: Meson1.12.1, Mako1.4.3, PyYAML6.0.3,
  packaging26.3, MarkupSafe3.0.3. Ninja1.13.2.
- Host LLVM22.1 plus SPIRV-Tools2026.3.1. The additional host package
  `spirv-llvm-translator-22.1.6-1-x86_64.pkg.tar.zst` was extracted into
  `host-deps/`, not installed system-wide. SHA256
  `d7582c62782e073795b1bdf9d7cbe2c953e93d8bb756a5b3813b7ac36085ed0e`.
  Its pkgconfig prefix was adjusted to that extracted `usr` directory.

Build the matching Mesa host `mesa_clc`, `vtn_bindgen2`, and `panfrost_compile`
first. The saved `host-build-options.json`, `cross-build-options.json`,
`mesa-cross.ini`, and configure/build logs record exact options. Host configuration
uses no graphics drivers, enables mesa-clc, tools=panfrost, install-mesa-clc and
install-precomp-compiler. Keep the isolated Python environment in PATH during
configure, regeneration and builds. Expose the three host executables in PATH.

The ARM build uses gallium-drivers=panfrost, vulkan-drivers empty,
platforms=wayland, glvnd enabled, LLVM disabled, mesa-clc=system and
precomp-compiler=system. Rusticl, VA, GLX, GLES1, tests, sensors, libunwind and
Valgrind are disabled. Prefix `/opt/pixel-mesa`, libdir `lib`, release build.
`mesa-cross.ini` and the two option records name the device workspace as
`@PIXEL_ROOT@` in place of the absolute path used for the build; substitute it
(or regenerate) when reproducing. Its compiler/linker sysroot and pkgconfig paths must point to ARM files.

## Test package and replay

Saved package: `out/checkpoints/20260925-gpu-v15/mesa-g710.tar.gz`, SHA256
`2e33b3b94fff1792a1d94941e9bf76fb44fadeecde9a20e72fbbb40b5ca12f68`.
It contains only `opt/pixel-mesa/`; extract into the phone's RAM root after Arch
restore. It replaces no distro libraries. Environment for opted-in clients:

```sh
export LD_LIBRARY_PATH=/opt/pixel-mesa/lib
export GBM_BACKENDS_PATH=/opt/pixel-mesa/lib/gbm
export __EGL_VENDOR_LIBRARY_FILENAMES=/opt/pixel-mesa/share/glvnd/egl_vendor.d/50_mesa.json
unset GBM_ALWAYS_SOFTWARE LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE
timeout 25 /root/pixel-gpu-render-test /dev/dri/renderD128
```

Compile the probe from `mainline/pixel-gpu-render-test.c` with the ARM compiler,
EGL/GLES headers and `-ldl`. Copy it into `/root` on the phone. The
`scripts/pixel-gpu-session-start.sh` launcher checks a shader before starting the
hardware-rendered Hyprland and shared mobile UI. Existing v11 software launchers
remain separate fallback recipes. The old saved terminal banner says “software
display”; it is static text, not renderer detection.

Shared OS packages and the OnePlus project remain unchanged. The model patch is
a candidate for an eventual shared Mesa package after broader validation; the
Pixel session launcher and experimental kernel remain device-specific.
