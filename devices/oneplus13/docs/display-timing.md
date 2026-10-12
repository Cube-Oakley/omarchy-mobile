# 120 Hz compositor presentation — October 12, 2026

The initial DMA driver could transfer immutable DRM buffers at about 120.8 Hz,
but Hyprland's presentation test ran at about 60.4 fps. Enabling Hyprland's
alternate render scheduler did not change the result.

A bounded 64-entry kernel timing trace identified the missed interval. In the
compositor workload, programming the DMA took about 4.7 µs, the transfer wait
about 11.98 ms, and preparing the next commit after completion about 4.56 ms.
The old driver notified userspace only after the entire transfer completed;
rendering then missed the next panel TE.

The `early_flip` path separates presentation notification from DMA retirement:

1. Detach the current commit's event before kickoff, so a subsequent atomic
   state swap cannot make this worker send another commit's event.
2. Program and start the transfer as before.
3. Require cleared address-flush bits and a processed TE after kickoff, then
   notify the compositor so it can prepare the next frame during this transfer.
4. Continue waiting for actual ping-pong completion before retiring either
   framebuffer. Failures retain potentially active DMA objects and reject
   subsequent updates.

The experiment does not change panel timings, GPU frequency, DMA addresses,
clock ownership or the inherited DSI/DSC/SPR setup. The persistent startup now
loads `oneplus13-scanout.ko enable=1 early_flip=1`.

## Validation

- RAM boot: 1,169 presentation intervals, median 8,277 µs, mean 120.61 fps;
  1,167 consecutive vblank-sequence steps. Hardware transfer completion cadence
  also measures about 8,277 µs, with approximately 48 µs between completion and
  the next commit worker. This is backed by DMA completion, not just events.
- Owner: significantly smoother interaction; slight flicker during
  portrait-to-landscape switching, otherwise visually correct.
- Four rotation changes and three screen-off/wake cycles: zero transfer
  failures, with both portrait and landscape transforms accepted.
- Installed normal boot: median 8,276 µs and mean about 120.73 fps. Full boot
  readback matches the RAM-tested image. Seven other active boot partitions,
  including stock VBMeta, remain byte-identical to their baseline.
- Slot A marked successful only after normal-boot validation, then independently
  confirmed in fastboot as successful, bootable, with six retries.

Current image: `out/native-persistent20-earlyflip/boot-persistent.img`, SHA256
`c25e21e52f951db43c2e0af6c5c6177fc609818a30ae238214a41f84e6952313`.
The prior `native-persistent20-usb` image is retained for rollback. Both use
the probe 20 ABI. Private timing logs, summaries, readbacks and boot records
are in those ignored output directories.

The rotation-transition flicker remains open. This is still a retained-panel
driver: true panel power-down, native cold initialization, brightness control,
suspend/resume and long-duration thermal behavior remain unfinished. The
earlier physical power-off/on test covered the USB revision before this timing
change; it has not been repeated physically on the early-flip image.
