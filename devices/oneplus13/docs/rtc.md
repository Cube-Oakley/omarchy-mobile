# PMK8550 clock

The probe 20 kernel's stock-matched PMK8550 RTC node works with its matching
`drivers/rtc/rtc-pm8xxx.ko`. The raw counter uses a vendor offset and initially
appears as 1971. Do not write the hardware RTC to correct that offset.

`scripts/rtc-clock.py` loads a hash- and kernel-pinned module and restores only
Linux wall time using an anchor saved on the native ext4 root. It checks the
board/root identity, RTC name, anchor range and elapsed counter. A reset,
wrap or stale anchor fails without setting a guessed time. If module loading
applies the raw counter through kernel HCTOSYS before validation fails, the
helper restores the previous software clock plus elapsed monotonic time.

## Installation

Build the RTC module with the same configuration and kernel release as the
running image. Install it as `/opt/oneplus13-rtc/rtc-pm8xxx.ko`, with
`manifest.json` containing `kernel_release` and its SHA256 in `sha256`.
Install the helper as
`/usr/local/lib/omarchy-mobile/oneplus13/rtc-clock.py` (mode 0755).

Run `rtc-clock.py --calibrate UNIX_EPOCH` with a fresh epoch from a separately
verified clock. This saves `/var/lib/omarchy-mobile/rtc-anchor.json` privately
on the native root. Do not distribute another phone's anchor. The persistent
session runs `--restore` before the desktop when an anchor exists; diagnostics
go to `/run/rtc-clock.log`. Without a usable anchor it retains the existing
software-clock fallback.

## Evidence and limits

On October 12, the counter advanced 110 seconds across a clean power-off and
physical Power-button boot, matching host elapsed time. A subsequent normal
reboot restored the correct wall time before Hyprland started. Private evidence
is in `out/native-persistent20-usb/rtc-*`. This does not validate long-term
drift, battery-disconnected retention, RTC alarms or wake from suspend. No RTC
time/alarm writes, Android offset changes or calibration partition writes
were needed.
