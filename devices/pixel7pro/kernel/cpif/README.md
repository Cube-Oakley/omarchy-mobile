# Pixel 7 Pro cpif port

**ONLINE, AT, physical-SIM registration and cellular IPv4 traffic verified.**
The [RAM-only runtime](../../modem/README.md) uses stock cbd/rfsd with verified
private NV copies. Firmware version and radio-off AT match expectations.
With an approved Tello SIM, rmnet0 passes ping, carrier DNS and verified HTTPS.
SMS and voice remain untested.

The Samsung S5300 uses Google's CPIF-20220408R1 driver for `/dev/umts_*`
(boot, SIT RIL, RFS, AT/DUN) and `rmnet*` data interfaces. This directory
reproduces that GPL driver from 59 pinned source/header files plus patches.
The inputs come from LineageOS's Google gs201 kernel at
`40ff93424549ffebfbba32e9435dfc58c40decb2`. No firmware or proprietary RIL,
cbd or rfsd code is included.

## Current validation

- All three modules (`shm_ipc`, `cpif`, `cpif_page`) compile and modpost
  resolves their imports against vmlinux and the [host](../modem/README.md).
- `cpif.ko` imports the real `exynos_pcie_*` and separated-MSI exports;
  the original root-complex stubs have been removed.
- A fresh network assembly, with all upstream hashes verified, produced
  64 files identical to the source tree used for the successful build.
- Cached inputs are hash-checked too, including offline builds.
- Existing upstream warnings remain (mostly old-style inline declarations).
- `shm_ipc` was manually loaded and unloaded twice. It validated 2,048 IPC,
  8,192 packet-buffer and one MSI page as reserved. The mapping probe passed
  twice, including cached release, repeated vmap/release and rejection of
  ordinary RAM, AoC SRAM and invalid indices. It never reads memory contents.
- `cpif` loaded normally and completed full boot. Failure injection after all
  I/O devices were created returned `-EIO`, leaving no modem nodes, rmnet
  interfaces, binding or link diagnostic groups, with no kernel fault warnings.
- Four DL and two UL queues initialize; RFS opens before boot and confirms
  ONLINE. Native AT returns the expected baseband and radio state 0.
- Physical-SIM data activation, rmnet0 TX/RX, carrier DNS and TLS-verified HTTPS
  passed with no network errors or drops. Test addresses/routes were removed.
- `cpif_page` remains unloaded. No module was installed into an autoload path.

## Patches

| Patch | Change |
| --- | --- |
| 0001 | Stand-in headers for unavailable vendor infrastructure |
| 0002 | Linux 7.3 API conversions and Kbuild |
| 0003 | Return errors instead of panicking; partial probe cleanup |
| 0004 | Bind only `144d:a5a5`, register PCI driver once, reject unused SPI boot, prevent unsafe unload |
| 0005 | Replace RC stubs with real host declarations; preserve Linux's BAR0 assignment and validate the fixed doorbell window |
| 0006 | Validate DT indices/ranges and reserved pages; use WB mappings with IOCC; fix cached release; block SRAM and freeing reserved DMA windows |
| 0007 | Check GPIO request/direction/IRQ errors, preserve deferred-probe errors, and release GPIO requests on failed probe |
| 0008 | Pre-boot resource cleanup, defer link diagnostics, gate early opens, check packet geometry, inject probe failures, and permit CP pin defaults without the missing GS201 pinctrl driver |
| 0009 | Propagate the modem's suspend refusal instead of entering sleep while CP2AP_WAKEUP is asserted |

The original conversions include GPIO lookup by bank fwnode, allocated dummy
NAPI devices, current timer/class/netif/string APIs, and the vendor's older
per-CPU workqueue semantics. The `/dev/umts_*` ioctl structures retain their
ABI. Kernel-panic and unsupported SPI-boot ioctls fail instead of acting.

Configuration matches the S5100/PCIe selections in Google's
`cloudripper_gki.fragment`: CPIF, PCIe, shared-memory, pktproc DL/UL, IOCC,
channel extension, QoS and the voice-call suspend notifier are enabled.
Vendor PCIe SysMMU/zero-copy, DIT, TP monitor, thermal, SPI boot, BTL, direct
DM and on-SoC-modem paths are disabled. This retains reserved packet buffers
and the copying receive path. `cpif_page` is built but not imported by this
configuration.

The remaining headers provide logbuffer registration failure (cpif logs to
dmesg), unused ECT frequency lookup, and a failing SMC helper for the
unselected on-SoC shared-memory link. PCIe uses the host's actual SMC path.

## Reproduce and build

From the repository root, first build `kernel/modem` as its README describes:

```sh
python3 devices/pixel7pro/kernel/cpif/assemble.py \
  "$PWD/devices/pixel7pro/out/cpif"
# --offline uses the SHA-verified cache; --cache DIR chooses another cache.

K="$PWD/devices/pixel7pro/mainline/linux"
# This local kernel's Module.symvers omits some built-in exports.
python3 - "$K" > devices/pixel7pro/out/cpif-extra.symvers <<'PY'
import pathlib, sys
k = pathlib.Path(sys.argv[1])
old = {line.split()[1] for line in (k / 'Module.symvers').read_text().splitlines()}
print(''.join(line for line in (k / 'vmlinux.symvers').read_text().splitlines(True)
              if line.split()[1] not in old), end='')
PY
make -C "$K" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  M="$PWD/devices/pixel7pro/out/cpif/cpif" \
  KBUILD_EXTRA_SYMBOLS="$PWD/devices/pixel7pro/out/modem-build/Module.symvers $PWD/devices/pixel7pro/out/cpif-extra.symvers" \
  modules
```

The output directory must not already exist when assembling. Keep modules
in ignored `out/` on the host and `/root/tools` on the phone. Never install
under `/lib/modules`, run depmod over them, or add them to udev/boot startup.
Their OF aliases match the live `/cpif` and `/cp_shmem` nodes.

## Shared-memory mapping test

`probes/cp-shmem-probe.c` is a disposable mapping test that does not read or
write memory contents.
Build with `M=<repo>/kernel/cpif/probes`, an ignored `MO=<output>`, and
`KBUILD_EXTRA_SYMBOLS=<assembled>/cpif/Module.symvers`. Copy the stripped
probe and `shm_ipc.ko` only to `/root/tools`. With CP and cpif off, manually
load `shm_ipc`, check its platform binding, then load the probe with `run=1`.
It logs `result=0` and returns `-EAGAIN` on success. Unload `shm_ipc` afterward.

The port validates the four exact stock reserved regions by `rmem_index`,
independent of phandle order, then checks DT subregion bounds and all DRAM
pages before exposing mappings. Normal DRAM has a WB linear alias and uses
WB vmap aliases plus host IOCC. Cached release clears the stored pointer
without vunmapping the linear map. Vendor on-SoC dynamic memory maps and
reserved-page freeing are disabled for this PCIe configuration.

`VSS_AOC` at `0x197fd000` is SRAM. Its physical metadata remains available,
but CPU mapping is refused until its memory attributes and ownership are
integrated with AoC voice support. It must never go through `phys_to_page`.

## Manual loading and remaining limits

1. Verify original backup hashes and block-layer read-only flags for all nine
   protected partitions. Prepare the RAM-only runtime before full firmware boot.
2. Arm the watchdog, stream filtered logs and make `rmnet*` unmanaged. Load the
   tested `pixel-gpio modem=1` (unbind/rebind both amps for replacement), manual
   PCIe host, then shm_ipc. Confirm all reserved pages and subregions validate.
3. Load cpif only from `/root/tools`. Its init applies a live DT changeset marking
   CP pin states `pinctrl-use-default`; otherwise driver core defers indefinitely
   before calling probe. The explicit GPIO driver owns the CP pin subset and
   preserves the required wake pull-up. Reboot restores the original DT.
4. Optional `pixel_probe_fail=1|2|3` fails after link creation, the first I/O
   device, or all I/O devices. Value 3 was tested on hardware. The module has no
   exit, so reboot between attempts. These helpers cover pre-boot probe cleanup,
   not teardown after firmware or interrupts have started.
5. Once cpif binds, expose only the boot/RFS channels to the prepared runtime.
   Run RFS first, then CBD. Require the full initialization handshake and ONLINE;
   a successful insmod or ROM test alone is insufficient.
6. Keep radio off until explicitly approved. Power CP off before stopping RFS.
   Stop the watchdog before reboot. Do not add modem loading to boot startup yet.

The shared-memory aliases use WB plus host IOCC. AoC voice SRAM is metadata
only; CPU mapping remains disabled. System suspend and crash recovery remain
untested. AT and cellular traffic exercised idle PCIe wake/retrain. ST54J/eSIM
and approved-number restrictions remain.
