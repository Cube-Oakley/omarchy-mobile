# Panel brightness and sleep — October 12, 2026

The retained-panel scanout driver now sends DCS commands to the AA569 panel
through the DSI0 controller's command DMA engine (`panel=1`). This gives a
standard backlight device and real panel sleep while the screen is off. The
panel's DSI/DSC/SPR setup is still the bootloader's: there is no cold panel
initialization or rail power-down yet.

## What is implemented

- `/sys/class/backlight/oneplus13-panel`: DCS `0x51` with a big-endian DBV,
  range 1..3515 (the stock normal maximum; 3516..4094 is the panel's boost range,
  not exposed). The shell's brightness slider and remembered level use it.
- Screen off (DPMS off): one black frame, then command page 0, `0x28` display
  off (20 ms), `0x10` sleep in (120 ms). TE stops; power mode reads `0x09`.
- Screen on: the stock 120 Hz on sequence (`timing@wqhd_sdc_120`, 150 commands
  with sleep out and its 120 ms wait, ending in display on), then the current
  brightness. TE and frame transfers resume.
- `oneplus13_scanout` debugfs adds panel state: asleep, brightness, live power
  mode, DSI command count, sleeps and wakes.

## How the commands are sent

DSI0 is at `0x0ae94000`. It is DSI 6G v2.9; register offsets are the upstream
XML offsets plus 4 (`DSI_6G_REG_SHIFT`). The retained state: `CTRL 0x1f5`
(command mode, four lanes), `CMD_DMA_CTRL 0x14000000` (memory fetch, LP),
`TRIG_CTRL 0x80000004` (software DMA trigger, TE-started MDP frames), all DSI
interrupt masks clear. The driver checks this exact profile before enabling
panel control.

A command is written to a 32-bit coherent buffer in MSM layout (header bytes
1, 2, 0, then last/long/read flags; the controller adds ECC), followed by
`DMA_BASE`, `DMA_LEN`, `TRIG_DMA`. The display stream's SMMU context is in
bypass, so the physical address works. Completion is **polled** on
`INTR_CTRL` bit 0: the MDSS interrupt line belongs to the scanout's frame IRQ
handler, which knows no DSI sources, so DSI masks must stay disabled. A mutex
orders commands against frame transfers. Reads send a maximum-return-size
packet, clear `RDBK_DATA_CTRL`, then a DCS read with the BTA flag; the short
response is in `RDBK_DATA0`.

The bootloader leaves power mode `0x9c`. After any sleep-in, sleep-out reads
`0x9d` even after replaying the full stock sequence; bit 0 is a sticky
status the stock driver never sees because it power-cycles the panel. The
owner confirmed a normal picture after wake.

Brightness writes take effect at the panel's next internal frame; an immediate
`0x52` readback still returns the previous value.

## Measured

On USB (input pinned at the 500 mA SDP limit), the battery went from about
14 mA to 70 mA into the battery with the screen off, and its voltage rose:
roughly 240 mW less than a lit desktop. The battery's `current_now` is
negative when charging on this firmware, despite `status` reading Discharging.

Eight automatic off/on cycles in the RAM test and five more after install:
zero transfer failures, zero DSI faults.

## Not done

- Rail power-down (VCI, reset GPIO, VDDIO) and cold initialization.
- Low-power AOD mode (`lp1`/`nolp` commands, 30/1 Hz), refresh switching,
  stock PWM-mode switching below DBV 1102, HBM, ESD checking.
- Panel serial/ID reads and the OPlus gamma/color calibration paths.

Sources: `kernel/display-probe/oneplus13-scanout.c`,
`kernel/display-probe/aa569-on-120.h` (generated from the captured stock DT),
`kernel/display-probe/dsi-peek.c` and `dsi-cmd-test.c` (one-shot test modules).
