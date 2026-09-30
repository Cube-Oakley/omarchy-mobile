# Modem (Samsung S5300 CP on PCIe channel 0)

The bring-up follows the staged plan in the feasibility study (kept outside
the repo). This directory holds what has been built and run on the phone so
far. Full boot, AT, physical-Tello-SIM registration and cellular IPv4 traffic
now pass; see the
[RAM-only runtime](../../modem/README.md) for G2/G3 evidence and setup.

## Rules

The early probe stages keep the radio off. The authorized Tello runtime enables
it through the supervised userspace service. Only registers named in Google's
sources are touched, with the watchdog kicker armed. The boot script sets the partitions
that hold this phone's identity and calibration read-only in the block
layer, and the modem firmware partitions too: persist, efs, efs_backup,
modem_userdata, devinfo, mfg_data, fips, modem_a and modem_b. Nothing here
writes them. Raw backups of the first seven are kept on the host, outside
git.

## Stage 0: preparation (done 2026-09-29)

- Backups taken, SHA-256 checked against the phone.
- The CP handover inputs are present in `/chosen/plat` (`modem_hw`,
  `modem_sku`, `rf_sub`, `rfid`, stage fields) and `/chosen/config`. The
  `model_info-system_rev` node is absent, but its cbd routine only publishes
  a diagnostic property. Full boot succeeded without fabricating that input.
- PCIe channel 0 supplies: S2MPG12 LDO16M and LDO18M are on (CTRL 0xca and
  0xec, opmode bits 7:6 = 11).
- PMU PHY isolation: channel 0 (0x18063EC0) is 0x0, isolated; channel 1
  (0x18063EC4) is 0x1, bypassed for Wi-Fi.

## Stage 1: CP power through GPIO ([cp-power-probe.c](cp-power-probe.c))

A throw-away module runs cpif's power sequences on the stock DT's `/cpif`
lines and prints them (`action=status|on|off`; it never stays loaded):

| Line | Pin | Bootloader state |
| --- | --- | --- |
| `ap2cp_cp_pwr_on` | gph1-2 | input, pull-down |
| `ap2cp_nreset_n` | gph1-3 | input, pull-down |
| `ap2cp_cp_wrst_n` | gph1-6 | input, pull-down |
| `ap2cp_pda_active` | gph1-1 | input, pull-down |
| `ap2cp_dump_noti` | gph0-3 | input, pull-down |
| `ap2cp_pm_wrst_n` | gpa0-3 | input, pull-down |
| `cp2ap_cp_wrst_n` | gpa0-2 | input, reads 0 |
| `cp2ap_cp_ps_hold` | gpa8-2 | input, reads 0 |
| `cp2ap_wake_up` | gpa8-3 | input with pull-up, reads 0 |
| `cp2ap_phone_active` | gpa8-4 | input, reads 0 |

Bank offsets come from Google's `pinctrl-gs201.c`: HSI1 0x11840000 (gph0 +0,
gph1 +0x20), ALIVE 0x180D0000 (gpa0 +0), FAR_ALIVE 0x180E0000 (gpa8 +0x40).
gph1-0 is the AoC's bus-ownership pin, and gph1-4/5, gpa0-4..7 and
gpa8-5/6 belong to other hardware. The module changes only the CP's own bits,
read-modify-write.

Results on 2026-09-29, power-on without PCIe (PERST# stays low):
- `cp2ap_wake_up` goes high at power-on and low again at power-off.
- `cp2ap_cp_wrst_n`, `cp2ap_cp_ps_hold` and `cp2ap_phone_active` stay low for
  10 s. On stock these come after the CP ROM boots its image over PCIe, so
  this is expected before stage 2.
- The power draw could not be checked. The PMIC power meters
  (`kernel/odpm`) had stopped updating: their 20-bit sample count sat at its
  limit, and neither ASYNC_RD nor restarting the meter cleared it. Google
  defines a meter reset (MRST) in the MT_TRIM bank, which is not written
  here until its use in Google's driver is known.

After power-off, every AP line is back to input with pull-down.


## Stage 2: PCIe and signed ROM boot (G1 passed 2026-09-29)

The disposable probe and the Linux PCI host both booted the stock signed
BOOT image to `boot_stage=0x3fff`, `err_report=0`, and observed
`CP2AP_WAKEUP=1` after the ROM link shut down. The bootloader then established
Gen3 x2. No MAIN image, NV, SIM command, or radio-power command was sent.
No S2MPU or SysMMU changes were needed.

| Test | Observed |
| --- | --- |
| Initial ROM link | Gen1 x1, LTSSM `0x11` |
| Vendor secure-ATU SMC | returned 0 |
| Endpoint | `144d:a5a5` |
| BAR0 | PCI `0x14e00000`, CPU `0x40000000`, 1 MiB |
| Doorbell | CPU `0x40060000`, value `0x10000` |
| ROM mailbox | `0xff` → `0x3fff`, no error, about 24 ms after ringing |
| Linux MSI | four vectors allocated; two actual interrupts received |
| Bootloader link | Gen3 x2, endpoint readable after PCI state restoration |

### Files

Runtime wake timing was corrected during the September 29 SIM tests. The host
defaults to no diagnostic checkpoint delays (`diagnostic_delay_ms=0`); the ROM
probe retains 150 ms pauses. Hardware reset and PERST delays remain. Following
the pinned vendor sequence, the host waits 2.8–3 ms after the first L0 and checks
the requested speed, training bit and L0 again before endpoint access. The first
L0 can be a transient Gen1 state while the endpoint retrains to Gen3.

Over 325 hardware wakes measured a median 35.1 ms from isolation release to
secure ATU, versus roughly 1.7 seconds with diagnostic pacing. No power-on
failures were logged in that window. A cellular DNS query and TLS-verified
HTTPS transfer passed; subsequent bearer reconnection remains under diagnosis.

- `cp-rom-probe.c`: direct, disposable hardware probe. `phase=1` tests the
  link, `2` adds secure ATU and endpoint ID, `3` boots signed BOOT, `4` adds
  the bootloader's Gen3 link. The default refuses without touching hardware.
- `pixel-pcie-cp.c`: manually loaded channel-0 host. Loading leaves CP off;
  `exynos_pcie_poweron/off` manage PCIe, while the caller manages CP GPIOs.
  It uses Linux PCI enumeration with custom configuration operations and
  only the MSI component of DesignWare. It never calls DWC host setup,
  iATU detection, or iATU programming. Only the first DBI page and the
  separate DBI2 BAR-control page are mapped.
- `cp-host-probe.c`: disposable test of the host's PCI enumeration, four
  MSI vectors, signed ROM boot, power cycling and Gen3 state restoration.
  Run with `run=1`. Refuses a bound cpif or endpoint driver.
- `pcie-cp-hw.h`: the shared, tested PHY/controller sequence.
- `cp-probe-gpio.h`: CP GPIO sequence shared by the disposable probes.
- `pcie-cp-api.h`, `exynos-pci-noti.h`: the cpif-facing RC API and Google's
  notification ABI.
- `sources.json`: exact upstream revision and hashes for the hardware
  sequences and vendor interfaces. The sources are GPL kernel code.

Successful disposable probes deliberately return `-EAGAIN` from module
initialization so no probe remains resident. `insmod` consequently prints
“Resource temporarily unavailable.” Read `result=0` and the stage results
in the kernel log to determine success. All returning paths after CP
power-on switch it off again. The host stays loaded until reboot.

### Details the earlier handoff missed

1. The cached `dwc-whi` CAL is newer than the pinned 5.10 source. The modem
   follows the pinned CAL: PCS `0x008/0x808` bit 4 set, bit 5 clear; no newer
   PCS `0x184` rate-switch writes. The 33 common and 88 lane-table writes
   were compared in order with the pinned source. Both lanes are configured.
2. `request_pcie_int()` wraps `int_ap2cp_msg` in `DOORBELL_INT_MASK`.
   Interrupt index zero must be written as **`0x10000`**, not zero.
3. Programming the RC MSI target alone is insufficient. The endpoint MSI
   capability must also point to `0xf6200000`; the ROM uses that mailbox.
4. On the BOOT→BL1 transition, `AP2CP_WAKEUP` must rise before retraining,
   with cpif's 5 ms delay. Omitting it reproduced an LTSSM `0x3` timeout;
   adding it produced Gen3 x2.
5. First L0 may momentarily report Gen1 during speed negotiation. The host
   harness checks the settled link after poweron, not only the initial log.
6. PCI core must leave the ROM target at Gen1. The host config operations
   preserve the selected target even if generic PCI link recovery tries to
   lift that restriction. Class-zero EPs are exposed as class `0xff0000`.

### Host behavior and limits

- The PCI bridge window is `0x40000000–0x405fffff`. Root BAR0/ROM are disabled
  using the vendor DBI2 sequence. BAR0 assignment is checked before success.
- Fixed MSI target `0xf6200000`. Block 0 uses the DWC MSI domain; blocks 1–4
  each clear their pending bit before invoking their registered queue
  callback. ELBI event IRQs feed the vendor link-down/timeout callbacks.
- Configuration and DWC MSI register accesses are guarded while the PHY is
  isolated. IRQs are disabled and synchronized before power-down.
- IO coherency is enabled before DMA. The probes use cacheable mappings of
  the already cacheable reserved DRAM, avoiding mismatched WC aliases.
- ASPM/L1 substates are disabled; its RC API is a documented no-op.
- No automatic probe, OF alias, module installation, or boot-script change.
  The host is intentionally non-unloadable until a complete removal
  lifecycle exists; use an orderly reboot.
- Tested: initial enumeration, base MSI delivery, ROM DMA, BOOT→BL1,
  repeated power cycles and PCI restore. **Not tested:** pktproc queue MSI
  delivery, recovery callbacks, system suspend, full cpif boot or networking.
- A standalone ROM or BL link sometimes acknowledges PME without entering
  L2. The probes log this and finish with PERST, LTSSM disable, PHY
  power-down and CP GPIO power-off. Runtime power management needs more work.

### Build and manual test

From the repository root (build products belong in ignored `out/`):

```sh
SRC="$PWD/devices/pixel7pro/kernel/modem"
OUT="$PWD/devices/pixel7pro/out/modem-build"
make -C devices/pixel7pro/mainline/linux ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- M="$SRC" MO="$OUT" W=1 modules
```

The five modules build without warnings with `W=1` against the running
`7.3.0-rc2-pixel-panel19-g5225b8eec4c9-dirty` kernel. Manual tests use stripped modules under `/root/tools`; the opt-in persistent
service uses a checksum-pinned private bundle. Neither uses general module
autoloading. Before any test, verify
all protected partition flags, arm `setsid -f /root/tools/wdtkick`, and
stream filtered `dmesg -W` to the host. Then use either the direct probe or
load the host followed by its probe. They cannot own the controller together.

```sh
# Direct route (no pixel-pcie-cp loaded):
insmod /root/tools/cp-rom-probe.ko phase=4

# Linux PCI/MSI route, after a clean boot:
insmod /root/tools/pixel-pcie-cp.ko
insmod /root/tools/cp-host-probe.ko run=1
```

Stop `wdtkick` with SIGTERM and allow two seconds before an orderly reboot.
The local test logs are under `out/modem-stage2/` (not tracked).

## Stage 3 prerequisites: GPIO/IRQ and reserved memory

Validated 2026-09-29 with CP firmware and radio off:

- [pixel-gpio](../gpio/README.md) adds the CP pins only with `modem=1`.
  `cp-gpio-probe.ko run=1` resolves and owns all eleven `/cpif` GPIOs, arms
  wake and phone-active interrupts, powers CP on/off, and releases resources.
  Two consecutive final runs each reported `rises=1 falls=1 type_error=0
  result=0`. Successful probes deliberately return `-EAGAIN`.
- CP wake must retain its bootloader pull-up. Applying Android's pull-down
  lost the ROM wake signal; preserving the pull restored real interrupts.
- Both speaker drivers rebound successfully alongside the added pins.
  Phone-active transitions and suspend wake remain untested.
- The [shared-memory port](../cpif/README.md) validated all 10,241 reserved
  DRAM pages and passed repeated mapping/release tests. It uses write-back
  mappings consistent with the linear map and host IOCC. AoC voice SRAM is
  retained as metadata but blocked from this DRAM mapping path.
- `cpif` itself remains unloaded. No full firmware or NV transfer occurred.
  No modules were added to autoload paths or boot scripts. Protected block
  devices stayed read-only; the phone was returned to normal Linux startup.

Local test evidence is under `out/modem-stage3/` (not tracked).

## Next integration gate

The [cpif port](../cpif/README.md) builds against the host's real exports,
preserves PCI BAR resources, and checks GPIO request/direction/IRQ errors.
Finish the remaining failed-probe cleanup audit, provide the missing system
revision handover input, and prepare the tmpfs NV/RFS/boot environment before
loading cpif. AoC voice mapping also needs integration. Keep the radio off;
the physical SIM is needed at the later registration stage. Calls, SMS and
cellular data are not working yet.

## Persistent service and receive resume

The validated `pixel-pcie-cp` build includes a receive-ring check after PCI
state restoration. Packet descriptors survive link power collapse while the
MSI controller is reinitialized; each registered pktproc handler checks its
queue and schedules NAPI if needed. This does not invent or acknowledge an
MSI status bit. The same module was used for the confirmed two-way calls, SMS,
LTE HTTPS tests, and the persistent startup tests. It remains non-unloadable;
recovery uses an orderly full-phone reboot. See the
[persistent service instructions](../../modem/README.md#persistent-startup).
