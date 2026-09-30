# diag-router for modem logging

linux-msm [diag](https://github.com/linux-msm/diag) at `23c12c1`, built on the
phone as `/root/radio-bringup/src/diag/diag-router` with libqrtr's sources
compiled in, and started before the modem by `scripts/phone-radio-start.sh`
when `/root/radio-bringup/cellular/diag-at-boot` exists
(see `docs/cellular-sim-20260926.md`).

`0001-drop-disconnected-clients-and-push-all-masks.patch` fixes two router
bugs met on this phone:

- A Unix client that disconnected stayed in the broadcast list. Its queue kept
  growing and held the peripherals' flow control shut, so the modem's DIAG
  data stopped for the rest of the boot. Disconnected clients are now removed
  and their queued buffers' flow credit returned.
- A peripheral that connected was sent only message masks. Log and event masks
  set before it came up never reached it; they are now sent too.

`0002-advertise-mask-centralization.patch` advertises mask centralization,
as the vendor driver does, so the modem takes its log masks from this side,
and line-buffers stdout so the router's feature report reaches its log.

Build on the phone:

    cd /root/radio-bringup/src/diag && Q=/root/radio-bringup/src/qrtr
    gcc -Wall -O2 -g -DHAS_LIBUDEV=1 -DHAS_LIBQRTR=1 -I$Q/include -Irouter \
        -o diag-router router/*.c $Q/lib/qrtr.c $Q/lib/logging.c $Q/lib/qmi.c -ludev
