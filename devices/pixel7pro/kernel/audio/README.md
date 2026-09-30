# Built-in speakers and microphones

Audio goes through the AoC DSP ([aoc](../aoc/README.md)). This page covers
the microphone and speaker paths.

## Microphones

The microphones' supplies (S2MPG12 LDO20M, S2MPG13 LDO19S and LDO20S) are off
when the bootloader hands over, and the AoC then records a decaying burst and
zeros. [aoc-power](../aoc-power/README.md) switches them on.

Recording is AoC EP1, PCM 8 on the card, with the stock record routing:
- `EP1 TX Mixer INTERNAL_MIC_TX` on;
- `BUILDIN MIC ID CAPTURE LIST` 0,3,2,-1;
- `MIC DC Blocker` on;
- 22 dB soft gain.

## Speakers

Two CS35L41 amplifiers sit on SPI7 ([spi](../spi/README.md), with pins and
interrupts from [gpio](../gpio/README.md)):

| Amplifier | Speaker | Controls |
| --- | --- | --- |
| left, `cs35l41@0` | top (earpiece) | no prefix |
| right, `cs35l41@1` | bottom | `R` |

The codec is mainline `sound/soc/codecs/cs35l41*`, built out of tree with
`wm_adsp` and `cs_dsp`, because the kernel configuration leaves them off.

Playback is AoC EP6, PCM 5, deep buffer: `TDM_0_RX Mixer EP6` routes it to
TDM_0. TDM_0 carries four 32-bit slots at 48 kHz in DSP_A format, with the
amplifiers as clock consumers.

The amplifiers run the stock protection firmware:
- `cs35l41-dsp1-spk-prot.wmfw`, with the stock top and bottom tunings,
  installed as `cirrus/cs35l41-dsp1-spk-prot-cheetah{.wmfw,.bin,-r.bin}`;
- the factory calibration from `persist:/audio/speaker.cal`, which holds
  CAL_R, CAL_STATUS, CAL_CHECKSUM and CAL_AMBIENT for each amplifier.

persist is read with `debugfs`, read-only, and never mounted. The session
script writes the calibration to the DSPs' `CAL_*` controls and routes the DAC
through the DSP (`PCM Source` DSP). If that fails, both amplifiers are muted
rather than left unprotected.

It then sets the rest of the stock default path (`mixer_paths.xml`) on both
amplifiers:

| Control | Value | Why |
| --- | --- | --- |
| `Analog PCM Volume` | 17 (17.5 dB) | Stock "AMP PCM Gain" 17. The reset value, 0.5 dB, is 17 dB quieter. |
| `DRE Switch` | on | Stock. |
| `DSP RX1 Source`, `DSP RX2 Source` | ASPRX1 | Both protection inputs from the amplifier's own slot, as stock. |

`Digital PCM Volume` stays at its 0 dB default (817), as stock.

## Userspace

The UCM profile and the WirePlumber rule are in [../../audio](../../audio).
PipeWire shows "Built-in Audio Speakers" and "Built-in Audio Microphones". The
rule turns mmap off: this card's mmap maps the AoC service ring itself, and
the stock HAL opens these endpoints without mmap.

Validated on 2026-09-29:
- **Microphones.** The noise floor is live, and the vibration motor shows up
  as a clear step.
- **Speakers.** A 1 kHz tone on each channel reaches only its speaker, with
  the protection firmware running on both amplifiers: HALO state 2, no CSPL
  errors, 25 °C.
- **PipeWire.** `pw-play` to `pw-record` carries the tone end to end.
- **By ear**, with the stock gain: a voice recorded through the microphones
  plays back clearly. Each channel plays from its own speaker, top on the
  left and bottom on the right. At full volume it is loud and undistorted.
