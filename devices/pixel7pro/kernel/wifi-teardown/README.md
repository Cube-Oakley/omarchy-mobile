# Wi-Fi: the chip is reset before its buffers are freed (2026-10-08)

**What happened.** On October 8 at 18:30:48, 1.4 s after the sleep path
unloaded Wi-Fi (`rmmod brcmfmac` before a deep sleep), a fork of
`nm-dispatcher` found a page-table entry of `0x000003b7_00000012`
(`WARNING: mm/swapfile.c … swap_dup_entry_direct`, then `Bad swap file entry`
without end): no swap exists, and the value reads as a brcmfmac msgbuf
header, type `0x12` (`MSGBUF_TYPE_RX_CMPLT`), interface 0, request ID 951. The
fork retried the entry for three hours at 100 % of one core, holding the
process's mmap lock, so everything that read its `/proc` entries hung:
`pgrep` in the Settings and Clock launchers (the apps closed again), the
shell watchdog, the keyboard. A 20 s `ip route` stall at 18:32 then ended
`data_network.py`, and the modem manager stopped the cellular session (fixed
in `modem/data_network.py` the same day). Logs: pstore and
`/var/log/pixel-kernel-warnings-20261008-freeze.log` on the phone.

**The cause.** `brcmf_pcie_remove()` masked the chip's interrupts, freed the
posted RX buffers (`brcmf_detach`) and the rings (`release_ringbuffers`), and
only then reset the chip (`brcmf_pcie_reset_device`). The firmware ran on
until that reset, still associated, storing received frames in the freed
buffers and completions in the freed rings. `brcmf_pcie_reset()` (the
firmware-hang recovery) had the same order. Measured with a test build that
read the D2H rings' write indices at three points of an unload under a 2 ms
ping flood from the laptop: 67, 59 and 60 completions after the interrupts
were off, and 6, 6 and 5 of them after the RX buffers had been freed. The sleep
path has unloaded Wi-Fi like this before every deep sleep since the
persistent session began, so other memory may have been hit before.

**The fix.** `0001-brcmfmac-pcie-reset-the-chip-before-freeing-its-buffers.patch`
moves `brcmf_pcie_ds_disable()` + `brcmf_pcie_reset_device()` ahead of every
free in both paths: the same watchdog reset the driver already did, earlier.
Clearing bus mastering first (`pci_clear_master`) was tried and rejected: the
load after such an unload trapped in the firmware ("RO overwritten",
"dongle is not responding"), even after the PCIe link and WL_REG_ON were
cycled, until Wi-Fi and Bluetooth were both unloaded and loaded again.

**Validation.** Reset-first test build: 6 unload/load cycles under the
flood, each associated again in 6–7 s. The final module (no test code) in
the modem bundle: 3 `pixel-wifi-restart` cycles under the flood (associated
in 7 s each), and Wi-Fi from the bundle after two reboots. No firmware traps
or `Bad swap` entries.

**Installed** on October 8: `/var/lib/omarchy-mobile/modem/brcmfmac.ko`
`4138efa2…` (was `0116c460…`, kept as
`/root/backups/modem-brcmfmac.ko.before-teardown-reset`; manifest backed up
beside it), manifest updated and `manager.py verify` OK. Built from
`mainline/linux` with the image build's per-directory module steps
(`make … M=drivers/net/wireless/broadcom/brcm80211 modules` with the rfkill,
cfg80211 and `out/scanout-stats-v44/modules/extra.symvers` symbols) and
`strip --strip-debug`; before this change the tree reproduced the bundle's
module byte for byte.
