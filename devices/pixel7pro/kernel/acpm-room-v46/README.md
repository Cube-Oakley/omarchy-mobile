# v46: room for every ACPM reply (2026-10-11)

v45 (`kernel/mfc-pool-v45`) with `0001-acpm-room-for-each-reply.patch` on
`drivers/firmware/samsung/exynos-acpm.c`. Same configuration and command
line; built with `scripts/build-pixel-shell.py --persistent-root --seconds 0
--cma 256M --mfc-cma 384M` (`out/acpm-room-v46`, build ID `f00b99f0…`,
image `5f930207…`).

**The crash.** The battery run ended on 2026-10-10 at about 08:30 in an
"APM Watchdog" reset (reboot reason 0xcbcd, RST_STAT APM_CPU_WDTRESET). In
the kernel log, 55 ms after a deep-sleep exit, while the modem brought its
PCIe link up, every ACPM channel timed out ("Timeout! ch:0 s:48", then the
thermal channel 9); the next suspend hung. The bootloader's ACPM dump
(pstore console) has the firmware at PC 0x7baa: a `wfi` loop right after a
log call with the string `qfull`, in the routine at 0x7b3c of the APM image
in bl2_a (address = file offset - 0x1e000; see the APM crash notes). That
routine takes a slot for a reply on a channel; when the slot after it is the
host's read position (the replies before it unread) it logs `qfull` and
halts instead of dropping the reply, and the APM watchdog resets the SoC.
All its callers are in the command dispatcher, which keeps the channel in
r6: r6 was 2 in the dump, the PMIC channel, whose reply ring has five
elements (four usable). After a wake several drivers talk to the PMIC at
once (the RTC and power-key interrupts, BCL, the PMIC sleep modes).

**Why mainline let it fill.** The firmware answers the command in TX slot i
in RX slot i. Mainline's `acpm_wait_for_queue_slots()` only waits until the
next TX slot is not the RX *front*, the firmware's write position; it never
looks at the RX *rear*, what the host has read. Replies are read only by
senders waiting for theirs (polling mode), so while those are held up the
unread replies count against the ring and the next command can still go out
with no room for its answer. Google's `acpm_ipc.c` reads replies from an
interrupt and still keeps two elements in reserve "to prevent buffer full
and qfull".

**The change.** Before a command is written, `acpm_wait_for_reply_room()`
reads the replies that are waiting (`acpm_get_rx()` with sequence number 0
keeps every one for its sender, as for any other sender's reply), then sends
only while the commands ahead of this one, counted from the RX rear, leave
its reply a slot and one to spare: at most `qlen - 3` ahead, stock's
reserve. Channels shorter than three elements keep the plain check (stock
never sends on them this way). A wait longer than the TX timeout logs "No
room for a reply on ch:N" and fails the transfer instead of halting the
firmware.

**Validated.** RAM-booted on 2026-10-11: build ID matched, the ACPM channels
came up (channel 2 qlen 5), no ACPM errors, thermal zones and the RTC read,
mobile data and Wi-Fi up, timed naps through SYS_SLEEP on the cable.
**Installed in boot_a** (readback matched; previous image v45 `ad6bba60…`);
an orderly reboot came up on it (SSH 36 s, desktop 58 s, mobile data). Four
100 s naps through SYS_SLEEP (always-on clock and dark) with pixel-sleep's
modem link check: each woke within a second of its time, 14 deep sleeps, none
refused or aborted, no ACPM error. Whether the qfull halt is gone takes days
of sleeps to show (the run before it had about 17 hours of them).
