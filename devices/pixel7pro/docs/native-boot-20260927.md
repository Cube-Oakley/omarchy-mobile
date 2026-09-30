# Native boot from boot_a — September 27, 2026

The phone now starts Linux from `boot_a` by a normal power-on or reboot, with
no `fastboot boot`. On the first normal boots UFS was too slow for the desktop
to start (PWM gear 1, 0.57 MB/s) and USB never came up, so the phone had to be
RAM-booted from fastboot every time. Three fixes close that gap:

1. **UFS HS gear 4** (image I): the out-of-tree `pixel-ufs` driver applies
   Google's GS201 PHY calibration and switches the link to HS-G4 rate B on two
   lanes: 1.2 to 1.8 GB/s reads. Details, sources and verification are in
   [the storage README](../kernel/storage/README.md#hs-gear-4-september-27-2026).
2. **USB PHY bring-up** (images J and K): the kernel powers up the USB 2 PHY
   itself when the bootloader left it isolated.
3. **Startup tolerance** (image L plus the boot script): CPU scaling accepts the
   normal-boot clock state, and a refused CPU-scaling request no longer stops
   the desktop from starting.

Kernel checkpoint: [`kernel/native-boot-v20`](../kernel/native-boot-v20/README.md).

## USB on a normal boot

ABL's fastboot mode leaves the USB PHY running, so a RAM boot inherits a
working link and `pixel_usb.c` only had to register DWC3 on top of it. A normal
boot leaves the PHY isolated and unconfigured. DWC3 then registers and the
gadget binds, but the host never sees a device.

`scripts/pixel-usb-diag.py` (installed as `/usr/local/sbin/pixel-usb-diag`) is a
read-only `/dev/mem` snapshot of the HSI0 power domain, the PMU USB isolation
controls, the USB clock registers, the USB PHY controller block at 0x11200000
and a few DWC3 registers. The boot script saves it together with `dmesg` for
every boot in `/var/log/pixel-boot-diag/<boot_id>.{usb,dmesg}`. Comparing a
failing normal boot with a RAM boot:

| Register | Normal boot (no USB) | RAM boot from fastboot |
|---|---|---|
| PMU 0x18063eb0 (USB PHY isolation) | `00000002` (bit 0 clear: isolated) | `00000003` |
| PMU 0x18063eb4 (DP PHY isolation) | `00000000` | `00000001` |
| USBCON +0x04 LINK_CTRL | `00043e00` | `00043ff0` |
| USBCON +0x54 HSP | `15000300` | `15003200` |
| USBCON +0x58 HSP_TUNE | `31233333` | `736f3537` |
| USBCON +0x5c HSP_TEST | `03000000` (SIDDQ set) | `c6080000` |

The power domain and the USB clocks are the same in both. Only the PMU
isolation and the PHY controller differ.

`pixel_usb_phy_init()` now runs before DWC3 registers. If PMU 0x18063eb0 bit 0 is
already set it logs `PHY already running (fastboot handoff)` and changes nothing.
Otherwise it follows the vendor `phy-exynos-usbdrd.c` / `phy-exynos-usb3p1.c`
USB 2 path (`phy_version` 0x301):

- **Isolation release:** the PMU is secure, so it writes 0x18063eb0 and 0x18063eb4
  bit 0 through the secure-register SMC `0x82000504(pa, 1, value)`. Reads are
  open.
- **Link and clock setup:**
  - pulse the link reset;
  - set the Q-channel controls (force active after 500 µs);
  - hold both PHY resets;
  - clear the UTMI suspend and pulldown forces;
  - clear COMMONONN;
  - bypass the bus filters;
  - force VBUS valid and B-session valid;
  - write the stock HS tune `0x736f3537` and reference-clock select 0.
- **PHY start:** clear SIDDQ, wait 15 µs, release the PHY resets, and wait
  75–90 µs.
- **SS side:** set the U2/U3 over-current select and the PIPE3 forcing the vendor
  driver uses without the SS combo PHY, and put the combo PMA in low power.

Image J also set `LINK_PCLK_SEL`, which selects the PIPE3 clock. Without the SS
combo PHY that clock is dead, and DWC3 failed with `failed to enable ep0out`.
Image K leaves it clear, and a normal reboot of K brought USB back 15 seconds
after the reboot. Kernel log from the normal boot:

```
Pixel USB: PHY isolated (normal boot); bringing it up
Pixel USB: PHY up: LINK_CTRL=00053ff0 CLKRST=00001004 UTMI=00000030 HSP=15003200 TUNE=736f3537 TEST=02080000
Pixel USB: registering DWC3 with inherited PHY, USB2 peripheral
```

This is USB 2 peripheral only; SuperSpeed and host mode are not implemented.

## CPU scaling on a normal boot

`pixel_acpm` starts cpufreq only from the rates it has verified. The
fastboot handoff leaves the three clusters at 1197, 1197 and 1106 MHz, but a
normal boot leaves the little cluster at 1598 MHz. The guard returned `ERANGE`,
and because the boot script runs with `set -e`, startup stopped before the GPU
and the desktop. Running the remaining steps by hand on that boot started
Hyprland and quickshell normally.

- **Image L:** the guard also accepts 1598 MHz on the little cluster. The cpufreq
  core moves a rate that is not in the table to a table rate when the policy
  starts. The thermal checks (seven live zones, 5–60 °C) are unchanged.
- **Boot script:** `scripts/pixel-persistent-session.sh` now treats a refused
  cpufreq request as non-fatal. The CPUs keep the bootloader's rates, the
  schedutil loop is skipped, and the GPU and desktop still start.

## Images

All built with `scripts/build-pixel-shell.py --persistent-root --seconds 0
--initcall-debug` into `out/checkpoints/20260927-ufs-hs/image-*`:

| Image | Change | SHA256 |
|---|---|---|
| I | UFS HS-G4 driver in the root; USB diagnostics | `9aae9e6059217462021b4895ed2d49f918ba0fccbf5682fb26daa5b71f6687d4` |
| J | USB PHY init, with `LINK_PCLK_SEL` (fails: ep0out) | `c609644679a4bb25a8872672415b89044134c8d9f3626b288e295e88a7f36c8d` |
| K | `LINK_PCLK_SEL` left clear: USB works on normal boot | `3e164c8d11db99f16c825a4d60c87b1113ac7e8bddb167657dfc5d4e1a35a7f8` |
| L | ACPM accepts the normal-boot little-cluster rate | `79b60372e65bc6069dd02a77d3175f227b1d0e0ed01ff40514911b61116baaef` |
| M | UFS device reset (`dev_reset=1`): no first-probe failure | `b737d85be8ae2ea23e7b00aeccd4bb5fae875e8680c236365dff5f71ffc2b529` |
| N | S2MPG12 RTC module: real time at boot | `684f3704d8f7526f421d1ae2089f8a0bcdb85b3c9196fc27fd1671a8dfe2dbe1` |

M and N use the same v20 kernel patch as L; only the out-of-tree modules and
the boot scripts changed. N is in `boot_a`, and its readback matches.

`install-pixel-boot.py` writes `boot_a` only while the same kernel build is
running from a RAM boot, and then reads the partition back to check it.

## Verified: image L from boot_a

L passed a RAM boot (CPU scaling on, desktop up) and was then installed over K.
The `boot_a` readback matches L's SHA256. An orderly normal reboot (`kill -TERM 1`),
with no fastboot involved:

- the USB gadget re-enumerated 16 s after the reboot request and SSH answered at 18 s;
- the kernel took the normal-boot USB path (`PHY isolated (normal boot); bringing it up`);
- UFS reached `HS_GEAR4`, `HS_RATE_B`, 2 lanes;
- cpufreq logged `CPU0: Running at unlisted initial frequency: 1598000 kHz, changing to: 1197000 kHz`,
  and all three policies run schedutil;
- `persistent desktop startup complete` in under a minute, with Hyprland and the
  shared shell on screen (screenshot checked).

On the host, the Pixel's NetworkManager profile has autoconnect off, and a
generic shared profile claims the same interface name first. After each
re-enumeration, run `nmcli con up <pixel profile>` to restore 10.77.7.2/30.

## Reboot to fastboot without buttons

`scripts/pixel-reboot.py fastboot` moves the phone from Linux to fastboot, and
`scripts/pixel-reboot.py linux` returns it from fastboot or restarts Linux.
Each waits for the target; the Linux direction also waits for the desktop.
`pixel-reboot.py wait` restarts nothing: it waits for Linux after a RAM boot
and restores the host network. The
trick is a warm PSCI reset, which keeps the bootloader mode in the PMU; see the
[reboot module README](../kernel/reboot/README.md). A new boot image can now be
RAM-tested and installed with no button presses:

1. `pixel-reboot.py fastboot`
2. `boot-pixel-shell.py --image <new> --sha256 <hash>`
3. `pixel-reboot.py wait` (restores the host network), then `install-pixel-boot.py`
4. `pixel-reboot.py linux`

While probing PMU registers around SYSIP_DAT0 from `/dev/mem`, reading
0x18060800–0x1806081c (other than 0x810) hung the SoC until the watchdog
reset it; it rebooted into Linux on its own. Only 0x810 is known safe to read there.

## UFS device reset and RTC (images M and N)

- **UFS first probe:** it used to fail on every boot until one full retry.
  M resets the UFS device before the host enable, as the vendor driver does, so
  the first probe succeeds. See the
  [storage README](../kernel/storage/README.md#device-reset-september-27-2026).
- **RTC:** N reads the battery-backed S2MPG12 RTC through ACPM; see the
  [RTC module](../kernel/rtc/README.md). The system clock is real time about
  2 s into boot, replacing the clock floor.
- **USB flap:** on one normal boot out of about ten, the host saw the USB gadget
  disconnect and re-enumerate once, about a second after it first appeared.
  The phone logged `dwc3: remote wakeup not configured` at that moment. The
  host helper now looks the interface up again and retries the network step.

## Remaining

- RTC writes: the [RTC module](../kernel/rtc/README.md) is read-only, so Linux
  cannot correct its drift (about 30 s ahead on September 28). Setting it needs
  the vendor WUDR write sequence plus a network time source.
- USB SuperSpeed, host mode and Type-C role handling.
