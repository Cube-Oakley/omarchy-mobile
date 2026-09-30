# V25: sleep, Wi-Fi power and brightness

`sleep-v25.patch` is a complete patch against the pinned base in
`kernel-base.txt`, with every added file. Do not stack it on v24. It
reverse-applies cleanly to the working source: v24's 50 files, plus
brcmfmac's `bus.h` and `msgbuf.h` and Bluetooth's `btbcm.c`.

`kernel.config` is image E's configuration, with the workspace path replaced
by `@PIXEL_ROOT@`. It differs from v24 in four ways:
- `CONFIG_PM_DEBUG` and `CONFIG_PM_SLEEP_DEBUG`, for staged suspend tests
  (`/sys/power/pm_test`);
- `CONFIG_BACKLIGHT_CLASS_DEVICE=y`, selected by the scanout driver;
- `mem_sleep_default=s2idle` on the command line;
- `pixel_scanout.panel_sleep=1` on the command line.

Screen off, connected to Wi-Fi and idle, image E draws 1.47 W at the USB input.
V24 drew 2.11 W. Modules outside the kernel image carry the rest of the work:
- L1 substates in [pcie](../pcie/README.md);
- wake-up keys in [keys](../keys/README.md);
- the flashlight in [torch](../torch/README.md);
- vibration in [haptics](../haptics/README.md), on the buses in
  [i2c](../i2c/README.md).

## Wi-Fi deep sleep (brcmfmac)

The BCM4389's firmware sets `PCIE_SHARED_INBAND_DS`. The driver now advertises
`HOSTCAP_DS_INBAND_DW` and implements bcmdhd's in-band device-wake state
machine:
- **Sleep:** when the device asks to sleep, the host acknowledges once it has
  no work queued.
- **Wake:** before new work, the host asserts device wake and waits for the
  exit note (up to 1 s). It then deasserts, so the device may ask again.
- **Transport:** all of this is mailbox data, which firmware from shared
  version 6 (without `USE_MAILBOX`) sends as control-ring messages 0x23 and
  0x24. brcmfmac used to drop these. Trap notices now arrive too.

Other changes:
- **Host-active brackets.** Each msgbuf submission point runs between two
  bus ops, `host_active` and `d2h_mb_data`, and completions are processed
  control ring first, as bcmdhd does.
- **Power domains.** Once the firmware runs, the driver drops its request for
  the chip's power domains, so they can power down. It requests the ARM
  domain around TCM mailbox reads, and all domains again before a reset.
- **Suspend.** Sleep requests are dropped from the D3 inform until the D0
  acknowledgement, as bcmdhd's `skip_ds_ack`. One left pending across D3 was
  acknowledged after resume; the device never answered device wake again, and
  Wi-Fi died.
- **D0 inform.** From shared version 6 the driver sends none; the hostready
  doorbell resumes the device.
- **Attach race.** The firmware sends mailbox data as soon as the host is
  ready, before attach finishes. The ISR and the mailbox sender now wait for
  a `ready` flag. An early interrupt used to dereference a NULL protocol
  pointer, and a failed attach left the pointer dangling.
- **Parameters:** `deep_sleep` (on) and `mb_trace` (logs mailbox data).
- **Status file:** `/sys/bus/pci/devices/0000:01:00.0/deep_sleep` shows the
  state and counters.

Measured screen-off at the USB input:

| Configuration | Draw |
| --- | --- |
| Deep sleep only (link in L0) | 2.10 W |
| Deep sleep and L1.2 | 1.74 W |
| Wi-Fi chip off | 1.65 W |

Throughput is unchanged.

## Display

- **Backlight.** The panel's DBV (DCS 0x51, big-endian, the vendor's normal
  range 4 to 2047) is a backlight class device, `pixel-panel`.
  `actual_brightness` reads the panel back through DCS 0x52. A level set while
  the screen is off applies at the next unblank. A mutex serialises these
  commands with transfers and mode changes, which all need DECON idle.
- **Panel sleep.** With `panel_sleep`, the panel sleeps in while the screen is
  off. That is 0.32 W less than display-off alone. On wake the driver resends
  DSC on and the WQHD PPS, then sleep-out and tearing-effect on, as
  `s6e3hc4_enable()` does. After this wake the panel reports power mode 0x9c;
  after display-on alone it reports 0x9f. The image is correct either way.

## Bluetooth

`btbcm` sets the BCM4387's quirks (broken MWS transport config, broken LE
Coded PHY, extended-advertising PHY fix-up) for chip id 170, the BCM4389. Its
ROM firmware rejects Get MWS Transport Config, which failed initialisation.
Bluetooth itself is not working yet; see [bluetooth](../bluetooth/README.md).

Image E SHA256: `b4bb1e43e3fb312e569515d0c5b0cd508b721ef7fe35242a5650e369d730af13`

Image F uses the same kernel. It adds the MCT tick broadcast and cluster
power-down ([cpupm](../cpupm/README.md)). SHA256:
`376b4d9271465f365966763a41b242ced47bc4074b4b83f89c90b3860807408c`
