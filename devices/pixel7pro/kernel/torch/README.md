# Flashlight

`pixel-torch.c` drives the rear LM3644 flash as an LED class torch,
`white:torch`. The shell's flashlight toggle finds it.

The stock DT's lwis `flash-lm3644` node places the chip at 0x63 on hsi2c_15
([i2c](../i2c/README.md)). It has two enables:
- gpp8-2, HWEN, active high. The module drives it high only while the torch
  is on.
- gpp27-0, active low. It stays as the bootloader leaves it, low.

The module reads the device ID (0x02) at load and refuses to load without it.

Brightness is the LM3644 torch step, applied to both LEDs: 0.977 mA plus
1.4 mA per step, up to 127. The `max_level` parameter caps it at 64 by default.
The shell lights the torch at half of `max_brightness`, which gives step 32.

Measured at the USB input, over a 1.48 W idle draw:

| Step | Extra power |
| --- | --- |
| 16 | +0.62 W |
| 32 | +0.81 W |
| 64 | +1.86 W |
