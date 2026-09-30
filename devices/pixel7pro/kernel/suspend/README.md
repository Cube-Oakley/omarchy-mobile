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
