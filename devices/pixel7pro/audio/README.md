# Audio configuration for the Pixel root

Installed into the Arch root by `scripts/build-pixel-root-image.py`:

| File | Installed as | Purpose |
| --- | --- | --- |
| `ucm2/conf.d/aoc-snd-card/aoc-snd-card.conf` | `/usr/share/alsa/ucm2/conf.d/aoc-snd-card/` | UCM entry for the AoC card (driver name `aoc-snd-card`) |
| `ucm2/Google/aoc/HiFi.conf` | `/usr/share/alsa/ucm2/Google/aoc/` | HiFi verb: Speaker (PCM 5, AoC EP6 to TDM_0) and Mic (PCM 8, AoC EP1 from the built-in microphones) |
| `wireplumber/51-pixel-aoc.conf` | `/etc/wireplumber/wireplumber.conf.d/` | no mmap, 20 ms periods for the AoC PCMs |

The verb sets the AoC routes itself. An AoC front end with no route to a back
end cannot be opened (ASoC DPCM returns EINVAL), and PipeWire probes the PCMs
before it enables any device.

The amplifiers' protection firmware and calibration are set up at boot by
`start_audio` in `scripts/pixel-persistent-session.sh`
([kernel/audio](../kernel/audio/README.md)), not by UCM.
