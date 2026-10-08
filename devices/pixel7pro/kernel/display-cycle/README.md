# Display power cycle (test module)

`pixel-disp-cycle.ko` runs Stage 1 of the
[display cold-init plan](../../docs/display-cold-init-plan-20261001.md) on the
running phone without a new boot image: the DPU/DISP domains powered off
and the DSIM, DECON, DSC, DMA and DPP brought back from the registers read
just before.

With the screen off (the panel asleep, DECON idle with its trigger masked):

```sh
insmod pixel-disp-cycle.ko
echo 1 > /sys/module/pixel_disp_cycle/parameters/cycle  # DSIM stop/ULPS + DECON soft reset
echo 2 > /sys/module/pixel_disp_cycle/parameters/cycle  # + PHY isolation, DPU/DISP off hold_ms
```

Then turn the screen on as usual; the scanout driver wakes the panel and
sends the next frame. The module logs every register in `pixel-disp-regs.h`
that reads back differently; status registers are marked and not restored.

**The sequence**
- Down, as stock's `dsim_reg_stop_and_enter_ulps()` does for an SM DDI:
  1. HS clock off.
  2. ULPS on the clock and data lanes.
  3. Link clock to OSC.
  4. PHY lanes off, PLL off.
  5. SW reset.

  Then the DSIM PHY isolation (PMU `0x3eb8` bit 0), and the stock genpd
  sequence for DPU and then DISP. The save lists come from `kernel/pd`.
- Up, in the reverse order:
  1. DISP, then DPU: domain on, secure context restore, save list (DISP's
     two BUSY waits polled), S2MPU CTRL0.
  2. The isolation bypass.
  3. `dsim_reg_init()`: DPHY reset, BIAS, PLL, timing, lanes, PLL lock, lane
     ready.
  4. `dsim_reg_set_config()` in its order. Shadow reads go on first; CONFIG
     (DSC enable), the compression, slice and command-timer registers only
     take after the shadow enable.
  5. ULPS exit and start.
  6. Every DECON, window, DSC, DMA and DPP configuration register written
     back. DECON's enable and status bits are left off; the next transfer
     sets them.

**Results (October 1)**
- `cycle=1` and `cycle=2` (domains off 2 s) both pass. The screen then
  came on with frames completing, no failures and no DSI underruns.
- The DPU's three S2MPUs come back from the power cycle at CTRL0=1, which
  blocks DMA, and are set back to 0.

**What failed on the way**
- **DSIM CONFIG's DSC enable was lost.** It was written before the shadow
  enable; the fix is `dsim_reg_set_config()`'s order.
- **OF_SPLIT_SIZE, OF_SPLIT_IDX and OF_SIZE_1 were not restored.**
  `gen-disp-regs.py` skipped their definitions, which carry trailing
  comments. Without them the first frame underruns at the DSIM and times
  out.
- **A failed frame deadlocks the DMA** (RDMA_IRQ bit 17), and its soft reset
  then never completes. The scanout driver fails closed and the display
  needs a reboot.

`gen-disp-regs.py` generates `pixel-disp-regs.h` from Google's cal_9845
register headers (kernel/google-modules/display @ 0137241). It leaves out
FIFO, packet and secure registers, and every DPP sub-block but the common
one.

Build out of tree like the other modules (`make -C mainline/linux
M=$PWD/kernel/display-cycle`). It includes `../pd/pixel-pd-lists.h`.
