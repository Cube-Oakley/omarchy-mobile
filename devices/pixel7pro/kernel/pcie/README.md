# PCIe channel 1 (Wi-Fi link)

`pixel-pcie.c` brings up GS201 PCIe channel 1, the Gen2 x1 link to the BCM4389
Wi-Fi/Bluetooth chip, as a mainline DesignWare host. It binds the stock DT
node `pcie@14520000` and follows Google's GS201 sequence: `pcie-exynos-rc.c`,
plus the PHY settings from `pcie-exynos-gs201-rc-cal.c`. Channel 0, the modem
link, is refused before any register access.

The boot script loads it after the desktop starts, then the Wi-Fi stack; see
[Wi-Fi](#wi-fi) below. On success the log shows
`link up on attempt 1: Gen2 x1, LTSSM 0x11`, with the chip at `01:00.0`
(14e4:4441).

## Order matters

The bootloader leaves the PHY isolated in the PMU (0x18063EC4 bit 0). While it
is isolated, even a read of the controller's ELBI registers hangs the SoC. The
driver therefore releases isolation first, through the secure register SMC,
although Google's probe touches ELBI before that. After that it runs the
vendor steps in order:
1. PHY power-down.
2. WL_REG_ON high for 200 ms.
3. PHY configuration.
4. Soft resets.
5. PERST# release.
6. Controller setup and cache coherency (AxCACHE and HSI2 sharability).
7. LTSSM enable.

The DesignWare core then does root-port setup, the iATU and enumeration.

The stock node's pinctrl has no mainline driver, so a device bound to it
waits forever for pins. The module creates its own platform device that
reuses the node (`of_node_reused` skips pin binding) and sets the pins
itself.

## Differences from Google's driver

- **Interrupts:** INTx only; the stock DT has `use-msi = "false"` for this
  channel.
- **Link power management:** L1 substates follow the Wi-Fi network device
  instead of calls from the Wi-Fi driver. They come on when brcmfmac
  registers it, once the firmware runs, and go off when it unregisters,
  before any chip reset (a driver reload or crash recovery). The enable and
  disable orders and values are the vendor's for `EP_BCM_WIFI`. The module
  first checks that both ends keep their capabilities where the vendor
  driver expects them. The root port still advertises no ASPM, so the PCI
  core leaves the link alone.
- **Link turn-off:** before PERST#, the link leaves L1 under software control
  and PME_Turn_Off takes it to L2, as the vendor's power-off does. A link torn
  down while in L1.2 left the next bring-up hanging in link training.
- **Memory access:** no SysMMU. It stays in bypass, so DMA addresses are
  physical, and the DT's `dma-coherent` matches the coherency setup.
- **Left out:** link history and UDBG trace.

Parameters:
- `attempts` (3): link training attempts, each with a full PHY configuration.
- `ia` (on): program the IA sequencer as the vendor does.
- `trace`: log and pause at every stage.
- `l1ss` (off; the boot script sets it): L1 substates while the Wi-Fi
  network device exists. Writable at runtime.
- `link_state` (read-only): the LTSSM state, the PHY power state (0 L0,
  2 L1, 5 L1.1, 6 L1.2) and whether L1 substates are on.

Measured on image A at home, screen off, connected and idle: 1.74–1.77 W at
the USB input, against 2.10 W with the link in L0. With the Wi-Fi chip off it
is 1.65 W. An idle link samples in L1.1 or L1.2, and LAN throughput is
unchanged (11.7 MB/s out, 11.1 MB/s in; the home network's limit).

## Wi-Fi

The image carries everything else Wi-Fi needs:
- **Driver stack:** rfkill, cfg80211, brcmutil, brcmfmac and brcmfmac-wcc,
  built from the kernel tree with the [v24](../wifi-v24/README.md) brcmfmac
  changes.
- **Firmware:** the stock `fw_bcmdhd.bin`, `bcmdhd_clm.blob` and `bcmdhd.cal`
  from `vendor_a`, under brcmfmac's `brcmfmac4389c1-pcie.*` names, plus the
  signed regulatory database. The builder takes them from the ignored
  `out/stock-vendor/wifi` and checks their SHA256.

The boot script:
1. Points `kernel.modprobe` at an initramfs helper, because brcmfmac loads its
   vendor module with `request_module()`.
2. Loads the modules.
3. Starts the system bus and NetworkManager.

NetworkManager leaves the USB link alone and writes `/etc/resolv.conf`'s
target ([`10-pixel.conf`](../../adapter/network/NetworkManager-pixel.conf)).

There is no watchdog during boot. Before the link comes up, the script writes
and syncs `/var/lib/omarchy-mobile/pixel-wifi-starting`, and removes it
afterwards. If a boot hangs while bringing up the link, the next boot finds
the marker and skips Wi-Fi. Delete the file to try again.

## Opt-in system sleep link shutdown

`system_link_off=1` is an experimental s2idle-only path, disabled by default.
After the PCI children suspend, it requires the known BCM4389 endpoint with
wake disabled, sends PME_Turn_Off and requires L2, then resets the host link
and powers down its PHY. WL_REG_ON remains high to retain endpoint firmware.
The shared IRQ is masked and synchronized before PHY isolation is asserted.
Resume releases isolation before any ELBI access, configures the PHY and
root port, rebuilds address windows, and retrains before PCI config restore.
The early-resume phase restores the vendor L1 substate policy. Sysfs link
queries avoid isolated registers if recovery fails.

An enabled endpoint wake request causes a clean refusal; this path does not
support wake-on-Wi-Fi. Short diagnostic trials temporarily disabled the
endpoint wake setting and restored it afterwards. Cellular and RTC wake
remain independent. `sleep_entries`, `sleep_resumes` and `sleep_errors`
record the host callbacks.

On-device validation: the wake-policy guard first refused the request with
no powerdown. With wake disabled, an 8.604-second RTC sleep recorded one
entry/resume and zero errors, with Wi-Fi HTTPS and modem verification passing.
A combined UFS Hibern8 and guarded SICD-hint trial slept 8.888 seconds and
also recovered Wi-Fi and the modem. This is not enabled by the boot helper.

Host removal now also completes PHY isolation, but only after
`dw_pcie_host_deinit()` has removed the PCI children and `devm_free_irq()` has
removed the host handler. Before that point ELBI must remain accessible.
Unload/reload with this ordering and subsequent Wi-Fi HTTPS passed.
