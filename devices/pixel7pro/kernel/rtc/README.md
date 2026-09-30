# GS201 / S2MPG12 real-time clock

`pixel-rtc.c` reads the battery-backed RTC in the S2MPG12 main PMIC through
the firmware's ACPM PMIC protocol and registers it as `rtc0`, with a wake-up
alarm. With
`CONFIG_RTC_HCTOSYS`, registering it sets the system clock, so the phone has
the real time about two seconds into boot. Android keeps this RTC in UTC.

Transport and registers, from the stock DT and Google's GS201 sources
(`google-modules/soc/gs`, branch `android-gs-pantah-6.1-android16`):

- ACPM IPC channel 2, PMIC bus 0, RTC bank 2 (`s2mpg12-core.c` `I2C_ADDR_RTC`).
  The [power-key module](../powerkey/README.md) uses the same channel with bank 1.
- Registers 0x00–0x1b match the S2MPG10's (`rtc-s2mpg12.h`); the S2MPG12 adds
  nonce and scratch registers after them.
- Time is SEC..YEAR at 0x06–0x0c, binary and 24-hour. The module refuses to
  load unless RTC_CTRL shows binary 24-hour mode.

Reading follows the vendor `s2m_rtc_update()`:
1. Read RTC_UPDATE (0x01).
2. Clear RUDR, then set it, to latch the counters.
3. Wait 1 ms before reading the time.

What is written to RTC_UPDATE always has the time write (WUDR) and freeze
bits cleared, so the time never changes. The module has no `set_time`,
WTSR/SMPL or oscillator changes, so Linux cannot correct RTC drift. On
September 28, 2026 it read about 30 s ahead of an NTP-synced host.

## Alarm and wake-up

Alarm 0 is an RTC alarm (`/sys/class/rtc/rtc0/wakealarm`, `rtcwake`) that
wakes the phone from s2idle:
- **Setting it:** alarm 0 lives at 0x0d-0x13, in the time's layout, and
  bit 7 of each field enables it. It is written and then moved into effect
  with an RTC_UPDATE.AUDR pulse, as the vendor `s2m_rtc_set_alarm()` does.
- **Its interrupt:** RTCA0 is PM-bank INT2 bit 2. It is unmasked in INT2M
  (0x06) only while the alarm is enabled; the PMIC's other interrupts stay
  masked.
- **Delivery:** the PMIC signals on its I3C bus with an in-band interrupt.
  The SoC turns that into GIC SPI 72, the `s2mpg12mfd` node's interrupt,
  through `SYSREG_VGPIO2AP`. The common source mask (IBIM1) already routes it.
- **Handling:** as in the vendor `s2mpg12_irq_thread()`, the handler clears
  `INTC0_IPEND` (0x182F0290) and reads INT1 to INT5 as one block, which
  acknowledges the PMIC. A read of INT2 alone returns only its live 1-second
  tick and leaves the interrupt asserted. It was measured that way: that
  mistake gave an interrupt storm.

The interrupt is a wake-up source, so the kernel's alarmtimer takes the RTC,
and the module cannot be unloaded while alarms are possible. `alarm=0` gives
the time-only driver.

Validated on 2026-09-29, with the AoC, audio, sensors and Wi-Fi running:
- four `rtcwake -m mem -s 20` cycles suspended to s2idle and woke on the
  alarm (`pm_wakeup_irq` was the RTC interrupt);
- USB came back in 1-2 s;
- audio and the sensors still worked afterwards.

The persistent boot script loads it right after ACPM activation. The earlier
clock floor (newest log timestamp) stays as a guard in case the RTC ever reads
earlier than it. Verified on image N: RAM boot and three normal reboots set
the clock at about 2 s.
