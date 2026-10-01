# GS201 UFS handoff driver

`pixel-ufs.c` connects the standard Linux UFS/SCSI core to cheetah's retained
bootloader configuration. It is an experimental bring-up driver, not complete
GS201 platform support: the bootloader still owns the clocks. Runtime suspend,
automatic Hibern8, inline encryption and clock scaling are disabled.

## Deep suspend with the link off (September 30, 2026)

Stage 4 of the SYS_SLEEP bring-up (kernel/suspend) showed that HSI2 is
powered down in SYS_SLEEP: the Hibern8 link did not come back. Stock uses
UFS PM level 5 there (`exynos_ufs_override_hba_params`): the device powers
down, the link goes off, and the host is initialized again on resume.
`deep_link_off=1` does the same, for deep suspends only, and needs
`vendor_cal=1 dev_reset=1`:

- Suspend, after the core turns the link off (`__exynos_ufs_suspend`):
  RST_N low and the PHY isolated (PMU 0x3ec8 bit 0 = 0, written through the
  secure monitor).
- Resume, before the core's reset and restore (`__exynos_ufs_resume`):
  PHY isolation bypassed and IOCC (SYSREG_HSI2 0x710 bits 1:0) set again.
  The driver maps the device DMA-coherent, so no request may run before
  IOCC is back. The UniPro clock is read again and must still be the rate
  the calibration used (kernel/suspend restores CMU_HSI2 from its domain
  save list).
- Host enable (`exynos_ufs_hce_enable_notify`): the vendor software reset,
  the vendor host configuration as the bootloader left it (a probe-time copy
  of 25 registers), and an RST_N pulse. Link startup then applies the
  calibration as at boot.

`reinits` and `reinits_lost` count completed re-initializations and those
that found HSI2 power-cycled (IOCC reset). s2idle is unchanged.

Results: three round trips in a deep suspend under `pm_test=devices` (HSI2
kept power) re-initialized the host at HS-G4 on both lanes, with a synced
16 MiB file and a fresh 4 MiB write reading back intact with the cache
dropped. After a real SYS_SLEEP, where HSI2 was power-cycled, link startup
failed on all five attempts. With the link-off path, the driver logs one
`diag` line of host, UniPro and PHY registers after the first link, on
resume and after each calibration, to find what the power loss leaves out.
The fixes that followed (kernel/suspend and docs/suspend-20260930.md): the
device's VCC (gpp0-1) is switched off with the link and on again 10 ms before
the reset, as stock's level 5 does; after a power loss the monitor's
`SMU(INIT)` sets up the DMA filter again (`smu_call`, default 2; data
transfers time out without it); pixel-sleep puts S2MPU_HSI2 back to bypass.
With these, UFS comes back from real SYS_SLEEP at HS-G4. The FMP descriptor
call is not made: it selects 128-byte PRDT entries this driver does not use.

## System-suspend Hibern8 (September 30, 2026)

`system_hibern8=1` opts into UFS PM level 1: the device remains powered and
the link enters Hibern8. It defaults off and can be changed through the module
parameter before the next suspend. The prepare callback selects the level
before the UFS WLUN child suspends; the standard UFS core owns command draining,
link transitions, interrupt suspension and error recovery.

This requires the calibrated two-lane HS link. The variant callback applies
Google's GS201 `post_h8_enter` and `pre_h8_exit` PHY tables and internal clock
stop sequence. Resume restores the retained clock-control values. External CMU
clocks, PHY isolation and device reset remain unchanged. Runtime PM remains
forbidden. `hibern8_entries`, `hibern8_exits` and `cdr_timeouts` expose diagnostic
counters.

A temporary RAM boot passed an initial RTC sleep (8.131 seconds) and four
off/on comparisons. The two further Hibern8 sleeps lasted 7.086 and 4.482
seconds before modem wakeups; all three entries had matching exits, zero CDR
timeouts, and an unchanged synced 16 MiB random file after resume. The two
disabled comparisons also resumed successfully. Shorter enabled samples make
the partial-rail power comparison inconclusive; this does not establish lower
standby drain or validate deeper SoC idle. A second image also passed an 8.344-second RTC sleep with the restored battery
model. That image is now installed in slot A; Hibern8 still defaults off.

## HS gear 4 (September 27, 2026)

With `vendor_cal=1 hs_gear=4` (the installed bootstrap's setting) the link runs
HS gear 4 rate B on both lanes: 1.2 GB/s for a 1 GiB direct read, 1.8 GB/s with
4 MiB requests, and a 512 MiB file written and synced in 1.4 s. Without it
the link stays in PWM gear 1 on one lane at 0.57 MB/s, where a cold desktop
start took about six minutes; on HS-G4 the desktop starts in seconds.

Linux's HCE reset at probe clears the UniPro-side PCS configuration the
bootloader left (the PHY PMA block keeps its own), so link startup ran on PCS
defaults and any gear change failed. `vendor_cal` applies Google's GS201
calibration from `google-modules/soc/gs` (`drivers/ufs/gs201/ufs-cal-if.c`,
branch `android-gs-pantah-6.1-android16`): `init_cfg_evt0` before link
startup (evt1 is identical; 38.4 MHz reference; the SMDK board, `brd-for-cal`
1), `post_init_cfg_evt0` after it, and `calib_of_hs_rate_b` /
`post_calib_of_hs_rate_b` around the power mode change, including the per-lane
CDR lock wait. It also sets the two UniPro debug option suites the vendor
host driver writes before link startup.

Several PCS values derive from the UniPro clock. The driver reads it from the
CMU instead of assuming it: CMU_HSI2's UFS_EMBD user mux over CMU_TOP's
CLKCMU_HSI2_UFS_EMBD (divide by 3) from PLL_SHARED0 / 2 / 2, with PLL_SHARED0
at 24.576 MHz × 347 / 4. That is 177.664 MHz; any other clock path is refused.
Register offsets are from Google's `cmucal-node.c` / `cmucal-sfr.c`.

Verification on the connected phone: `boot_a` read at HS-G4 matches image H's
SHA256, 12 GiB of sustained reads left every UFS error counter at zero, and a
512 MiB random file read back identically through ext4. HS-G1 (206 MB/s) and
HS-G3 (773 MB/s) also work. A probe that fails after asking for HS retries
once in PWM gear 1; two lanes without CDR lock stop further HS requests.

## Device reset (September 27, 2026)

Until image M, the first probe failed on every boot, RAM boots and normal boots
alike (11 of 11 recorded). Setting `fDeviceInit` got a query response of 0xff
(`-EINVAL`), and the driver's single full retry then succeeded. The vendor
`exynos_ufs_hce_enable_notify()` pulses the device's RST_N (vendor HCI
register 0x70, `HCI_GPIO_OUT`, low for 5 µs) before every host enable, but
this driver never reset the device. The device therefore stayed in the
bootloader's session while the host reset underneath it.

`dev_reset=1` adds that pulse as the UFS core's `.device_reset` op. The core
calls it before the host enable at probe and in its error-recovery resets.
The driver first checks that the bootloader left RST_N high (register value 1).
With it, the first probe succeeds: no retry, 53 ms instead of 172 ms, still
HS-G4 rate B on two lanes. Checked on image M:
- a RAM boot and two normal reboots;
- a 2 GiB direct read at 1.5 GB/s;
- `boot_a` read back to the expected SHA256;
- no UFS errors.

The boot script loads with `dev_reset=1`, and if that load fails it loads once
more without it.

Required handoff details established on the connected 256 GB Pixel:

- HCI 3.0 at `0x14700000`, vendor registers at `0x14701100`, stock DT IRQ.
- Standard PRDT layout with 4 KiB segments; stock GS201 marks the old Exynos
  PRDT/OCS defects fixed. Do not copy the corresponding legacy quirks.
- CPort needs `N_DEVICEID=0`, `N_DEVICEID_VALID=1`, `T_PEERDEVICEID=1` and
  `T_CONNECTIONSTATE=1` before link startup. Omitting these times out NOP OUT.
- Retained SYSREG_HSI2 IOCC reads `0x13`: DMA must be coherent. Noncoherent
  allocations produced stale management responses.
- The controller advertises 64-bit DMA and RAM extends above 4 GiB. A 32-bit
  data-buffer mask fails SCSI mapping. Descriptor-address experiments did not
  explain the first-probe failure: both layouts failed before recovery. The
  working policy conservatively keeps coherent descriptors below 4 GiB while
  allowing 64-bit data buffers; this is not proof of a hardware descriptor limit.

With these settings, Linux enumerates all four logical units and their GPT
partitions. Full reads of `boot_a` and `init_boot_a` match the independently saved
stock/Magisk reference SHA256 values. Ext4 on userdata also passes a 16 MiB write/remount/readback test. The Arch root
was installed through a checked host-built sparse image. Image G mounts it and
passes GPU shader readback. Saved files survive reset and another recovery RAM
boot; autonomous boot remains under diagnosis. Cold
library reads take minutes at the current PWM gear 1 speed.

The inherited link returns invalid `0xff` to the first initialization queries.
The module now permits one complete failed-probe teardown/reinitialization,
without bypassing layout or outstanding-request guards. Image F discovers all
partitions in a fresh boot by 1.1 seconds. A second failure aborts module loading;
a bound controller is never retried. The cause of this handoff defect still needs
a full GS201 platform-driver fix.

`pixel-ufs-inspect.c` is the earlier read-only register inspection utility.
`reprobe=1` relaxes only the initial HCE check after an unsuccessful probe; it is
for unmounted development sessions, never for a mounted root.

Sources: Google's pinned [GS201 Exynos UFS driver](https://android.googlesource.com/kernel/gs/+/b3c9095e01cefb36f35723b5c66638bf15f5144a/drivers/scsi/ufs/ufs-exynos.c),
the matching `gs201/ufs-cal-if.c` tables, and the UFS core/GS101 driver in the
repository's pinned Linux base. Raw inventories and vendor downloads stay local.
See the [installation record](../../docs/persistence-power-20260925.md).
