# APM watchdog reset of October 3

At 22:51 on October 3 the phone reset by itself while it showed the
always-on display. The bootloader recorded `RST_STAT: 0x4 - APM_CPU_WDTRESET`
and `Reboot reason: 0xcbcd - APM Watchdog`. The modem guard then blocked the
modem for the next 15 hours, which also blocked sleep. `modem/manager.py`
now starts the modem again after a session that ended while running
(commit 027da1a).

## What the dump says

The power-management firmware (APM) halted itself while it powered the DPU
display block off. The kernel log ends with two ACPM `Timeout! ch:9`
(temperature reads) and `AOD power-off failed`, 0.3 s after a deep-sleep
exit. That exit came during a burst of modem wakes, one every 0.6 s.

The bootloader's `ACPM_LOG` dump decodes against the APM firmware. That
firmware is inside `bl2_a`: Thumb code, address = file offset - 0x1e000. Do
not commit it.

- Lines are newest first. `pmu_irq+ 100000` is interrupt group 20, and
  `pend_grp 1000` is bit 12, whose argument is domain 0x14 (DPU,
  `0x18062200`). The handler is the local power-down sequence.
- `pdn_err 14`, `fsm_step 13`, `address 18062224`, `bit 6` and `wait_val 0`
  come from the firmware's wait helper. `pdn_err` is the domain index and
  `fsm_step` is that domain's STATES register. After a timeout the helper
  executes `udf #0xff` on purpose: the "ACPM DUMP GPR - EXCEPTION ff" line.
  The APM watchdog then resets the SoC.
- The sequence asks the block to stop: OUT (+0x20) |= 0xd, wait for IN
  (+0x24) bit 5, clear OUT, wait for bit 5 to clear, then for bit 6. Each
  wait allows 100 us. Almost every domain uses it.

`pixel-dpu-in.ko` samples DPU/DISP STATUS, OUT and IN every few
microseconds and logs each change. It is read-only and refuses to stay
loaded. On a normal power-off, IN bits 5 and 6 rise together and both clear
within about 11 us. On October 3 bit 6 stayed set for more than 100 us.

## What did not reproduce it

| Test | Display power cycles | Result |
|---|---|---|
| `dpu-cycle-test.sh 1500`: AOD hibernate on and off | 1,496 | no fault |
| `dpu-cycle-test.sh 1500 tmu`: the same with about 1,500 ACPM temperature reads a second | 1,488 | no fault |
| `aod-resume-loop.sh 600`: deep sleep, alarm wake, then the pipeline up and off 0.15 s later | 600 | no fault |

Temperature reads arriving during the handshake did not cause it, and neither
did a power-off shortly after a deep-sleep exit. Whatever kept the block
busy is rarer, perhaps tied to the modem wake burst. The firmware's
interconnect "PM lock" handshakes use the memory-controller domains, not the
DPU.

## If it happens again

The next boot saves pstore under `/var/log/pixel-pstore/<boot id>/`. Compare
`pdn_err` (domain), `fsm_step`, the waited address and bit, and the kernel
lines just before the reset. Stock powers the DPU off only after disabling
its windows, clearing and masking the DMA and DPP interrupts, and stopping
the DECON (`exynos_drm_decon.c` `decon_enter_hibernation`). The scanout
driver skips those steps. That difference is the first candidate if a
second dump points at the DPU again.

## Building

```sh
make -C devices/pixel7pro/mainline/linux ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- M=<copy of this directory> modules
```
