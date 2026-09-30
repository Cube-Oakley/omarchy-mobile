# GS201 deep sleep (SYS_SLEEP) plan — September 30, 2026

Host-only research. Nothing was run on the phone and no code changed. The plan
covers the path from today's s2idle (0.81–0.85 W on the ODPM rails, 0.94 W
awake) to the stock `SYS_SLEEP`, and a standby budget toward the stock Pixel 7
Pro's ~25 mA from the battery (≈0.1 W at 3.9 V).

**Evidence tags**
- `S:` stock GPL source, LineageOS `android_kernel_google_gs201` @ `40ff934`, `path:lines`.
- `DT:` the same tree's `arch/arm64/boot/dts/google/`.
- `VDT:` the phone's saved base DT, `out/restart-20260924/saved-vendor-first.dts`.
- `DTBO:` the phone's saved `dtbo_a.img`. All 24 cheetah entries agree on every
  fragment cited here.
- `R:` this repository. `M:` the pinned mainline tree, `mainline/linux` @ `5225b8e`.
- `LIVE:` the coordinator's PMIC CTRL dump from 2026-09-30 (screen on, USB plugged).
- `INF:` an inference. `EST:` an engineering estimate, not a measurement.

## Summary

- **Where the saving comes from.** Stock standby saves power by switching off
  PMIC rails, not by stopping clocks. In `SYS_SLEEP` the PMU drops PWREN, and every
  regulator in `SEC_OPMODE_SUSPEND` turns off. That is 32 of the 80 stock rails,
  including every CPU/INT/MIF/TPU/CAM/G3D buck and the TCXO, PLL, HSI and PCIe
  LDOs (`DT: gs201-pmic.dtsi`, `S: drivers/regulator/s2mpg12-regulator.c:38-49`).
- **Why s2idle and SICD saved little.** Neither drops PWREN, so all of those rails
  stay up. This is consistent with Astra's unchanged partial-rail readings (`INF`).
- **What the kernel does for `SYS_SLEEP`.** It is small and fully table-driven:
  - wake masks;
  - `CPU_INFORM[cpu0] = 4`;
  - read 545 CMU registers;
  - three enter writes;
  - PSCI `SYSTEM_SUSPEND`;
  - on wake, 37 exit writes (or 9 early-wakeup writes) plus the restore.

  No ACPM IPC is sent. BL31 and the APM firmware run the sequencers.
- **What makes it hard.** The hard part is not the sequence. It is everything our
  kernel assumes survives power loss:
  - CPU hotplug (never tested);
  - the MCT broadcast timer (in MISC, which is power-cycled);
  - the display, UFS, USB and Wi-Fi blocks, which still run on bootloader state;
  - PMIC opmodes the bootloader left non-stock;
  - two unverified wake paths (the PMIC's RTC and power key over I3C/VGPIO).

## 1. Stock SYS_SLEEP, step by step

### 1.1 Ordering (generic kernel plus stock modules)

1. `PM_SUSPEND_PREPARE`: cpif marks the PCIe as PM-suspended
   (`S: drivers/soc/google/cpif/modem_ctrl_s5100.c:1726-1745`).
2. Device suspend (the `suspend`, `late` and `noirq` phases):
   - **Modem:** `suspend_cp()` refuses with `-EBUSY` while CP2AP_WAKEUP is high.
     Otherwise it drives AP2CP_AP_ACTIVE (gph1-1) low
     (`S: modem_ctrl_s5100.c:1669-1693`; `CONFIG_CP_LCD_NOTIFIER` is not in
     `cloudripper_gki.fragment`).
   - **UFS:** it suspends at `spm_lvl = 5`: device PowerDown and link off, then
     `HCI_GPIO_OUT = 0` (RST_N) and PHY power off
     (`S: drivers/scsi/ufs/ufs-exynos.c:606, 880-904`).
   - **PCIe hosts:** they isolate PHYs of links that are down
     (`S: drivers/pci/controller/dwc/pcie-exynos-rc.c:5372-5419`).
   - **Power domains:** exynos-pd `suspend_late` clears `GENPD_FLAG_ALWAYS_ON`, so
     genpd powers off every idle domain in `noirq`
     (`S: drivers/soc/google/exynos-pd.c:380-400`). That covers DPU, DISP, HSI0,
     G3D and the camera, TPU and codec domains. `pd_hsi2` and `pd_aoc` are
     disabled genpds, so they are never switched by the kernel
     (`DT: gs201-pm-domains.dtsi`).
   - **Devfreq:** MIF and INT go to their `suspend_freq`, 421 MHz and 100 MHz
     (`DT: gs201.dtsi:933, 999`).
   - **exynos-cpupm:** `suspend_noirq` sets `system_suspended`. From then on,
     `CPU_PM_ENTER` is ignored (`S: exynos-cpupm.c:974-976, 1383-1392`).
3. `pm_sleep_disable_secondary_cpus()` (`M: kernel/power/suspend.c:453`):
   - CPUs 1–7 hotplug off through `cpuhp_cpupm_offline()`, which calls
     `cal_cpu_disable()`. That sets `CPU_INFORM = C2`, enables GRP2 and clears
     GRP1 for the dying CPU.
   - The last CPU of cluster 1 or 2 also hints CPD
     (`S: exynos-cpupm.c:1130-1152`; `S: cal-if/pmucal_cpu.c:54-90, 171-190`;
     `S: cal-if/gs201/flexpmu_cal_cpu_gs201.h:19-47`).
4. `syscore_suspend()` runs in reverse registration order
   (`M: suspend.c:462`):
   - `exynos_pm_syscore_suspend()` runs first (`S: drivers/soc/google/exynos-pm.c:273-314`).
     It chooses the mode: `pcieon_suspend_mode_idx = 13` (SYS_SLEEP_HSI1ON) if
     the modem link is up (`pcie_linkup_stat()`), otherwise
     `suspend_mode_idx = 8` (SYS_SLEEP) (`DT: gs201.dtsi:380-411`; `VDT:11352-11357`).
     It then calls `exynos_prepare_sys_powerdown()`, described in §1.2.
   - `cpu_pm_suspend()` runs last and sends `CPU_PM_ENTER`, which cpupm ignores
     (`M: kernel/cpu_pm.c:176-201`).
5. `psci_system_suspend_enter()` calls `cpu_suspend(0, psci_system_suspend)`.
   This is SMC `0xC400000E` (PSCI 1.0 `SYSTEM_SUSPEND`, SMC64) with
   `x1 = __pa(cpu_resume)` and `x2 = 0`. `SYSTEM_SUSPEND` takes no power_state
   (`M: drivers/firmware/psci/psci.c:530-558`).
   - The stock kernel uses this same generic path; there is no vendor PSCI code.
   - For comparison, C2 uses `CPU_SUSPEND` with power_state `0x0010000`
     (`DT: gs201-cpu.dtsi:129-161`).

### 1.2 `exynos_prepare_sys_powerdown()` (what we must replicate)

**Wake masks** (`S: exynos-pm.c:233-250`). All PMU writes go through the secure
SMC `0x82000504`, as `set_priv_reg()` does (`S: exynos-pmu-if.c:56-59`;
`S: pmucal_rae.c:140-148`).

| PMU offset | Name | Stock value | Meaning |
|---|---|---|---|
| 0x3a80, 0x3a84, 0x3a88 | EINT_WAKEUP_MASK 1–3 | `exynos_eint_wake_mask_array[]` | 1 = masked. Built by `irq_set_wake()` on wake-up EINTs: bit = `eint_num + hwirq + wake_mask_bit_offset` (`S: drivers/pinctrl/samsung/pinctrl-exynos.c:418-435`, banks in `pinctrl-gs201.c:73-90`) |
| 0x3950, 0x3970 | WAKEUP_STAT, WAKEUP2_STAT | written 0 first | |
| 0x3944 | WAKEUP_INT_EN | **0x1001f0bf** | RTC_ALARM, RTC_TICK, TRTC_ALARM, TRTC_TICK, EINT, EINT_FAR, MAILBOX_AOC2AP, EXT_PCIE_GEN4A/B_0/1, USB_REWA, INTREQ_PCIE_GEN4A_0 |
| 0x3964 | WAKEUP2_INT_EN | **0x1f0** | MAILBOX_APM2AP, AOCA32, AOCF1, AOCP6, DBGCORE |

These values come from `DT: gs201.dtsi:401-406` and `VDT:11357`, with the bit
names at `DT: gs201.dtsi:412-483`. GS101 used `0x100BF/0x0`
(`DT: gs101.dtsi:365`).

**`cal_pm_enter(8)` calls `pmucal_system_enter()`** (`S: cal-if/pmucal_system.c:27-83`):
- `tcxo_req` is false only when xHCI votes for USB-host L2 sleep
  (`S: drivers/usb/host/xhci-exynos.c:210-211`). Only then does it call
  `exynos_acpm_set_rate(ACPM_DVFS_HSI0_TCXO, 0)`. **Normal SYS_SLEEP sends no
  ACPM IPC at all.**
- `pmucal_powermode_hint(4)` writes CPU0's `CPU_INFORM` (PMU 0x860) = 4
  (`CPU_INFORM_SLEEP`). HSI1ON uses 6, SICD 3, C2 1 and CPD 2
  (`S: cal-if/gs201/flexpmu_cal_define_gs201.h:6-13`; `S: cal-if/gs201/cal_data.c:46-52`).
- It reads the save list, then writes the enter list (next table).

**Stock also reads ACPM FLEXPMU_DBG counters before the PSCI call.** These are
the SLEEP AP (SoC) and MIF down counts, early wakeups and the MIF request
master (`S: drivers/soc/google/acpm/acpm_flexpmu_dbg.c:122-147`). Our patch
0003 already exposes the same words (`R: kernel/suspend/0003-acpm-read-only-flexpmu-counters.patch`).

### 1.3 The GS201 SYS_SLEEP lists (`S: cal-if/gs201/flexpmu_cal_system_gs201.h`)

| List | Lines | Entries | Content |
|---|---|---|---|
| `enter_sleep` | 140-144 | 3 | intr_gen GRP2_INTR_BID_ENABLE bit 0 set; GRP1 pending bit 0 cleared; PMU CLUSTER0_CPU0_INT_EN (0x1044) bit 3 set. CPU0 is the only waker. |
| `save_sleep` | 145-695 | **545** | Types: 300 SAVE_RESTORE; 41 COND_SAVE_RESTORE; 170 READ and 34 COND_READ (QCH `DBG_NFO` debug reads, not restored). All are **CMU** registers, grouped by base in the next table. Conditions: HSI1 only if PMU 0x2104 (HSI1_STATUS) is on; G3D, TPU and AUR SHORTSTOP only if their PD is on. **No DMC/DRAM, TCXO or PMIC access.** |
| `exit_sleep` | 696-734 | 37 | Clear WAKEUP_INT_EN and WAKEUP2_INT_EN. Clear intr_gen GRP27/GRP31 pending (0x1b08/0x1f08). GRP2 bit 0 off and cleared. CPU0_INT_EN[3] = 0. **MIF DMC `PWRMGMT_BUNDLE_PwrMgmtMode` bit 31 = 0** on MIF0–3 (0x20840000, 0x20940000, 0x20a40000, 0x20b40000, +0xf23c). DRCG re-enabled in 19 sysregs (4 conditional on G3D/NOCL power). UFS `MISC` (0x147011b4) bit 8. **PMU TOP_OUT (0x3920) atomic set of bits 7, 9, 11, 12, 13, 14** through the `offset \| 0xc000` alias (`S: pmucal_rae.c:167-181`); INF: pad/IO retention release. TREX_D_NOCL1A 0x20512000 = 0xf000. Then the save list is restored. |
| `early_sleep` | 735-745 | 9 | Clear the INT_ENs, GRP27/31, GRP2 and GRP1 bit 0, CPU0_INT_EN[3]. SYSTEM_CTRL (0x3a10) bit 14 = 0. GRP4 pending bit 0 cleared. Then restore and `pmucal_powermode_hint_clear()`. No DRCG or TOP_OUT writes: TOP never went down. |
| SYS_SLEEP_HSI1ON | 1439-1452 | = SLCMON | Same 545/37/9 lists; only CPU_INFORM differs (6). |
| Firmware sequencers | 1466-1665 | — | Built only under `ACPM_FRAMEWORK`, i.e. inside the APM firmware: `soc_sleep_down/up` (ids 2/3), `smc_sleep_down/up` (0x12/0x13, the DRAM controller), `mif_sleep_down/up` (0x19/0x1A). |

The `save_sleep` entries by base:

| CMU | Base | Entries |
|---|---|---|
| APM | 0x18000000 | 136 |
| MISC | 0x10010000 | 112 |
| PERIC0 | 0x10800000 | 94 |
| HSI1 | 0x11800000 | 72 |
| PERIC1 | 0x10c00000 | 60 |
| TOP | 0x1e080000 | 15 |
| CPUCL0, CPUCL1, CPUCL2 | 0x20c00000, 0x20c10000, 0x20c20000 | 12 each |
| MIF0–3 | 0x2080–0x20b0 | 4 each |
| NOCL0, TPU, AUR, G3D | — | 1 each |

**Consequence (INF):** MISC, PERIC0/1, APM, TOP, MIF, CPUCL and HSI1 clocks lose
their state in SYS_SLEEP. The watchdogs (0x10060000, 0x10070000) and the MCT
(0x10050000) live in MISC. HSI0, HSI2, DPU and DISP are *not* in the save list.
Stock has them powered off (genpd) before sleep; HSI2 is firmware-managed.

`pmucal_lpm_init` (lines 8-111) is run once at boot by stock `cal_if_init`. We
have never run it. Its PMU items are timing parameters the sequencer uses on
wake:

| Register | PMU offset | Value |
|---|---|---|
| EXT_REGULATOR_MIF_DURATION | 0x3cb0 | 0x29e |
| TOP, CPUCL2, CPUCL1, G3D, TPU durations | 0x3cb4–0x3cc4 | 0xa0 or 0xbe |
| TCXO_DURATION | 0x3cc8 | 0x66c |

It also clears MIF `PwrMgmtMode` bit 31 and sets MASK_IRQ0 (0x18020420, bits
28–31), SHORTSTOP, CLKDIVSTEP, HCHGEN and DRCG.

### 1.4 What firmware does, and exit and early-wakeup detection

- **Entry (BL31 and APM firmware; closed):** BL31 sees CPU0's `CPU_INFORM = 4`.
  APM's flexpmu then runs, in order:
  1. SoC down;
  2. DRAM self-refresh (`smc_sleep_down`);
  3. MIF down;
  4. PWREN low, which turns off the PMIC SUSPEND rails.

  It then waits on the wake masks.
- **Early wakeup:** a wake source already pending makes the firmware abort and
  return to the caller.
- **Wake:** APM restores rails and TCXO using the `lpm_init` durations. CPU0
  warm-boots through BL31 to `cpu_resume`. `cpu_pm_resume()` (CPU_PM_EXIT) then
  runs first in `syscore_resume`, and `exynos_pm_syscore_resume()` follows
  (`S: exynos-pm.c:316-358`):
  - it compares the SLEEP AP-down count with its pre-sleep value;
  - equal means an early wakeup, "return to originator": `cal_pm_earlywakeup()`
    runs `early_sleep` and restore (`S: pmucal_system.c:163-220`);
  - otherwise `cal_pm_exit()` runs `exit_sleep` and restore
    (`S: pmucal_system.c:93-153`);
  - an unchanged MIF-down count is logged as "MIF blocked" with the MIF request
    master.
- **Wake reason:** read from WAKEUP_STAT and WAKEUP2_STAT, plus the alive and
  far EINT_PEND registers (0x180D0A00 and 0x180E0A00). `wakeup-stat-rtc = 0`
  means stock expects PMIC RTC alarms in WAKEUP_STAT[0] (`S: exynos-pm.c:177-231`).
- **Debug flag:** `MIF_ALWAYS_ON` is only a debugfs knob
  (`S: acpm_flexpmu_dbg.c:397-420`). **Stock never writes it; we must not either.**
  Clearing it during SICD reset the phone (`R: docs/suspend-20260930.md`).

### 1.5 Contrast with SICD (what was tried)

SICD's enter list is empty. Its save list is 4 AUR clock registers, and it uses
masks `WAKEUP_INT_EN = 0x0ff00000` (CPU nIRQOUT) plus the EINT mask
(`DT: gs201-cpu.dtsi:207-240`; `S: flexpmu_cal_system_gs201.h:114-139`;
`S: exynos-cpupm.c:815-868`). `R: kernel/cpupm/pixel-sicd.h:204-216` reproduces
this correctly. It gated on `is_acpm_ipc_flushed()` and idle-IP, which SLEEP
does not use. Firmware counted one `sicd_soc_down` with MIF up
(`R: kernel/suspend/README.md:71-77`). SICD keeps PWREN and rails up, so no
rail-level saving was possible (INF).

## 2. What we have and what is missing

**Already have:**
- **PSCI SYSTEM_SUSPEND:** registered when PSCI_FEATURES reports it
  (`M: psci.c:582-592`). The firmware does report it: the build script notes that
  the kernel would otherwise default to `deep` (`R: scripts/build-pixel-shell.py:69-73`).
- **CPU_INFORM and PMU SMC writes, intr_gen GRP1/GRP2 bookkeeping, C2/CPD:**
  `R: kernel/cpupm/pixel-cpupm.c:101-131`.
- **Wake-mask save/restore, NSCODE CANCEL_FLAG and a PMU write helper:**
  `R: pixel-sicd.h:58-170`.
- **Firmware counters** (patch 0003) and the **ACPM idle snapshot** (patch 0002).
- **Watchdog pause/restore** in `noirq` (`R: kernel/watchdog/pixel-wdt-pm.c`).
- **Wi-Fi link to L2 with PHY isolation**, opt-in (`R: kernel/pcie/README.md`,
  `system_link_off`).
- **UFS Hibern8**, opt-in (`system_hibern8`).
- **The stock cpif suspend behaviour:** the modem PCIe host powers down, and
  AP_ACTIVE goes low.
- **Staged testing:** `CONFIG_PM_DEBUG` / `pm_test` (`R: kernel/sleep-v25/README.md:10-12`).

**Missing, in dependency order:**

| # | Missing piece | Stock reference |
|---|---|---|
| M1 | CPU hotplug PMU handling. `pixel-cpupm` has none (`R: pixel-cpupm.c:34-36`), and offlining a CPU has never been tested. It needs C2 hint, GRP2 enable and GRP1 clear on the dying CPU, CPD on the last CPU of clusters 1/2, clear on online, and C2 refused while hotplugging. | `M: drivers/soc/samsung/exynos-pmu.c:278-362`; `S: exynos-cpupm.c:1105-1152` |
| M2 | `CPU_PM` notifier awareness of system suspend: ignore ENTER/EXIT during deep, or it rewrites CPU_INFORM to 1 over our 4. | `S: exynos-cpupm.c:974-976`; mainline `sys_insuspend` |
| M3 | An exynos-pm equivalent (`pixel-sleep`): syscore ops registered after `cpu_pm`; an EINT wake registry (we have no pinctrl-samsung, so the keys, gpio, cpif and rtc modules must report wake bits); stock INT_EN values; CPU_INFORM 4; the 545/3/37/9 lists with stock conditions; counter-based early-wake detection; a wake-reason dump; clearing CPU_INFORM on both paths. | §1.2–1.4 |
| M4 | `lpm_init` parity: read back the PMU durations, MIF PwrMgmtMode, MASK_IRQ0 and SHORTSTOP; set the PMU ones only if they differ. | lines 8-111 |
| M5 | MCT across power loss. `pixel-mct` has `tick_resume = mct_shutdown` and never restarts the free-running counter (`R: pixel-mct.c:116-124`). After MISC loses power the counter stops, so the broadcast timer is dead and CPUs in C2 never wake. Mainline restarts the FRC in `exynos4_frc_resume()`. | |
| M6 | PMIC opmodes. We have no s2mpg12/13 regulator driver, but stock rewrites every always-on rail's enable field at boot (`S: s2mpg12-regulator.c:51-83`): field 1 = on while PWREN (off in SLEEP), 2 = on while PWREN_MIF, 3 = always on. LIVE: `LDO_CTRL1 = 0x38` decodes as L7M = 0, **L11M_CPUCL1_M = 2, L12M_CPUCL0_M = 3**, L13M = 0, where stock wants 1 for L11M, L12M and L13M. The bootloader leaves at least one SUSPEND rail always on. Also check PCTRLSEL1–14 (0x9B–0xA8) against DT `sel_vgpio` (`DT: gs201-pmic.dtsi:123-125`). | |
| M7 | Device parity for power loss and stock rails. See §3: display (DPU/DISP), UFS, USB (HSI0), Wi-Fi out-of-band wake, BT host-wake or BT off, S2MPU_HSI1 (stock `resume_cp()` calls `s2mpu_restore()`, `S: modem_ctrl_s5100.c:1695-1712`), and INT at 100 MHz. | |
| M8 | Userspace. `pixel-suspend.py` accepts only `[s2idle]` (`R: scripts/pixel-suspend.py:52`); it needs a deep mode, a one-shot budget, health checks and fallback. | |

## 3. Preconditions stock enforces before SLEEP, and our state

| Item | Stock | Ours today | Needed for deep |
|---|---|---|---|
| Secondary CPUs | Hotplugged off; clusters 1/2 CPD | Never offlined | M1 |
| Modem (CP, PCIe ch0 / HSI1) | Refuse if CP2AP_WAKEUP is high. AP_ACTIVE low. Link up → HSI1ON (6); link down → SLEEP (4). ch0 is `use-pcieon-sleep = "true"`, `phy-power-off = "false"` (`DTBO fragment@34`). L16M/L18M PCIE0 are SUSPEND rails. | cpif powers the host down during suspend (`R: docs/suspend-20260930.md`), so mode 4 applies. HSI1 is power-cycled. | Snapshot S2MPU_HSI1 CTRL0 before and after; restore before cpif resumes the link. |
| Wi-Fi (PCIe ch1 / HSI2) | bcmdhd D3, link L2, PHY off. L3S/L18S PCIE1 are SUSPEND rails, so they are off in SLEEP. Wake-on-WLAN uses the OOB `wl_host_wake` gpa6-1 (`DTBO fragment@64/66`). | L1.2 in s2idle. `system_link_off` refuses when wake is enabled, and there is no OOB wake. | L2 plus PHY off always; OOB wake later. Until then, unload Wi-Fi or disable its wake for trials. |
| UFS (HSI2) | `spm_lvl` 5: PowerDown, link off, RST_N low, PHY off; full re-init on resume. L2S_PLL_MIPI_UFS is a SUSPEND rail. | Link active, or opt-in Hibern8 on bootloader-owned clocks | Level-5-style power-down with probe-style re-init, plus HSI2 CMU restore (`hsi2_save`, `S: flexpmu_cal_local_gs201.h:605-654`) if HSI2 is shown to be power-cycled. |
| USB (HSI0) | Disconnected; `pd_hsi0` off (`power-down-ok = PD_OK_USB`); PHY LDOs off. TCXO vote only in host L2. | Gadget always configured, HSI0 on; L8M/L9M/L10M on (LIVE) | Unplugged for all trials. Later: PHY/HSI0 off when unplugged, restore through `hsi0_save`. |
| Display | Off; DPU/DISP genpd off (`suspend_late`) | Panel sleep-in; DPU/DISP powered; driver relies on bootloader DECON/DSIM state | Either power DPU/DISP off with a cold-init path, or save and restore `dpu_save`/`disp_save` (`S: flexpmu_cal_local_gs201.h:670-770`) plus DECON/DSIM registers before panel sleep-out. |
| GPU | G3D off when idle | gs201-g3d-pd genpd | Verify G3D STATUS (0x1e04) is 0 before deep. |
| Touch (S3908) | goog/syna driver puts the IC to sleep; rails L25M/L26M stay on | No sleep command. The worker freezes, the IC keeps scanning (`R: mainline/pixel-touch-input.c`) | Send the TCM deep-sleep command on screen-off and exit on wake. |
| AoC | Always on (`pd_aoc` disabled); wakes AP by mailbox (WAKEUP[7], WAKEUP2[5–8]) | Running; USF non-wakeup channel | Unsubscribe sensors at screen-off; count AoC mailbox wakes. |
| Clocks and DVFS | MIF 421 MHz, INT 100 MHz at suspend | MIF 421 MHz on screen-off; INT left at the bootloader value | Set INT (and INTCAM etc.) to `suspend_freq` through ACPM DVFS, as MIF already is. |
| Idle-IP / ACPM queues | Checked for SICD only, not SLEEP | — | Keep the ACPM-idle check as an extra guard. |
| Watchdog | Stopped and restored by the driver | `pixel-wdt-pm` in `noirq` | Force `keep_running = 0` in deep: MISC powers off, so the watchdog cannot act as a deadman during SLEEP. |

## 4. Wake sources and PMU routing

EINT bits are computed with the stock formula (§1.2) from the alive banks
(gpa0, gpa1, gpa2, gpa3, gpa4, gpa5, gpa9, gpa10), then the far banks (gpa6,
gpa7, gpa8, gpa11). They match exynos-pm `gpa-use` (`DT: gs201.dtsi:394`) and
`pixel-sicd` (`~(BIT11|BIT28|BIT29)` in 0x3a84). Everything goes through
WAKEUP_INT_EN[4] (EINT, alive banks) or [5] (EINT_FAR).

| Source | Stock route | PMU bit | Ours today |
|---|---|---|---|
| Power key | S2MPG12 PWRONB → I3C IBI → VGPIO2AP (SPI 72); s2mpg12 driver, GIC wake only (`S: drivers/mfd/s2mpg12-core.c:535-558`). Sleep path: PMIC/VGPIO. WAKEUP2[13] VGPIO2PMU_EINT is named but **not** in stock INT_EN, so it is firmware- or hardware-routed (unverified). gpa10-0 is muxed EINT with pull-up but unused (`DTBO fragment@28`). | EINT 43 → 0x3a84[11] (our fallback) | `pixel-keys` on gpa10-0: GIC wake only |
| Volume down / up | gpio-keys `wakeup-source` (`DTBO fragment@29`) | 44 → 0x3a84[12]; 62 → [30] | `pixel-keys`: GIC only |
| RTC alarm | S2MPG12 RTC → IBI → VGPIO; stock expects WAKEUP_STAT[0] (INF); the tick must stay masked, since RTC_TICK is enabled | WAKEUP_INT_EN[0] | `pixel-rtc` (SPI 72); INT2M unmasks only RTCA0 (good) |
| CP2AP_WAKEUP | cpif EINT gpa8-3 with `enable_irq_wake` | 60 → 0x3a84[28] | cpif and `pixel-gpio modem=1`: GIC only |
| CP phone active | gpa8-4 | 61 → 0x3a84[29] | Same |
| Modem PCIe (HSI1ON) | EXT_PCIE_GEN4A_0 [12], INTREQ_PCIE_GEN4A_0 [28] | Enabled | Not used (link down) |
| WLAN host wake | bcmdhd OOB gpa6-1 | 46 → 0x3a84[14]; EXT_PCIE_GEN4A_1 [14] | None: in-band only |
| BT host wake | gpa6-6 | 51 → 0x3a84[19] | Not wired; rfkill BT during deep |
| USB / charger | MAX77759 gpa9-4 (39 → [7]), USB-PD gpa9-7 (42 → [10]), USB_REWA [16] | | `pixel-battery` polls; no IRQ |
| AoC | MAILBOX_AOC2AP [7]; WAKEUP2 [5–8]; aoc2ap gpa8-7 (64 → 0x3a88[0]) | Enabled | AoC mailbox IRQs, GIC |
| APM | MAILBOX_APM2AP, WAKEUP2[4] | Enabled | — |

Never set wake on touch (gpa7-0, 53), the amplifiers (gpa6-3, 48; gpa8-6, 63),
SMPL_WARN (gpa5-0) or the PMIC pin gpa0-6. Every mask must be written and then
**read back and compared** before the PSCI call; a mismatch aborts.

## 5. Where the power goes, and the 25 mA budget

### 5.1 Today's s2idle, 0.81–0.85 W metered

The per-rail values are the September 28 awake, screen-off image X table
(`R: docs/power-20260928.md`), adjusted for later changes. s2idle keeps PWREN
high, so none of these rails can turn off.

| Rail group (ODPM channels) | mW | In stock SYS_SLEEP |
|---|---|---|
| LLDO/MLDO feeds (S6M, S9M, S3S, S6S, S7S, S10S) | ≈250–260 | Downstream TCXO, PLL, HSI, PCIe and CPU_M LDOs are SUSPEND, so they go off; feeds stay for the ON LDOs |
| INT + INT_M (S5M, S7M) | ≈120–135 | Off |
| CPU clusters (S2M, S3M, S4M; PLLs running in CPD) | ≈100–120 | Off |
| SA + SD | ≈100 | SA stays on (1.8 V feed); SD off |
| CAM + G3D bucks (S1S, S2S), domains off | ≈95 | Off |
| MIF + DRAM + SLC (S1M, S4S, S5S, L15M) | ≈60 | MIF and VDDQ off; VDD2H/VDD2L on (self-refresh) |
| Display (panel sleep) | ≈0 | — |
| Modem + RFFE, WLAN/BT (VSYS shunts) | ≈25–60 (EST; unmeasured since the modem came up) | Unchanged: chip-internal PS |

The battery side is roughly 1.1–1.3 W, **≈300 mA** (EST: from 1.55 W USB input
awake and the ODPM ratio). That is about 12× the stock target. Only SYS_SLEEP
removes the first six rows.

### 5.2 Budget toward ~25 mA

Values are mA at the battery.

| Component | Stock (EST) | Ours now (EST) | Stock suspend mechanism | Our action |
|---|---|---|---|---|
| SoC logic, CPU, cluster, CAM and GPU bucks, SoC LDOs | 2–3 (ALIVE + APM) | ≈180 | SYS_SLEEP, PWREN rails off | M1–M6 |
| DRAM 12 GB self-refresh, MIF, SLC | 1.5–3 | ≈17 | smc/mif sequencers | SYS_SLEEP; confirm `sleep_mif_down` increments and MIF req master is 0 |
| Modem, LTE+IMS idle DRX | 3–7 | 4–10 | CP DRX; AP_ACTIVE low | Measure VSYS_MODEM/RFFE; keep the filters |
| AP wake overhead | 2–5 | n/a | Few wakes | ≤1 wake/min. At ~0.2–0.4 J per wake, one wake every 10 s costs 5–10 mA |
| Wi-Fi + BT (BCM4389) | 1.5–3 | 3–6 | Link L2, OOB wake, chip PS | L2 plus OOB (Stage 6); BT not discoverable, or off |
| AoC, sensors, DMICs | 2–5 | 2–5 | AoC always on | Sensors off at screen-off |
| Touch S3908 | ~0.1 | 2–8 | IC deep sleep | TCM sleep command |
| Panel | ~0 (no AOD) | 0.3–1 | Panel rails off | Later: cold panel init |
| UFS | ~0.1 | 0.5–2 | spm_lvl 5, VCC off | Level-5-style power-down |
| USB PHY / HSI0 | ~0 | 1–3 | PD off, PHY LDOs off | Off when unplugged |
| Amplifiers ×2, haptics | ~0.1 | ≤1 (unknown) | Hibernate | Verify standby |
| NFC / eSE (ST54J, eSIM) | 0.1–0.3 | Same | Standby | **Never touch** (the line lives there) |
| GNSS, UWB, UDFPS | 0.1–0.5 (GNSS rails on) | 0 (LIVE: rails off) | — | Already below stock |
| GSC, PMIC quiescent, charger/TCPC/gauge | 1–2 | 1–2 | — | — |
| **Total** | **≈15–30** | **≈300** | | |

### 5.3 Work ranked by mA per effort and risk

1. **SYS_SLEEP core (M1–M5): about −200 mA.** High effort, high but bounded
   risk; nothing else reaches the target.
2. **Touch deep-sleep at screen-off: −2 to −8 mA (EST).** About a day, low risk,
   independent of everything else. **Cheapest win.**
3. **INT (and other domains) at stock `suspend_freq` on screen-off.** Tens of mW
   in s2idle (EST), and a SYS_SLEEP precondition. Low effort (the ACPM DVFS path
   exists for MIF), low to medium risk.
4. **PMIC opmode parity (M6): up to tens of mA inside SYS_SLEEP.** Low effort,
   medium risk (PMIC writes). Only after SYS_SLEEP works.
5. **Modem wake-rate instrumentation and reduction: −2 to −10 mA after deep.**
   Low effort, low risk. Classify CP2AP wakes by cpif channel (IPC, RFS, data or
   none), then address the data PDN and keep-alive traffic.
6. **Wi-Fi L2 plus OOB host wake: −2 to −4 mA versus L1.2,** and required for
   deep with Wi-Fi on. Medium effort, medium risk.
7. **USB PHY/HSI0 off when unplugged: −1 to −3 mA.** Medium effort, medium risk
   (USB recovery history).
8. **UFS power-down: −0.5 to −2 mA.** Medium effort, high risk (rootfs).
9. **BT page-scan, or off: −0.1 to −1 mA.** Trivial.
10. **Panel rails off: −0.3 to −1 mA.** High effort.

**Skip:** SICD (no measured saving) and runtime CAM/G3D buck changes (SYS_SLEEP
switches them off; reclocking a gated GPU has reset the phone before).

## 6. Staged implementation plan

Every stage follows the same rules:
- the watchdog feeder is armed first;
- non-ALIVE MMIO is read only when that block's PMU STATUS is on;
- there is no `/dev/mem` and no ACPM debug-memory writes;
- every register write is read back;
- one attempt per module load (a budget, as with `sicd_budget`);
- `mem_sleep_default=s2idle` stays until Stage 7;
- sync after each deploy.

### Stage 0 — Baseline and read-only audit (no sleep changes)

**Code:** a `pixel-sleep-audit.ko` report. It reads:
- **PMU ALIVE:** 0x3944, 0x3950, 0x3964, 0x3970, 0x3a80–0x3a88, 0x3a10, 0x3920,
  0x1044, 0x860–0x87c and 0x3cb0–0x3cc8, plus every PD STATUS (AOC 0x1884,
  HSI0 0x2084, HSI1 0x2104, HSI2 0x2184, DPU 0x2204, DISP 0x2284, G3D 0x1e04,
  and the rest).
- **intr_gen:** GRP1, GRP2, GRP4, GRP27 and GRP31.
- **`lpm_init` and `save_sleep` targets:** only in powered blocks. Read CPUCL1/2
  only while that cluster's NONCPU STATUS is on, with the reading thread pinned
  there.
- **S2MPU_HSI1 and HSI2 CTRL0:** 0x11880000 and 0x145e0000, only with the block on.
- **The MCT G_TCON.**
- **All S2MPG12/13 regulator CTRL registers and PCTRLSEL1–14,** over the existing
  ACPM PMIC read.
- **The FLEXPMU stats.**

A host script diffs the report against the stock tables (§1.3, the PMIC opmodes)
and a raw copy of this page's tables.

**Measure:**
- all 24 ODPM channels, awake and in s2idle;
- MAX77759 gauge coulombs (QH, 0x4D; confirm read-only on this gauge) over 30
  minutes unplugged in s2idle, which gives the battery-side mA baseline;
- CP2AP wakes per hour, by channel.

**Optional 0b:** `fastboot boot` the saved stock images (no flash) and read the
same registers, `mif_always_on` and the PMIC opmodes under the stock kernel
through debugfs. This gives ground truth for M4 and M6.

**Hang risk:** only from reading an unpowered block, and the STATUS gating
prevents that.

### Stage 1 — CPU hotplug (M1, M2)

**Code:** CPU hotplug callbacks in `pixel-cpupm` (port mainline GS101), CPD
for the last CPU of clusters 1/2, an `in_cpuhp` C2 refusal, and a
`deep_in_progress` flag that makes the CPU_PM notifier a no-op.

**Test, unattended:**
1. 1,000 offline/online cycles for each of CPUs 1–7, mixed with idle.
2. Check CPU STATUS = 0 while offline, and cluster NONCPU = 0 when a cluster is
   empty.
3. Run `echo deep > /sys/power/mem_sleep; echo processors > /sys/power/pm_test`
   followed by a suspend. This offlines and onlines all secondaries with no
   firmware sleep.

**Success:** no "failed to die", no RCU stall, and CPD and C2 counters still
moving.

**Hang risk:** a refused CPU_OFF leaves the CPU in the park loop. It is warned
and bounded, and the watchdog is live.

### Stage 2 — Survive power loss of MISC and TOP (M5)

**Code:**
- `pixel-mct`: restart the FRC in `tick_resume` or a clocksource `.resume`,
  mirroring mainline `exynos4_mct_frc_start()`, then re-run the one-shot
  self-test.
- `pixel-wdt-pm`: refuse `keep_running` when deep is selected.
- Snapshot helpers (reads only) for the `hsi2_save`, `dpu_save`, `disp_save` and
  `hsi0_save` CMU lists and S2MPU_HSI1.

**Test:** `pm_test=core` (§3), which runs all syscore ops, then pinned-timer
sleeps on every CPU.

### Stage 3 — `pixel-sleep` in dry run and `pm_test=core` (M3, M4)

**Code:**
- Generate the lists from `flexpmu_cal_system_gs201.h` with a host script, and
  keep the COND semantics exactly.
- syscore suspend:
  1. wake registry → EINT masks;
  2. clear STAT;
  3. INT_EN = 0x1001f0bf / 0x1f0;
  4. CPU_INFORM[0] = 4;
  5. save;
  6. enter;
  7. **read back and compare everything**;
  8. record the counters.
- syscore resume: early or exit list plus restore, wake-reason log, and a
  CPU_INFORM clear on both paths.
- Refuse deep if the modem link is up (mode 6 is deferred).

**Test:** `pm_test=core` (TEST_CORE: syscore runs, then a 5 s delay, and
`suspend_ops->enter` is skipped; `M: suspend.c:462-474`). The firmware is never
entered. Expect the early path. Afterwards every restored register must equal
its pre-sleep value, and INT_EN and CPU_INFORM must be back to 0. In the first
runs, only log differences instead of writing CMU registers.

**Hang risk:** CMU restore writes. Stock writes identical values in the same
order, and this stage proves them to be no-ops before any real sleep.

### Stage 4 — First real SYS_SLEEP, attended (user standing by for a long-press reset)

**Setup:**
- A RAM-booted image with no UFS writes (read-only or RAM root).
- USB unplugged; use Wi-Fi SSH and pstore stage markers.
- Screen off, touch asleep, GPU idle.
- Wi-Fi unloaded; BT rfkilled.
- Modem registered. Its CP2AP wakes (observed every 0.8–73 s) act as a natural
  deadman.
- Wake sources: RTC at 30 s, power and volume keys over EINT, and CP2AP.
- INT at 100 MHz, MIF at 421 MHz.

**Success:**
- the PSCI call returns and the same boot continues;
- `sleep_soc_down` +1 (not early);
- WAKEUP_STAT names the source;
- all CPUs online, timers firing on every CPU, the MCT counting;
- the modem answers, and S2MPU_HSI1 is compared;
- ODPM interval shows the SoC rails near zero for the sleep.

**Learn:** which local domains were power-cycled (CMU snapshot diffs for HSI2,
DPU, DISP and HSI0), and whether `sleep_mif_down` moved (MIF req master).

**Failure handling:**
- no wake within about 2 minutes → the user long-presses power;
- a reset → read the pstore markers;
- an early wake → iterate.

### Stage 5 — Prove the wake paths, then go unattended

1. Wake on RTC only (every EINT masked, modem offline). Record WAKEUP_STAT and
   WAKEUP2_STAT.
2. Wake on the power key only.

Only when RTC wake is proven, run unattended loops: N cycles of 30–60 s with
the RTC, a health check after each, and an abort to s2idle on any anomaly.

**If the RTC cannot wake:** in an attended trial, try WAKEUP2[13]
(VGPIO2PMU_EINT, a stock-named bit).

### Stage 6 — Peripheral parity, one item per session, gauge-measured

In order:
1. Touch sleep.
2. PMIC opmode fixes: one rail at a time, only rails that stock marks SUSPEND,
   read back each. Never L14S (NFC) or the ALIVE rails.
3. Wi-Fi L2 with OOB wake: expose gpa6-1 through `pixel-gpio`, add a
   brcmfmac-PCIe OOB host-wake hook, and set EINT bit 46.
4. Display: save/restore of DPU/DISP state, or power-off.
5. UFS power-down and re-init.
6. USB off when unplugged.
7. BT host-wake gpa6-6, or off.

### Stage 7 — Policy and long tests

- Deep on screen-off, falling back to s2idle after K failures.
- Modem wake reduction.
- An 8-hour unplugged gauge run, compared with 25 mA.
- Only then change `mem_sleep_default`.

## 7. Top risks and open questions

1. **Wake paths are partly invisible.** The PMIC power key and RTC reach the PMU
   through I3C/VGPIO, and stock does not enable WAKEUP2[13]. A wrong assumption
   means a phone that never wakes, and the AP watchdog is powered off in SLEEP.
   **Mitigation:** independent EINT wakes (keys, CP2AP) with read-back, attended
   first trials, and the modem as the natural deadman.
2. **State lost in blocks the kernel assumes retained:**
   - the MCT FRC stops, so the broadcast timer dies and the system hangs after
     resume;
   - the DPU/DISP, HSI2 (UFS, Wi-Fi PHY) and HSI0 bootloader state is gone;
   - S2MPU_HSI1 may come back blocking modem DMA;
   - TOP_OUT retention not released leaves the GPIO-driven peripherals dead.

   **Mitigation:** the Stage 2 fixes, vendor save lists with snapshot diffs,
   running `exit_sleep` exactly, and a RAM boot.
3. **Firmware policy traps:**
   - `mif_always_on = 1` may keep the DRAM out of self-refresh, capping the
     saving;
   - un-run `lpm_init` durations may break the wake sequence;
   - the bootloader's PMIC opmodes (L12M = 3, L11M = 2) keep rails on in SLEEP;
   - a stray CPU_INFORM (1 over 4, or 4 left set after resume) misleads the
     firmware.

   **Mitigation:** read-only audit plus stock ground truth (Stage 0b), writing
   PMU-documented registers only, never ACPM debug memory, and explicit
   CPU_INFORM ownership during deep.

**Also open:**
- Whether the architectural counter keeps counting in SLEEP (TCXO off).
  Compare CLOCK_REALTIME with the RTC after long sleeps.
- Whether EL3 restores the GIC distributor. Check the `/proc/interrupts`
  deltas after resume.
