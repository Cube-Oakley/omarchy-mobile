# Wireless charging (plan, 2026-10-02)

Nothing is written yet; this records what stock's device tree and Google's
GPL charger source say.

## Hardware (stock overlay, CHEETAH MP)

- **Receiver:** IDT/Renesas P9412, `idt,p9412` at 0x3C on `hsi2c_15`.
  `pixel-hsi2c` already runs that bus at boot, for the torch and haptics.
- **Interrupt:** `gpa9-5`, active low.
- **Enable:** `gpp0-0` (`idt,gpio_wlc_en` and `idt,gpio_qien`, active low;
  pinctrl `wc_en`, output, no pull).
- **Other control lines** (stock wiring):
  - `idt,gpio_ben` on the MAX77759 GPIO 4;
  - `gpio_boost` and `gpio_extben` on S2MPG12 GPIOs 3 and 2;
  - `gpio_dc_switch` on S2MPG12 GPIO 0.

  They are for reverse charging and wireless direct charge.
- **Power path:** the receiver's output feeds the MAX77759 charger's
  wireless input (WCIN). `idt,max_vout_mv` is 19500 mV for EPP and wireless
  direct charging.
- **Calibration:** foreign-object detection tables `fod`, `fod_epp`,
  `fod_hpp` and `fod_hpp_hv`, plus a Q factor of 0x38.

## Software

Google's driver is `p9221_charger.c` and `p9221_chip.c` (about 9.5k lines,
GPL, in the bms tree). Most of it is EPP/MPP negotiation, foreign-object
detection, reverse charging (battery share), wireless direct charge, and
gcpm power votes.

## Plan

1. **Basic Qi (BPP, 5 W) without a driver.** A Qi receiver powers up from the
   pad's field and negotiates BPP by itself. With `gpp0-0` enabling it and
   the charger accepting WCIN, it may charge already. Test with a pad:
   CHG_DETAILS_00 WCIN_DTLS, and the gauge's current. Needs the user and a
   Qi pad.
2. **pixel-battery:** WCIN as a second input (presence, input limit
   `CHG_CNFG_10`, mode), the same temperature limits, and the `usb` and
   `wireless` power supplies.
3. **A small P9412 driver:** status, Vout/Iout, the alignment and
   foreign-object interrupts on `gpa9-5`, and loading the FOD tables. Then
   EPP (up to 15 W), using the same port-temperature style guard on the
   coil's NTC (`qi_therm`, S2MPG13 NTC channel 2, which `pixel-port-temp`
   can read).
4. **Not planned:** reverse charging (battery share) and wireless direct
   charging.

Wireless charging also needs a sleep policy, as wired fast charging does:
the temperature checks stop in SYS_SLEEP.
