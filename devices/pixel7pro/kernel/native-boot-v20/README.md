# V20: native boot from boot_a

`native-boot-v20.patch` is a complete patch against the pinned base in
`kernel-base.txt`: the cumulative display, ACPM, GPU and panel work plus this
checkpoint's changes, with every added file. Apply it to a clean checkout of
that commit; do not stack it on earlier checkpoints. It reverse-applies
cleanly to the working source (all 40 files). `kernel.config` is image L's
configuration with the workspace path replaced by `@PIXEL_ROOT@`.

New in v20:

- `arch/arm64/kernel/pixel_usb.c`: `pixel_usb_phy_init()` releases the PMU USB
  PHY isolation through the secure-register SMC and runs the vendor USB 2 PHY
  sequence when the bootloader left the PHY isolated (normal boot). It skips a
  fastboot handoff, where the PHY is already running.
- `arch/arm64/kernel/pixel_acpm.c`: the CPU-scaling guard accepts the
  little cluster at 1598 MHz, the rate a normal boot leaves.

The UFS driver is out of tree: `../storage/pixel-ufs.c`, loaded by the
persistent bootstrap with `vendor_cal=1 hs_gear=4`.

Build from the project root:

```
python3 scripts/build-pixel-shell.py --output <new-output> --persistent-root --seconds 0 --initcall-debug
```

Image L SHA256: `79b60372e65bc6069dd02a77d3175f227b1d0e0ed01ff40514911b61116baaef`.
An orderly normal reboot of L reaches the desktop by itself. Images M (UFS
device reset) and N (RTC module) use this same kernel patch; only the
out-of-tree modules and boot scripts differ. N is the installed image.
Evidence and the J/K/L chronology: [native boot record](../../docs/native-boot-20260927.md).
