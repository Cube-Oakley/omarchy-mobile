# v35: Wi-Fi runtime suspend (2026-10-02)

v34 plus runtime suspend for the BCM4389, as stock's bcmdhd does it: after
500 ms with nothing to do, the chip is told D3 and the host takes its PCIe
link down; the chip wakes the host on WL_HOST_WAKE.

**Why.** Associated and idle, the chip held all its save/restore power domains
on in D0 whatever the firmware settings: about 97 mW on VSYS_WLAN_BT, and still
about 90 mW with the interface down. Stock never leaves it idle in D0 with the
link up. See "Wi-Fi idle" in [the deep-sleep plan](../../docs/deep-sleep-plan-20260930.md).

**Changes.**
- **brcmfmac** (`pcie.c`, in this patch). `rt_idle_ms`, default 500, 0 to turn
  it off:
  - **Suspend:** once the host has been idle that long and the chip sleeps
    (in-band deep sleep), brcmf_pcie_pm_enter_D3()'s handshake runs without
    taking the bus down (D3_INFORM, then D3_ACK). Then the host's link hooks.
  - **Resume:** host work resumes the chip in brcmf_pcie_host_active() before
    it touches the chip, as does WL_HOST_WAKE. Then the hot path of
    brcmf_pcie_pm_leave_D3(), with the BAR1 window reprogrammed (PERST# resets
    it).
  - **Guard:** while the link is off, every BAR register and TCM accessor
    refuses access, with one warning, instead of hanging the SoC.
  - **Elsewhere:** reset, system suspend and removal resume the chip first.
  - **Counters:** the `runtime_pm` attribute.
- **pixel-pcie** ([kernel/pcie](../pcie/README.md), out of tree, in the
  image's initramfs): the link half. `pixel_pcie_wifi_suspend()` and
  `pixel_pcie_wifi_resume()` follow the tested s2idle `system_link_off` path,
  with the config of both ends saved and restored. `pixel_pcie_wifi_set_wake()`
  owns gpa6-1's EINT on its own GIC line. Config accesses fail cleanly while
  the link is off.

brcmfmac finds the hooks by name. Without them, on an older image, it runs as
before.

**Measured** (the live modules, idle on 5 GHz, VSYS_WLAN_BT over 60 s, two
rounds):

| `rt_idle_ms` | mW |
| --- | --- |
| 0 | 96.7, 96.2 |
| 500 | 19.8, 18.9 |
| 2000 | 41.7, 40.2 |

About 77 mW, roughly 20 mA from the battery, whenever Wi-Fi is up and idle.

**Checked before install:**
- 91 cycles without a failure;
- outgoing and incoming pings, the first incoming one in about 150 ms;
- unloading while suspended;
- 11.1 MB/s down and 9.8 MB/s up, unchanged.

**Validated by RAM boot** (`power-image-13`, build ID `a7753fe0…`): Wi-Fi up
at boot from the modem bundle's brcmfmac (updated the same day, manifest
re-signed, `manager.py verify` OK), runtime suspend running with no failures,
the modem data link up, `idle_refresh` 10 and `mif_floor_mhz` 1352.

**Installed in boot_a on October 2** (pre-approved). Readback matched
`e6ad7373…`; the previous image was v34, `b4341700…`. After the reboot the
same checks passed, and the Wi-Fi power hook set `pm2_sleep_ret` 200 again.

Built with `scripts/build-pixel-shell.py --persistent-root --seconds 0 --cma
256M`. `kernel.config` matches v34's except for the initramfs path.
`wifi-v35.patch` is complete against `kernel-base.txt` (58 files). It also
carries two changes that were already in v34's tree and image but missing from
its patch: `drivers/bluetooth/hci_bcm.c` (the BCM4389's host-powered match
data, September 28) and `include/linux/firmware/samsung/exynos-acpm-protocol.h`
(September 30).
