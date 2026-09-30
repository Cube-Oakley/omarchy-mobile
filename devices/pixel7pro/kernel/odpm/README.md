# Per-rail power meters (ODPM)

`pixel-odpm.c` reads the on-device power meters in both PMICs (S2MPG12 and
S2MPG13), 12 channels each, through ACPM. It is in the image but not loaded
at boot; load it when measuring:

```sh
insmod /proc/1/root/lib/modules/pixel/pixel-odpm.ko
cat /sys/module/pixel_odpm/parameters/power
```

## What it measures

The channels cover the rails that matter for idle power:
- **Main PMIC:** MIF, the three CPU clusters, INT and INT_M, the LLDO1 and
  LLDO3 feeds, and SLC.
- **Sub PMIC:** camera, GPU, both DRAM rails (VDD2H, VDDQ), the LLDO1/2/3
  feeds, MLDO, and the SA and SD bucks.
- **External (VSYS shunts):** display, modem, mmWave, RF front end and
  WLAN/BT.

Each read returns the low-pass filtered power per channel and a total.

For an average spanning system suspend, read `parameters/interval_power`.
The first read configures accumulator power mode and starts a baseline;
each following read latches the sample sums, divides by the sample count,
and starts a new interval. The meters accumulate while the AP sleeps, so
read immediately before and after a bounded sleep to include its transitions.
These are independent intervals, not cumulative counters to subtract.
Use intervals shorter than one hour (the 20-bit count wraps after about
2.3 hours at 125 Hz). Do not mix readers during a measurement.

## How

The meters run at 125 Hz, with LPF power mode on all channels. Registers,
mux codes and resolutions come from Google's `s2mpg1x-meter.h`,
`s2mpg12-powermeter.c`, `s2mpg13-powermeter.c` and `odpm.c`.
- **Internal rails:** resolution per mux code.
- **External rails:** VRAIL × VSHUNT × TRIM × 10 / shunt, using the stock DT's
  shunts (10 mΩ for mmWave, display and WLAN/BT; 5 mΩ for the modem and RF
  front end).

Only the meter bank (0x0A) is written:
- CTRL1 (the meter enables and internal rate; the NTC rate bits are kept);
- CTRL2 (external channel enables and rate);
- MUXSEL0–11;
- the LPF mode registers.
- on the first interval read, accumulator mode; each interval read strobes
  CTRL2's ASYNC_RD and waits for the hardware to clear it.

Unloading restores CTRL1 and CTRL2 and, if used, the accumulator mode.

## Caveats

- **Coverage:** the metered total is about half the USB input. The rest is
  charger and buck conversion loss, plus loads the 24 channels do not cover.
- **Noise:** single readings of INT and the CPU rails swing by tens of mW.
