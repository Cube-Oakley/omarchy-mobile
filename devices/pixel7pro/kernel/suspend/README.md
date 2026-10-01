# Suspend validation fixes

`0001-wifi-commit-d3-ack-before-suspend.patch` applies after the brcmfmac
changes in [sleep-v25](../sleep-v25/README.md), against the same pinned kernel.
It is an incremental patch, not a complete replacement kernel patch.

The BCM4389 firmware sends its D3 acknowledgement as a control-ring message.
The old handler woke the PM task from inside message processing, before the
ring read pointer was committed. PM could then set the device to STATE_DOWN,
and the ring write callback refuses writes in that state. This leaves a race
between acknowledging the firmware message and suspending the transport.

The patch defers publishing D3 completion until the interrupt thread has
finished RX processing. Legacy TCM-mailbox acknowledgements retain their
existing behavior. Eight consecutive RTC sleep/resume cycles passed with normal Wi-Fi idle
power saving and working Wi-Fi HTTPS after every wake. An incoming call then
woke the AP after 57.6 seconds asleep and worked normally. The patched module
is pinned in the private persistent modem bundle and selected by the boot
helper after bundle verification; the boot image itself is unchanged.

Build only the Broadcom modules, supplying the matching cfg80211 exports:

```sh
make -C devices/pixel7pro/mainline/linux ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- \
  M=drivers/net/wireless/broadcom/brcm80211 \
  KBUILD_EXTRA_SYMBOLS="$PWD/devices/pixel7pro/mainline/linux/net/wireless/Module.symvers" \
  -j8 modules
```

The other suspend changes are in [CPIF](../cpif/README.md),
[the modem host](../modem/pixel-pcie-cp.c),
[watchdog PM](../watchdog/README.md), and the
[validation notes](../../docs/suspend-20260929.md).

## Nonblocking ACPM idle snapshot

`0002-acpm-idle-snapshot.patch` exports `exynos_acpm_is_idle()` for last-CPU
system-idle experiments. Apply it to the kernel before building the new
cpupm modules. Its caller must disable interrupts and establish that every
other host CPU is idle. It requires channel 0, no channel mutex owners or
pending sequence numbers, empty TX/RX rings, and matching channel-0 request
and response front indices (the stock `is_acpm_ipc_flushed()` check).

The snapshot only reads queue memory already mapped and validated by the
ACPM driver. It cannot guarantee future firmware activity will not occur;
it is a prerequisite check, not a lock on firmware. The read-only audit and
bounded SICD experiment resolve the symbol optionally so C2/CPD remain usable
on an older running kernel. The helper was compiled into a RAM-booted image
and observed four idle opportunities during a 10.273-second sleep with Wi-Fi
unloaded and UFS Hibern8 enabled. The later installed logging image includes
this read-only helper; the system-idle experiments still default off.

## Read-only firmware low-power counters

`0003-acpm-read-only-flexpmu-counters.patch` adds a GS201-only `flexpmu_stats`
read-only sysfs attribute to the ACPM platform device. It follows Google's
[`acpm_get_buffer()`](https://android.googlesource.com/kernel/gs/+/refs/heads/android-gs-pantah-5.10-android13-d1/drivers/soc/google/acpm/fw_header/framework.h)
and [`acpm_flexpmu_dbg.c`](https://android.googlesource.com/kernel/gs/+/refs/heads/android-gs-pantah-5.10-android13-d1/drivers/soc/google/acpm/acpm_flexpmu_dbg.c)
layout, using the existing SRAM mapping. It bounds the plugin table, checks
buffer-support versions, caps list traversal, validates every range, and
accepts only the exact `FLEXPMU_DBG` name. There are no writes or arbitrary
memory reads exposed to userspace. Firmware counters are independently
updated, so these are snapshots rather than an atomic structure.

The purpose is to distinguish an accepted CPU hint from executed SoC/MIF
power-down sequences. Full hint-to-wake intervals alone showed no clear
saving in the initial partial-rail comparisons. This patch needs a new kernel;
module replacement alone cannot add the attribute.

On-device validation in the RAM-booted image: the named buffer was found and
all sequencer counters initially read zero, with `mif_always_on=1`. An
8.369-second combined Wi-Fi-link/UFS/SICD trial advanced `soc_down` and
`sicd_soc_down` to 1, with zero early wakeups. Both MIF counters stayed zero
and the always-on flag stayed 1. Wi-Fi HTTPS and modem verification passed.
This confirms a SoC sequence, while identifying the memory-interface policy
as a remaining obstacle. No firmware policy flag was changed by this patch.

## Persistent console ownership

`0004-earlycon-ramoops-ownership.patch` lets the framebuffer console coexist
with `CONFIG_PSTORE_RAM`. Apply it after the existing display/earlycon patches.
With ramoops enabled, all custom RAM-log writes (including MMU-off assembly
stamps and setup_arch notes),
header clearing and custom mappings are skipped. Framebuffer setup and the
DRM takeover remain available. Without ramoops, the legacy log path remains.

The persistent-root builder enables `PSTORE_RAM` and `PSTORE_CONSOLE`, using
only the stock DT's 4 MiB region at `0xfd3ff000` and its console/pmsg split.
The boot helper archives previous records under `/var/log/pixel-pstore/`,
with directory mode 0700 and file mode 0600. It does not delete pstore records
or prevent startup when logging is unavailable. These private logs may contain
device identifiers and must not be committed.

## SYS_SLEEP: Stage 0 audit and Stage 3 sequence

These modules implement Stage 0 and Stage 3 (M3, M4) of the
[deep-sleep plan](../../docs/deep-sleep-plan-20260930.md). Neither is loaded
at boot, and `mem_sleep_default=s2idle` is unchanged.

| File | Role |
|---|---|
| `gen-sleep-lists.py` | Host generator for `pixel-sleep-lists.h` |
| `pixel-sleep-lists.h` | Generated stock tables. GPL-2.0-only, Samsung copyright kept |
| `pixel-sleep-common.h` | Read side shared by both modules: block map, power gates, FLEXPMU reader |
| `pixel-sleep-audit.c` | Stage 0 read-only register report |
| `sleep-audit-diff.py` | Host diff of an audit report against the stock tables |
| `pixel-sleep.c` | Stage 3 syscore sequence |

Build out of tree. `pixel-sleep.ko` imports `pixel_cpupm_system_sleep()` from
`pixel-cpupm.ko`, so build that first and load it first:

```sh
K=devices/pixel7pro/mainline/linux
make -C $K ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=<copy of kernel/cpupm> modules
make -C $K ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=<copy of kernel/suspend> W=1 \
  KBUILD_EXTRA_SYMBOLS=<copy of kernel/cpupm>/Module.symvers modules
```

Both modules build with no warnings at `W=1`. Build from a copy of each
directory so that no build output lands in the repository.

### Generated lists

Regenerate the tables with:

```sh
gen-sleep-lists.py --cal-dir <stock>/drivers/soc/google/cal-if/gs201
gen-sleep-lists.py --fetch
```

`--fetch` downloads the three headers at LineageOS `40ff934`. The generator
copies the tables entry for entry from `flexpmu_cal_system_gs201.h`:

| Stock array | Lines | Entries | C array |
|---|---|---|---|
| `pmucal_lpm_init` | 9-110 | 102 | `ps_lpm_init` |
| `enter_sleep` | 141-143 | 3 | `ps_enter` |
| `save_sleep` | 146-693 | 545 | `ps_save` |
| `exit_sleep` | 697-733 | 37 | `ps_exit` |
| `early_sleep` | 736-744 | 9 | `ps_early` |

- **Entries are copied unchanged.** Each entry keeps its stock access type,
  with the same `enum pmucal_seq_acctype` values, and its mask, value,
  condition register and source line.
- **The type breakdown matches the plan.** The save list has 300
  SAVE_RESTORE, 41 COND_SAVE_RESTORE, 170 READ and 34 COND_READ entries.
- **The generator refuses unfamiliar input.** It fails when a list size
  changes, when a type would need polling (WAIT, DELAY or RETRY), when an
  atomic set falls outside PMU_ALIVE, or when a condition register lies
  outside PMU/INTR_GEN. It also fails when `[SYS_SLEEP]` in
  `pmucal_lpm_list` names other arrays.
- **Every base is reviewed.** Each base must appear in the generator's
  `BLOCKS` table, which records the block's name, its gating PMU STATUS and
  its write class. The PMU STATUS offsets come from the `*_STATUS` reads in
  `flexpmu_cal_local_gs201.h` and `flexpmu_cal_cpu_gs201.h`, and HSI1 0x2104
  from the stock save condition. The block for each status comes from the
  `cmu_id` in `gs201-pm-domains.dtsi`.
- **The output also lists the power registers.** It emits every PD STATUS
  register with its source line (`ps_pd[]`, 38 entries).
- **It also copies the power-domain save lists.** Stock saves each
  `pmucal_pd_list[].save` array in `flexpmu_cal_local_gs201.h` before powering
  the domain off and restores it after powering it on
  (`pmucal_local.c:63,119`). The generator emits their SAVE_RESTORE entries
  in order as `ps_pd_save` (1,023 entries, 24 domains; "line" is in the local
  file) with `ps_pd_lists[]`. The debug READs are left out. The HSI0, DISP,
  TPU and AUR lists also wait for a PLL lock, which the runtime does not do,
  so those four are flagged `PS_PD_WAITS` and only compared. Each entry's
  block must be gated on its own domain's STATUS.

### Register access rules (both modules)

- **Only stock-listed registers are touched.** Every register is a stock
  table entry, or a fixed PMU/INTR_GEN/EINT_PEND/S2MPU/MCT offset cited at
  its definition.
- **ALIVE and TOP blocks need no power check.** PMU_ALIVE, PMU_INTR_GEN, APM,
  MISC, PERIC0/1, CMU_TOP, MIF and CPUCL0 are read without a gate.
  CPUCL0 is read only from CPUs 0-3.
- **Every other block is gated on its power domain.** A block is accessed
  only while its PMU STATUS bit 0 is set. This is the stock COND test
  (`pmucal_rae.c:67-77`), applied to every access, including the stock
  entries that have no condition.
- **CMU_CPUCL1/2 is gated on its cluster.** It is accessed only while that
  cluster's NONCPU STATUS (0x1404 or 0x1604) is on. The audit also reads it
  only from a CPU of that cluster, over an IPI. Stock reads these entries
  without a condition; `gate_cpucl=0` restores that behaviour.
- **PMU_ALIVE writes use the secure SMC.** They go through `0x82000504`
  (`set_priv_reg`), exactly as `pmucal_write_reg()` sends 0x1806xxxx
  (`pmucal_rae.c:140-148`). A masked write reads the register over MMIO first.
  An atomic set writes the bit number to `offset | 0xc000`, and the alias
  itself is never read.
- **All other writes use `writel`.** That covers INTR_GEN, CMU, sysreg, DMC,
  UFS and TREX, as in stock.
- **Some registers are never touched.** These include ACPM debug memory
  (`MIF_ALWAYS_ON`) and any S2MPU register other than CTRL0 (offset 0).
  L14S_CTRL (the eSE/eSIM rail) is never read or written, and no NFC pin
  ever becomes a wake source (see `eint_deny`).

### pixel-sleep-audit.ko (Stage 0)

The module has no write path. It contains no SMC, PMU write, PMIC write or
`writel`; the `nm` output shows no `__arm_smccc_smc`. On load it reads:

- **PMU_ALIVE:** 0x3944, 0x3950, 0x3964, 0x3970, 0x3a80-0x3a88, 0x3a10, 0x3920,
  0x1044, 0x860-0x87c, 0x3cb0-0x3cc8, the PCIe PHY controls 0x3ec0/0x3ec4
  (`gs201-pcie.dtsi:54,114`), and all 38 PD STATUS registers.
- **INTR_GEN:** GRP1/2/4/27/31 UPEND and GRP2 ENABLE.
- **All 102 `lpm_init` targets and all 545 `save_sleep` entries**, with
  stock save semantics and the gates above.
- **S2MPU_HSI1/HSI2 CTRL0**, only while HSI1 or HSI2 is on.
- **MCT G_TCON.**
- **PMIC registers** over the ACPM PMIC read (channel 2, PM bank): 63
  S2MPG12 registers (every buck/LDO CTRL, LDO_CTRL1-3, the CTRL2 registers
  and PCTRLSEL1-14) and 57 S2MPG13 registers (every buck/LDO CTRL except
  L14S, LDO_CTRL1-2, the CTRL2 registers and PCTRLSEL1-11). No INT, STATUS
  or *SRC register is read, because those clear on read.
- **The patch-0003 FLEXPMU counters**, when the kernel has them.

Parameters:
- `oneshot=1` prints the report to the kernel log and fails the load with
  `-EAGAIN`.
- `domains=0` limits `lpm_init` and `save` reads to the ALIVE, TOP, MIF and
  CPU blocks.
- `g3d=1` also reads G3D-gated targets. They are skipped by default because
  G3D switches under GPU runtime PM while the audit runs.
- `pmic=0` skips the PMIC reads.

The report is `/sys/kernel/debug/pixel-sleep-audit/report`, one record per
line: `pmu`, `pd`, `intr`, `lpm` (value, stock value, mask, ok/DIFF/skip),
`save`, `s2mpu`, `mct`, `pmic`, `flexpmu` and `summary`.
`sleep-audit-diff.py report.txt [--dt live.dts]` prints every mismatch
against:
- the stock `lpm_init` values;
- the plan's awake-state expectations (INT_EN 0, CPU_INFORM, TOP_OUT
  retention bits 7/9/11-14, MCT started);
- the PMIC enable field the stock s2mpg12/13 drivers write for each
  always-on rail (`of_map_mode`: SUSPEND 1, MIF 2, anything else 3, shifted
  into the enable mask);
- PCTRLSEL against `sel_vgpio`.

Without `--dt`, the script uses a built-in table taken from the saved live
DT (dtbo index 6).

### pixel-sleep.ko (Stage 3)

**syscore order.** `register_syscore()` at module load places this module
after `cpu_pm` (`core_initcall`). Suspend runs in reverse registration
order, so `ps_suspend()` runs before `cpu_pm_suspend()`, and
`ps_resume()` runs after `cpu_pm_resume()`. This is the same relation stock
`exynos-pm` had: it was registered at `arch_initcall`, after `cpu_pm`
(`exynos-pm.c:671`).

**Suspend.** The steps follow `exynos-pm.c:233-314` and
`pmucal_system.c:27-83`:
1. Skip unless the target is `PM_SUSPEND_MEM` and `budget` is nonzero. One
   unit is consumed per attempt.
2. Check the preconditions (next section). If one fails, the suspend fails
   with `-EBUSY`.
3. Write EINT_WAKEUP_MASK1-3 from `eint_wake`: 1 means masked, and
   bit = stock EINT number.
4. Write WAKEUP_STAT = 0, WAKEUP_INT_EN = 0x1001f0bf, WAKEUP2_STAT = 0 and
   WAKEUP2_INT_EN = 0x1f0 (`gs201.dtsi:402-406`).
5. Call `pixel_cpupm_system_sleep(true)`, then write `CPU_INFORM[0] = 4`.
6. Read the save list, then run the enter list.
7. Read back each EINT mask, both INT_ENs, `CPU_INFORM[0]` and each enter
   WRITE. Any mismatch or SMC error undoes every write, calls
   `pixel_cpupm_system_sleep(false)` and fails the suspend with `-EIO`.
   WAKEUP_STAT is recorded but not compared, because it latches live wake
   events.
8. Record the FLEXPMU counters.
9. Write the stock pin power-down states of the HSI1/HSI2 pins (see
   `pixel-sleep-gpio.h`), then save the GPIO banks of the domains that power
   down, and every domain save list whose domain is on.

A CPU_PM notifier at `INT_MIN` priority re-reads `CPU_INFORM[0]` when
`cpu_pm_suspend()` sends CPU_PM_ENTER, the last step before PSCI
SYSTEM_SUSPEND. If the value is no longer 4, the notifier fails the suspend.

**Resume.**
1. Read the wake reason: WAKEUP_STAT, WAKEUP2_STAT and EINT_PEND for alive
   and far banks, at `0x180d0a00`/`0x180e0a00` (`exynos-pm.c:20`).
2. Decide early wakeup or exit, as described under early and exit detection.
3. Write `CPU_INFORM[0] = 0`. Stock does this in CPU_PM_EXIT
   (`pmucal_cpu.c:32`), before exynos-pm resumes.
4. Restore the GPIO banks, before the exit list releases pad retention (as
   stock's pin controller does from its earlier syscore resume).
5. Run `early_sleep` or `exit_sleep`, then restore the save list with
   `pmucal_rae_restore_seq()` semantics.
6. After an exit, compare every saved domain list and log each changed
   register. With `pd_restore=1` (default) the saved value is written back,
   except in `PS_PD_WAITS` lists and, unless `pd_restore_noc=1`, the NOCL
   lists. This stands in for genpd's power-on of each domain in stock
   `resume_noirq`; HSI2 and the NOCs have no stock domain driver, so their
   lists are the part to watch.
7. Restore INT_EN and the EINT masks to their pre-suspend values, and verify
   them.
8. Call `pixel_cpupm_system_sleep(false)`.

**Preconditions, in every mode:**
- CPU 0 is the only online CPU.
- `pm_test` reads `[core]` at PM_SUSPEND_PREPARE, unless `real_sleep=1`.
- `eint_wake` is valid: bits below 96, and none of the denied bits (6, 28,
  36, 48, 53, 63).
- The modem PCIe PHY is isolated: PMU 0x3ec0 bit 0 is 0. SLEEP_HSI1ON
  (mode 13) is deferred.
- WAKEUP_INT_EN and WAKEUP2_INT_EN are 0.
- `CPU_INFORM[0]` is 0.

**Preconditions added with `real_sleep=1`:**
- `dry_run=0`.
- The FLEXPMU counters are readable.
- CPUs 1-7 read STATUS off.
- The Wi-Fi PCIe PHY is isolated.
- G3D is off.
- The `lpm_init` PMU durations equal stock.
- The ACPM queues are idle (patch 0002).
- Every block the save list reads without a condition is on.

**Early and exit detection.** The module compares `sleep_soc_down` (the
SLEEP AP-down counter, `DID_AP_COUNT_SLEEP`) before and after, as
`exynos-pm.c:326` does: equal means early. It reads the counter through
patch 0003's `flexpmu_stats` attribute, calling the attribute's `show()`
directly into a private page, because syscore cannot read sysfs.

If the counters are unavailable, the fallback is the `pm_test` interlock.
Without counters the module arms only under `pm_test=core`, where firmware is
never entered, so the path is early. If a real attempt armed with counters but
cannot read them afterwards, the module assumes exit, because releasing
retention is the safer guess.

**Dry run** (`dry_run=1`, the default) requires `pm_test=core`.
- **Performed:** every PMU_ALIVE write (through the SMC) and every INTR_GEN
  write. These are the wake masks, INT_EN, CPU_INFORM, CPU0_INT_EN,
  SYSTEM_CTRL, GRP1/2/4/27/31 and TOP_OUT.
- **Logged instead of written:** every other write, with the current and the
  would-be value. That covers the save-list restore (a comparison of saved
  against current), and the exit list's DMC, DRCG, UFS and TREX writes.
- **Written by `dry_run=0`:** the restore, which then writes the saved values
  back.

**Parameters.**

| Parameter | Default | Effect |
|---|---|---|
| `budget` | 0 | Attempts allowed |
| `dry_run` | 1 | See dry run |
| `real_sleep` | 0 | Permit firmware entry |
| `eint_wake` | 43,44,62,60 | Power key, volume down, volume up, CP2AP |
| `gate_cpucl` | 1 | Gate CMU_CPUCL1/2 on the cluster |
| `lpm_init_pmu` | 0 | At load, write the differing stock PMU durations |
| `verbose` | 0 | Log every write |
| `pd_restore` | 1 | After an exit, write back changed domain save-list values |
| `pd_restore_noc` | 0 | Also for the NOCL lists |
| `hsi2_cycle` | 0 | Test: with `pm_test=core` and `dry_run=0`, power HSI2 off and on at arm (stock's domain sequence; 1 also asks the monitor for DTZPC save/restore, which it refuses for HSI2) |

Besides the stock lists, the module saves at arm and restores on resume: the
GPIO banks and pin power-down states (`pixel-sleep-gpio.h`), the domain
save lists, S2MPU_HSI1/HSI2 CTRL0 (both come back enabled, blocking DMA) and
the USIs in use (`pixel-sleep-usi.h`, after the CMU restore).

The counters `attempts`, `armed_count`, `refused`, `aborted`, `early_count`
and `exit_count` are read-only. `/sys/kernel/debug/pixel-sleep/last` holds
the full last record and the write log: list, stock line, PA, before, value,
after and action.

At load, the module compares the `lpm_init` targets in the ALIVE, TOP, MIF
and CPUCL0 blocks and logs every difference. With `lpm_init_pmu=1` it also
writes the seven PMU durations that differ, with read-back. The
subsystem-domain and CPUCL1/2 targets are covered by the audit instead.

### On-device tests

Results on 2026-09-30:
- **Stage 0** (`domains=0`): no hang, 844 report lines, no read errors. 35 of
  102 `lpm_init` targets differ from stock (the PMU wake durations, TCXO
  duration, DRCG, SHORTSTOP, CLKDIVSTEP), and 34 PMIC rails that stock sets to
  follow PWREN (off in SYS_SLEEP) are left always on by the bootloader,
  including every CPU, INT, MIF, camera and GPU buck. S2MPU_HSI1/HSI2 CTRL0
  read 0. The firmware reports `mif_always_on 1`.
- **Stage 3a** (dry run, `pm_test=core`): armed with the stock masks, CPU_INFORM
  still 4 at CPU_PM_ENTER, resumed through `early_sleep` by the SLEEP AP-down
  counter, 314 restore entries with none differing, 12 list writes, no SMC or
  verify errors. Two PMU bits do not read back as written and are now
  expected: EINT_WAKEUP_MASK3 implements three bits, and WAKEUP_INT_EN bit 7
  (MAILBOX_AOC2AP) does not stick. Other attempts were refused by cpif or by
  the modem PCIe link being up; stock would use SYS_SLEEP_HSI1ON (mode 13)
  for the latter, which is not implemented.
- **Stage 2** (`pm_test=core`, new pixel-mct at boot): the MCT resume self-test
  ran in each armed cycle with no errors; pinned 0.5 s sleeps took 0.506-0.509 s
  on every CPU afterwards.
- **Stage 3c** (`real_sleep=1` under `pm_test=core`): every real-sleep
  precondition held with Wi-Fi unloaded (its PHY isolated, 0x3ec4 = 0),
  secondaries off, G3D off, the stock `lpm_init` PMU durations written
  (`lpm_init_pmu=1`) and ACPM idle; two armed cycles took the early path with
  verify clean, UFS Hibern8 cycled, and Wi-Fi reloaded and reconnected.
  pixel-pcie's `system_link_off` refuses deep suspends (it is s2idle-only), so
  Wi-Fi has to be unloaded for SYS_SLEEP until that path accepts deep. The
  retention opmodes (kernel/suspend/pixel-pmic-opmode.c `retention=1`) are
  set.
- **Stage 4** (first real firmware entry, attended, USB unplugged, Wi-Fi
  unloaded, root remounted read-only first): PSCI SYSTEM_SUSPEND entered the
  stock SYS_SLEEP and returned through `exit_sleep`, verify clean. The
  firmware counted `sleep_soc_down` 0 -> 1 and `sleep_mif_down` 0 -> 1: the
  memory interface went down and DRAM kept its contents (with the retention
  opmodes set). The 40-second RTC alarm woke it through the PMIC
  (WAKEUP2_STAT bit 13, VGPIO2PMU_EINT, although WAKEUP2_INT_EN does not
  enable that bit). All CPUs came back, the timers ran, and 9 of the 314
  restored clock registers had lost their values. Two things did not survive:
  - UFS: the HSI2 domain was power-cycled, and Hibern8 exit timed out
    (`-110`). The runner found root unreadable and rebooted, as designed.
  - The modem: cpif saw PHONE_ACTIVE low right after wake and declared a CP
    crash. The GPIO banks of power-cycled domains (HSI1's gph0/gph1 carry the
    CP control lines) were not restored before pad retention was released;
    stock's pinctrl driver saves and restores them.
  The modem's startup guard then had to be archived and the phone rebooted.
- **Stage 5** (the pin power-down states, GPIO banks and domain lists added;
  UFS link off; see docs/suspend-20260930.md): entered and exited again with
  8 pin states written, 52 GPIO registers and 1 CMU_HSI2 register restored.
  HSI2 was power-cycled; UFS link startup failed after it, and the modem
  crashed after the 2.2 s the UFS retries held resume.

**Stage 0 (audit).** Mount debugfs if needed, then run the audit twice. The
first run covers only ALIVE, TOP, MIF and the CPUs:

```sh
insmod pixel-sleep-audit.ko domains=0
cat /sys/kernel/debug/pixel-sleep-audit/report > /root/audit-alive.txt
rmmod pixel-sleep-audit; sync
insmod pixel-sleep-audit.ko
cat /sys/kernel/debug/pixel-sleep-audit/report > /root/audit-full.txt
rmmod pixel-sleep-audit; sync
```

On the host, run `sleep-audit-diff.py audit-full.txt`.

It passes when:
- both loads return without a hang;
- the summary line ends `overflow 0`;
- the report has 27 `pmu`, 38 `pd`, 6 `intr`, 102 `lpm`, 545 `save`,
  2 `s2mpu`, 1 `mct` and 120 `pmic` lines, none of them `err:`;
- skips are only `cond` (a stock condition), `pd-off`, `cluster` or
  `excluded` (G3D).

The diff output is the Stage 0 result.

**Stage 3a (dry run).** This needs the new `pixel-cpupm.ko` loaded, Stage 1
(`pm_test=processors`) passing, and a kernel with patches 0002 and 0003.

```sh
insmod pixel-sleep.ko budget=1
echo deep > /sys/power/mem_sleep; echo core > /sys/power/pm_test
echo mem > /sys/power/state
echo none > /sys/power/pm_test; echo s2idle > /sys/power/mem_sleep
dmesg | grep -E 'pixel-sleep|suspend debug'; cat /sys/kernel/debug/pixel-sleep/last
grep . /sys/module/pixel_sleep/parameters/*
```

It passes when:
- the log shows `armed SYS_SLEEP (dry run), masks ffffffff afffe7ff ffffffff`;
- the log shows the 5-second `suspend debug` wait;
- the log shows `resumed via early_sleep (SLEEP AP-down counter), verify ok`;
- `CPU_INFORM at CPU_PM_ENTER 4`, not replaced;
- `sleep_soc_down` is unchanged;
- restore `differed 0`, or only listed status-bit differences;
- the list writes are `done 12` (3 enter + 9 early) with dry 0, gated 0, SMC
  errors 0 and verify 0 bad;
- `early_count 1`, `aborted 0`, `refused 0` and `budget 0`;
- after resume, all CPUs are back online with timers ticking, SSH returns and
  the modem answers.

Two optional checks exercise the interlocks:
- `budget=1` with `pm_test=none` must refuse (`echo mem` fails with EBUSY)
  and never enter firmware;
- `budget=0` with `pm_test=core` must log `budget 0: not armed`.

**Stage 3b (live writes under `pm_test=core`).** Run
`echo 0 > .../dry_run; echo 1 > .../budget`, then repeat the same sequence
three times. It passes with the same criteria, except that the restore is
now `written N` with `differed 0`. N is roughly the number of SAVE_RESTORE
entries read (about 276-317). A save list from the audit before and after
the runs must also match, except for status bits. Never set `real_sleep` in
Stage 3.
