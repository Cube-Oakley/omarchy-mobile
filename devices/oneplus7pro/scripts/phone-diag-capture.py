#!/usr/bin/env python3
"""Record modem DIAG debug messages through linux-msm's diag-router.

diag-router (built on the phone in /root/radio-bringup/src/diag) serves raw,
unframed DIAG packets on the abstract Unix socket "\\0diag", one packet per
SOCK_SEQPACKET message. This client sets the F3 message masks and appends
every packet it receives to a file as <f64 monotonic time><u32 length><bytes>
until SIGINT/SIGTERM or the time limit. scripts/decode_diag.py turns the file
into text with the modem build's qdsp6m.qdb.

Masks: ERROR and FATAL for every subsystem, MED and up for SSID 14 (RF, which
holds all of rflm_qlnk's messages), and the LTE NAS (EMM/ESM, 0xB0E0-0xB0EF)
and RRC (0xB0C0) over-the-air log packets.

diag-router never drops a disconnected Unix client, and that client's backlog
then stalls the modem's DIAG data for the rest of the boot. So run one
capture for the whole boot (seconds 0 = no limit; the boot hook does) and
take slices of its file, rather than connecting per trial. Extra SSID=MASK arguments override single
SSIDs (MASK bits: 1 LOW, 2 MED, 4 HIGH, 8 ERROR, 16 FATAL); "all" enables every
level on every SSID, a flood for short checks only.

Run on the phone:
  python3 - OUTFILE [seconds] [all | SSID=MASK ...] < scripts/phone-diag-capture.py
"""
import collections
import select
import signal
import socket
import struct
import sys
import time

MSG_MED, MSG_HIGH, MSG_ERROR, MSG_FATAL = 0x2, 0x4, 0x8, 0x10
SSID_RF = 14

stop = False


def on_signal(*_):
    global stop
    stop = True


def main():
    out_path = sys.argv[1]
    limit = float(sys.argv[2]) if len(sys.argv) > 2 else 3600
    limit = limit or float("inf")
    extra = sys.argv[3:]
    everything = "all" in extra
    overrides = {SSID_RF: MSG_MED | MSG_HIGH | MSG_ERROR | MSG_FATAL}
    for arg in extra:
        if "=" in arg:
            k, v = arg.split("=")
            overrides[int(k)] = int(v, 0)
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    # diag-router binds with sizeof(sockaddr_un): the abstract name is "\0diag"
    # padded with NULs to the whole 108-byte sun_path.
    s.connect(b"\0diag".ljust(108, b"\0"))
    # Extended message configuration (0x7d). "Set all" (5) crashes diag-router
    # when the result is a partial mask (it broadcasts a NULL range to
    # diag_cmd_get_msg_mask), so fetch the SSID ranges (1) and set each (4).
    s.send(struct.pack("<BB", 0x7D, 1))
    resp = s.recv(65536)
    while resp[:2] != b"\x7d\x01":
        resp = s.recv(65536)
    count = struct.unpack_from("<I", resp, 4)[0]
    for i in range(count):
        first, last = struct.unpack_from("<HH", resp, 8 + 4 * i)
        base = 0x1F if everything else MSG_ERROR | MSG_FATAL
        masks = [base] * (last - first + 1)
        for ssid, mask in overrides.items():
            if first <= ssid <= last and not everything:
                masks[ssid - first] = mask
        s.send(struct.pack(f"<BBHHH{len(masks)}I", 0x7D, 4, first, last, 0, *masks))
    print(f"masks set on {count} SSID ranges", flush=True)
    # Logging configuration (0x73), set log mask (3): LTE is equipment 0xb.
    bits = bytearray(0xF0 // 8)
    for item in list(range(0xE0, 0xF0)) + [0xC0]:
        bits[item // 8] |= 1 << (item % 8)
    s.send(struct.pack("<B3xIII", 0x73, 3, 0xB, 0xF0) + bytes(bits))

    counts = collections.Counter()
    start = time.monotonic()
    with open(out_path, "ab", buffering=0) as out:
        while not stop and time.monotonic() - start < limit:
            if not select.select([s], [], [], 0.5)[0]:
                continue
            data = s.recv(65536)
            if not data:
                break
            out.write(struct.pack("<dI", time.monotonic(), len(data)) + data)
            counts[data[0]] += 1
    print(" ".join(f"0x{k:02x}:{v}" for k, v in sorted(counts.items())), flush=True)


if __name__ == "__main__":
    main()
