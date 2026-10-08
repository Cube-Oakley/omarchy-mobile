# v38: the idle UFS link in Hibern8 (2026-10-03)

v37 with one change in the storage driver: `pixel-ufs idle_hibern8=1`, passed
by the initramfs (`scripts/pixel-persistent-start.sh`). The UFS core's clock
gating puts the idle HS-G4 link in Hibern8 150 ms after the last request,
through the driver's GS201 `post_h8_enter` / `pre_h8_exit` tables; see
[storage](../storage/README.md#idle-hibern8-while-running-october-3-2026).

**Why.** With the link always active, the PMIC meters showed L2S_PLL_MIPI_UFS
at 188 mW and L4M_HSI at 72 mW whenever the phone was awake, screen on or off.

**Validated by RAM boot** (power-image-16): 1 GiB of random data written and
read back identically across about 200 Hibern8 round trips, no CDR timeout
or UFS error; deep sleeps with the link-off re-initialization unchanged
(deep current the same with `clkgate_enable` 0 and 1).

**Installed in boot_a on October 3** (pre-approved); readback matched
`f4fddd17…`, the previous image was v37 (`f8e5b857…`). On the installed boot,
screen on at brightness 237, battery only, `clkgate_enable` 1 vs 0:
200.6 / 197.9 mA vs 362.2 / 351.0 mA.

The kernel is v37's source (the tree differs from v37's `kernel-tracked.patch`
only in `hci_bcm.c`, built as the hci_uart module, which the session loads
from its own copy). Built with
`scripts/build-pixel-shell.py --persistent-root --seconds 0 --cma 256M`.
