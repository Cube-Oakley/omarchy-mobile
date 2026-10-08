# Brownout protection (pixel-bcl)

On October 5, 2026 at 01:08 the phone reset under an all-core load: a 4-job
llama.cpp compile with BIG and MID at their top clocks, plus a 1.7 GB Wi-Fi
download. Eighty seconds earlier the battery log read −3.9 A at 3.99 V. The
bootloader reported `RST_STAT 0xc0000 PIN_RESET|PO_RESET`, "Reboot reason
0xcfce UVLO Reboot(PMIC MAIN)", with the battery at 92 % and the chip at
69 °C, and S2MPG12 OFFSRC1 read 0x90 against stock's normal 0x10. The supply
had sagged below the PMIC's undervoltage lockout. Files written in the last
minute (the Waydroid images, a model download) came back truncated.

Stock prevents this in hardware. The main PMIC warns when a CPU buck draws
too much current (OCP_WARN) or the system voltage gets low (SMPL_WARN), and
those warnings drive each CPU cluster's CMU clock divider (CLKDIVSTEP), with
no code running. Here the bootloader leaves both halves off for MID and BIG:

| | here | stock |
|---|---|---|
| S2MPG12 B2M_OCP_WARN (BIG), B3M_OCP_WARN (MID) | 0x00 | 0x80 (on, 12 A / 8 A) |
| S2MPG12 SMPL_WARN_CTRL | 0x68 (2.9 V) | 0xa8 (3.1 V, set by init.gs201.rc) |
| CPUCL1/2 CLKDIVSTEP_SMPL_FLT | 0 | 0x8000ffff |
| CPUCL1/2 CLKDIVSTEP_CON_LIGHT | 0 | 0x00f041c3 / 0xfff041c3 |
| CPUCL1 CLKDIVSTEP | 0x1 | 0x381 |

`pixel-bcl.c` sets stock's final values (the kernel's lpm_init and google_bcl
probe, then the init.gs201.rc writes):
- the S2MPG12 OCP warnings for BIG and MID and the SMPL warning, over ACPM;
- each cluster's CLKDIVSTEP registers, from a CPU hotplug callback that runs
  on the CPU coming online. CPUs 4-7 are offline while the screen is dark
  (the display hook) and get the values when they return. The registers kept
  their values across a day of that hotplugging, and pixel-sleep's save list
  (stock's save_sleep) restores them after SYS_SLEEP.

Loaded with `apply=1` by the boot script before CPU scaling starts. Without
`apply` it prints both sides beside the stock values, plus the PMIC's
PWRONSRC/OFFSRC registers (cleared by the read), and refuses to stay loaded.
Unloading puts back the values it found.

Left alone: the S2MPG12 BUCK_OCP_CTRL1-5 and S2MPG13 OCP_CTRL1-7 registers
(stock 0xbb, here 0x00; their fields are not documented anywhere), the G3D
and TPU warnings (nothing configures their dividers), and the software layer
(the MAX77759 UVLO1/UVLO2/BATOILO interrupts with frequency caps).

Every register is from Google's GPL sources: `flexpmu_cal_system_gs201.h`
(lpm_init lines 13-23, save_sleep 660-674), `s2mpg12-register.h`,
`s2mpg13-register.h`. Research notes:
`.work/research/reports/Pixel 7 Pro brownout throttling (BCL).md`.
