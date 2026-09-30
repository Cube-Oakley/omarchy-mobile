# V26: audio, sensors and Bluetooth

`audio-v26.patch` is a complete patch against the pinned base in
`kernel-base.txt`, with every added file. Do not stack it on v25. It
reverse-applies cleanly to the working source. It is v25's 53 files plus
Bluetooth's `hci_bcm.c`, and `btbcm.c` has changed since v25. `kernel.config`
is image G's configuration, identical to v25's apart from the initramfs path.

The kernel image's code is v25's apart from the Bluetooth drivers, which are
modules. The work is in modules and userspace that the image carries:

| Area | What |
| --- | --- |
| Bluetooth ([bluetooth](../bluetooth/README.md)) | The BCM4389 on UART18 at 3 Mbaud with Google's patch firmware. `btbcm` downloads it without waiting for acks (`brcm,bcm4389-bt`). `hci_bcm` keeps the operating speed for this host-powered chip. |
| AoC ([aoc](../aoc/README.md)) | Google's Trusty, GSA and AoC drivers, assembled from pinned sources plus patches. They load the AoC firmware without pKVM. |
| Audio ([audio](../audio/README.md)) | Microphones through the AoC. Speakers on two CS35L41 amplifiers: [spi](../spi/README.md), [gpio](../gpio/README.md) and mainline `cs35l41`, with the stock protection firmware and factory calibration. |
| Supplies ([aoc-power](../aoc-power/README.md)) | The microphone and sensor PMIC rails. |
| Sensors ([sensors](../../sensors/README.md)) | The AoC's sensor framework over USF, served as iio-sensor-proxy's D-Bus API. |
| RTC alarm ([rtc](../rtc/README.md)) | The S2MPG12 alarm wakes the phone from s2idle (`rtcwake`). |

Image H (`out/checkpoints/20260929-audio/image-h`, readback SHA256
`ebbcc45e97ac848cff8ec552a0be54bd25e63da45f09d3d50a3c8df9847d92bf`) is in
`boot_a`. It is image G plus the RTC alarm; image G's readback was
`4df8404ec07156b75ea52ac1fe6352d76f1dbfefae314df8a1098cc1b7ebfd09`.

The session script starts, in order:
1. Bluetooth;
2. the AoC with audio;
3. the sensor proxy;
4. PipeWire.

Screen off and idle, with all of these running, the phone draws 1.48 W at
the USB input, against v25's 1.47 W.
