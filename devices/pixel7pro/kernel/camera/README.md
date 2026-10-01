# Camera power and first frames

`pixel-camera-power.c` powers the four cameras the way the stock DT's LWIS
sensor nodes do. It is milestone (a) of the
[camera plan](../../docs/camera-plan-20260930.md): power a sensor and read its
chip ID. The module touches no register inside the camera power domains
(pd_csis, pd_pdp).

`cam-id.py` reads a chip ID over i2c-dev. The camera buses come from
`pixel-hsi2c-cam` ([i2c](../i2c/README.md)).

Milestones (b) and (c), a running CSIS link and a raw frame in memory, use
four more bring-up tools (see [First frame](#first-frame-ultrawide)):
`cam-stream.py`, `pixel-csis-iso.c`, `csis-probe.py` and
`pixel-csis-capture.c`, plus `raw2png.py` on the host.

The module replaces the first test module, `pixel-cam-pmic`. That module only
raised the SLG51002's enable lines.

## Hardware

| | Ultrawide | Front | Main | Tele |
| --- | --- | --- | --- | --- |
| sysfs name, stock node | `uw`, sensor-sandworm | `front`, sensor-dokkaebi | `main`, sensor-nagual | `tele`, sensor-kraken |
| Part, chip ID (register) | Sony IMX386, 0x0386 (0x0016) | Samsung 3J1, 0x30A1 (0x0000) | Samsung GN1, 0x08E1 (0x0000) | Samsung GM5, 0x08D5 (0x0000) |
| Bus, address | hsi2c_3, 0x1a | hsi2c_2, 0x10 | hsi2c_1, 0x3d | hsi2c_4, 0x2d |
| Bus pins (SCL/SDA) | gpp6-0/1 | gpp4-0/1 | gpp2-0/1 | gpp8-0/1 |
| Reset (high = run) | gpp6-3 | gpp4-2 | gpp2-2 | gpp6-2 |
| MCLK clock, pad | CIS_CLK0, gpp3-0 | CIS_CLK1, gpp5-0 | CIS_CLK3, gpp9-0 | CIS_CLK2, gpp7-0 |
| Supplies | L12S, SLG LDO6 1.1 V, LDO2 2.85 V | L12S, LDO8 1.1 V, SLG GPIO4 | L12S, LDO4 2.9 V, GPIO3, LDO8 1.1 V, LDO1 2.85 V | L12S, LDO7 1.0 V, LDO3 2.25 V |
| Default | enabled | `experimental=1` | `experimental=1` | `experimental=1` |

The supplies:
- **L12S** is S2MPG13 LDO12S (`L12S_CAMIO`, 1.8 V), the camera IO rail. All
  four sensors share it. The module reaches it through ACPM (PMIC channel 2,
  S2MPG13), as [aoc-power](../aoc-power/README.md) does.
- **The SLG51002** camera PMIC, revision AB (ID 0xB13103), sits at 0x75 on
  hsi2c_8. It provides LDO1-8 and GPIO1-4. LDO8 is shared by the main and
  front sensors. Three S2MPG13 GPIOs hold the chip in reset: GPIO1 (bb),
  GPIO3 (buck) and GPIO0 (cs).

## Power sequences (stock DT)

Each delay runs after its step.

| Camera | Power-up | Power-down |
| --- | --- | --- |
| Ultrawide | L12S → LDO6 → LDO2, 1 ms → MCLK on, 1 ms → reset high, 10 ms | reset low, 1 ms → MCLK off, 1 ms → LDO2 → LDO6 → L12S, 1 ms |
| Front | L12S → LDO8 → GPIO4, 1 ms → MCLK on, 1 ms → reset high, 8 ms | reset low → MCLK off, 1 ms → GPIO4 → LDO8 → L12S, 1 ms |
| Main | L12S → LDO4, 1 ms → GPIO3 → LDO8 → LDO1, 2 ms → reset high, 1 ms → MCLK on, 9 ms | MCLK off, 1 ms → reset low, 1 ms → LDO1 → LDO8 → GPIO3, 6 ms → LDO4 → L12S |
| Tele | L12S, 0.3 ms → LDO7 → LDO3 → MCLK on, 1 ms → reset high, 10 ms | reset low → MCLK off → LDO7, 1 ms → L12S → LDO3 |

Around each sequence the module follows LWIS (`lwis_dev_power_up_locked`,
`lwis_i2c_device_enable`):

| When | What |
| --- | --- |
| Before power-up | SLG51002 out of reset if it is the first sensor, then the CIS clock |
| After power-up | bus pins to `on_i2c`, then 2 ms |
| Before power-down | bus pins back |
| After power-down | CIS clock restored, then the SLG51002 back into reset if it was the last sensor |

## What the module does

| Part | Registers | Action |
| --- | --- | --- |
| L12S | S2MPG13 PM bank 0x01, CTRL 0x37 | Requires the bootloader's 1.8 V (bits 5:0 = 0x2C). Sets or clears bit 7, reads it back, then waits 128 µs (`S2MPG13_ENABLE_TIME_LDO`). If L12S is already on at load, the module leaves it alone. |
| SLG51002 enable lines | S2MPG13 GPIO bank 0x0C, GPIOn_SET 0x05 + n | Require output enable (bit 6) and a low value at load. Raised in Google's probe order: bb, 2 ms; buck, 2 ms; cs, 10 ms. Lowered in Google's remove order: cs, 10 ms; buck, 1 ms; bb, 1 ms. |
| SLG51002 check | PATN_ID 0x1105-0x1107, MATRIX_CONF_A 0x110D, GPIO1-4_CTRL 0x1710-0x1713, FAULT_LOG1 0x1115 | After each release from reset: ID must be 0xB13103 and every output off. |
| SLG51002 LDO voltage | LDOn_VSEL; MINV/MAXV at VSEL + 0x60/+0x61 | The stock DT voltage's selector, written only if it lies inside the chip's MINV-MAXV and differs from VSEL. Read back. |
| SLG51002 LDO enable | MATRIX_CONF_A bit n-1 | Read-modify-write, read back. |
| SLG51002 GPIO output | GPIOn_CTRL bit 0 | Read-modify-write, read back. Rev AB needs no software test mode. |
| SLG51002 status | LDOn_STATUS (VSEL + 0xC1) | Read after power-up. VOUT_OK (bit 1) is expected and ILIM (bit 0) is not; either one off is logged. |
| CIS_CLKn | CMU_TOP mux 0x1010, div 0x1814, gate 0x2038, DFTMUX Q-channel 0x3004 (each + 4n) | Mux 0 (OSCCLK), divider 0: 24.576 MHz, the EXTCLK every stock sensor table assumes. Mux and divider are written only if they differ, and BUSY is polled. The gate is made to pass as Google's `ra_set_gate` does. The Q-channel gets ENABLE 0 and CLOCK_REQ 1, as stock's `GATE_DFTMUX_CMU_CIS_CLKn` enable leaves it. All four are restored at power-down. |
| Reset pin | gppN CON/DAT | Value, then output, as `gpiod_direction_output` does. |
| MCLK pad | gppN CON/DAT/PUD/DRV, pin 0 | On: function 2, no pull (`sensor-mclkN-fn`). Off: driven low, pull-down (`-out`). |
| Bus pins | gppN CON/PUD/DRV, pins 0/1 | Function 3 with pull-up (`hsi2cN-bus`) while the sensor is on. Otherwise as found at load. |

Every pin a sensor uses must be an input at load, as the bootloader leaves
it. Otherwise that sensor is marked unavailable. Its pins are restored as
found when it powers down. The camera bus adapter (`Pixel hsi2c_N`) must be
registered before a sensor powers up. The module holds that adapter while the
sensor is on.

Shared rails are reference counted: L12S, every SLG output, and the SLG51002
itself. A failed power-up undoes exactly the steps it took, in power-down
order. Unloading powers every sensor down.

## Interface

```
/sys/devices/platform/pixel-camera-power/uw/power     0 or 1
/sys/devices/platform/pixel-camera-power/front/power  (experimental=1)
/sys/devices/platform/pixel-camera-power/main/power   (experimental=1)
/sys/devices/platform/pixel-camera-power/tele/power   (experimental=1)
/sys/devices/platform/pixel-camera-power/status
```

`status` shows:
- SLG51002: in reset or on, with MATRIX_CONF_A and FAULT_LOG1;
- the L12S CTRL register and its users;
- each SLG rail's users, with VSEL/range/STATUS while the chip is on;
- each sensor's state, CIS_CLK registers and pin fields.

Parameters:
- `experimental=1` allows powering the front, main and tele sensors.
- `takeover=1` is for a warm reboot with a camera on. The S2MPG13 keeps its
  state across a warm reboot, so the SLG51002 lines stay high and the module
  refuses to load. With `takeover=1`, it lowers them and switches L12S off at
  load.

## Test: ultrawide chip ID

Verified on 2026-09-30: `pixel-hsi2c-cam buses=3` configured hsi2c_3 from
scratch, the ultrawide's stock power-up ran (SLG51002 out of reset, L12S,
SLG LDO6 1.1 V and LDO2 2.85 V, CIS_CLK0 24.576 MHz, reset high), and the
IMX386 answered register 0x0016 with `03 86`. Power-down returned the SLG51002
to reset, L12S to 0x2c and USI3 to unconfigured.

Prerequisites:
- `pixel-hsi2c buses=15,8` is loaded (boot);
- `i2c-dev` is loaded;
- `pixel-cam-pmic` is not loaded (`rmmod pixel_cam_pmic`; its unload lowers
  the lines);
- `wdtkick` is armed and `dmesg -w` is streamed to the host.

```
insmod pixel-hsi2c-cam.ko buses=3
insmod pixel-camera-power.ko
cat /sys/devices/platform/pixel-camera-power/status
echo 1 > /sys/devices/platform/pixel-camera-power/uw/power
python3 cam-id.py uw
cat /sys/devices/platform/pixel-camera-power/status
echo 0 > /sys/devices/platform/pixel-camera-power/uw/power
rmmod pixel_camera_power
rmmod pixel_hsi2c_cam
```

Expected results:

| Step | Expected |
| --- | --- |
| `pixel-hsi2c-cam` | `hsi2c_3 as found: USI 0 mux 0x10 div 0 gates 0x200000/0x200000 qch …`, then `hsi2c_3 configured: …` and `hsi2c_3 at 0x10920000` |
| `pixel-camera-power` | the three lines high, `SLG51002 up: MATRIX_CONF_A 0 …`, the lines low again (cs, buck, bb), then `… LDO12S off at load; sensors: uw` |
| `uw/power` = 1 | lines high, SLG up, `CIS_CLK0 at 24576000 Hz`, `S2MPG13 LDO12S on`, LDO6 and LDO2 VSEL set if needed (0x8C, 0xA5), then `on`, and finally `uw (Sony IMX386) on` |
| `cam-id.py uw` | `… 0x1a register 0x0016: 03 86` and `chip ID 0x0386: as expected` |
| `uw/power` = 0 | LDO2, LDO6 and LDO12S off, lines low, `uw off` |
| `status` afterwards | SLG51002 in reset, LDO12S CTRL 0x2c, every pin and CIS_CLK0 register as at load |

A read-only second check is the module EEPROM:
`cam-id.py --bus "Pixel hsi2c_3" --addr 0x50 --reg 0 --len 16`.

What a failure means:
- **`ENXIO`**: the sensor NACKed. The bus runs, but the sensor is not
  answering. Check `status`: STATUS VOUT_OK, the reset DAT and the MCLK CON.
- **`ETIMEDOUT`**: the bus pins or the controller are not driving the bus.

## First frame (ultrawide)

On 2026-09-30 the IMX386 delivered its first raw frames into memory on this
kernel: 2016×1508 RAW10 at 60 fps, CSIS link 2, WDMA context 0. They show
the room the phone faced.

### Procedure

The camera domains must stay on at boot. Create
`/var/lib/omarchy-mobile/camera-dev` and reboot: the boot script then leaves
`csis` and `pdp` out of the `pixel-pd-off` list. There is no genpd power-on
path yet. Then, after the chip ID test's power-up (`uw/power` = 1):

```
insmod pixel-csis-iso.ko                     # DC-PHY isolation bypass, CAM DVFS 400 MHz
python3 cam-stream.py uw init_PD_212.txt mode_0x919860_2016x1508_73.txt
python3 csis-probe.py setup 2 2 --rate 1481  # link 2, DC-PHY 2+3, 4 lanes
python3 csis-probe.py status 2               # want int0 0, int1 0x6, frm counting
mount -t debugfs none /sys/kernel/debug      # if not mounted
insmod pixel-csis-capture.ko
cat /sys/kernel/debug/pixel-csis-capture/status
cat /sys/kernel/debug/pixel-csis-capture/frame > /root/uw.raw
rmmod pixel_csis_capture
```

On the host, `raw2png.py uw.raw uw.png` gives a half-size RGB view (RGGB,
black level 64).

`cam-up.sh` and `cam-down.sh` script the same steps from `/root/tools`:
- `cam-up.sh` loads what is missing, powers the UW, plays the tables with
  8× gain at 30 fps, and sets up and checks link 2;
- `cam-down.sh` stops the link and the sensor and powers the UW down.

The sensor tables come from the user's own stock camera HAL and are never
committed (camera plan, section 2). The mode's own exposure is 1779 of 1799
lines (about 16 ms at 60 fps) at 1× analog gain. Indoors, that leaves about
50 DN above black. For a test, raise the analog gain (0x0204/0x0205, gain
1024 / (1024 − code), so 0x0380 = 8×). For more exposure, double the frame
length (0x0340/0x0341 = 0x0e0e, 30 fps) and the coarse integration time
(0x0202/0x0203 = 0x0dfa). Write these inside a group hold (0x0104).

### Autofocus

The UW has an AF actuator: an AKM AK737x at hsi2c_3 0x0f
(`act-slenderman-sandworm`). It runs on L12S + SLG51002 LDO4 (2.9 V), which
the UW power-up now switches on as well.

`cam-af.py POS` wakes it (mode register 0x02 = 0) and moves the lens to
POS, 0–4095, written as POS << 4 to registers 0x00/0x01 (the AK7375
protocol). Two quirks:
- asleep, it reads `00 ff ff ff`;
- the first write after waking NACKs unless it waits a few ms.

A contrast sweep found the focus. `pixel-csis-capture keep=1` left the DMA
running while the lens stepped. The score is the Laplacian variance of the
center of one green plane:

| Position | 0–800 | 1050 | 1250 | 1400 | 1550 | 2000 | 4000 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Score | 207–216 | 282 | 496 | **590** | 466 | 213 | 176 |

The scene was a desk about 0.5–1 m away. Below about 1000 the lens does not
seem to move off its stop.

### What each piece does

- **`cam-stream.py`** plays the register tables over i2c-dev, writes
  0x0100 = 1 and polls the IMX386 frame counter (0x0005). `--stop` writes
  0x0100 = 0. `--set REG=VAL …` writes single registers inside a group hold:
  after the tables, or on their own while streaming.
- **`pixel-csis-iso.c`**:
  - sets the shared DC-PHY isolation bypass, PMU 0x3ebc bit 0, through the
    PMU SMC;
  - votes the ACPM CAM DVFS clock to 400 MHz, as LWIS does when a camera
    opens.

  CAM boots at 67 MHz. At that rate the 4-lane link showed ECC, CRC and
  overflow errors and the resolution-mismatch bits. At 400 MHz the link runs
  clean. Unloading restores both.
- **`csis-probe.py`**:
  - releases the PHY resets in SYSREG_CSIS 0x500, before any PHY access (a
    PHY in reset hangs the bus);
  - replays the stock HAL's D-PHY sequence, as the Pixel 6 port recovered
    it;
  - sets up the link: 4 lanes, VC0 RAW10, quad pixel mode, 2016×1508.

  `status` reads only status registers. Our ultrawide sits on **link 2 and
  DC-PHY 2+3** (a 4-lane pair). It was found with the sensor streaming, by
  setting up one PHY bank at a time.
- **`pixel-csis-capture.c`**:
  - allocates a 32-bit DMA buffer and fills it with a pattern;
  - resets and enables the WDMA, bypasses the EBUF and sets the link's
    pixel align;
  - routes the link (below), programs context 0, channel 0 (format 6 = 10 bits
    in 16, slot 0 only) and waits for two frame ends;
  - reports how many words changed;
  - on unload, puts SYSREG_CSIS back and frees the buffer.

  `tpg=1` feeds the WDMA from its own test pattern generator. `keep=1` leaves
  the channel running, so the debug counters can be read live.

### GS201 differs from the Pixel 6 port here

The Pixel 6's CSIS recipe (EBUF bypass, IP_PROCESSING, frame pointer, slot 0
only, pixel align) carries over unchanged. The routing does not:

| SYSREG_CSIS | GS101 (Pixel 6 port) | GS201 (measured) |
| --- | --- | --- |
| 0x430 | link of WDMA slot 0 | link that feeds the WDMA; every context sees it. Without it, no frame start arrives |
| 0x408/0x40c/0x410 | WDMA context → slot mux | the HAL's `CSIS_SC_CON0..2`, "PDP MUX", 3 bits each. At their reset value 0 the WDMA receives frame and line syncs but **no pixel data**. Any non-zero value in all three lets it through |
| 0x488 | per-(link, slot) data enable | no effect |

With SC_CON at 0, the failure looked like a bus problem:
- frame start and end interrupts on time;
- `ACT_CTRL` active on slot 0;
- LASTDATA and LASTADDR errors (GS201 DMA INT bits 14 and 15);
- not one word of the pattern changed.

Three readings narrowed it down:
- the CSIS SysMMUs read disabled (CTRL 0x24) and the S2MPUs read
  CTRL0 = 0;
- the test pattern generator filled the whole buffer;
- the WDMA debug counters (DEBUG_EN 0x100; HDCNT at 0x154 = lines 31:16,
  data 15:0) showed lines arriving with a data count of 0.

The HAL's register table (`liblyric_hwl.so`, 48-byte {block, name, offset}
entries) names only SC_CON0..2 in SYSREG_CSIS. It also confirms that the
GS201 WDMA common block follows the Exynos 2100 layout
(`csis_cmn_dma_cfg_csis1` = 0x48).

Programming the EBUF instead of bypassing it, as the HAL does, moved a little
data but stalled before any frame start reached the WDMA. Bypass is the
working path.

## V4L2 driver (`pixel-camera.c`)

One media device, `pixel-camera`, with a sensor subdev, a capture node and,
for the ultrawide and main, an AK737x lens subdev per camera. Each camera
streams on its own (they share WDMA context 0). Starting a stream powers
`pd_csis` and `pd_pdp` on through `pixel-pd-off` unless they are already on
(kernel/pd), sets the DC-PHY isolation bypass, votes CAM (400 MHz, 533 for the C-PHY sensors) and INT
(400 MHz), powers the sensor through `pixel_camera_power_set()`, loads its
tables from `pixel-camera/<name>.bin` (`make-camera-firmware.py`), sets up
the PHY, link and WDMA and starts the sensor. Frames are MIPI RAW10 (WDMA
format 7 with pixel align), lines padded to 64 bytes. `cameras=` picks the
cameras (default all four).
libcamera's side is in `adapter/camera/libcamera/`.

At boot `pixel-boot.sh` loads the media modules, the dma-buf heaps,
`pixel-hsi2c-cam`, `pixel-camera-power` and this driver from
`/usr/local/lib/omarchy-mobile/camera/`, so Omarchy Camera works in every
boot; nothing is powered until an app streams. `pixel-reboot` unloads the
driver and the power module before restarting, which puts the camera PMIC
back in reset. The `camera-dev` flag now only keeps CSIS and PDP on from
boot, which the raw tools below need.

## The other three cameras

Measured 2026-09-30 with `experimental=1`, `pixel-hsi2c-cam buses=1,2,3,4`
and each sensor powered on its own.

| | Front 3J1 | Tele GM5 | Main GN1 |
| --- | --- | --- | --- |
| Chip ID | 0x30A1 ✓ | 0x08D5 ✓ | 0x08E1 ✓ |
| Tables | init 368 + mode 1920×1368 | soft reset + init `default` 4047 + mode 2016×1512 (279, emulated out of the HAL) | soft reset and clock enable + `init_a` 1565 + mode 2016×1136 |
| Streams (0x0005) | 60 fps | 60 fps | 120 fps |
| CSIS | **link 0, DC-PHY 0**, D-PHY 4 lanes, ~1665 Mb/s | **link 4, DC-PHY 4**, C-PHY | **link 1, DC-PHY 1**, C-PHY |
| Frames in memory | **yes** | **yes** (2026-10-01) | **yes** (2026-10-01) |

### Front

- Bayer order: GRBG (Intel's `s5k3j1.c`).
- The 3J1 sends 4224 words ahead of each image on VC0: PDAF-like samples,
  then embedded data (values 0–30).
  - The link passes them to the WDMA, which writes them back to back with
    the image.
  - So the image starts 2 rows and 384 pixels into the buffer, and the link
    reports an H-size mismatch.
  - `raw2png.py --offset 4224` skips them.
  - A driver has to move them to another VC or another WDMA channel, or turn
    them off at the sensor.
- The mode's own exposure is tiny. For a picture, set coarse integration
  0x0202 near the frame length and analog gain 0x0204 (0x0020 = 1×) with
  `--set`.
- The first frame was dark and blurred. The fixed-focus front camera was
  facing something close.

### Main: working (2026-10-01)

Three things stood between the GN1's clean link and whole frames:

- **The CAM clock.** At the CAM DVFS vote of 400 MHz the link's image FIFO
  overflows (ERR_OVER) and the WDMA stops at 65,536 16-bit words (128 KB,
  whatever the format). Halving the sensor's output rate (0x0312 = 2, the
  OP post-scaler, with 0x0342 doubled) let a whole 2016×1136 frame through
  at 400 MHz; at full rate (1596 Msps, 120 fps) a CAM vote of 533 MHz or
  more does. `pixel-camera.c` votes 533 MHz for main and tele.
- **Channel parking.** The HAL parks channels 1–3 on VC 1–3 with DT 0x3f.
  The GN1 sends a second stream on VC1, and with a channel on VC1 the WDMA
  gets only the low 8 bits of each pixel (2016 bytes a line in format 7).
  Parked on VC 15, as `csis-probe.py --park-vc 15` does, format 7 writes
  MIPI RAW10.
- **The bus clock.** With libcamera's GPU debayer starting next to the
  120 fps stream, the WDMA missed a frame end in 8 of 10 starts at the
  default INT DVFS of 200 MHz: OVERLAP (context INT_SRC bit 20), then no
  more frames. INT at 400 MHz: none in 10 (310 MHz: 5 in 10).
  `pixel-camera.c` votes INT 400 MHz while any camera streams.

The AK737x at i2c-1 0x0c focuses it, as the UW's does.

### Tele: working (2026-10-01)

The GM5's tables leave it at two trios (0x0114 reads 0x0101 after them)
with a 24 MHz EXTCLK; scaled from the GN1's PLL that is about 1060 Msps.
On link 4 / PHY 4 it decoded frame starts and nothing else:
- three trios: malformed-CRC errors, no lines;
- two or one trio: V-size mismatch, lost frame ends, no long packets, with any
  ENABLE_DAT lane mask, any settle count up to 13, either init table and at
  1060 or 1800 Msps; D-PHY mode gave only SoT sync errors, and no other link
  or PHY saw it.

The GM5 sends C-PHY LRTE packet delimiters. With LRTE_CONFIG (0x600) EPD_EN
(bit 31) set and the spacer fields at their reset value, frame ends arrive
and the WDMA writes whole 2016×1512 frames; `pixel-camera.c` sets it for
this camera only (`epd`). It streams 60 fps. Its AF (SEM1215SA, i2c-4 0x34)
is not driven yet, so close subjects are out of focus.

### History: C-PHY before the fixes above

Both Samsung sensors write 0x0118 = 0x0104, against 0x0102 on the D-PHY
3J1, and they decode only in `csis-probe.py --cphy` mode. The HAL writes
0x6028 = 0x4000, 0x6010 = 1 and 0x6226 = 1 (clock enable) in code, ahead of
the tables (Pixel 6 `s5kgn1.c`). `samsung_reset_clk.txt` does the same from
a table.

With the Pixel 6 port's C-PHY receive values:

| Trios (sensor 0x0114 and link) | What the link and WDMA see |
| --- | --- |
| 3 | FS/FE decode. CRC and malformed-CRC errors on every frame, plus invalid HS codes on lane 2. No lines reach the WDMA |
| 2 or 1 | FS/FE decode with no errors. VRESOL mismatch and lost FS/FE flags. Still no lines |

C-PHY repeats the packet header on every trio and stripes the payload across
them. Here short packets work and long ones never land. That points at the
receive side, not the sensors:
- GS201's DC-PHY C-PHY settings, which the GS201 HAL writes from code and
  GS201's vendor `phy-exynos-mipi.c` leaves as TODO;
- or a trio order or polarity setting.

Ruled out since then (2026-09-30, late):
- **The PHY values.** GS201's HAL programs C-PHY in `0xb05580`, called from
  the context start at `0xb046b0` with the PHY's `m1_dphy_sNc_gnr_con0`
  offset. Its values are the Pixel 6 port's:
  - bias: CON4 = 0x40;
  - per trio at +0x100:
    - 1, 0x1450, 9, 0x82b8, 1, 0x8600, 0x4000, 0x200, 0x638, 0x40;
    - +0x30: settle 7 above 999 Msps (else 9), mask 0xff; bit 8 below
      500 Msps;
    - then 0x32, 0x1503, 0x32.
- **ENABLE_DAT.** Pablo's `csi_hw_s_lane` sets 0xf for three C-PHY trios
  and 0x3 for one or two. `csis-probe.py` now does too. CRC errors remain.
  So do they with all four PHY data blocks programmed (`--blocks 4`).
- **D-PHY.** With the GN1 streaming, a D-PHY receiver on PHY 1 sees nothing
  at 3 or 4 lanes.
- **CCS signalling mode.** The GN1 reads 0x0110 = 0x1002, which is
  "D-PHY" if read as CCS. Writing 0x0003 silenced the link completely, so
  the register does not mean that on this part.

The same context start confirms the SYSREG routing measured on the UW:
- 0x430 + slot × 4 = link;
- `CSIS_SC_CON[n]` = slot;
- 0x488 bit link + n × 8.

Progress later that night:
- **INTERLEAVE_MODE.** GS201's link start (`0xb06xxx`) writes CMN_CTRL with
  INTERLEAVE_MODE (bits 11:10) = 3, DESKEW_LEVEL 2 and PHY_SEL = C-PHY,
  PHY_CMN_CTRL = 0x1f, and parks channels 1–3 on VC 1–3 with DT 0x3f.
  `csis-probe.py --interleave 3` does the same.
- With it, the GN1's link is clean apart from two flags:
  - the CRC errors and the V-size mismatch are gone;
  - frame starts and ends arrive on channels 0 and 1 (the GN1 sends a
    second stream on VC1);
  - RAW10 (`--dt 0x2b`) counts the right number of lines, and any other
    data type gives a V-size mismatch;
  - ERR_WRONG_CFG remains with every data type;
  - ERR_OVER remains.
- **The WDMA.** It then writes exactly 65,536 words per frame and stops, with
  LASTDATA/LASTADDR errors. The samples are 10-bit values shifted left by 6:
  black about 65 after `>> 6`.
  - The cut-off does not change with the WDMA format (6, 5, 4, 0x1e), the
    pixel mode (dual or quad; single is worse), the SC_CON value (1–7) or
    the frame rate (120 or 40 fps).
  - The link overflows whether or not the WDMA is routed.

Still open:
- the link-side C-PHY setup the HAL writes through its field setters
  (CMN_CTRL and the `lrte_config` it lists; the link reads 0x7fff7fff, EPD
  off);
- sensor-side C-PHY timing that the HAL may write in code.

The GM5 modes were not exported before. `ext_kraken.py` (session scratch)
emulates the HAL function at 0x905a88. It yields:
- 4032×3024 (1530 writes);
- 4032×2272, 2016×1136 and 2016×1512 (279 writes each).

All four use an OP PLL of 0x030e = 3 and 0x0310 = 220 or 249.

## Safety

- The module touches only registers named above: CMU_TOP, PERIC0 pins, the
  S2MPG13 through ACPM, and the SLG51002 at 0x75 on hsi2c_8. All of them are
  in always-on or PERIC0 blocks. None is in a camera power domain.
- `cam-id.py` refuses address 0x08 on every bus. `pixel-hsi2c` refuses it on
  hsi2c_8, where it is the ST54J NFC controller and the eSIM.
- The camera buses refuse writes to the module EEPROMs (0x51 on hsi2c_2, 0x50
  on hsi2c_3), except the address bytes ahead of a read.
- **gpp8** holds the tele bus pins and the torch's HWEN (gpp8-2).
  `pixel-torch` has its own lock, so a tele power toggle racing a torch
  toggle can lose one of the two CON updates. The result is the torch off or
  the tele bus dead, not a hang. This is one reason the tele is experimental.
- L12S also feeds the laser AF (`stmvl53l1`) and the OIS/AF devices. Nothing
  else drives them on this kernel.
- Power sensors down before suspending. The module has no suspend handling.
- The streaming tools need pd_csis and pd_pdp on, which only the camera-dev
  boot flag gives. `csis-probe.py` releases a PHY's reset before touching
  it. Its `status` and `pixel-csis-capture` read only documented status and
  debug registers. Undefined offsets in the CSIS DMA block read 0xdeadc0de
  rather than hanging, but no tool sweeps them.
- Registers and sequences come from:
  - Google's gs201 `slg51002-core.c`, `slg51002-regulator.c`, `slg51002.h`;
  - `s2mpg13-regulator.c`, `s2mpg13-register.h`, `s2mpg1x-gpio-gs201.c`;
  - `cmucal-sfr.c`, `ra.c`, `drivers/clk/samsung/composite.c`,
    `pinctrl-gs201.c`;
  - LWIS `lwis_device.c`, `lwis_device_i2c.c`, `lwis_gpio.c`;
  - the stock DT's sensor, `slg51002@75` and pin state nodes.

## Build

```
make -C <kernel> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=<copy of this directory> modules
```
