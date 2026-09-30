# Cameras: hardware map and bring-up plan (September 30)

Host-only research. The phone was not touched. Nothing here has been run yet.

**Recommendation:** start with the **ultrawide**. It is a Sony **IMX386**, not
the IMX381 the earlier report assumed: its logged chip ID is 0x0386. The
reasons:
- it needs the fewest rails;
- its registers use a Sony SMIA-style map with 8-bit values;
- all its init and mode tables have been extracted from the stock HAL;
- a mainline-style `imx386.c` and a working CSIS + CSIS-DMA driver already
  exist for the same camera front-end on the Pixel 6, from the community
  gs101 mainline port.

Milestone (a), the chip-ID read, is a recipe of about 14 steps (§4). Every
step uses a mechanism this port already exercises.

The two biggest unknowns:
- which CSIS link and D-PHY each sensor is wired to. Neither the DT nor the
  decodable HAL data says;
- how pd_csis/pd_pdp come back on. They are switched off at boot today, and
  the IOMMU state behind them is unverified.

Evidence tags:
- **[DT]** the bootloader-merged live DT saved on 2026-09-24,
  `out/restart-20260924/root-baseline/live.dts`. It is base + dtbo index 6
  (`ro.boot.dtbo_idx` 6, board_id 0x30306). The camera nodes are at lines
  18328–18646.
- **[SRC]** Google's GS201 kernel, LineageOS `android_kernel_google_gs201` at
  `40ff934`, and Google's LWIS (`kernel/google-modules/lwis`, branch
  `android-gs-pantah-5.10-android14`).
- **[BR]** the 2026-09-08 bugreport (`out/pstore-2026-09-08/unpacked/`), with
  line numbers in the main `.txt`.
- **[HAL]** the stock camera HAL apex in `out/stock-vendor/vendor_a.img`
  (`/apex/com.google.pixel.camera.hal.apex` → `lib64/liblyric_hwl.so`).
- **[GS101]** the community Pixel 6 mainline port,
  [m8l8th814n-eng/gs101-mainline](https://github.com/m8l8th814n-eng/gs101-mainline/tree/gs101-7.2)
  (branch gs101-7.2, commits 2026-09-14 to 09-22), and
  [libcamera-gs101](https://github.com/m8l8th814n-eng/libcamera-gs101).
- **[I]** inference.

## 1. Hardware map

### Sensors

| | Main | Front | Ultrawide | 5× tele |
| --- | --- | --- | --- | --- |
| LWIS node [DT] | `sensor-nagual` | `sensor-dokkaebi` | `sensor-sandworm` | `sensor-kraken` |
| Part | Samsung **GN1** (S5KGN1) | Samsung **3J1** (S5K3J1) | Sony **IMX386** | Samsung **GM5** (S5KGM5) |
| Chip ID (logged by the stock HAL) [BR] | 0x08E1 (line 9662) | 0x30A1 (11918) | 0x0386 (11904) | 0x08D5 (11900) |
| ID register | 0x0000 (u16) | 0x0000 (u16) | 0x0016 (u16) | 0x0000 (u16) |
| Bus [DT] | hsi2c_1: PERIC0 USI1, 0x10900000, pins gpp2-0/1 | hsi2c_2: USI2, 0x10910000, gpp4-0/1 | hsi2c_3: USI3, 0x10920000, gpp6-0/1 | hsi2c_4: USI4, 0x10930000, gpp8-0/1 |
| Stock bus number and speed | i2c-1, 1 MHz | i2c-2, 950 kHz | i2c-3, 950 kHz | i2c-4, 1 MHz |
| Address; register/value width | 0x3d; 16/16 | 0x10; 16/16 | 0x1a; 16/8 | 0x2d; 16/16 |
| Reset line (1 = running) | gpp2-2 | gpp4-2 | gpp6-3 | gpp6-2 |
| MCLK clock (DT rate) | CIS_CLK3 (26 MHz) | CIS_CLK1 (24 MHz) | CIS_CLK0 (24 MHz) | CIS_CLK2 (24 MHz) |
| MCLK pad (function 2 = MCLK) | gpp9-0 | gpp5-0 | gpp3-0 | gpp7-0 |
| CSI lanes (from the HAL tables) | 3-trio C-PHY (0x0114 = 0x0201) | 4 (0x0114 = 0x0301) | 4 (0x0114 = 0x03) | not yet read |
| Stock modes [HAL] | 8160×6144 … 1280×720 | 3648×2736 … 1920×1116 | 4032×3016, 4032×2272, 2016×1508, 2016×1136; RAW10 | 4032×3024 from 8064×6048 |
| CSIS link / D-PHY | **unknown** | **unknown** | **unknown** | **unknown** |

- **Part identities.** Each ID was logged by `sensor_generic_driver.cc:5073
  … ReadSensorId success` and matches a constant in `liblyric_hwl.so`.
  - GN1 0x08E1 is confirmed by MTK `kd_imgsensor.h` and by the gs101
    `s5kgn1.c`.
  - 3J1 0x30A1 is confirmed by Intel's `s5k3j1.c`.
  - GM5 0x08D5 was identified by ID and output size only [I]; no source names
    it. The GM1 in the earlier report was wrong.
  - The ultrawide IMX381 in the earlier report was also wrong.
- **Actual MCLK.** Every stock table programs EXTCLK as 24.576 MHz: Samsung
  0x0136 = 0x1894, Sony 0x0136/0x0137 = 0x18/0x93. That includes the main
  sensor, whose DT asks for 26 MHz. The stock MCLK is therefore almost
  certainly OSCCLK (24.576 MHz, mux 0, ÷1) [I]: Google's divider rounding
  turns both 24 and 26 MHz requests into ÷1 of a 24.576 MHz parent [SRC
  `cal-if/ra.c` `ra_set_div_rate`]. Use OSCCLK ÷1 so the stock PLL tables
  stay exact. Confirm with the read-only survey in §4.

### Power sequences [DT; LWIS semantics from SRC `lwis_device.c`]

Order in which LWIS acts:
- it first enables the node's clocks (CIS_CLKx plus the DFTMUX gate);
- it then runs the sequence, sleeping each step's delay *after* that step;
- a `gpio` step drives 1 on power-up and 0 on power-down;
- `mclk_on`/`mclk_off` only switch the pad between MCLK (function 2) and
  driven low (function 1, pull-down).

| Camera | Power-up | Power-down |
| --- | --- | --- |
| Main | L12S → SLG LDO4 2.9 V, 1 ms → SLG GPIO3 → SLG LDO8 1.1 V → SLG LDO1 2.85 V, 2 ms → reset high, 1 ms → MCLK on, 9 ms | MCLK off, 1 ms → reset low, 1 ms → LDO1 → LDO8 → GPIO3, 6 ms → LDO4 → L12S |
| Front | L12S → SLG LDO8 1.1 V → SLG GPIO4, 1 ms → MCLK on, 1 ms → reset high, 8 ms | reset low → MCLK off, 1 ms → GPIO4 → LDO8 → L12S, 1 ms |
| **UW** | **L12S → SLG LDO6 1.1 V → SLG LDO2 2.85 V, 1 ms → MCLK on, 1 ms → reset high, 10 ms** | reset low, 1 ms → MCLK off, 1 ms → LDO2 → LDO6 → L12S, 1 ms |
| Tele | L12S, 0.3 ms → SLG LDO7 1.0 V → SLG LDO3 2.25 V → MCLK on, 1 ms → reset high, 10 ms | reset low → MCLK off → LDO7, 1 ms → L12S → LDO3 |

L12S is S2MPG13 LDO12S (`L12S_CAMIO`, 1.8 V, not always-on). It is shared by
every camera device and by the laser AF.

Rail roles below are inferred [I]:
- LDO1/LDO2 2.85 V and LDO3 2.25 V are the analog supplies.
- LDO6/LDO7/LDO8 are the digital cores. LDO8 is shared by the main and front
  sensors.
- LDO4 2.9 V is the main/UW autofocus supply; both actuators list it.
- SLG GPIO3/GPIO4 are enable outputs for external supplies. The front sensor
  has no SLG analog LDO, so GPIO4 is probably its AVDD enable.

### Other devices on the camera buses [DT, BR]

| Device | Bus, address (reg/val bits) | Supplies | Part [I unless noted] |
| --- | --- | --- | --- |
| `act-slenderman` (main AF) | i2c-1 0x0c (8/8) | L12S, LDO4 | AKM AK737x |
| `ois-gargoyle`, `eeprom-gargoyle` | i2c-1 0x24 (16/32) | L12S, SLG GPIO1+GPIO2 | ON Semi LC898128 |
| `stmvl53l1` (laser AF) | i2c-1 0x29 | vio L12S; pwren gpp8-3, xsdn gpp17-0, irq gpa6-5 | ST VL53L3 |
| `eeprom-smaug-dokkaebi` | i2c-2 0x51 (16/8) | L12S | module EEPROM |
| `eeprom-smaug-sandworm` | i2c-3 0x50 (16/8) | L12S | module EEPROM |
| `act-slenderman-sandworm` (UW AF) | i2c-3 0x0f (8/8) | L12S, LDO4 | AKM AK737x |
| `act-/ois-/eeprom-jotnar` (tele) | i2c-4 0x34 (16/8) | L12S, SLG LDO5 2.85 V, GPIO1 | SEMCO SEM1215SA |
| `flash-lm3644` | i2c-15 0x63 | HWEN gpp8-2 | LM3644, working ([torch](../kernel/torch/README.md)) |

Some pin banks are shared:
- **gpp8** holds the hsi2c_4 pins, the torch HWEN and the laser-AF pwren.
- **gpp6** holds the UW bus and both the UW and tele resets.

Pin writes on these banks must be read-modify-write under one lock that
`pixel-torch` also takes.

### Camera PMIC: SLG51002 on hsi2c_8

`slg51002@75` is on **hsi2c_8** [DT; BR `slg51002 8-0075: chip_id: 0xb13103`,
line 48994]. `pixel-hsi2c buses=15,8` already drives that bus, and it refuses
only the NFC address 0x08. The chip uses 16-bit registers and 8-bit values.
It is revision **AB** (0xB13103). For AB, Google's driver skips the rev-AA
tuning block, and the GPIO outputs need no software-test-mode unlock [SRC
`slg51002-core.c`, `slg51002-regulator.c`].

Three **S2MPG13 GPIOs** hold the chip in reset: bb = GPIO1, buck = GPIO3,
cs = GPIO0. Google's probe raises bb, waits 2 ms, raises buck, waits 2 ms,
then raises cs and waits 10 ms ("turn-on time from CS HIGH to Ready is
~10 ms"). No other stock node uses these three GPIOs.

| SLG51002 register | Address | Meaning |
| --- | --- | --- |
| PATN_ID B0–B2 | 0x1105–0x1107 | chip ID, read 03 31 B1 |
| MATRIX_CONF_A | 0x110d | enable for LDO n at bit n−1 |
| LDO1–8 VSEL | 0x2000, 0x2200, 0x2300, 0x2500, 0x2700, 0x2900, 0x3100, 0x3200 | LDO1–5: 1.2 V + 10 mV × sel; LDO6–8: 0.4 V + 5 mV × sel |
| LDOn MINV/MAXV | VSEL + 0x60/0x61 | the chip's allowed selector range |
| GPIO1–4 CTRL | 0x1710–0x1713 | bit 0 = output on |
| FAULT_LOG1 | 0x1115 | fault history |

Selectors for the stock voltages: 2.85 V = 0xA5, 2.9 V = 0xAA, 2.25 V = 0x69,
1.1 V = 0x8C, 1.0 V = 0x78.

### S2MPG13 access, over the existing ACPM PMIC channel (channel 2, PMIC 1) [SRC]

- **LDO12S:** PM bank 0x01, register 0x37. Bit 7 enables it. The voltage is
  0.7 V + 25 mV × bits 5:0, so 1.8 V is 0x2C. The full write is 0xAC. This is
  the same mechanism `pixel-aoc-power` uses for LDO5S/LDO7S
  (`s2mpg13-regulator.c`, `s2mpg13-register.h`).
- **GPIOs:** bank 0x0C (`I2C_ADDR_GPIO`, `s2mpg13-core.c`). GPIOn_SET is at
  0x05 + n: bit 6 is output enable, bit 5 the value, bits 4/3 pull-up/down.
  Google writes the value first, then the output enable (`s2mpg1x-gpio-gs201.c`).

### Clocks and pins [SRC `cmucal-sfr.c`, `cmucal-node.c`, `pinctrl-gs201.c`]

MCLK comes from CMU_TOP (0x1e080000, always on):

| | MUX (select) | DIV (ratio−1) | GATE (bit 20 manual, bit 21 CG_VAL) | DFTMUX Q-channel |
| --- | --- | --- | --- | --- |
| CIS_CLK0 (UW) | 0x1010 | 0x1814 | 0x2038 | 0x3004 |
| CIS_CLK1 (front) | 0x1014 | 0x1818 | 0x203c | 0x3008 |
| CIS_CLK2 (tele) | 0x1018 | 0x181c | 0x2040 | 0x300c |
| CIS_CLK3 (main) | 0x101c | 0x1820 | 0x2044 | 0x3010 |

The mux parents are:

| Select | Parent |
| --- | --- |
| 0 | OSCCLK |
| 1 | SHARED0/3 |
| 2 | SHARED1/3 |
| 3 | SHARED2/2 (399.36 MHz) |
| 4 | SHARED3/2 |
| 5 | SPARE |

Google's DVFS LUT picks 3, which yields 23.49 MHz for 24 MHz and 26.62 MHz
for 26 MHz. Those rates contradict the stock tables, so that LUT is probably
not what the running HAL uses [I].

The camera I2C controllers use CMU_PERIC0 (0x10800000) and sysreg_peric0.
The layout is the same as the USI7/USI8 code in `kernel/spi` and
`kernel/i2c`:

| USI | user mux | DIV | IPCLK / PCLK gate | SW_CONF (sysreg_peric0) |
| --- | --- | --- | --- | --- |
| 1 | 0x650 | 0x1810 | 0x2068 / 0x206c | 0x10821000 |
| 2 | 0x660 | 0x1814 | 0x2070 / 0x2074 | 0x10821004 |
| 3 | 0x670 | 0x1818 | 0x2078 / 0x207c | 0x10821008 |
| 4 | 0x680 | 0x181c | 0x2080 / 0x2084 | 0x1082100c |

PERIC0 pin banks start at 0x10840000. Each bank has CON at +0, DAT at +4,
PUD at +8 and DRV at +0xc:

| Bank | Offset | Pins |
| --- | --- | --- |
| gpp2 | 0x040 | 0/1 hsi2c_1, 2 main reset |
| gpp3 | 0x060 | 0 MCLK → UW, 1 VSYNC to AoC |
| gpp4 | 0x080 | 0/1 hsi2c_2, 2 front reset |
| gpp5 | 0x0a0 | 0 MCLK → front |
| gpp6 | 0x0c0 | 0/1 hsi2c_3, 2 tele reset, 3 UW reset |
| gpp7 | 0x0e0 | 0 MCLK → tele |
| gpp8 | 0x100 | 0/1 hsi2c_4, 2 torch HWEN, 3 laser-AF pwren |
| gpp9 | 0x120 | 0 MCLK → main |

In the stock DT the `default` pin state of hsi2c_1–4 is `hsi2c*-bus-in`
(function 0, input, no pull). The `on_i2c` state (function 3, pull-up) applies
only while a sensor is powered. The bootloader therefore almost certainly
leaves USI1–4 **unconfigured**, like USI7 (see [spi](../kernel/spi/README.md)),
and not like hsi2c_15/8, which it leaves configured [I]. The survey in §4
confirms this.

## 2. Register tables

### Where they are [HAL]

- Nothing sensor-related is in `/vendor/etc` or `/vendor/lib64`. The HAL is an
  apex; its `apex_payload.img` is ext4.
- `etc/lib64/camera/cheetah_*_*.binarypb` hold ISP tuning, factory
  calibration and AWB LUTs, but no sensor registers.
- `etc/camera/csis_phy_link_phy_settings.binarypb` is 20 bytes:
  `{1:{1:2,2:12}, 2:{3:{3:1,4:31,5:9,6:10,7:61}}}`. Its schema is unknown.
- **Every sensor register table is compiled into `lib64/liblyric_hwl.so`**
  (17 MB):
  - Records are 16 bytes, `{u64 addr, u64 value}`, with no delay field.
  - Some tables are plain `.rodata` arrays, found through `{ptr,count}` spans
    and `GetInitSettings()` stubs.
  - Others are built in code from a literal pool. These were recovered with a
    small AArch64 emulator.
  - Stream-on (0x0100 = 1) is written separately by code.

### What was extracted (host scratch, not committed; 53 files)

| Sensor | Init | Modes | Notes |
| --- | --- | --- | --- |
| **IMX386 (UW)** | `init_noPD` (139 writes) and `init_PD` (212 writes; the stock log uses the PD one, BR 11905) | 13 × 73 writes: 4032×3016, 4032×2272, 2016×1508, 2016×1136, in three PLL sets; plus 2 PLL-variant patches | e.g. mode `0x9167e0`: RAW10, 4 lanes, FLL 3098, LLP 4296, 30.0 fps, OP PLL ≈ 1548 Mb/s/lane |
| 3J1 (front) | 368 (0xFCFC paging, soft reset 0x6010, firmware patch) | 9 × 99–108 writes, 3648×2736 … 1920×1116 | lane rate 1665 or 832 Mb/s, not settled |
| GN1 (main) | 7 variants (1565–8694 writes; firmware patch through 0x6028/0x602A/0x6F12) | 12 modes, 8160×6144 … 1280×720 | which init table is "v2.4" (the one the log shows) is open |
| GM5 (tele) | default, HDR and "new" (3816–4047) | 4032×3024 mode located, not exported | |

Missing from the tables (closed code):
- delays between writes (e.g. after the 0x6010 soft reset);
- OTP and calibration loading;
- per-frame exposure and gain writes;
- GN1 remosaic.

For a first frame, the gap is the delays. Use generous ones: 10 ms after
soft reset, 5 ms after init.

**Licensing:** these are Google/Sony/Samsung tables from a proprietary blob.
Keep them out of git. Instead, commit the extraction script and generate the
tables from the user's own `vendor_a.img` at build time. Move the scripts
(`emu2.py`, `ext_sw.py`, `ext_dok.py`, `whole.py`) from the session scratch
into the repo in a follow-up.

### Open drivers and relatives

| Sensor | Best starting point | Status |
| --- | --- | --- |
| IMX386 | gs101 [`imx386.c`](https://github.com/m8l8th814n-eng/gs101-mainline/tree/gs101-7.2): mainline CCI style, ID 0x0386 at 0x0016, same address 0x1a and the same CIS_CLK0; Rockchip BSP [`imx386.c`](https://github.com/rockchip-linux/kernel/blob/develop-6.1/drivers/media/i2c/imx386.c) (24 MHz, 4 lanes) | Works on the Pixel 6 UW, which is wired 2-lane there. Swap in our 4-lane tables. |
| GN1 | gs101 `s5kgn1.c` (HAL tables, C-PHY 3 trios); MTK [`s5kgn1spmipiraw_Sensor.c`](https://github.com/danascape/linux-daria-mt6877/blob/HEAD/drivers/misc/mediatek/imgsensor/src/common/v1_1/s5kgn1sp_mipi_raw/s5kgn1spmipiraw_Sensor.c) | On gs101 the link locks but no image lines arrive yet (09-22) |
| 3J1 | Intel ipu6 [`s5k3j1.c`](https://github.com/intel/ipu6-drivers/blob/HEAD/drivers/media/i2c/s5k3j1.c) (V4L2, ID 0x30A1, 19.2 MHz); Galaxy S21 `is-cis-3j1-setA.h` | Needs our 24.576 MHz tables |
| GM5 | Nearest: MTK/Samsung GM1SP drivers (ID 0x08D1, 4-lane) | Relative only; use the HAL tables |

Mainline structure templates: `drivers/media/i2c/s5kjn1.c` and `s5k3m5.c`.

## 3. Capture path

### Blocks [DT `lwis_csi@1A440000`; SRC `gs201-isp.dtsi`, `gs201-isp-event-info.dtsi`]

| Region | Address | Known registers |
| --- | --- | --- |
| csis-link0–7 | 0x1a440000 + n × 0x10000 | INT_MSK0/SRC0 0x10/0x14, INT_MSK1/SRC1 0x18/0x1c, FS 0x20/0x24, FE 0x28/0x2c |
| csis-ebuf | 0x1a4c0000 | INT src/mask/clear 0xd0/0xd4/0xd8 |
| csis-dma | 0x1a4d0000 | 10 contexts of 0x1000 from +0x1000: ZSL0–2, STRP0–2, **CSIS_DMA0–3 at +0x7000..+0xa000**; per context: VC n at +n × 0x100, control block at +0x400 (INT enable/src +0x404/+0x408), mux at +0x500 |
| csis-phy | 0x1a4f0000 | 8 Samsung DC-PHYs "m0s4s4s4s4s4": 0x1300 and 0x1b00 are 4-lane D-PHY at 4.5 Gb/s or 3-trio C-PHY; the 2-lane units at 0x2300/0x2600/0x2b00/0x2e00/0x3300/0x3600 pair up into 4-lane banks |
| csis-sysreg | 0x1a420000 | PHY reset_n bits 0–7 at 0x500; WDMA routing: link slot 0x430 + slot × 4, DMA mux 0x408 + ctx × 4, enable 0x488 (bit link + slot × 8) [GS101] |

Interrupts:

| Source | GIC SPI |
| --- | --- |
| CSIS link n | 189 + n |
| CSIS DMA 0–3 | 197–200 |
| EBUF | 201–204 |
| STRP DMA | 217–219 |
| ZSL DMA | 234–236 |

The meaning of each bit is in `include/dt-bindings/lwis/platform/gs201/csi.h`.

The full CSIS link and DMA register map is in Samsung's open Exynos sources,
and its offsets match every GS201 offset above:
- Exynos 2100 CSIS v5.4:
  [`is-hw-csi-v5_4.h`](https://github.com/antiagainst/SM-G991B/blob/main/drivers/media/platform/exynos/camera/sensor/csi2/is-hw-csi-v5_4.h);
- Exynos 2200 CSIS v8.0:
  [`is-hw-csi-v8_0.h`](https://github.com/ExtremeXT/android_kernel_samsung_s5e9925/blob/lineage-23.2/drivers/media/platform/exynos/camera/csi/is-hw-csi-v8_0.h).

The Tesla FSD series (CSIS v4.3) was not merged as of v7.3-rc5 and has a
different DMA layout, so it only shows the overall structure.

### The Pixel 6 port is the template [GS101]

What the gs101 port contains:
- **`gs101-mipi-csis.c` + `-regs.h`** (2.3k + 3k lines): a V4L2 subdev and
  vb2-dma-contig capture node over CSIS link + WDMA, with the SYSREG routing
  above and a camera CMA pool.
- **`phy-exynos-mipi-gs101.c`**: D-PHY/C-PHY sequences replayed from the stock
  HAL, with PHY reset release through csis-sysreg.
- libcamera patches `0009` (registers `gs101-mipi-csis` with the simple
  pipeline handler, SoftISP on) and `0010` (IMX355/IMX386 helpers).

Their state on 09-22: the front IMX355 and the UW IMX386 give frames; GN1
streams but delivers no lines.

Lessons that carry over:
- The vendor PHY driver only releases PMU isolation. The HAL programs the
  DC-PHY from the normal world, so no secure hand-off is involved.
- Their "DC-PHY write freezes the SoC" turned out to be a stack overrun in the
  vendor `phy_configure` (`u32 info[4]` indexed by SETTLE = 4). Fix that before
  reusing it.
- The PHY must be out of reset (csis-sysreg 0x500 bit n = 1) before any PHY
  register access, or the access hangs.
- Use EBUF bypass, PIXEL_MODE quad and DBG_OPTION_SUITE bit 23.
- Use WDMA format 6 (10-bit in 16-bit words, exposed as SRGGB10). Format 4 is
  not CSI-2 packed.
- IMX355 sends one embedded line first, which shifts the Bayer phase.
- CSIS link and PHY wiring was found empirically, by streaming the sensor and
  checking each PHY bank. On the Pixel 6 the UW is on link1/dcphy1 and the
  front on link4/dcphy4. **Our wiring will differ.** Our UW is 4-lane.
- DMA ran on physical CMA addresses with no SysMMU or S2MPU programming. On
  GS101 at least, the CSIS IOMMUs pass traffic at hand-off.

### Power, isolation, IOMMU on GS201

- **Power domains.**
  - pd_csis (PMU 0x18062400, CMU_CSIS 0x1a400000, DTZPC 0x1a410204) holds the
    links, EBUF, WDMA, both CSIS SysMMUs and both CSIS S2MPUs.
  - `lwis_csi` itself names **pd_pdp** (0x18062480), and the CSIS↔PDP bridges
    span both domains, so both must be on.
  - `pixel-pd-off off=all` switches both off at boot and has no power-on path.
    For development, boot with an `off=` list that omits `csis,pdp` (the
    module accepts a comma list).
  - The real fix, as already decided in [pd](../kernel/pd/README.md), is a
    genpd provider like `gs201-g3d-pd`:
    1. flexpmu `csis_on`/`pdp_on`: CONFIGURATION bit 0 through the
       secure-register SMC 0x82000504, then poll STATUS;
    2. the **DTZPC restore SMC 0x82000410** (restore = 1);
    3. the `csis_save`/`pdp_save` CMU list
       [SRC `flexpmu_cal_local_gs201.h`, `pmucal_local.c`].
  - The gs101 port found that its own genpd attempt left the bank inaccessible
    when the SMC ran decoupled from the power-on, so keep them atomic.
- **DC-PHY isolation:** PMU offset 0x3ebc, bit 0 is the bypass. One bit covers
  all eight PHYs. Write it through the PMU SMC [SRC `phy-exynos-mipi.c`].
- **CMU_CSIS gates to check before any CSIS access:**

  | Gate or control | Offset |
  | --- | --- |
  | CSISX8 C2_CSIS | 0x2014 |
  | CSISX8 CSIS_DMA | 0x2018 |
  | CSISX8 EBUF | 0x201c |
  | PHY_LINK_WRAP CSIS0–7 | 0x206c–0x2088 |
  | QE_CSIS_DMA0–3 | 0x209c–0x20b8 |
  | SysMMU gates | 0x2108–0x2114 |
  | Q-channels | 0x3050–0x3064 |
  | user mux | 0x600 |

- **SysMMU v8** (CSIS0 0x1a510000, CSIS1 0x1a540000):
  - Google's driver treats CTRL bit 0 = 0 as disabled and sets BLOCK (bit 1)
    when it disables. Untouched, the SysMMU passes traffic untranslated [SRC
    `samsung-iommu.c`].
  - Read CTRL only. Require bits 0 and 1 to be clear.
  - No mainline driver supports v8, so bypass it with physical CMA addresses.
- **S2MPU** (0x1a520000, 0x1a550000):
  - On stock, pKVM owned them [BR 46501]. We do not run pKVM.
  - Their L1 attributes reset to PROT_NONE. CTRL0 bit 0 is the enable, and
    Google's pre-pKVM driver disables them by writing CTRL0 = 0 [SRC
    `kvm_s2mpu.h`, `s2mpu-lib.c`].
  - Read **CTRL0 only**; an AoC S2MPU VERSION read reset the SoC. If ENABLE
    is set, write 0.
  - Re-check after any pd_csis power cycle.
- **The ISP chain can be skipped.** CSIS_DMA0–3 write the selected VC/DT
  straight to memory. PDP/3AA/ITP are never programmed. Raw goes to libcamera
  SoftISP, which accepts unpacked 10-bit and CSI-2-packed Bayer.

## 4. Staged plan

Rules for every on-device step, from the bus-hang, deploy and reboot notes:
- arm `wdtkick` first;
- stream `dmesg -w` to the host;
- touch only registers defined in the sources quoted here, and never sweep;
- read a block's gate/power state before its first register;
- log after each write;
- read S2MPU CTRL0 and nothing else;
- stay clear of hsi2c_8 0x08 (NFC/eSIM).

The GS101 port's "probe every bank" technique relied on GS101 reads returning
0xdeadc0de. On GS201 such reads can hang, so limit it to documented status
registers (INT_SRC, PHY_STATUS 0x700).

### (a) Power the UW IMX386 and read its chip ID — 2–3 days

**Step 0: a read-only survey.**
1. USI1–4: SW_CONF, CMU_PERIC0 mux/div/gates, gpp2/4/6/8 CON/DAT/PUD.
2. CMU_TOP CIS_CLK0–3 mux/div/gate/Q-channel.
3. ACPM: S2MPG13 PM 0x37, GPIO bank 0x0C registers 0x04–0x08.

Do not read a controller at 0x109x0000 unless both of its gates read on.

**Step 1: the recipe.** Add a new `pixel-camera-power` module in the
`pixel-aoc-power` + `pixel-spi` style, with a `sensor=` parameter. It powers
down in reverse order on unload.

| # | Action | Value |
| --- | --- | --- |
| 1 | S2MPG13 GPIO1 (SLG bb) output high: ACPM ch 2, PMIC 1, bank 0x0C, register 0x06 | set bit 5, then bit 6; 2 ms |
| 2 | S2MPG13 GPIO3 (SLG buck): register 0x08 | same; 2 ms |
| 3 | S2MPG13 GPIO0 (SLG cs): register 0x05 | same; 10 ms |
| 4 | SLG51002 ID: i2c-8 0x75, 0x1105–0x1107 | expect **03 31 B1** |
| 5 | Voltages: LDO6 0x2900 ← 0x8C (1.1 V), LDO2 0x2200 ← 0xA5 (2.85 V) | only inside MINV/MAXV (0x2960/61, 0x2260/61) |
| 6 | CIS_CLK0: mux 0x1e081010 = 0 (OSCCLK), div 0x1e081814 = 0, gate 0x1e082038 manual + CG_VAL | **24.576 MHz**; poll BUSY (bit 16) |
| 7 | MCLK pad parked: gpp3-0 CON = 1, DAT = 0, PUD = 1 | stock `mclk1-out` |
| 8 | Reset low: gpp6-3 CON = 1, DAT bit 3 = 0 | |
| 9 | L12S on: PM bank 0x01 register 0x37 | require bits 5:0 = 0x2C, set bit 7 |
| 10 | SLG MATRIX_CONF_A 0x110d: bit 5 (LDO6), then bit 1 (LDO2) | 1 ms |
| 11 | MCLK pad on: gpp3-0 CON = 2, PUD = 0 | 1 ms |
| 12 | Reset high: gpp6-3 DAT bit 3 = 1 | 10 ms |
| 13 | hsi2c_3: SW_CONF 0x10821008 = 4; USI3 mux/div/gates copied from USI8; USI_CON (+0xc4) out of reset; timing registers copied from the live hsi2c_8; gpp6-0/1 function 3, pull-up | 400 kHz |
| 14 | `i2ctransfer -y 3 w2@0x1a 0x00 0x16 r2` | expect **0x03 0x86** |

For step 13, add a "configure from scratch" path to `pixel-hsi2c` (cloned from
`pixel-spi`), so buses 1–4 register as ordinary adapters. The other three
sensors then need only their own table rows:

| Sensor | Read | Expect |
| --- | --- | --- |
| Front | i2c-2 0x10, register 0x0000 | 0x30A1 |
| Main | i2c-1 0x3d | 0x08E1 |
| Tele | i2c-4 0x2d | 0x08D5 |

Risks:
- a controller access while its gate is off (a hang); step 0 guards against it;
- an unexpected SLG LDO voltage; refuse anything outside the chip's MINV/MAXV
  or away from the DT value;
- gpp6/gpp8 read-modify-write races with `pixel-torch`;
- losing L12S or the SLG rails while the laser AF or OIS also want them.
  Nothing else uses them today.

### (b) Stream the sensor and see CSIS frame-start counts — about 1 week

What it needs:
1. csis and pdp left on (the `off=` list).
2. Port the gs101 `phy-exynos-mipi-gs101.c` with the overrun fixed. On GS201
   the PMU register goes through the SMC, not a syscon.
3. PHY isolation bypass, then PHY reset release.
4. Port `gs101-mipi-csis.c`, with registers cross-checked against
   `is-hw-csi-v8_0.h`.
5. Play the IMX386 `init_PD` table, then mode `0x9167e0`, then 0x0100 = 1.
6. Watch FS_INT_SRC/FE_INT_SRC.

**Finding the link:**
- Stream the sensor, then read only INT_SRC0/FS_INT_SRC/PHY_STATUS on each
  4-lane bank (dcphy0, 1, 2, 4, 6) with its PHY configured.
- SOT errors (INT0 bits 16–23) mean the right bank with the wrong settings.
- The HAL's per-sensor 534-byte config structs (`liblyric_hwl.so`
  0xfb7148/7578/79a8/7bc0) probably encode the link and could spare the
  search, but are not yet decoded.
- Lane rate: ~1548 Mb/s/lane at 4032×3016 30 fps. Use a binned 2016×1508
  mode first if the PHY settle values need tuning.

### (c) First raw frame via CSIS_DMA — a few days after (b)

What it needs:
- the SysMMU CSIS0/1 CTRL read, which must show disabled;
- the S2MPU CSIS0/1 CTRL0 read, cleared if enabled;
- a camera CMA pool (the gs101 port needed 256 MiB for GN1);
- WDMA ctx0 set to format 6 and routed through csis-sysreg;
- a wait for the DMA FRAME_END (src +0x7408).

Dump the frame and view it with the OnePlus raw-preview tooling. Failure modes
are an IOMMU fault or an empty buffer, not a hang, once (b) is stable.

### (d) V4L2 and libcamera — 1–2 weeks for the UW

- **Sensor driver:** `imx386.c` from gs101 with our 4-lane tables and
  24.576 MHz. Runtime PM calls the (a) power code, through small providers
  (SLG51002 regulators, the S2MPG13 GPIO chip, a CIS_CLK clock) or one board
  module at first.
- **CSIS:** the gs101 media graph (sensor → csis → video node, MC-centric).
- **libcamera 0.7.x:** port gs101 patch 0009 (a simple-pipeline entry for our
  driver name) and 0010 (IMX386 helper: gain 1024/(1024−code), black level 64).
  Take the SoftISP tuning from the Pixel 6 `imx386.yaml`.
- **Autofocus:** the AK737x at 0x0f with the OnePlus `ak7375` patch.
- **Power:** replace the `off=` workaround with genpd for csis/pdp.

After the UW, expect about a week each for the front (3J1) and tele (GM5). The
main GN1 (C-PHY, firmware patch, remosaic) goes last.

## 5. Blockers

1. **CSIS link and PHY wiring** per sensor: undocumented. It must be found
   empirically, with status registers only, or by decoding the HAL config
   struct.
2. **Camera power domains:** off at boot with no on path. A proper genpd needs
   the DTZPC restore SMC timed right.
3. **IOMMU state** (SysMMU v8, S2MPU) on our boot and after a power cycle: not
   yet read on GS201. The GS101 evidence says bypass works.
4. **Camera I2C** (USI1–4) is probably unconfigured. It needs the USI-from-
   scratch path; the SLG51002 sits behind S2MPG13 GPIOs over ACPM.
5. **Table licensing:** the tables must stay out of git (generate them at build
   time). Inter-write delays and OTP handling are unknown.
