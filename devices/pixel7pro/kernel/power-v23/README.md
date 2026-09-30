# V23: idle power

`power-v23.patch` is a complete patch against the pinned base in
`kernel-base.txt`, with every added file; do not stack it on v22. It
reverse-applies cleanly to the working source (the same 40 files as v22).
`kernel.config` is image X's configuration, with the workspace path replaced
by `@PIXEL_ROOT@`; apart from paths it matches v22. See the
[power record](../../docs/power-20260928.md) for the measurements.

New since [v22](../smooth-v22/README.md):

- **Memory clock with the screen off** (`pixel_bandwidth.h`). The display
  used to hold the bootloader's 2028 MHz MIF even with the panel off; its
  release now sets 421 MHz, and the next screen-on raises it again (to 2028 MHz
  on a normal boot, 1352 MHz on a fastboot RAM boot). In an A/B test with the
  screen off, 421 MHz averaged 1.82 W and 2028 MHz 1.97 W at the USB input.
- **Thermal poll** (`pixel_acpm.c`). The `pixel-cpu-thermal` thread polls
  every 100 ms below 60 C and every 20 ms from 60 C. A BIG core gains under
  4 C in 100 ms, so the 20 ms cadence resumes long before the 75 C first cap.
  Its sleep is now interruptible, so it no longer adds 1 to the load average.

The panel still only gets DCS display off (about 87 mW from VSYS while off).
Sleep in brought it down to 0 mW, but the first frame after sleep out timed
out: sleep loses the DSC state that only the bootloader's panel init sets up.
That change was reverted.

The CPU idle and power-domain fixes are out-of-tree modules in the same
image: [cpupm](../cpupm/README.md) and [pd](../pd/README.md).

Image X SHA256: `b6b5de2e51a8f508ba34692e88068918825aeb56e3035bbc46f9457661d3f0d1`
(installed in `boot_a`).
