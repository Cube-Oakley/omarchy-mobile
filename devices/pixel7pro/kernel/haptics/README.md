# Vibration

`pixel-haptics.c` presents the CS40L26A haptics driver (0x43 on hsi2c_8, see
[i2c](../i2c/README.md)) as a force-feedback rumble device, `Pixel haptics`.
The shell's `vibrate` command finds it.

The driver uses the chip's ROM firmware; no RAM firmware or tuning files are
loaded. The bootloader leaves the chip out of reset (gpp24-3 high) with that
firmware running (HALO state 2). The chip hibernates between uses and NACKs
the transfer that wakes it. The driver refuses to load unless it finds a
CS40L26A (device ID 0x40A260) at revision A1, B0 or B1, the revisions whose
ROM memory map is known.

For each effect the driver:
1. Wakes the chip and sends PREVENT_HIBERNATE.
2. Sets the boost peak current to the stock DT's 2.5 A. The chip's default is
   4.5 A, and the setting does not survive hibernation.
3. Triggers a ROM effect through the mailbox. Effects of 30 ms or less play
   ROM click 0; longer ones play the ROM buzz (about 0.7 s), triggered again
   every 600 ms and stopped at the effect's end.
4. Sends ALLOW_HIBERNATE 0.5 s after the last effect.

The strength setting is ignored: it needs RAM firmware.

Registers and commands follow Cirrus Logic's cs40l26 driver
(CirrusLogic/linux-drivers, `v6.18-cs40l26`).

Measured at the USB input: a 1000 ms effect peaks at 2.68 W, against a
1.73 W idle peak. It has not yet been confirmed by hand.
