# Pixel 7 Pro display: cold init after a DPU/DISP power cycle (research, 2026-10-01)

Host-only research. No phone access, no flashing.

**Sources**
- Google's display modules,
  [kernel/google-modules/display @ 0137241](https://android.googlesource.com/kernel/google-modules/display/+/0137241105270acf53c329e899ea9c4b6b1c8e66/).
  Paths below are relative to its `samsung/` directory.
- LineageOS `android_kernel_google_gs201` @ `40ff934` (the tree `kernel/cpif`
  and the deep-sleep plan cite): `drivers/phy/samsung`,
  `drivers/soc/google/cal-if`, `drivers/soc/google/pm`, `arch/arm64/boot/dts/google`.
- Our driver: the display files of `kernel/deepsleep-v26/deepsleep-v26.patch`
  (`drivers/gpu/drm/sysfb/pixel_scanout.c`, `pixel_panel.h`,
  `pixel_bandwidth.h`, `pixel_handoff.c`). Line numbers refer to those files.

**Markers**
- **INF** means inferred and not verified on hardware or in source.
- "Stock" means the Google driver at the pinned commit.

## 0. Which of our files is live

`kernel.config` sets both `CONFIG_DRM_PIXEL_SCANOUT=y` and `CONFIG_DRM_PIXEL_HANDOFF=y`.
`pixel_handoff.c:398` returns early when SCANOUT is set, so the live driver is:
- `pixel_scanout.c`: native DMA scanout and DECON frame IRQs;
- `pixel_panel.h`: DCS reads and writes, the 60/120 Hz group, blank and sleep;
- `pixel_bandwidth.h`: the MIF floor.

`pixel_handoff.c` (the CPU-copy bridge) depends on a strict subset of the same ABL state.

---

## 1. What the bootloader (ABL) leaves configured, and what we depend on

### 1.1 Checked explicitly by our probe

| Block | Register (PA) | Required value | Where checked |
|---|---|---|---|
| PMU | DPU_STATUS `0x18062204` bit0, DISP_STATUS `0x18062284` bit0 | 1 (on) | scanout.c:529-531 |
| SysMMU DPU0 | `0x1c100034` VERSION / `+0x874` CAPA1 / `+0x0` CTRL / `+0x8000` VM0_CTRL / `+0x800c` VM0 FLPT | `0x80200001` / `0xe0a1` / 0 / 0 / 0 (translation off, bypass) | scanout.c:532-536 |
| DECON0 | INT_EN `0x1c240060` | `0x3000` (frame start+done enabled, global INT_EN bit0 off) | scanout.c:541 |
| DECON0 | GLOBAL_CON `0x1c240020` bit8 | 1 (command mode) | scanout.c:542 |
| DECON0 | TRIG_CON `0x1c240030` & 0x11 | `0x10` (HW trigger masked, HW_TRIG_EN off) | scanout.c:543 |
| DPP L0 DMA | BASEADDR_Y8 `0x1c0b0040` and shadow `+0x440` | `0xfac00000` | scanout.c:544 |
| DPP L0 DMA | IN_CTRL_0 shadow `+0x408` | `0xff400000` (alpha 0xff, IC_MAX 0x40, fmt 0 = ARGB8888) | scanout.c:545 |
| DPP L0 DMA | SRC_SIZE shadow `+0x410` | `0x0c3005a0` (3120×1440) | scanout.c:545 |
| DPP L0 DMA | SRC_OFFSET shadow `+0x414`, STRIDE_0 shadow `+0x450` | 0 | scanout.c:546 |
| DT | `/drmdecon@0x1C240000` | frame_start SPI 241, frame_done SPI 240 | scanout.c:548-560 |
| DSIM0 | VERSION `0x1c2c0000` | `0x02060000` | panel.h:345 |
| DSIM0 | CONFIG `+0x4c` | `0x04887cff` | panel.h:346 |
| DSIM0 | CMD_CONFIG `+0x80` | 0 (no packet-go pending) | panel.h:346 |
| DSIM0 | CMD_TE_CTRL1 `+0x88` / CMD_TE_CTRL0 `+0x84` / UNDERRUN_CTRL `+0x34` | `0x33eb3b26` / 86 / 7945 | panel.h:348-351 |
| DPHY | PLL_CON0 `0x1c2e0100` & 0x173f | `0x1202` (PLL_EN, S=2, P=2) | panel.h:341 |
| DPHY | PLL_CON1 `+4` & 0xffff | `0x1355` (K) | panel.h:342 |
| DPHY | PLL_CON2 `+8` & 0x3ff | `0xdb` (M) | panel.h:343 |
| Panel | DA/DB/DC | `0a 80 06` (PVT rev, see 2.6) | panel.h:353-361 |
| Panel | 0x54 WRCTRLD | `0x20` | panel.h:362 |
| Panel | B9 (16 bytes), BD (5 bytes) | HS, manual, changeable-TE baseline | panel.h:365-373 |
| ACPM | MIF and INT rates | 421 or 2028 MHz, INT 200 MHz | bandwidth.h:84 |

`CONFIG_DSIM0` decodes as:
- lanes_en `0x1f`, 4 data lanes, EOTP on;
- pixel format `0x3e` (RGB24), DSC on (bit19), command mode (bit18 = 0);
- plus BURST_MODE (bit26) and HSE_DISABLE (bit23). Stock sets these two only in video mode (`cal_9845/dsim_reg.c:1621-1629`). In command mode they are harmless (**INF**).

### 1.2 Used without a check (all lost in a power cycle)

**CMU_DPU** `0x1c000000` and **CMU_DISP** `0x1c200000`
- User muxes `PLL_CON0_MUX_CLKCMU_{DPU,DISP}_NOC_USER` (+0x600).
- NOCP dividers (`DIV_CLK_DPU_NOCP` +0x1800, `DIV_CLK_DISP_NOCP` +0x1804).
- All QCH_CON (DPU +0x3014..0x306c; DISP +0x3008..0x3020).
- CONTROLLER_OPTION (+0x800).
- SYSREG_DPU and SYSREG_DISP DRCG_EN/MEMCLK (+0x104/+0x108).
- DPUF_DMA L0..L5/L12 DYNAMIC_GATING_EN and GLB_CGEN.

These are exactly the stock `dpu_save`/`disp_save` lists (`flexpmu_cal_local_gs201.h:670-735, 747-773`). They are already generated in `kernel/suspend/pixel-sleep-lists.h:1992-1993` (37 + 11 entries).

The DISP list contains `PMUCAL_WAIT` entries: BUSY bit16 of +0x1804 and +0x600 must read 0 before each restore. pixel-sleep's `disp_power_on()` writes the list without that wait (**INF**: harmless if the values are unchanged).

The NOC clock rates come from CMU_TOP (`CLK_CON_MUX_MUX_CLKCMU_DPU_NOC` 0x1054, `..._DISP_NOC` 0x104c, `CLK_CON_DIV_CLKCMU_DPU_NOC` 0x1850). ACPM DVFS sets them (DISP is ACPM domain 12, measured 400 MHz). They are always-on and not touched.

**DECON0**
Main `0x1c240000`, WIN `0x1c250000`, SUB (DSC/DSIMIF) `0x1c260000`, WINCON `0x1c270000`.
- Data path:
  - DATA_PATH_CON_0 is `0xB1` (DSCC | DSC1 | DSC0 | OUTIF_DSI0) plus enhance bits [14:12]. These are 1 = dither, 2 = DQE, 4 = RCD. ABL's value is **unknown**.
  - DSIMIF0_SEL is 0.
- SRAM enables.
- Blender BG size.
- OUTFIFO sizes.
- Urgent and DTA.
- Both DSC encoders with their PPS.
- Window 0 (position, blend, CHMAP = DPP0, WIN_EN).
- CLOCK_CON `0x1c245000` (auto clock gating).

All values are in §2.4.

**DPP L0**
DMA `0x1c0b0000`, DPP `0x1c0d0000`: IRQ masks, QoS LUTs, IMG/SRC sizes, format, IO_CON, deadlock counter, assigned MO. Values are in §2.5.

**DSIM0 and DPHY**
All link registers in §2.3:
- CLK_CTRL (word and escape clocks, prescaler);
- ESCMODE, TIMEOUT, THRESHOLD, RESOL, NUM_OF_TRANSFER;
- CPRS_CTRL, SLICE01, SFR_CTRL, INTMSK.

All DPHY analog and timing registers, bias block `0x1c2e0000`.

**SYSREG_DISP (`disp_ss`)**
- `0x1c221008` DISP_DPU_MIPI_PHY_CON: bit0 M_RESETN_M0 = 1 (DPHY out of reset), bit4 SEL_RESET_DPHY = 0.
- Stock writes it in `dsim_reg_init` (`cal_9845/dsim_reg.c:2057-2078, 2085-2106`).

**PMU (outside DISP)**
- `0x18063eb8` bit0 = 1 is the M4M4 DSIM PHY isolation bypass.
- DT `gs201-drm-dpu.dtsi:193-198` `isolation = <0x3eb8>`.
- `phy-exynos-mipi-dsim.c:63-79` writes it through `exynos_pmu_update`, using the set_priv_reg SMC aliases `|0xc000` (set) and `|0x8000` (clear).
- The PHY driver treats an ABL-set bypass as INIT_DONE (`:115-136`).

**S2MPU DPU0/1/2**
- `0x1c160000`, `0x1c170000`, `0x1c180000` (`gs201-s2mpu.dtsi:57-76`, all in pd_dpu).
- CTRL0 (+0) must be 0 (disabled) for DPU DMA to work. Never read or checked (**INF**: 0 at boot, like HSI and CSIS).

**SysMMU DPU1/2** `0x1c110000`, `0x1c120000` (L2-L5): not checked. L0 uses DPU0.

**Panel state left by ABL**
- Reset released, sleep out, display on, TE on (0x35).
- Compression on (`9D 01`) and the WQHD PPS.
- Full init voltages (VLIN1/VGH/VREG), 60 Hz HS manual, changeable TE.
- WRCTRLD 0x20, DBV.
- C3 0C (WQHD).
- CASET 0..1439 and PASET 0..3119.

**TE pin: gpp0-3**
- PERIC0 `0x10840000`, bank offset 0, CON bits [15:12].
- Must be function 0xF (`gs201-pinctrl.dtsi` `disp_te_pri_on`) to drive DECON's HW trigger from DDI0.
- Not checked; HW-triggered transfers work, so it is set (**INF**).
- PERIC0 keeps its CON across SYS_SLEEP; BT_REG_ON on the same block came back.

**Reset pin: gpa7-1**
- FAR_ALIVE `0x180E0000`, bank offset 0x20: CON `0x180e0020`, DAT `0x180e0024`.
- `gs201-cheetah-display-common.dtsi:9`, GPIO_ACTIVE_HIGH; physical low means reset.
- ABL leaves it high. It is an ALIVE pad, so it holds through SYS_SLEEP.

**Panel supplies** (stock DT, cheetah)

| Supply | Rail | Voltage | Notes |
|---|---|---|---|
| `vddi` | `s_bucka` = S2MPG13 BUCKA "SA" | 1.8 V IO | `regulator-always-on`, shared; cannot go off |
| `vddd` | `m_ldo28` = S2MPG12 L28M "L28M_DISP" | normal 1.125 V (`CHEETAH_3HC4_VDDD_NORMAL_MICROVOLT`), LP 1.05 V | ramp 1200 µV/µs, enable delay 125 µs (`gs201-cheetah-pmic.dtsi:21-27`) |
| `vci` | `m_ldo27` = S2MPG12 L27M "L27M_DISP_VCI" | 3.025 V (`gs201-cheetah-pmic.dtsi:15-18`) | |

- No `enable-gpios`. The `disp_*` fixed regulators in `gs201-drm-dpu.dtsi:481-535` are disabled.
- ELVDD/ELVSS come from the panel module's own AMOLED PMIC, fed from VSYS. That feed is the ODPM external channel `VSYS_PWR_DISPLAY` (S2MPG13 VSEN_C4, 10 mΩ, `gs201-pmic.dtsi:1328-1335`).
- No SoC-side control is visible, so the DDIC sequences them (**INF**).
- PMIC registers: L27M CTRL `0x46`, L28M CTRL `0x47`. Bits [7:6] are the enable/opmode field as `kernel/suspend/pixel-pmic-opmode.c:83-84` uses it. Bits [5:0] are vsel.
  - L27M: group 5, 1.8 V + 25 mV/step, so 3.025 V = `0x31`.
  - L28M: group 3, 0.725 V + 12.5 mV/step, so 1.125 V = `0x20`, 1.05 V = `0x1A`, 1.2 V = `0x26`.
  - Source: `s2mpg12-regulator.c:286-289`, `s2mpg12-register.h:552-565`.
- Which VDDD voltage ABL sets is **unknown**. Read L28M CTRL[5:0].

**DSIM PLL reference**: OSC 24.576 MHz (`gs101-display-timing.dtsi:13`). It is not a CMU clock.

---

## 2. The stock cold power-up sequence, in order

### 2.1 Overall order (stock DRM)

`exynos_atomic_commit_tail` (`exynos_drm_fb.c:742-852`) calls `drm_atomic_helper_commit_modeset_enables`, which runs:

1. **CRTC enable**: `decon_enable` (`exynos_drm_decon.c:1236-1328`).
   - `pm_runtime_get_sync` on DECON → genpd `pd_disp`, then `pd_dpu` (DPU's parent is DISP, `gs201-pm-domains.dtsi:81-102`).
   - `_decon_enable_locked` → `decon_reg_init` + `decon_enable_irqs` (`:969-973, :955-967`). It does not start the DECON.
2. **Bridge pre_enable**: `exynos_panel_bridge_pre_enable` → `drm_panel_prepare` → rails on (`panel-samsung-drv.c:3651-3664, 740-751`).
3. **Encoder enable**: `dsim_encoder_enable` → `_dsim_enable` (`exynos_drm_dsim.c:337-374, 278-335`).
   - `phy_power_on` (PMU isolation bypass), `dsim_reg_init`, `dsim_reg_start` (HS clock), then TE pinctrl "hw_te_on".
4. **Bridge enable**: `exynos_panel_bridge_enable` → `s6e3hc4_enable` (`panel-samsung-drv.c:3496-3554`, `panel-samsung-s6e3hc4.c:1024-1085`).
   - Reset, DSC/PPS, init commands, features, display on.
5. **Planes**:
   - `decon_update_plane` → window control + `dpp_update` → `__dpp_enable` (`dpp_reg_init`) + `dpp_reg_configure_params` (`exynos_drm_decon.c:610-700`, `exynos_drm_dpp.c:487-495, 960-990`).
   - `decon_atomic_flush` → `decon_reg_start` + HW-trigger unmask (`exynos_drm_decon.c:805-911`; `cal_9845/decon_reg.c:1905-1935`). The first frame goes out on the next TE.

Display-on (0x29) is sent before the first frame (step 4), which is the same order our `pixel_enable` uses.

### 2.2 genpd power-on (outside the display driver)

`pmucal_local_enable` (`cal-if/pmucal_local.c:17-81`):
1. `disp_on`: PMU `DISP_CONFIGURATION` `0x18062280` bit0 = 1 (SMC `0x82000504` set_priv_reg); wait `DISP_STATUS 0x18062284` bit0 = 1 (`flexpmu_cal_local_gs201.h:742-745`).
2. `exynos_pd_tz_restore(0x1C210204)`: SMC `0x82000410`, args (1, 0x1C210204, 2) (`pmucal_local.c:52-60`).
3. `pmucal_rae_restore_seq(disp_save)`: wait BUSY, then restore (`:63`).
4. The same for DPU: `DPU_CONFIGURATION 0x18062200` / `DPU_STATUS 0x18062204`, TZPC `0x1C010204`, then `dpu_save` (`:665-735`).
5. **Not in stock genpd but needed here**:
   - S2MPU_DPU0/1/2 CTRL0 (`0x1c160000`, `0x1c170000`, `0x1c180000` +0) back to 0. Stock's pKVM does this. Our CSIS/HSI precedent (`kernel/pd/pixel-pd-off.c:24-30, 209-216`) shows S2MPUs return enabled (CTRL0 = 1) after a power cycle and silently drop DMA (**INF** for DPU, by analogy).
   - Check SysMMU DPU0 CTRL/VM0_CTRL == 0 with the probe's known-safe reads.

Power-off is the reverse (`pmucal_local.c:93-160`):
- save list, `exynos_pd_tz_save` (SMC arg 0);
- `dpu_off`: CMU_DPU CONTROLLER_OPTION `0x1c000800` bit24 = 0, `DPU_CONFIGURATION` bit0 = 0, wait STATUS = 0;
- then `disp_off`: CMU_DISP `0x1c200800` bit24 = 0, `0x18062280` = 0, wait.

This is exactly pixel-sleep `disp_power_off/on` (`kernel/suspend/pixel-sleep.c:591-659`), which already works (`docs/suspend-20260930.md` "display domains").

### 2.3 DSIM + DPHY programming (`dsim_reg_init`, `cal_9845/dsim_reg.c:2080-2120`)

**Mode parameters**
- `1440x3120` pmsk = P 2, M 0xDB, S 2, K 0x1355; hs 1346 Mbps/lane; esc 20 MHz (`gs101-display-timing.dtsi:57-64`).
  - The DSIM picks this by prefix match of mode name `1440x3120x60/120` (`exynos_drm_dsim.c:552-581`).
  - fvco = (219 + 0x1355/65536) × 12.288 MHz × 2 = 5384 MHz, divided by 2^S = 1346.
- 4 lanes (`samsung_s6e3hc4.data_lane_cnt`).
- Command mode, non-continuous clock (`MIPI_DSI_CLOCK_NON_CONTINUOUS`, no `MODE_VIDEO`).
- DSC: 2 encoders, 2 slices, slice 720×52 (`panel-samsung-s6e3hc4.c:1335-1385`).
- Underrun parameters: `te_idle_us` 350, `te_var` 1 (`:1318-1321`).
- Derived: word clock = 1346/16 = 84 MHz; escape divider = 5, giving an escape clock of 16 MHz (`dsim_reg.c:1752-1767`).

**DPHY timing**
Table row `{1350, 54, 22, 5, 72, 53, 9, 65, 40, 8}` (`dsim_reg.c:260`): clk_prepare 54, clk_zero 22, clk_post 5, clk_trail 72, hs_prepare 53, hs_zero 9, hs_trail 65, lpx 40, hs_exit 8.
- hs < 1500, so HSTX_CLK_SEL = 1 and no skew calibration.
- `b_dphyctl[16-7]` = `0x190`.

All are writes to registers of powered DISP blocks. Base addresses: DSIM `0x1c2c0000`, DPHY `0x1c2e0100`, BIAS `0x1c2e0000`, SYSREG `0x1c221000`.

| # | Register | Value / action | Source |
|---|---|---|---|
| 1 | SYSREG `+0x008` DISP_DPU_MIPI_PHY_CON bit4 | 0 (DPHY reset from SYSREG) | dsim_reg.c:2057 |
| 2 | DSIM CLK_CTRL `+0x20` bit26 CLOCK_SEL | 0 (OSC) | :1283 |
| 3 | DSIM SWRST `+0x04` bit0 | 1, poll until 0 (≤2 ms) | :627-638 |
| 4 | DSIM CONFIG `+0x4c` [4:0] | `0x1f` (lanes) | :658 |
| 5 | CLK_CTRL [12:8] LANE_ESCCLK_EN | `0x1f` | :819 |
| 6 | CLK_CTRL bit17 WORDCLK_EN | 1 | :1297 |
| 7 | SYSREG `+0x008` bit0 M_RESETN_M0 | 0 (assert DPHY reset) | :2069 |
| 8 | (skip if PLL_STAT0 `+0x40` bit0 already locked: stock returns -EBUSY) | | :1731-1734 |
| 9 | BIAS_CON0..4 (`0x1c2e0000`..`+0x10`) | `0x10, 0x110, 0x3223, 0, 0` | :94-100, 539 |
| 10 | PLL_CON0..8 (`0x1c2e0100`..`+0x20`) | `0,0,0,0,0,0x500,0x8E,0x1450,0xA28` | :102-112, 548 |
| 11 | CLK_CTRL bit16 ESCCLK_EN + [7:0] prescaler | 1, `5` | :810 |
| 12 | MC_TIME_CON0..4 (`+0x230..0x240`) | `0x1280, 0x1636, 0x0848, 0x5, 0x190` | :441-479 |
| 13 | MC_DESKEW_CON0 `+0x250` | 0 | :468 |
| 14 | MD_TIME_CON0..4(i) (`+0x330..0x340 + 0x100·i`, i = 0..3) | `0x1280, 0x0935, 0x0841, 0x30, 0x190` | :483-504 |
| 15 | MC_GNR_CON0/1 `+0x200/0x204` | `0, 0x1450` | :557 |
| 16 | MC_ANA_CON0/1 `+0x208/0x20c` (CON2 not written) | `0x7033, 0` | :566 |
| 17 | MD_GNR_CON0/1(i) `+0x300/0x304 + 0x100·i` | `0, 0x1450` | :576 |
| 18 | MD_ANA_CON0..3(i) `+0x308..0x314 + 0x100·i` | `0x7033, 0, 0, 0` | :587 |
| 19 | PLL_CON1 = K; PLL_CON0 [10:8] S, [5:0] P; PLL_CON2 [9:0] M | `0x1355`; S 2, P 2; `0xDB` | :422-439 |
| 20 | PLL_CON6 [11:8] WCLK_BUF_SFT | 3 | :413 |
| 21 | PLL_CON7 lock count | `(500+0)·P = 1000` | :1825-1826 |
| 22 | DSIM INTSRC `+0x50` bit31 (W1C), PLL_CON0 bit12 PLL_EN = 1, read back, poll PLL_STAT0 `+0x40` bit0 = 1 (≤2 ms) | | :774-801. Stock calls `dbg_snapshot_emergency_reboot` if PLL_EN does not read back; we should just fail. |
| 23 | MC_GNR_CON0 and MD_GNR_CON0(0..3) | `0xffffffff` (full write); poll bit1 PHY_READY on each (≤2 ms) | :698-740 |
| 24 | SYSREG `+0x008` bit0 | 1 (release DPHY reset) | :2105 |
| 25 | CLK_CTRL bit26 | 1 (word clock) | :2108 |
| 26 | `dsim_reg_set_config` (:1547-1655): SFR_CTRL `+0x64` bit1 = 1; CLK_CTRL bit25 NONCONT = 1; DESKEW_CTRL `+0x24` bit15 = 0; TIMEOUT `+0x28` = `0x00ffffff`; ESCMODE `+0x2c` bit7 CMD_LPDT = 0, [31:21] = 0xA; UNDERRUN `+0x34`; THRESHOLD `+0x38` = 480; RESOL `+0x3c` = `0x0c3001e0`; NUM_OF_TRANSFER `+0x30` = 3120; CONFIG lanes-1 = 3, EOTP 1, format `0x3e`, VC 0, bit18 = 0, bit19 CPRS_EN = 1; SFR_CTRL bit0 SHADOW_EN = 1; CPRS_CTRL `+0x74` = `0x0a`; SLICE01 `+0x78` = `0x02d002d0`; CMD_CONFIG bit16 PKT_GO_EN = 0; CMD_TE_CTRL0 `+0x84` = 28; CMD_TE_CTRL1 `+0x88` | see the rate table below | |
| 27 | `dsim_reg_start`: CLK_CTRL bit20 TX_REQUEST_HSCLK = 1 (non-continuous, no wait); INTMSK `+0x54` unmask bits 30,29,28,24,23,19,18,15; INTSRC = `0xffffffff` | | :2122-2128, 1891 |

Per-rate values. Formulas are `dsim_calc_underrun` (`exynos_drm_dsim.c:2458-2501`) and `dsim_reg_get_cmd_timer` (`dsim_reg.c:1206-1211`).

| Rate | UNDERRUN `+0x34` | CMD_TE_CTRL1 `+0x88` |
|---|---|---|
| 60 Hz | **7945** (`0x1f09`); matches ABL exactly | stock formula gives `0x34073c3e`; ABL has `0x33eb3b26` (it computes with about 1343 MHz) |
| 120 Hz | **1004** | `0x1a031e1f`; already used by panel.h:305-307 |
| 30 Hz (AOD) | **21827** (`0x5543`) | `0x680f787d` |

CMD_TE_CTRL0: stock uses 28 (hactive·2/100 with DSC); ABL has 86 (the non-DSC formula).

**INF**: the stock-computed `0x34073c3e`/28 versus ABL's `0x33eb3b26`/86 are both tolerated. Restore ABL's snapshot values to stay on the validated path.

Optional ULPS exit (the hibernation path, `dsim_reg.c:2189-2198, 1959-2000`, DDI type 0 = SM DDI):
1. ESCMODE bits 3|1 (TX_ULPS_DATA|CLK) = 1.
2. Poll DPHY_STATUS `+0x1c` [7:4] = 0xF and bit9.
3. Set ESCMODE bits 2|0 (exit).
4. Poll until both clear.
5. Wait 100 µs.
6. Clear bits 3|1, then 2|0.

`CONFIG_EXYNOS_PLL_SLEEP` and `CONFIG_EXYNOS_EWR` are not in `cloudripper_gki.fragment`, so those paths are compiled out.

### 2.4 DECON0 programming (`decon_reg_init`, `cal_9845/decon_reg.c:1847-1903`)

`dsc.cfg` is NULL for this panel, so the encoder PPS is computed (`dsc_calc_pps_info` :873-1032). With DSCC on, each encoder sees a 720-wide picture with one 720×52 slice:
- `dual_slice_en` = 0 and `slice_mode_change` = 1 (:768-794);
- the computed PPS matches the panel's `WQHD_PPS_SETTING` byte for byte, except pic_width (720 per encoder versus 1440 at the panel).

| Register (offset in DECON main `0x1c240000` unless noted) | Value | Source |
|---|---|---|
| CLOCK_CON `+0x5000` | clear 0x11111 \| bit24 \| bit28 (no auto-CG) | :142-150, 1861 |
| SRAM_EN_OF_PRI_0 `+0x210` / PRI_1 `+0x214` / SEC_0 `+0x218` | `0x11111111` / `0x00011111` / `0x00001111` | :175-203, 1850-1858 |
| GLOBAL_CON `+0x20` bit8 OPERATION_MODE_CMD | 1 | :83-93 |
| BLD_BG_IMG_SIZE_PRI `+0x220` | `0x0c3005a0` | :1453-1462 |
| OF_URGENT_EN `+0x2c0` / RD_URGENT_0 `+0x2c4` / RD_URGENT_1 `+0x2c8` / WR_URGENT_0 `+0x2cc` | `0x1` / `0x08000400` / `0x10` / `0` | :1811-1831; DT `gs201-drm-dpu.dtsi:314-321` |
| OF_DTA_CONTROL `+0x2d0` / OF_DTA_THRESHOLD `+0x2d4` | `0x1` / `0x32000600` | DT :323-326 |
| TRIG_CON `+0x30` | stock init `0x11` (HW_TRIG_EN, SEL DDI0 = 0, MASK); ABL leaves `0x10`; per frame we unmask | :595-625 |
| OF_PIXEL_ORDER `+0x2a0` [6:4] | 0 (RGB; BGR only without DSC) | :1427-1435 |
| DATA_PATH_CON_0 `+0x200` [7:0] | `0xB1`; [14:12] enhance: DQE (2) / RCD (4) only if ABL used them (read it) | :351-424 |
| DSIMIF0_SEL `0x1c268000` | 0 (DSIM0) | :361 |
| DSIMIF1_SEL `0x1c269000` | 3 if it read < 2 | :335-344 |
| DSC0 `0x1c260000` and DSC1 `0x1c261000`: CONTROL_1 `+0x04` | `0x222` (byte swap, slice_mode_ch, flatness 2) | :845-871 |
| CONTROL_3 `+0x0c` | `0x30F0` (remainder 3, grpcntline 240) | |
| PPS00_03 `+0x20` / PPS04_07 `+0x24` / PPS08_11 `+0x28` / PPS12_15 `+0x2c` | `0x11000089` / `0x30800c30` / `0x02d00034` / `0x02d002d0` | :1048-1163 |
| PPS16_19 `+0x30` / PPS20_23 `+0x34` / PPS24_27 `+0x38` / PPS28_31 `+0x3c` / PPS32_35 `+0x40` | `0x02000268` / `0x002005c6` / `0x000a000c` / `0x01e20178` / `0x180010f0` | |
| PPS56_59 `+0x58` [15:0] / PPS76_79 `+0x6c` / PPS80_83 `+0x70` / PPS84_87 `+0x74` | `0x0102` / `0x1AB62AF6` / `0x2B342B74` / `0x3B746BF4` | :1153-1160 |
| PPS36..57 | not written; hardware reset defaults assumed equal to DSC 1.1 (**INF**) | |
| OF_SIZE_0 `+0x290` / OF_SIZE_1 `+0x294` / OF_SIZE_2 `+0x298` / OF_TH_TYPE `+0x29c` [2:0] | `0x0c3000f0` / `0xf0` / `0x003400f0` / `5` (1H) | :539-569, 205-240 |
| GLOBAL_CON bit0 DECON_EN_F | 0 (per-frame off until start) | :104 |
| INT_PEND `+0x70` W1C `0x303000`; INT_PEND_EXTRA `+0x74` bits 4,0 | | :1410-1425 |
| INT_EN `+0x60` | stock `0x303011`; ours `0x3001` | :2210-2234 |
| INT_EN_EXTRA `+0x64` | `0x11` | |
| GLOBAL_CON bit20 (10 bpc) = 0; DATA_PATH dither off | | :1985-2003 |
| WIN0 (`0x1c250000`): FUNC_CON_0 `+0x4`, FUNC_CON_1 `+0x8` (blend NONE, plane alpha 0xff, PD_FUNC 0xd) | snapshot ABL's values | :1700-1757 |
| WIN0: START_POSITION `+0xc` / END_POSITION `+0x10` / START_TIME_CON `+0x1c` | `0` / `0x0c2f059f` / `0` | :2020-2034 |
| WINCON0 `0x1c270000` | `0x1` (CHMAP 0 = DPP0, WIN_EN) | :1577-1586 |

**Per frame** (`decon_reg_start` :1905-1935). Already done by `pixel_transfer`:
- SHD_REG_UP_REQ `+0x50` |= `0x3f` (windows), then GLOBAL_CON |= 3, then SHD_REG_UP_REQ |= `0x80100000`;
- clear the frame-start pending bit, wait for RUN_STATUS bit4 (≤2 ms), unmask the trigger;
- after done, mask again (`:2109-2120`).

**DQE**: not required for a correct picture.
- Stock enables the DQE path by default (`exynos_drm_crtc.c:395`), but with all LUTs NULL. Gamma, CGC and ATC are off (`exynos_drm_dqe.c:561-591`), so it is a pass-through (**INF**).
- RCD (rounded-corner mask, `drmdpp8`) is a separate path bit.
- If ABL's DATA_PATH_CON_0 shows DQE or RCD on, a power cycle loses ABL's tables. Accept the change, or snapshot DQE0 `0x1c2a0000`. That block is large; avoid it unless the colours change.

### 2.5 DPP L0 (`dpp_reg_init` + `dpp_reg_configure_params`, `cal_9845/dpp_reg.c:804-845, 909-964`)

**DMA `0x1c0b0000`**
- RDMA_IRQ `+0x4`: unmask, bit0 IRQ_ENABLE.
- QOS_LUT_LOW/HIGH `+0x130/0x134` = `0x44444444`.
- DYNAMIC_GATING_EN `+0x140` = 0.
- IN_CTRL_0 `+0x8` = `0xff400000`.
- RDMA_ENABLE `+0x0` [31:24] ASSIGNED_MO = `0x40`.
- SRC_SIZE `+0x10` = `0x0c3005a0`, SRC_OFFSET `+0x14` = 0, IMG_SIZE `+0x18` = `0x0c3005a0`.
- BASEADDR_Y8 `+0x40` = framebuffer; BASEADDR_C8 `+0x44` = 0.
- DEADLOCK_CTRL `+0x100` = (rcv·51)<<1 | 1. Snapshot ABL's value.

**DPP `0x1c0d0000`**
- IRQ_MASK `+0x14` and IRQ_CON `+0x10` bit0.
- LC_CON `+0x20` LC_EN.
- IO_CON `+0x38` (format / alpha select / bpc).
- IMG_SIZE `+0x3c` = `0x0c3005a0`.

Stock re-inits the DPP after every hibernation: `_decon_reinit_locked` → `_dpp_disable`, then `__dpp_enable` on the next update.

### 2.6 Panel power-up (S6E3HC4, rev PVT)

ID `DB = 0x80` gives build code 0x80, so rev = 0x10 = `PANEL_REV_PVT` (`panel-samsung-s6e3hc4.c:1309-1316`, `panel-samsung-drv.c:354-391`). All `REV_LT(DVT1_1)` and `REV_LT(EVT1_1)` commands are skipped. `REV_GE(DVT1)` commands are sent.

**Rails on** (`_exynos_panel_set_power`, `panel-samsung-drv.c:469-520`):
1. VDDI on; wait 5-6 ms. It is always on here.
2. VDDD on (L28M 1.125 V; set by `exynos_panel_handoff` :593-606).
3. VCI on (L27M 3.025 V).

**Reset** (`exynos_panel_reset` :438-457): gpa7-1 high, 5 ms; low, 5 ms; high, 10 ms. Then `exynos_panel_init` (:396-436):
- read DA, DB, DC (extinfo);
- read the DDIC ID: F0 5A 5A, read `PANEL_SLSI_DDIC_ID_REG`, F0 A5 A5 (:312-331);
- `s6e3hc4_panel_init`: F0 5A 5A, B0 00 04 F6, F6 5A, F0 A5 A5 (:1548-1562).

**`s6e3hc4_enable`** (:1024-1085):
1. `9D 01` (compression on, DCS short write). PPS long packet (type 0x0A), 88 bytes `WQHD_PPS_SETTING` (:77-89; identical to `panel_pps` in `pixel_panel.h:29-41`).
2. Init command set (:959-1021), with PVT filtering:

```
11                      ; sleep out, then wait 10 ms
F0 5A 5A
BB 0B 04 04 41 3D 0C    ; disable black insertion
B0 00 58 94 / 94 0C 60 0C 60      ; delete toggle
B0 00 12 B1 / B1 08               ; VLIN1 7.9 V
B0 00 0E F4 / F4 18 18 18 18 18   ; VGH 7.4 V
B0 00 31 F4 / F4 00 00 00 00 00   ; VREG 4.5 V
F0 A5 A5                ; then wait 110 ms (≥120 ms after sleep out)
35                      ; TE on
F0 5A 5A
B0 00 04 F6 / F6 5A               ; AOD SAP
B0 02 F3 68 / 68 09 09 09 0A 0A 0A ; SP (GE DVT1)
B0 00 36 C5 / C5 11 10 50 05 4E 74 ; FFC 165 MHz
AB 83                             ; NS transition flicker
B0 00 3C B9 / B9 19 09 ; B0 00 40 B9 / B9 30 03 ; B0 00 05 F2 / F2 C8 C0  ; TSP sync
B0 00 10 BD / BD 00               ; frame insertion count
F7 0F
F0 A5 A5
2A 00 00 05 9F          ; CASET 0..1439
2B 00 00 0C 2F          ; PASET 0..3119
```

3. `F0 5A 5A, C3 0C, F0 A5 A5`. C3 0C is WQHD; FHD would be 0D.
4. `s6e3hc4_update_panel_feat(60, enforce)` (:287-503), one packet-go batch:
   - F0 5A 5A;
   - B0 00 04 B9 / B9 0C 42 00 1B ×3 (HS TE width);
   - B9 04 (changeable TE);
   - the TE2 block: B0 00 42 F2 / F2 0D; B0 00 01 B9 / B9 04; B0 00 1E B9 / B9 00 10 00 30 (:184-237);
   - B0 01 9B 92 / 92 21 and B0 02 F3 68 / 68 09 09 09 0A 0A 0A (IRC on);
   - F2 01 / 60 00 (HS);
   - BD 21 81 83 03 03; B0 00 1E BD / BD 00 00 00 02 00 06 00 16; BD 21; 60 01 (60 Hz manual);
   - F7 0F; F0 A5 A5.
   - Our `panel_refresh()` sends the subset from BD onward (panel.h:285-295).
5. `53 20` (WRCTRLD BCTRL), then `change_frequency` (the same group, not enforced).
6. Wait for vsync, then the display-on set (:779-793):
   ```
   F0 5A 5A; E4 00 2C 2C A2 00 00; B0 00 4F F4 / F4 70 (AMP);
   B0 00 31 F4 / F4 17 17 17 17 17 (VREG 6.8 V); E4 00 2C 2C 82 00 00; F0 A5 A5; 29
   ```
7. Brightness afterwards through the backlight: `51 hi lo`, 4..2047.

All commands are sent in HS (`ESCMODE.CMD_LPDT = 0`).

### 2.7 Power-down, and what stock skips in each "off" flavour

**Hibernation (self-refresh)**: the panel stays awake and keeps showing its GRAM image.
- Entered after 50 ms without commits (`exynos_drm_hibernation.c:32, 96-160, 324-372`) and on system suspend (`decon_suspend` → `exynos_hibernation_suspend`, `exynos_drm_decon.c:2376-2394`).
- DECON: wait for frame done, per-frame stop (TRIG mask, EN_F off, update request, wait RUN_STATUS = 0; `decon_reg_stop` :1941-1983, reset only after a timeout), DPP deinit, then `pm_runtime_put` (`exynos_drm_decon.c:1374-1399, 1138-1191`).
- DSIM: `_dsim_enter_ulps_locked` → `dsim_reg_stop_and_enter_ulps` (`dsim_reg.c:2200-2232`):
  1. clear and mask interrupts; HS clock off, poll STOPSTATE_CLK;
  2. ULPS enter, poll; link clock to OSC;
  3. PHY lanes disabled (GNR_CON0 = 0, poll not-ready); PLL off;
  4. SW reset.
- Then PHY power-off sets isolation (PMU `0x3eb8` bit0 = 0 when the share count reaches 0), and DPU/DISP genpd turn off (`exynos_drm_dsim.c:410-442`).
- **No panel commands.** Exit: `_dsim_exit_ulps_locked` (`:232-264`) = PHY on + full `dsim_reg_init` (PLL included) + ULPS exit. `decon_exit_hibernation` (`exynos_drm_decon.c:1193-1218`) = `decon_reg_init` + IRQs. The DPP is re-initialised by the next commit.
- **This is exactly the Stage 1 and Stage 3 mechanism.** Stock runs it every time the screen is idle and whenever the phone suspends with the screen or AOD showing.

**BLANK** (`blanked_mode` or `force_power_on`):
- `s6e3hc4_disable`: `disable_panel_feat(60)`, wait one frame, display-off set, 20 ms (`:1087-1120`).
  - The display-off set (:795-807) is `28; F0 5A 5A; E4 sync; B0 00 31 F4 / F4 00×5 (VREG 4.5 V); B0 00 4F F4 / F4 50 (AMP); E4 end; F0 A5 A5`.
- No sleep-in, rails stay on. DSIM is fully stopped (`_dsim_disable`, `dsim_reg_stop` :2130-2173), domains off.
- Re-enable: no reset and no init (`needs_reset` is false). DSC/PPS, C3, features and display-on are sent again.

**OFF** (normal Android screen-off): as BLANK, plus:
- sleep-in set (`10`, wait 10 ms, then NOP with a 100 ms delay; :133-142);
- `unprepare` → `_exynos_panel_set_power(false)`: reset low, VDDD off, VDDI off (refcount only), VCI off (:520-563);
- TE pin to function 0 (`hw_te_off`).
- Re-enable is the full §2.6.

**Ours today**:
- display off: 28, 20 ms, then sleep-in 10 and 120 ms with `panel_sleep`;
- wake: 9D, PPS, 11, 120 ms, 35, 29.
- That is OFF without the rails, and BLANK with sleep added. DSIM, DPHY and DECON stay powered and untouched.

---

## 3. S6E3HC4 AOD / LP mode

**Entry**: `s6e3hc4_panel_set_lp_mode` (`panel-samsung-s6e3hc4.c:865-921`), from an active panel:
1. `disable_panel_feat(vrefresh)`: manual, HS, changeable TE.
2. Wait for vsync (changeable-TE variant below 120 Hz). Display-off set. Wait for vsync.
3. `53 24` (`aod_on`).
4. Display-on set (AMP 0x70, VREG 6.8 V, `29`).
5. One packet-go batch:
   ```
   F0 5A 5A
   B0 00 52 94 / 94 01 07 98 02      ; AOD low mode
   B9 51                             ; fixed TE, sync on
   B0 00 01 60 / 60 00               ; 30 Hz
   B0 00 18 BD / BD 04 00 06 00 00 00 ; 10 Hz idle
   BD 25                             ; auto mode
   F7 0F
   F0 A5 A5
   ```
   (The rev < DVT1_1 AOD-timing lines are skipped on PVT.)
6. VDDD drops to 1.05 V (`panel-samsung-drv.c:4136`, `_exynos_panel_set_vddd_voltage` :459-467).

**Brightness in LP** is binned (`:144-167`, `panel-samsung-drv.c:815-859`), selected by the requested DBV:
- "low" (≤ 80): `F0 5A 5A; B0 00 52 94; 94 01 07 98 02; F0 A5 A5; 51 00 04` (10 nit).
- "high" (≤ 2047): the same with `94 00` (50 nit).
- DBV 0 means pixel-off (`22`); a later `13` restores normal on (`:736-776`).

**Exit**: `s6e3hc4_set_nolp_mode` (`:923-957`):
1. Batch: `F0 5A 5A; BD 21; B9 04; B0 00 01 60 / 60 00; F7 0F; F0 A5 A5`.
2. Wait vsync at 30 Hz, then the display-off set.
3. `F0 5A 5A; B0 00 52 94 / 94 00; F0 A5 A5; 53 20`.
4. Wait vsync at 30 Hz.
5. Feature group (enforced) for the target rate, `53 20`, frequency, display-on set.
6. VDDD back to 1.125 V before this sequence (`panel-samsung-drv.c:4138`).

**Refresh in LP**:
- Mode `1440x3120x30`: clock 149310, same porches and DSC (`:1481-1510`).
- TE is fixed at 30 Hz, TE width 600 µs (`:812-823`).
- When no frames arrive, the panel's auto mode drops to a 10 Hz idle refresh from GRAM. `set_self_refresh` in LP only records `panel_idle_vrefresh = 10` (`:595-611`).

**What stays powered in AOD**
- Panel: VCI 3.025 V, VDDD 1.05 V, VDDI 1.8 V, and the panel's ELVDD/ELVSS (it is emitting).
- SoC: nothing display-related while idle. Hibernation applies to any command-mode CRTC, LP included (`exynos_drm_hibernation.c:44-48`). With no commits for 50 ms, and always at system suspend, DECON stops, DSIM enters ULPS, the PHY is isolated, and DPU/DISP power off.
- Android's DOZE_SUSPEND then lets the kernel suspend with the AOD image held in panel GRAM.
- Each clock update is: wake → hibernation exit (full DSIM PLL and DECON re-init) → one frame transfer at the 30 Hz TE → back to hibernation after 50 ms → suspend.
- **So stock does not keep DPU/DSIM powered during AOD.**

---

## 4. Register writes outside the DPU/DISP domains

| Target | What | How | Notes |
|---|---|---|---|
| PMU (ALIVE) `0x18062200` / `0x18062280` | DPU / DISP CONFIGURATION bit0 | SMC `0x82000504` set_priv_reg (PMUREG_WRITE) | Already done by pixel-sleep and pixel-pd. Poll STATUS +4 with plain reads (already done). |
| PMU `0x18063eb8` bit0 | DSIM M4M4 PHY isolation bypass (1) / enable (0) | SMC `0x82000504`; stock uses aliases `0x1806feb8` (set) / `0x1806beb8` (clear) with value = bit number 0 (`exynos-pmu-if.c` `exynos_pmu_update`) | Precedent: `kernel/camera/pixel-csis-iso.c` writes CSIS's `0x3ebc` this way. Not in any pmucal SYS_SLEEP list (grep shows no 0x3eb8 in `flexpmu_cal_system_gs201.h`). |
| Secure monitor | DTZPC save/restore for DPU `0x1C010204` and DISP `0x1C210204` | SMC `0x82000410` (0 = save, 1 = restore; group 2) | Accepted on the phone (pixel-sleep). |
| CMU_TOP, ACPM | DISP/DPU NOC clocks, MIF/INT floors | ACPM DVFS through CCF (`pixel_bandwidth.h`) | Do not write CMU_TOP. The DISP rate is unchanged. |
| GPIO FAR_ALIVE `0x180e0020/24` | gpa7-1 reset (Stage 2) | CON[7:4] = 1 (output), DAT bit1 | Add the line to `kernel/gpio/pixel-gpio.c`, which has the same CON/DAT/PUD layout. |
| GPIO PERIC0 `0x10840000` | gpp0-3 TE function 0xF | read-only check | Leave it alone. |
| PMIC S2MPG12 via ACPM (channel 2, bank 1) | L27M `0x46`, L28M `0x47`: enable field [7:6] and vsel [5:0] | `kernel/suspend/pixel-pmic-opmode.c` already reads and writes these | Stage 2 and 3 only. |

Inside DPU/DISP (powered, vendor-defined, so writable):
- CMU_DPU/DISP and SYSREG_DPU/DISP save lists;
- `0x1c221008` (SYSREG_DISP + 0x1008, the `disp_ss` node);
- DECON/WIN/SUB/WINCON, DSIM, DPHY, BIAS;
- DPP/DMA;
- S2MPU_DPU0..2 **CTRL0 only** (a bus-hang rule: never read anything else in an S2MPU).

Never touch SysMMU DPU registers beyond the five probe reads.

Every DPU/DISP access must be gated on PMU STATUS = 1; with the domain off, an access hangs the SoC. DISP must be on before DPU and off after it.

---

## 5. Staged implementation plan

Today's driver writes about 10 hardware registers: DECON trigger, update, IRQ, the DMA base, three DSIM timing registers, and the DCS FIFO. A cold SoC-side init writes about 150 registers:

| Group | Writes |
|---|---|
| DSIM | ~25 |
| DPHY | ~60 (5 bias, 9 PLL, ~10 clock lane, 4 × 11 data lane) |
| DECON | ~35 main + 2 DSIMIF + 2 × 16 DSC + 7 window |
| DPP | ~15 |
| Domains (PMU, SMC, S2MPU) | ~10 |

The panel cold init is about 45 DCS packets more. In code that is roughly **700–900 lines for Stage 1 (INF)**, about 2× today's `pixel_scanout.c` + `pixel_panel.h`. Most of it is a transcription of `dsim_reg_init` and `decon_reg_init`.

### Stage 0: read-only snapshot (about 150 lines, no risk)

Add a debugfs dump. While both domains read ON, read every register listed in §2.3–2.5 and compare it with the stock-computed values above. Also read:
- DATA_PATH_CON_0 (are DQE or RCD on?);
- TRIG_CON, WIN0 FUNC_CON_0/1, DSIMIF0/1_SEL, CLOCK_CON;
- PMU `0x18063eb8`;
- S2MPU_DPU0..2 CTRL0;
- SysMMU DPU1/2 CTRL;
- gpp0 CON;
- L28M CTRL (VDDD voltage).

Grep each offset in the vendor headers before adding it, following the "pixel-bus-hang" rule. Keep the snapshot: it is the restore source for anything ABL set differently from stock (TE timers, CONFIG extras, window blend, deadlock count).

### Stage 1: SoC-side re-init across a DPU/DISP power cycle, panel rails kept (recommended first)

Use the panel state we already have: `panel_sleep` puts the panel in DCS sleep at screen-off, and `panel_wake()` resends 9D/PPS/11/35 (panel.h:165-181). Stage 1 therefore needs **no panel reset, no rails and no init table**, only the SoC side.

**Sequence at screen-off**, after `panel_blank(true)`, with DECON idle:
1. `dsim_reg_stop_and_enter_ulps` (§2.7).
2. `decon_reg_stop` and DPP deinit.
3. PHY isolation on (`0x3eb8` bit0 = 0).
4. Read and save S2MPU_DPU0..2 CTRL0 and both CMU save lists.
5. DPU off, then DISP off (SMC save, CONTROLLER_OPTION bit24 = 0, CONFIGURATION = 0, poll).
6. Release the MIF floor.

**Sequence at screen-on**:
1. DISP on, SMC restore, `disp_save` restore with the BUSY waits.
2. DPU on, SMC restore, `dpu_save` restore.
3. S2MPU CTRL0 = saved value (0). Check SysMMU CTRL == 0.
4. Isolation bypass (`0x3eb8` bit0 = 1).
5. `dsim_reg_init` + start (§2.3), then the SM-DDI ULPS exit.
6. `decon_reg_init` (§2.4) and the DPP init (§2.5).
7. Re-arm the INT_EN we use (`0x3001`).
8. `panel_wake()`, then 29.
9. First frame through the existing `pixel_transfer`.
10. Reacquire the MIF floor for 120 Hz.

Reuse `pixel_pd_power()` from `kernel/pd/pixel-pd-off.c` (save list, context SMC, S2MPU CTRL0). Add `"disp"`/`"dpu"` entries and widen `s2mpu[2]` to 3. Do not let pixel-sleep's `disp_off` own these domains any more; the display driver must, or pixel-sleep must call into it.

**Risks**
- An access to DECON/DSIM while a domain is off or half-restored, or before the DTZPC restore, is a hard hang. Gate every block access on the PMU STATUS bits and the SMC return value.
- S2MPU or SSMT leaves DPU reads blocked: frame-done timeout or DMA deadlock IRQ. This is recoverable. DPU has no SSMT DT node; secure firmware owns it (**INF**).
- PLL never locks, or PHY_READY never asserts because isolation was left on. Recoverable with a timeout, but the screen stays black until reboot.
- The DDIC sees an undefined line state during isolation. Using ULPS before isolation (as stock does) avoids this. A wedged DDIC cannot be recovered without the reset pin, so wire gpa7-1 early (it is part of Stage 2) as a safety net.
- Window, blend or DQE values differ from ABL. Use the Stage 0 snapshot.

**Safe test ladder**: RAM boot only, 1800 s auto-reboot. Arm `wdtkick` (cluster-0 WDT `0x10060000`) and stream `dmesg -w` with an msleep after each stage.
- **T1, warm re-init, no power cycle.** At screen-off (panel asleep, domains on): full DSIM stop (PLL off), DECON stop + GLOBAL_CON SRESET, DPP SW reset, then the full init and wake. This tests every value without any power-domain risk. Worst case is a black screen with SSH still alive.
- **T2, power cycle while awake.** Screen dark, domains off for 10 s, then on, re-init and wake. Check that the PMU, S2MPU and SysMMU readbacks are logged.
- **T3, inside SYS_SLEEP.** Combine with `pixel-pmic-opmode suspend=all` (INT, CAM, PLL_MIPI_UFS). Expect about 6–7 mA less (79.0 → 71.9 mA measured with only `disp_off`).
- **T4, soak.** 200 cycles plus 120 Hz GPU animation. Check `dsi_underruns` = 0 and that frame-done matches frame-start.

### Stage 2: panel rails off in sleep (about 300 lines)

**Adds**
- gpa7-1 output control.
- L27M/L28M through ACPM.
- The full §2.6 cold panel init: extinfo and ID read, PPS, the init table, C3, features, 53 20, display-on, DBV restore. Re-run `panel_validate`-style checks on the new state.

**Power-down order** (stock OFF): 28, 20 ms, then 10 and 110 ms; reset low; VDDD off; VCI off.

Two options:
- **(a)** Explicit ACPM writes at screen-off and screen-on. The CTRL enable field is 0 for off, 3 for on.
- **(b)** Hold reset low, then set the L27M/L28M fields to 1 (follow PWREN). The rails then drop only inside SYS_SLEEP, and the DDIC stays in reset when PWREN restores them at each wake.
  - (b) needs no PMIC writes per wake.
  - It cycles the rails on every modem wake (**INF**: inrush and possible DDIC stress).
  - Prefer (a).

**Risks**
- A wrong power or reset order can latch the DDIC, or flash ELVSS on screen. Follow stock's 5/5/10 ms and the 120 ms sleep-out rule.
- Without `REV_LT` filtering, the init table carries EVT-only commands. Use the PVT list above.
- After a cold init the panel defaults (60 Hz HS manual) differ from what `panel_validate` expects. Make validation state-aware.

**Testing**: get Stage 1 stable first. Then T1 again with only the reset pulse and rails on (no rail-off). Then rail-off while awake. Then inside SYS_SLEEP.

**Benefit is uncertain and likely small.**
- With the panel in DCS sleep, ODPM VSYS_PWR_DISPLAY read 0.0 mW inside SYS_SLEEP (round 5, `docs/suspend-20260930.md`).
- What remains is DDIC leakage on L27M/L28M, plus the LDO quiescent current. Datasheet-class figures for this are tens of µA to about 1–2 mA (**INF**).
- **Measure L27M (ODPM mux `0x3B`) and L28M (`0x3C`) while awake with the panel asleep before building this.**

### Stage 3: AOD in LP with the SoC asleep (about 400–600 lines kernel, plus a userspace renderer)

**Needs**
- Stage 1 hibernation: domains off with the panel awake, so ULPS before isolation is mandatory.
- VDDD 1.05 V (L28M vsel `0x1A`) via ACPM.
- The LP entry, exit and binned commands (§3).
- DSIM 30 Hz timers: UNDERRUN 21827, CMD_TE_CTRL1 `0x680f787d`, frame timeout at 30 Hz.
- An RTC wake once a minute. On wake: power on, re-init without panel commands, ULPS exit, one frame on TE, ULPS, power off, sleep.
- Optionally DECON partial update later (`exynos_drm_partial.c`).

**Risks**
- Transfers must align to the fixed 30 Hz TE, and auto mode must stay off while sending. Stock uses B9 51 and lets the panel early-exit.
- Burn-in: stock relies on userspace shifting the AOD content.
- Holding the MIF floor too long.

**Testing**: enter and exit LP with the domains kept on first. Then add hibernation while awake. Then SYS_SLEEP with RTC wakes.

---

## 6. Panel and display current hints

**Measured by us**:
- **Panel display-off but awake:** 87–97 mW on VSYS_PWR_DISPLAY (`docs/power-20260928.md`).
- **Adding DCS sleep-in:** saves 0.32 W at the USB input (`docs/status.md:127`; this includes conversion losses).
- **Panel asleep in SYS_SLEEP:** the ODPM display shunt reads 0.0 mW (`docs/suspend-20260930.md` round 5).
- **Display domains off plus all PWREN rails (INT, CAM, PLL):** 79.0 → 71.9 mA with USB connected. Domains off alone gave 78.1 mA.

**Stock code**:
- No current figures.
- Indirect hints: VDDD is lowered to 1.05 V in LP, suggesting VDDD carries meaningful DDIC current in AOD. Stock also turns VCI and VDDD fully off in its normal screen-off (OFF state).

**Not measured**: L27M and L28M with the panel asleep. These are the only numbers that decide Stage 2.

---

## 7. Open items to verify (all read-only)

1. DATA_PATH_CON_0 [14:12]: whether ABL enabled DQE or RCD.
2. S2MPU_DPU0..2 CTRL0 at boot; the expected value is 0.
3. PMU `0x18063eb8`: expected 1. Does it auto-clear after a DISP power cycle? (**INF**)
4. SysMMU DPU0..2 CTRL after power-on; the expected value is 0 (bypass).
5. L28M vsel: 1.125 V or 1.2 V.
6. gpp0 CON[15:12]: expected 0xF.
7. ABL's WIN0 FUNC_CON values, TRIG_CON upper bits, DSIM INTMSK, and DEADLOCK_CTRL.

---

## 8. Stage 0 results on the phone (2026-10-01)

`disp-snapshot.py` (session scratch) is a read-only `/dev/mem` reader for
the registers in §2. It refuses unless the DPU and DISP domains read
powered. It was run with the screen on at 120 Hz.

**The DPHY lane blocks are 0x100 higher than §2.3 says.** Measured from the
DPHY base `0x1c2e0000`:

| Block | Offset | GNR_CON0/1 | ANA_CON0 | TIME_CON0..4 |
|---|---|---|---|---|
| Clock lane (MC) | `+0x300` | `3` / `0x1450` | `0x7033` | `0x1280 0x1636 0x0848 0x5 0x190` |
| Data lanes MD0..3 | `+0x400 + 0x100·i` | `3` / `0x1450` | `0x7033` | `0x1280 0x0935 0x0841 0x30 0x190` |

Every value matches the stock-computed one. `+0x200..+0x250` reads zero.
Each data lane's `+0x50` reads `0xfa11fa11`. BIAS_CON0..4 and PLL_CON0..8
match stock (PLL `0x1202`/`0x1355`/`0xdb`, lock count `0x3e8`, CON6
`0x38e`).

**DSIM**
- CONFIG `0x04887cff`, as in §1.1.
- At 120 Hz: UNDERRUN 1004, CMD_TE_CTRL0 28 and CMD_TE_CTRL1 `0x1a031e1f`,
  the stock 120 Hz values, set by our panel driver.
- CLK_CTRL `0x06131f05`, ESCMODE `0x01400000`, INTMSK `0xffffe447`,
  SFR_CTRL `3`.
- DPHY_STATUS `0x10f`.
- PLL_STAT reads 0.

**PMU and SYSREG**
- The DSIM PHY isolation bypass (`0x18063eb8`) reads 1.
- SYSREG_DISP MIPI_PHY_CON reads 1 (DPHY out of reset).
- S2MPU_DPU0..2 CTRL0 all read 0.
- gpp0 CON `0x2010`: gpp0-3's field [15:12] is 2, not 0xF as §1.2 assumed.
  The TE function number needs checking against `gs201-pinctrl.dtsi`.

**DECON where ABL differs from stock**
- SRAM_EN_PRI_0/1 are `0x1111`/0 (stock `0x11111111`/`0x11111`).
- OF_URGENT, RD/WR_URGENT and DTA are all 0 (stock enables them).
- DATA_PATH_CON_0 is `0xb1`, so neither DQE nor RCD is on.
- GLOBAL_CON `0x133`, TRIG_CON `0x3070`, CLOCK_CON 0.

**Window and DSC**
- WIN0 FUNC_CON/END_POS and WINCON0 read 0 at the §2.4 offsets. ABL's
  window programming is somewhere else, perhaps in shadow copies or at
  other offsets. Check against `regs-decon.h` before Stage 1 writes any
  window register.
- DSC0/1 PPS16_19 read `0x020001b4` against the computed `0x02000268`, and
  PPS56_59 `0x7d7e0102`. Restore ABL's DSC values from the snapshot rather
  than computing them.

**DPP**
- RDMA_ENABLE `0x40000000`, DEADLOCK_CTRL `0x03d487a1`, IO_CON `0x80`.
- IRQ masks are 0.

**For Stage 1:** take every value from this snapshot, not from §2, wherever
the two differ. Re-check each §2 offset against Google's register headers,
since one block in §2.3 was off by 0x100.

## 9. Stage 1 done (October 1)

`kernel/display-cycle` proved the power cycle as a module, and
`kernel/display-v27` moves it into `pixel_scanout`. Three things the plan did
not have:
- **DSIM CONFIG's DSC enable is shadowed.** It only takes when written after
  SFR_CTRL's shadow enable, as `dsim_reg_set_config()` orders it.
- **OF_SPLIT_SIZE, OF_SPLIT_IDX and OF_SIZE_1 must be restored.**
- **The DPU's three S2MPUs come back enabled** (CTRL0 = 1) and must be set
  back to 0, as §2.2 inferred.

A frame sent after an incomplete restore deadlocks the DMA beyond a soft
reset; the driver then fails closed until a reboot.

In sleep, the display domains being off saves about 1.8 mA by themselves,
and they let the camera, INT and MIPI/UFS PLL rails follow PWREN, about 5.8
mA more. Next: Stage 3 (AOD), which uses the same hibernation path.
