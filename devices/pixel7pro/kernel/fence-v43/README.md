# A signaled fence's names are not read from its driver (2026-10-07)

**The crash.** On October 7 at about 21:13, right after the desktop session
was restarted and Android's session stopped and started again, the kernel
oopsed and the phone restarted (panic, then the watchdog): Android's
`RenderThread` asked for a sync file's fence information
(`SYNC_IOC_FILE_INFO`), and `strscpy` read the fence's timeline name from
`0x1110050`, an address no longer mapped (`sized_strscpy` from
`__arm64_sys_ioctl`; the console is in the boot's pstore record,
`/var/log/pixel-pstore/638510be-…`). The first such crash in this phone's
records.

**The cause.** `dma_fence_timeline_name()` and `dma_fence_driver_name()` in
this release candidate ask the fence's ops for the names unless the ops
pointer was cleared, and `dma_fence_signal` clears it only for fences with
neither a release nor a wait callback. GPU scheduler fences (`drm_sched`, used
by Panthor) have a release callback, so a signaled one still answers with
`fence->sched->name`, read from a scheduler that is freed with the GPU context
that owned it. The functions' own comment says reading driver data after the
fence signaled is forbidden; upstream returns fixed names for signaled
fences.

**The fix (v43).** `0001-dma-fence-no-driver-names-after-signal.patch` returns
`"signaled-timeline"` and `"detached-driver"` for a signaled fence before any
driver callback, in both functions. The kernel is v42's otherwise: built with
`scripts/build-pixel-shell.py --persistent-root --seconds 0 --cma 256M
--mfc-cma 128M` (`out/fence-v43`, build ID `904c4a8f…`, image `d41f736b…`),
same release string and command line, configuration differing only in the
initramfs path (the tree's other changes since v42 are in the s5p-mfc module).
RAM-booted on October 7, then **installed in boot_a** (readback matched; the
previous image was v42, `85ff2edf…`). On v43 Android's session was stopped
and started again (F-Droid) without an oops; the crash needs a freed
scheduler at the right moment, so one run shows little beyond the kernel
working.
