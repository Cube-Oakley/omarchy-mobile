# A830v2 Mesa experiment

The stock Arch Mesa 26.2.3 rejects the native kernel's chip ID
`0xe844050001`: it has a wildcard speed-bin entry for A830 revision 0, but only
an exact (no speed-bin) entry for revision 1. The kernel encodes this handset's
speed bin in bits 47:32. The patch extends the existing revision-1 entry with
Mesa's established `0xffff` wildcard convention. It preserves the existing
A830 hardware properties and also continues matching the KGSL ID.

Source: [Mesa 26.2.3](https://archive.mesa3d.org/mesa-26.2.3.tar.xz), SHA256
`1628058a8d2c0615975de5a15ab7bbb9638c50000b5bed9456ff423ea034a81f`.
The clean cached archive is extracted into `out/mesa-a830/`; Pixel patches are
not applied. Build using the matching AArch64 userspace sysroot, GCC 16.1,
Meson 1.12.1 and the saved cross file/configure log. Options: freedreno Gallium,
no Vulkan, Wayland, GLVND, LLVM disabled, mesa-clc/precomp-compiler auto; disable
Rusticl, VA, GLX, GLES1, tests, sensors, libunwind and Valgrind. Install under
`/opt/oneplus13-mesa`, leaving distribution libraries intact.

The test environment selects this isolated installation with:

```sh
export LD_LIBRARY_PATH=/opt/oneplus13-mesa/lib
export GBM_BACKENDS_PATH=/opt/oneplus13-mesa/lib/gbm
export __EGL_VENDOR_LIBRARY_FILENAMES=/opt/oneplus13-mesa/share/glvnd/egl_vendor.d/50_mesa.json
unset GBM_ALWAYS_SOFTWARE LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE
```

`gpu-render-test.c` must identify an Adreno renderer and verify shader output
RGBA 64,128,191,255 with no GL error. Render-node enumeration and firmware load
alone are not acceleration proof. Probe 10 passed that test twice and ran Hyprland and the shared shell on
Adreno; the owner confirmed improved smoothness. No conformance or Vulkan
claim is made. Package SHA256:
`775e4df04adeab6859f1b889169f5f23e9a2808bb780f7ca573fdd7be650a199`.
