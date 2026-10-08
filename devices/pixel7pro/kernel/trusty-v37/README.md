# v37: Trusty after CPU hotplug (2026-10-02)

v36 with one fix in Google's Trusty driver, which runs out of tree with the AoC
drivers: [`kernel/aoc/patches/0008`](../aoc/patches/0008-trusty-bind-each-nop-thread-to-its-CPU-again-when-it.patch).

**The bug.** Each CPU has a `trusty-nop-N` thread, created with
`kthread_create_on_cpu()`, that enters Trusty on that CPU when Trusty asks
for it (its doorbell interrupt). Offlining the CPU parks the thread after the
scheduler has stopped taking tasks there, so balance_push moves it away and
its binding is lost. `kthread_unpark()` binds again only `KTHREAD_IS_PER_CPU`
threads (smpboot's). Once the CPU was back, its thread ran elsewhere
(dmesg: `trusty-nop-4 ran on wrong cpu, 1`). A Trusty thread pinned to that
CPU never ran, and its caller waited forever.

The display hook takes CPUs 4-7 offline while the screen is dark (v35).
Goodix's fingerprint app pins itself to CPU 4 when idle and CPU 6 during
operations (`setTaCpuAffinity`), so after the first dark screen its HAL hung
in `tipc_read_iter`.

**The fix.** `trusty_cpu_up` binds the thread to its CPU again
(`set_cpus_allowed_ptr`) before unparking it.

**Validated by RAM boot** (`power-image-15`, build ID `c5ef9a55…`):
- CPU 4 taken offline and back: `trusty-nop-4` bound to CPU 4, no "wrong
  cpu", a TIPC connect from CPU 4 answered;
- the fingerprint lab: the trusted app, pinned to CPU 6 while CPUs 4-7 were
  offline, resumed as soon as they came back;
- binderfs, `idle_refresh` 10, `mif_floor_mhz` 1352, Wi-Fi runtime suspend,
  the modem's data link and the audio card as in v36.

**Installed in boot_a on October 2** (pre-approved). Readback matched
`f8e5b857…`; the previous image was v36, `63bd689b…`. After the reboot the
same checks passed.

The kernel itself is v36's: the tree matched `fp-v36.patch` exactly, and
`kernel.config` differs only in the initramfs path. Built with
`scripts/build-pixel-shell.py --persistent-root --seconds 0 --cma 256M`.
