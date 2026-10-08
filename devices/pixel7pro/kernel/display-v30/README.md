# V30: the panel keeps its TE settings across screen-off

`display-v30.patch` is a complete patch against the pinned base, as v29's
is. It differs from v29 in `pixel_panel.h` and `pixel_scanout.c`.

**The bug.** Sleep out resets the panel's TE block (DCS B9) to zeros. The
bootloader set it once (`00 04 00 00 0c 42 00 1b` ×3: TE option, TE2 option
and the TE width for each mode), and nothing set it again:
- `panel_dump` after a fresh boot: `B9 51 04 00 00 0c 42 00 1b 0c 42 00 1b 0c 42 00 1b`;
- after one screen-off/on: all 16 bytes `00`.

Normal mode still ran. The always-on display's fixed TE did not: after a
screen-off, entering or leaving AOD lost TE. Frames then timed out with no
TE (INT_PEND 0) until the next screen-off. v29's recovery power-cycled the
pipeline but not the panel, so it failed again; after three tries in a
minute it stayed failed closed. That is most likely the stuck clock the user
hit on v28.
- **Fresh boot, before the fix**, 8 stress cycles: 17 failures. Each cycle
  went lit → clock → lit, then off → dark → clock → lit.
- **Before the fix, after deep sleeps**, 10 cycles: 24 failures.

**The fix (`panel_wake`).** As stock's `s6e3hc4_update_panel_feat()` does
after every enable:
- after sleep out and TE on, the TE2 option and TE width go back to what the
  bootloader left (`B0 00 01 B9`, `B9 04 00 00 0c 42 00 1b` ×3);
- then the whole refresh group is written again, which sets the TE option
  (byte 0) for the current rate.

**Debugging aids (`pixel_scanout.c`)**
- `pixel_scanout.panel_dump` (write-only) logs the panel's power mode (0A),
  signal mode (0E), B9 and BD, and the DSIM command timers.
- A failed frame also logs the AOD state, refresh rates, DECON trigger and
  DSIM timers.

**Validated by RAM boot on October 1** (`power-image-5`, build ID
`75090c9a…`):
- B9 reads the same after a screen-off/on as after boot.
- 12 stress cycles, 48 AOD transitions, half of them after a screen-off:
  0 failures, 4,822 frames, no recoveries.
- The suspend adapter's minute loop (below) ran under the clock with 0
  failures.

**Outside the kernel: the clock's minute wakes**
- **The old path.** On the installed v29, unplugged with the clock up, the
  phone averaged 97.6 mA over 34 minutes, against 55.1 mA with the screen
  off (v27). It was awake 197 s of 2,040: each minute wake took about 3.5 s.
  The shell's sleep loop slept a fixed 2 s before sleeping again, started
  `quickshell ipc` for the redraw, and the adapter set up and put back
  everything for each sleep.
- **The minute loop.** Given `OMARCHY_MOBILE_MINUTE_TICK` (a file the clock
  watches), the adapter now does the minute wakes itself until the next
  background check. Each wake:
  - writes the file;
  - waits until pixel-scanout has sent the frame and powered the pipeline
    off (its debugfs counters);
  - sleeps again.
- **Result.** About 1 s awake per minute.
- **Busy modem.** Wake sources still active when the adapter reads
  `wakeup_count` (the modem sending) now count as a modem wake, not a
  refusal. Before, a refusal kept the phone awake for 60 s.
- **Same RAM boot, cable in (`input_off=1`), 5 minutes each:**

  | | Asleep | Average with wakes |
  |---|---|---|
  | Under the clock, minute loop | 101.8 mA | 105.4 mA |
  | Screen off | 76.6 mA | 79.0 mA |

  The minute wakes now cost about 1–2 mA over the sleep current. The clock
  itself costs about 25 mA asleep, as on v29 (68 against 44 mA). RAM boots
  read high, so only the difference counts.

Built with `scripts/build-pixel-shell.py --persistent-root --seconds 0 --cma
256M`. `kernel.config` matches v29's except for the initramfs path.

**Installed in boot_a on October 1** with the user's approval, through
`scripts/install-pixel-boot.py` from a RAM boot of the same image (direct
readback matched `c291657d…`; the previous image was v29, `93ddc7eb…`).
Reboot validation:
- the build ID matched;
- `display_power` and `display_off_required` were on, with `suspend=all`;
- B9 read intact after a screen-off/on;
- three rounds of screen-off → clock → lit ran with 0 failures and no
  recoveries.
