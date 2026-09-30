# Native reboot target

`pixel-reboot.c` registers a reboot notifier. Loading it only validates the stock
DT and maps the PMU for readback; no register is written until an orderly restart.
Normal restart selects mode 0. Setting the root-only `bootloader` parameter selects
0xfc on the next restart. The ordinary PSCI restart still performs the reset.

The module checks cheetah, `/exynos-reboot`, its syscon resource at `0x18060000`
and `reboot-cmd-offset=0x810`. It uses the same secure PMU write interface as
upstream `drivers/soc/samsung/gs101-pmu.c`, without binding GS201 to a GS101
power-management driver or writing battery-backed reboot storage.

## Reboot to fastboot (working, September 27, 2026)

The bootloader target works when the restart is a **warm** reset. The
firmware implements PSCI 1.1 `SYSTEM_RESET2`. With `echo warm >
/sys/kernel/reboot/mode`, Linux uses that reset and SYSIP_DAT0 keeps 0xfc, so
ABL enters fastboot. The default cold `SYSTEM_RESET` loses the value and ABL
boots normally. The stock driver avoids this differently: it also writes the
mode to battery-backed fuel-gauge storage (RSBM), which this module does not
touch. ABL clears the mode once used, so the following boot is normal.

`scripts/pixel-reboot.sh` (installed as `/usr/local/sbin/pixel-reboot`) sets
both the module parameter and the warm mode, then signals PID 1 for its orderly
stop. From the host, `scripts/pixel-reboot.py fastboot` or `linux` drives the
whole round trip over USB and waits for the target. Measured on image L:
fastboot 26 s after the request, Linux SSH 16 s after `fastboot reboot`, and
the desktop 19 s after it.

## Earlier result

The first test below ran before warm reset was known to matter. At that time
`boot_a` still held stock Android, so the "recovery" it reached was most likely
Android failing on the Linux userdata after a normal boot. ABL had ignored the
mode rather than mapped it to recovery.

Mode values and DT semantics come from Google's
[GS201 reboot module](https://android.googlesource.com/kernel/google-modules/power/reset/+/9b4bb088cf8f02bcebece637f0a1a6c33f3db29d/exynos-gs201-reboot.c).
The first hardware test of mode 0xfc, with the default cold reset, reached
Android recovery instead of the bootloader. Recovery ADB accepted
`adb reboot bootloader` and restored fastboot. Power+VolDown remains the
fallback if Linux is unreachable.
The stock driver also writes battery-backed reboot storage; this module does not.
Do not reboot while installation is writing storage, and do not factory-reset
from Android recovery after installing the Linux root.
