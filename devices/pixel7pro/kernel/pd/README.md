# Unused power domains off

The bootloader leaves almost every GS201 power domain on. The PMU STATUS
registers (documented reads in Google's `flexpmu_cal_local_gs201.h`) showed
them all on except the GPU, which the kernel's GPU domain driver manages.

`pixel-pd-off.c` powers off the blocks Linux does not use. The boot script
loads it with `off=all`, right after the C2 module:

- the TPU and AUR;
- BO and MFC (video codecs);
- G2D;
- EH;
- the camera pipeline: CSIS, PDP, DNS, G3AA, IPP, ITP, MCSC, GDC and TNR.

Each domain gets Google's `<block>_off` sequence. Before it, as for the GPU
domain, comes the secure context save the vendor `exynos-pd` driver does for
domains with a TZPC (the stock DT's `need_smc`):

1. SMC 0x82000410 (save, TZPC address).
2. The block's CMU `CONTROLLER_OPTION` bit 24 (automatic clock gating) is
   cleared.
3. PMU `<block>_CONFIGURATION` bit 0 is cleared through the secure register
   SMC.
4. `<block>_STATUS` bit 0 is polled until it reads 0.

Children go before their stock-DT parents: DNS before ITP, IPP before PDP.
There is no power-on path; the domains come back on the next boot.

Left on:
- DISP and DPU (the display) and HSI0 and HSI2 (USB, UFS);
- the NOC domains (interconnect);
- AOC (the stock DT leaves its domain driver disabled too).

Measured at the USB input with the screen off: the TPU saved about 0.08 W,
then AUR, BO, MFC, G2D and EH about 0.12 W together. The camera domains made
no difference that the input measurement could resolve. The camera and GPU
rails (S1S, S2S) still show 45–50 mW each with their domains off.

## A stopgap

This is a boot-time switch for blocks nothing uses yet. The camera, TPU and
codecs will be needed later. Each block then needs a real power domain (genpd,
as `gs201-g3d-pd` does for the GPU) that powers it on for its driver and off
again when idle. That means the `<block>_on` sequence, the CMU save/restore
and the secure context restore (SMC 0x82000410 with restore = 1), and
dropping the block from this module's list.
