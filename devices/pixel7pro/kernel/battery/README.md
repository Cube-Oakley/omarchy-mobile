# Battery state and charge control (MAX77759 over hsi2c_13)

`pixel-battery.c` drives the PERIC1 USI13 I2C controller at 0x10D60000 and
reads the MAX77759 fuel gauge (0x36) and charger (0x69). It registers:

- an I2C adapter, "Pixel hsi2c_13", for later drivers;
- a `battery` power supply: capacity, status, health, voltage, current,
  temperature, charge, time to empty or full, and the charge limit
  (`charge_control_end_threshold`);
- a `usb` power supply: whether USB input is present, and the input limit.

The shell reads these through `overlay/mobile/battery.py`. The boot script
loads the module after the thermal checks and runs `omarchy-mobile-battery
restore` to apply the saved charge limit.

## The bus

The bootloader already uses this bus, and leaves it set up:
- USI13 in I2C mode (sysreg_peric1 +0x14 = 4);
- gpp25-0/1 on the I2C function;
- clocks running;
- the HSI2C in auto mode with 400 kHz timings.

The module checks that handoff and refuses to load if it differs. It never
touches the clocks, pins or timings. Transfers follow mainline `i2c-exynos5`
(Exynos7 interrupt layout, 64-byte FIFOs), polled. A timeout resets the
controller and restores the saved configuration.

## Gauge

The gauge initially reported status 0x0082 (POR), DesignCap 3000 mAh and a
restarted cycle count. Its default model made capacity and percentage unreliable.
On September 30 the guarded model loader restored the phone's stock profile:
DesignCap now reports **5002 mAh** (0x09C5 at 2 mAh per LSB). Percentage changed
from 100% to 92%; this is a recalculated estimate, not a sudden loss of charge.
This nominal model capacity is not a measurement of battery aging or health.

- **Capacity:** charge units follow TaskPeriod: 1 mAh at 0x1680, 2 mAh at
  0x2D00. FullCAPRep is register 0x35 on this gauge, not the older 0x10 alias.
- **Temperature:** Config (0x2350) has TEx set, so the Temp register holds its
  power-on 22.0 °C. The module instead computes the battery temperature from the
  thermistor ratio (AIN, 0x27). It uses the stock model's NTC constants
  (TGain 0xED51, TOff 0x1EBA, from the DT `fg-params`) with the gauge's linear
  formula, without the small TCurve correction.
- **Cross-check:** the charger's own thermistor zone (CHG_DETAILS_03 THM_DTLS)
  agrees with it.

Scaling with the 5 mΩ sense resistor: current 312.5 µA, charge as above, voltage
78.125 µV, time 5.625 s per LSB. IIn (0xD0) reads the USB input current at
about 0.125 mA per LSB.

### Model restoration

Writing `1` to `/sys/bus/platform/devices/pixel-battery/restore_model` selects
the model by reading **only** EEPROM offset 0x17 on hsi2c_15. IDs 1 and 3 select
the matching 48-word model and 27 parameters in the bootloader's device tree;
unknown IDs, layouts or sense-resistor values are refused. This unit reports
ID 3 (Lishen). No EEPROM contents, cycle counts or saved learning records are
written.

The sequence follows Google's `max_m5_load_gauge_model()` and
`max_m5_update_custom_parameters()`: wait for data ready, unlock/write/read back
the model, relock and verify inaccessible model RAM, write ordered parameters,
trigger LdMdl and wait for completion, verify the version, then clear POR.
Every wait is bounded. Loading requires USB power, at least 3.5 V and a
temperature below 40°C. Charging pauses during loading and stays paused on a
partial failure. Property reads and charge-control writes share the loader's
mutex. A request with POR already clear preserves the initialized gauge.

The session bootstrap attempts restoration on powered startup after hsi2c_15
is available. The driver still provides useful temperature readings before
model restoration. The live load completed successfully, resumed normal
charging at 22.5°C, and a repeated request left the initialized model intact.
Normal boot from the installed image also retained the 5002 mAh reading.

Reference: Google's [ModelGauge m5 driver](https://android.googlesource.com/kernel/google-modules/bms/+/refs/heads/android-gs-pantah-5.10-android13-d1/max_m5.c)
and the cheetah battery profiles supplied in the phone's device tree.

## Charger

The bootloader leaves 1.93 A fast charge (CNFG_02 0x1D), a 4.35 V float
(CNFG_04 0x23) and hardware JEITA on (CNFG_08 bit 2). The module changes four
registers.

**Step charging (CNFG_02, CNFG_04).** The cell is a 4.45 V one: the stock DT
has `google,fv-max-uv` 4.45 V, and Google's charger driver only reports full at
4.40 V (`max77759,chg-term-voltage`). At the bootloader's 4.35 V float the
charge ended at about 92 % with the restored model, resting at 4.30 V. The
module now follows the stock step-charging tables from `/google,battery`, read
from the bootloader's DT at load:
- `chg-temp-limits` 0/10/20/42/46/48/55 °C bound six bands; below 0 °C or from
  55 °C nothing charges. The module's own 45 °C pause still applies.
- `chg-cv-limits` 4.20/4.30/4.45 V are the tiers. Each tier floats at its
  voltage; once the cell is within 10 mV of it, the next tier takes over.
  Tiers only rise until USB is removed.
- `chg-cc-limits` give each band and tier a current as a percentage of the
  gauge's DesignCap. A tier with no current holds the one below: from 0 to
  10 °C the float stays at 4.30 V.
- The current never goes above the bootloader's 1.93 A, and no float goes above
  4.45 V, whatever the DT says. Missing or odd tables leave both registers as
  the bootloader set them.
- A charger that already finished restarts after its float goes up.

Measured on September 30, at 22.5 °C: the float went from 4.35 V to 4.45 V, the
charger left DONE and charged at +1.0 A from 92 %. About 70 minutes later the
gauge read 100 % (5,016 of 5,016 mAh) at 4.42 V, with the charger still
tapering in constant voltage at +0.37 A.

**Input limit (CNFG_09).** The bootloader leaves 0x13: 500 mA under AutoIBUS.
In that mode the charger holds a computer port at 500 mA whatever CHGIN_ILIM
says. With the screen on, the phone drew about 2.6 W, so it drained about
90 mA while plugged in.
- Stock ran BC1.2 detection, found this host's port is a charging downstream
  port (`usbchg=USB_CDP`), and set 1.5 A.
- The module writes NO_AUTOIBUS | 1500 mA (0xBB), as Google's stack does once
  its detection has run.
- Measured: input about 1.46 A at VBUS 4.68 V, battery +0.9 to +1.0 A.
- `input_limit_ma` (100–1500) changes it; 0 leaves the bootloader's setting.
- The charger's input voltage regulation (AICL) backs off on a port that sags.

**Negotiated input limit (installed 2026-10-01).** The limit
follows what the port offers, so a weak port is never asked for more:
- **Source:** `pixel_usb_port_offer_ma()` in pixel-usb-switch reads the Type-C
  controller. 3000 or 1500 mA when the source advertises that Type-C current
  on CC. Otherwise the BC1.2 type: 1500 mA for a charging (CDP) or dedicated
  (DCP) port, 500 mA for a plain data port (SDP) or none. 500 mA while
  detection runs. pixel-battery looks it up by name each poll, so either
  module loads alone; without the switch module the limit stays at 500 mA.
- **Ramp:** from 500 mA up by 500 mA per 10 s poll, to the offer and at most
  `input_limit_ma` (now 100–3000, default 3000). It drops to a lower offer at
  once.
- **Sag:** if the charger's input voltage regulation acts (AICL_OK clear in
  CHG_INT_OK), the source is sagging under the load. The limit backs off
  250 mA and stays below that until the cable comes out.
- **Unplugging:** the charger keeps its limit without USB, so a poll that
  finds the cable out sets 500 mA again. Only a swap within one 10 s poll
  keeps the old limit until the next poll, with AICL as the guard.
- The charge current is unchanged (at most the bootloader's 1.93 A). On a
  3 A source the battery can now get all of it, against about 1.0 A from
  today's 1.5 A input.
- **The development PC:** its port reads as SDP (`typec_status`: BC1.2 SDP,
  default Type-C current), so it would get 500 mA, not the 1.5 A it supplies
  now. Stock detected the same port as CDP. The controller's detection ran
  when the cable went in, and running it again would disturb the USB link, so
  a replug test decides.

`sh kernel/battery/install-charging-negotiation.sh [WIFI-ADDRESS]` (from
`devices/pixel7pro`) installs it. It reloads both modules, keeps the first
old ones in `/root/*.before-negotiate`, and shows the limit ramping.

Measured on October 1:
- **The PC's port:** read as SDP, so the limit stayed at 500 mA. After a
  replug, BC1.2 read "none" (also 500 mA).
- **Lenovo 65 W USB-C brick:** advertised Rp 3.0 A. The limit ramped
  500 → 3000 mA in 50 s, with VBUS at 4.83 V under the full load and no
  AICL.
- **Battery current on the brick:** first only +0.3 A at 79 %, then 0 with
  the status "Full". That was the step-charging stall below. Once fixed: +0.77,
  1.03, 1.62, 1.87 and 1.90 A as the input limit rose. That is the
  bootloader's 1.93 A charge cap, about twice the +1.0 A of the fixed 1.5 A
  input.

**Step-charging stall (fixed October 1).** A tier moved on only when the
gauge read its voltage. The charger holds the float at its own output, and
the cell read 4.174 V under the 4.20 V float, so the charge tapered and the
charger ended it as done at 79 %. A tier now also moves on once the charger
is in constant voltage, top-off or done and the current is below what the
next tier allows. A tier first holds 20 s, since the charger's state lags a
float change.

**Port temperature and a 3 A charge current (October 1, 23:00).** The user
asked to raise the charge current if it can be done safely.
- **The port's temperature** comes from `pixel-port-temp`
  (`kernel/port-temp`), read each poll while USB power is there. It limits
  the input as stock's USB throttling does:
  - 500 mA at 45 °C, or above 42 °C when the port is also 2.5 °C over its
    second sensor or 5 °C over the board;
  - 1.5 A from 40 °C;
  - 1.5 A when the temperature cannot be read. Above 1.5 A needs a working
    port sensor.
- **The charge current** may reach `charge_max_ma` (default 3000) with the
  battery at 15–40 °C, still under the stock step table (at 20–42 °C:
  1.0 C, 0.7 C and 0.5 C, so 3.0, 3.0 and 2.5 A on this 5 Ah cell). Outside
  that range it stays at the bootloader's 1.93 A. Charging still stops at
  45 °C.
- **Legacy cables:** an advertised 1.5 or 3 A with a plain data port
  (BC1.2 SDP) is a legacy A-to-C cable with the wrong pull-up on a computer.
  It gets 500 mA.
- **The ramp** also backs off when VBUS falls below 4.6 V.

**Mode (CNFG_00).** The module only switches between 5 (charge + buck) and 4
(buck only: the phone runs from USB and the battery rests). It never leaves
off (0) or OTG boost.
- **Charge limit:** switches to buck only at the chosen limit, and restarts
  5 % below it.
- **Temperature:** switches to buck only at 45 °C, resuming at 40 °C, or when
  the temperature can't be read.

**Wireless (October 4).** The P9412 receiver (0x3C on hsi2c_15, through
pixel-hsi2c) feeds the charger's WCIN. The chip default already selects WCIN
(CNFG_12 0x6a) with a 500 mA limit (CNFG_10 0x0f), and on its own the receiver
negotiates the Qi baseline (BPP, 5 V) with its built-in calibration. On an
INIU Qi pad that charged at 497 mA, about 2.5 W. With a transmitter present
(WCIN_DTLS valid), the module now does what Google's `p9221_charger` does:
- **Calibration:** it writes the foreign-object (FOD) table for the
  negotiated mode (`fod`, or `fod_epp`) from the device tree's
  `idt,p9412` node into the receiver (0x70, 16 bytes), and reads it back. The
  receiver reports received power with it, and the transmitter compares that
  with what it sends to find metal heating in the field. The receiver forgets
  the table off the pad, so it is written each time.
- **Input limit (CNFG_10):**
  - BPP: 700 mA (687.5 at the charger's steps), and 900 mA (875) after 7
    minutes on the transmitter.
  - Other modes: 500 mA until they are characterised.
  - 200 mA at 100 %.
  - 500 mA from 40 °C until 37 °C.
  - It rises 100 mA per poll and backs off and holds if AICL acts.
  - Off the pad, or with USB in, it returns to the bootloader's value.
  - `wireless_limit_ma` caps it (125–900; 0 leaves the bootloader's).
- **Charging control:** step charging, the charge limit and the 45 °C pause
  apply as with USB.
- **Supply:** a `wireless` power supply reports online, the limit and the
  receiver's output voltage.

On the INIU pad the receiver then gave about 680 mA at 4.98 V and the battery
took 400–570 mA at 95 %, with the receiver die at 40 °C. The receiver's
interrupt (gpa9-5) and enable (gpp0-0, active low, left low by its
pull-down) are not used. While the phone sleeps the 10 s poll stops: the
charger keeps the last limit, and its own JEITA control still applies.

Unloading the module restores all five registers to the bootloader's values.
Settings are written only when CHGPROT reads unlocked, and every write is read
back.

## Not done

- No BC1.2 detection. The 1.5 A input limit applies to any port.
- Learned-state persistence and recovery of the historical cycle count.
- No interrupts: a 10 s poll sends the uevents.
- No USB PD: input is 5 V only.
- Wireless above the Qi baseline: EPP (15 W) and Google's Pixel Stand mode
  need a transmitter that offers them. Reverse wireless (Battery Share) is
  not started.

## Measurement aids (October 1, 2026)

- `input_off=1` puts the charger in mode 0 (buck off): with USB connected the
  system runs from the battery and the gauge reads its drain. Clearing it puts
  the charger back in buck mode, and normal charging control resumes on the
  next poll (10 s). Only modes 4 and 5 are ever switched to 0.
- `coulomb` (read-only) returns the gauge's coulomb counter QH:QL, raw and in
  nAh. `charge_now` (RepCap) moves in 2 mAh steps; QL adds 16 bits, so a
  few minutes of sleep can be measured. Checked against `current_now`: 1.313
  mAh in 10 s while charging at 464 mA.
