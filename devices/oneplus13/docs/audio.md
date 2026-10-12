# Audio — October 12, 2026

Speakers and the main microphone work through the stock ADSP firmware's
AudioReach framework, the mainline SM8750 audio drivers and PipeWire. The
installed image is `native-persistent22-audio` (see [recovery](recovery.md)).

## Hardware

| Part | Connection | Notes |
| --- | --- | --- |
| WCD9395 codec | SoundWire RX (unique ID 4) and TX (ID 3), reset TLMM101 | Wired as on the SM8750 MTP: L15B, BOB1, L2I supplies; mic bias 2.7 V as stock |
| Microphones | WCD9395 AMIC1, AMIC3, AMIC4 (bias 1, 4, 3) | All three carry signal; AMIC2/5 idle (headset/unpopulated). UCM uses AMIC1 |
| Amps | 2 × Awinic AW88461 (product 0x2308), I2C hub 4 at 0x34/0x35 | 0x34 channel 0 = top/earpiece, 0x35 channel 1 = bottom speaker (heard) |
| I2S | Secondary MI2S: TLMM121 BCLK, 123 WS, 122 data to amps, 124 amp feedback | 48 kHz, 32-bit slots, 64fs, as stock (`MI2S-LPAIF-RX-SECONDARY`) |

## Software pieces

- Kernel tree `out/linux-op13`: reflink of the 7.3-rc2 tree with
  `kernel/audio.config` and `kernel/bluetooth.config` merged (modules only;
  the added built-in options are ALSA/Bluetooth feature flags) and three patches:
  - `kernel/audio/0001`: new `aw88461` codec driver (by a subagent, reviewed).
    It loads the stock `aw882xx_acf.bin` profile file, keeps the amp muted and
    powered down until a stream, never exceeds the profile's own volume, and
    starts the amp from a work item: on AudioReach the I2S clocks only run once
    the graph starts, after DAPM power-up.
  - `kernel/audio/0002`: an `oneplus,cph2655-sndcard` entry in the sc8280xp
    machine driver: LPASS provides BCLK (no MCLK pin), amps as clock consumers,
    MI2S back ends forced to S32_LE.
  - `kernel/audio/0003`: log non-zero DSP write-buffer status (diagnostic).
- Device tree: `kernel/sm8750-oneplus13-audio.dts` (WCD9395, SoundWire, mic
  dai-link) and `kernel/sm8750-oneplus13-speakers.dts` (I2C hub wrapper and
  bus, amps, I2S pins, the `dai@12` bit clock `LPASS_CLK_ID_SEC_MI2S_IBIT`,
  speaker dai-link). Bluetooth moved into the base tree in
  `kernel/sm8750-oneplus13-bluetooth.dts` because any DT change renumbers the
  phandles the old runtime overlay checked.
- Topology `audio/OnePlus-13.m4` (Linaro's audioreach-topology on the include
  path), installed as `qcom/sm8750/OnePlus-13-tplg.bin`.
- UCM `audio/ucm2/` (`conf.d/sm8750/OnePlus-13.conf`, `OnePlus/OnePlus-13/HiFi.conf`):
  Speaker on PCM 0, Mic on PCM 2 (stereo, both channels from AMIC1).
- Package `/opt/oneplus13-audio`: a private module tree loaded with
  `modprobe -d` (udev never autoloads it, so it cannot race the separately
  packaged Wi-Fi/Bluetooth builds), firmware links for the kernel loader, and a
  manifest. `scripts/start-native-audio.sh` loads it; the session then starts
  PipeWire, WirePlumber and pipewire-pulse as root.

## Three traps found on the way

1. **The data line is the DSP's SD1.** With `SD_LINE_IDX_I2S_SD0` the
   interface runs (BCLK and WS toggle, the amps' PLL locks) but TLMM122 stays
   low and the amps' voltage/current sense read zero. With `SD1` the amps
   switch and their sense registers follow the signal.
2. **The I2S device subgraph needs a format converter.** The kernel gives the
   stream graph the front end's format (16-bit). Linaro's codec-DMA device
   subgraph has an MFC; the I2S one does not, so a 32-bit endpoint was fed
   mismatched data and the stream ran at twice real time. Our
   `audio/audioreach/subgraph-device-i2s-mfc-playback.m4` adds the MFC.
3. **The bit clock is only enabled if the DT declares it** on a `dai@<id>`
   child of `q6apmbedai`; and the I2C hub's parent wrapper
   (`i2c_master_hub_0`) is disabled by default.

`scripts/start-native-audio.sh` also loads `i2c-qcom-geni` (a module here).

## Verified

- Microphone capture through PipeWire, both channels, live signal.
- Left/right test tones: left from the top earpiece, right from the bottom
  speaker; owner confirmed both, clean.
- Chime through PipeWire at 100 % sink volume: clearly audible, somewhat
  below stock maximum. Normal boot autostart, slot A marked successful.

## Limits and next steps

- No speaker protection: stock runs Awinic's algorithm on the ADSP with the
  calibrated resistance/resonance (`aw-re-default`, `aw-f0-default` in the stock
  DT). The amps stay 6 dB under the stock profile; raise them only behind a
  PipeWire limiter/high-pass filter like the Pixel's.
- Earpiece-only call routing ("Receiver" profile on the top amp), voice call
  graphs, echo reference from the amps' I/V feedback (TLMM124).
- USB-C headsets (WCD9395 USB-SS mux, MBHC), Bluetooth audio (needs pairing).
- Reloading the sound card without a reboot leaves the ADSP's buffer mappings
  broken; reboot to retry.
