# V24: Wi-Fi

`wifi-v24.patch` is a complete patch against the pinned base in
`kernel-base.txt`, with every added file; do not stack it on v23. It
reverse-applies cleanly to the working source: v23's 40 files plus 10 brcmfmac
files. `kernel.config` is image Z's configuration, with the workspace path
replaced by `@PIXEL_ROOT@`. It differs from v23 only in
`CONFIG_BRCMFMAC_PCIE=y`, which selects `CONFIG_BRCMFMAC_PROTO_MSGBUF`.
Both are module options, so the kernel image's code is v23's.

The Wi-Fi chip is a BCM4389 (rev 3, "C1") on PCIe channel 1, brought up by the
[PCIe module](../pcie/README.md). It runs Google's stock firmware (20.101.76.2,
built for their bcmdhd driver) under mainline brcmfmac.

Measured on image Z at home, on 5 GHz with a 720–816 Mbit/s link rate:
- 11.5 MB/s down and 11 MB/s up (about 90 Mbit/s), stable across 60 s of
  continuous downloading.
- That is the home network's limit, not the phone's. A transfer across the
  LAN from a gigabit PC gets the same, and so does another phone on the same
  network.

## brcmfmac changes

From the Pixel 6 (GS101) work in
[m8l8th814n-eng/linux-mainline](https://github.com/m8l8th814n-eng/linux-mainline)
(commits `af7560b2bd29` and `9edb56075f6d`):
- The 4389's chip ID and RAM base, and SYSMEM sizing.
- Boot with an empty OTP.
- A sliding BAR1 window for dongle RAM beyond the 4 MB aperture.
- Function-level reset, as the vendor dongle reset does.
- A 36-bit DMA mask.
- The host capabilities word.
- RX buffer posting deferred until after the init commands.
- The vendor's reset and ring-init order.

Added here, each needed by this firmware; most follow Google's bcmdhd:
- **Firmware name.** 4389C1 files for chip rev 3 and later; 4389B1 is revs 0 to 2.
- **Host capabilities.** Deep sleep is off (`DS_NO_OOB_DW`): there is no device
  wake.
- **RX buffers.** At most 512 posted. The firmware asks for 1535, more than the
  1024 RX packet IDs, which are shared with the control buffers.
- **H2D sequence numbers.** Each control and RX-post item carries a sequence
  number (from 254, mod 253) in its epoch byte. Without it the firmware waits
  on the first item and traps ("h2d livelock").
- **Ring indices in host memory**, as bcmdhd does. The Pixel 6 patch forced
  TCM indices to avoid the livelock that the missing sequence numbers really
  caused.
- **D2H sync.** The firmware sets `PCIE_SHARED_D2H_SYNC_XORCSUM`: each
  completion carries a sequence number and XORs to zero. The driver waits for
  both before reading an item.
- **Join parameters.** Firmware WLC 14.2, 14.4, 16.1+ and 17+ (this one is
  14.4) takes the version 1 layout. The old one failed with BCME_BADSSIDLEN,
  which NetworkManager reported as a wrong password.
- **Host short frame header.** The firmware advertises `host_sfhllc`, so the
  host builds the 802.3 + LLC/SNAP header of every TX frame, as bcmdhd does.
  Left to the firmware, the LLC insertion overwrites the packet's own state
  (its ring slot reads back as the ethertype, 8). Every TX status then fails
  the firmware's checks, throughput collapses, and it traps within about 20 s.
- **Packet ID 0 never used.** RX buffers carry their ID as `request_id`, and
  the firmware traps on an RX completion whose `request_id` is 0.
- **Reused ring slots cleared.** A flow-ring create request reused an ioctl's
  slot and carried the ioctl buffer address as its TC, priority/IFRM mask and
  interrupt vector.
- **Power domains.** After each reset the driver requests the chip's four
  save/restore power domains (PCIe, ARM/backplane, MAC aux, MAC main) through
  ChipCommon `powerctl`, as bcmdhd's `si_srpwr_request()` does. From power-on
  only the first two are on. The firmware then cannot reset the MAC cores, and
  once the interface comes up its backplane faults and reads all-ones. A chip
  warm from an earlier run hid this: Wi-Fi worked after a driver reload but
  not from boot.

## Known gaps

- **Power.** Connected and idle with the screen off, the phone draws 2.11 W at
  the USB input; with the Wi-Fi chip powered off, 1.65 W. The link stays in
  L0 (no ASPM or L1 substates), and firmware deep sleep is off for lack of
  device wake. The power-domain request is not the cost: releasing it once
  the firmware is up leaves the domains on (the firmware holds them) and the
  draw unchanged.
- **Unrecognised channels.** Attach warns about a 6 GHz chanspec (`0x5001`)
  that mainline cannot decode, and ignores channels 169 to 177.
- **MAC address.** The OTP is empty, so the address is random on every boot.
- **P2P.** Creating wpa_supplicant's P2P device times out.
- **Mailbox data.** The firmware sends mailbox data (deep-sleep and trap
  notices) as control messages, and brcmfmac ignores them. A firmware trap is
  therefore silent; only the firmware console, in its RAM, shows it. The stock
  `vendor_a` image has `/firmware/fw_bcmdhd.map`, the firmware's symbol map.

Image Z SHA256: `bcdc339ad0801df786379b0d43435894d1d1dd287e8497823ce52e5a7ce76a34`
