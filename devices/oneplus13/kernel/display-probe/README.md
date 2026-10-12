# Powered display diagnostic

Current checkpoint: probes 14–18 also include the experimental retained-panel
KMS transport described below. Probe 17 adds a dedicated zero-filled DMA
buffer for screen-off; enable/disable/enable selected black/new framebuffers
with zero transfer failures. This fixes exposure of the old boot image but
does not power down the panel. Probe 18 starts this driver with the installed
root under a RAM-loaded kernel; normal persistent boot remains unverified.

This is a manually loaded RAM-probe module, not native display support. It
holds the upstream SM8750 register-bus bandwidth, three display clocks and
MDSS power-domain references before exposing selected configuration registers
through a read-only `state` attribute. It does not reset MDSS, attach a display
IOMMU, program planes, send panel commands or touch interrupt status/clear ports.

Probe 11 returned unexpectedly to Android during an unpowered `/dev/mem`
display read. No result or pstore survived; the exact failing register and reset
cause are unknown. Do not repeat that access method. The stock driver's
`sde_dbg_reg_base_reg_read` explicitly calls `pm_runtime_resume_and_get` before
register access. Its supported debug interface successfully captured SPR state.

The diagnostic DT deliberately substitutes its own compatible for MDSS and
removes the reset/IOMMU properties. Children are not populated. Keep the stock
simple framebuffer and render-only MSM configuration for this experiment.
The upstream DISPCC driver must also be loaded; its PLL/gating setup may affect
the inherited display, so this is only suitable for a timed, recoverable RAM boot.

Register offsets are relative to stock MDSS at `0x0ae00000`. Upstream DPU starts
at `0x0ae01000`, so its catalog offsets differ by `0x1000`; do not mix the two.
No claim of safe unload, suspend, frame synchronization or persistent boot.

Probe 12 successfully acquired the clocks/power and read DPU version
`c0000000`. VIG0/VIG1 each fetch 720 x 3168 from `fc800000`, stride 5760;
their source X offsets are 0 and 720. Both retained SPR blocks are active.
The application SMMU's display stream is ID `0800`, mask `0002`, using its
bypass workaround: S2CR selects context bank 82, whose SCTLR is zero and CBAR
selects stage-1/stage-2 bypass behavior. The GPU SMMU shares the compatible
string, so the diagnostic explicitly selects and checks `15000000.iommu`.

However, after stock DISPCC registration, panel GPIO TE and hardware
frame-done events stopped. Do not assume a successful register read preserved
scanout. The bounded `display-event-test` requests the real display IRQ and
temporarily enables only PP0/PP1 completion and INTF1 read-pointer sources,
then clears their enables and releases the IRQ. Optional `kickstart=1` writes
only CTL0 START after checking its retained mode and both original addresses;
this did not recover events in probe 12. It is not a buffer-flip test.

`0001-dispcc-retain-active-roots.patch` is a development experiment replacing
the AHB, ESC0 and MDP shared-root initialization with upstream's existing
`clk_rcg2_shared_no_init_park_ops`. Normal shared-root registration parks an
active clock before its consumer can acquire it. Test this change in a fresh
RAM boot, sampling GPIO TE before and after DISPCC registration, then again
after the powered diagnostic binds. This patch alone is not proof of handoff.

Probe 13 verifies that change: TE stays at 120.838/120.838/120.832 Hz across
those three steps. A 2.011667-second IRQ test counted 243 TE events and 122
PP0 frame completions (PP1 is not a separate completion source in this split
configuration). The inherited transport therefore completes about 61 frames/s,
despite the roughly 121 Hz TE cadence. This is not 120 fps desktop delivery.

`display-flip-test` is a separate explicit `run=1` experiment. It requires the
exact retained geometry/format/SPR/CTL registers, no existing IRQ owner, and
the verified application-SMMU bypass configuration. It allocates a 32-bit DMA
buffer, paints four quadrants, flushes the two VIG addresses, waits for two
hardware completions and restores the original address after two seconds.
Failure to confirm restoration pins the allocation and module until reboot.
Compile against the exact probe kernel; it is not a persistent KMS driver.


Probe 13b verified the corrected flip protocol. Stock SDE explicitly avoids
CTL_START while auto refresh is active; doing both in probe 13 stalled DMA.
Flushing without START passed twice and restored the original address. The
upstream auto-refresh drain sequence also passed, after which 120 alternating
manual transfers completed in 999,415 us and automatic refresh was restored.
`display-flip-test manual=1 flips=120 run=1` reproduces that bounded test.

`oneplus13-scanout enable=1` is a separate experimental KMS module. It requires
the powered diagnostic with unbinding disabled and pins that owner and itself
until reboot. It checks the inherited profile and SMMU bypass, drains auto
refresh, tests an original-buffer transfer, and removes simpledrm before
registering DSI-1. Only 1440 x 3168 linear XRGB8888 at pitch 5760 is accepted.
DMA/GEM buffers stay referenced through hardware completion; a failed transfer
pins both potentially active framebuffers and rejects further commits. This
is a retained-panel transport driver, not cold panel initialization or suspend.

Build the matching `drm_dma_helper.ko` and load it first. Probe 14 reserves
128 MiB CMA (rather than 32 MiB) for full-resolution display buffers. The
CPU-created DRM test completed 1209 flips in 10.005 s, restored the original
buffer, freed its allocations and reported no failures. The KMSRO/Adreno
DMA-buffer test passed all three color phases. The existing isolated Mesa
package works through its generic KMSRO fallback; no extra driver-name patch
was needed. Hyprland and the shared shell ran on card2/DSI-1; card0 disappears
when simpledrm is removed, so launchers must discover the output device.

The probe-14 compositor presentation test still measured roughly 60 fps.
Current source separates continuous INTF1 read-pointer/TE vblank from PP0 DMA
completion, matching upstream DPU command-mode handling. Probe 15 confirms those independent interrupts work, but both Wayland test
modes still have median 16,553 us presentation intervals. Holding the GPU at
342 MHz and toggling the compositor scheduler did not remove the half-rate
behavior. Further timing work is required. A screen capture is
not physical-panel or optical-cadence validation.
