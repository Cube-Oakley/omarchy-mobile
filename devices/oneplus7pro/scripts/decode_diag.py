#!/usr/bin/env python3
"""Decode a phone-diag-capture.py recording into text.

Records are <f64 monotonic time><u32 length><DIAG packet>. Handled packets:

  0x79  extended message: header, u32 args, then the format string and file.
  0x99  QShrink 4 message: header, u64 timestamp, u32 key into the modem
        build's qdsp6m.qdb, u16 database id (the qdb GUID's last two bytes),
        then the args, packed as <count> args of <size> bytes where the
        header's third byte is (size << 4) | count.
  0x92  QShrink 2/3 message: printed with its raw hash; its database is not
        in the modem image (these come from the other DSPs).
  0x98  subscription wrapper (version, reserved, u32 subscription id)
        around one of the above; per-subscription stacks (NAS, REG) use it.
  0x10  log packet; LTE EMM/ESM over-the-air messages are decoded.

The layouts were read from captures on this phone (2026-09-26).

  scripts/decode_diag.py CAPTURE [qdsp6m.qdb] [--ssid N ...] [--grep TEXT]
"""
import argparse
import re
import struct
import zlib

DEFAULT_QDB = ".work/guacamole-radio-firmware/image/qdsp6m.qdb"
LEVELS = {1: "LOW", 2: "MED", 4: "HIGH", 8: "ERROR", 16: "FATAL"}
SPEC = re.compile(r"%([-+ #0]*)(\d*)(?:\.(\d+))?(h|hh|l|ll|z)?([diuxXcsp%])")


def load_qdb(path):
    raw = open(path, "rb").read()
    # A 64-byte file header precedes the zlib stream; find the first valid
    # zlib header (CMF 0x78, CMF/FLG divisible by 31) that decompresses.
    for start in range(0, 0x200):
        if raw[start] == 0x78 and ((raw[start] << 8) | raw[start + 1]) % 31 == 0:
            try:
                text = zlib.decompress(raw[start:])
                break
            except zlib.error:
                continue
    else:
        raise SystemExit(f"no zlib stream in {path}")
    db = {}
    for line in text.split(b"\n"):
        parts = line.rstrip(b" \r").split(b":", 5)
        if len(parts) == 6 and parts[0].isdigit() and parts[3].isdigit():
            key, mask, ssid, lineno, src, fmt = parts
            db[int(key)] = (int(mask), int(ssid), int(lineno), src.decode(errors="replace"),
                            fmt.decode(errors="replace"))
    return db


def render(fmt, args):
    """printf-style formatting with integer args; %s cannot travel, so show hex."""
    it = iter(args)

    def sub(m):
        flags, width, prec, _len, conv = m.groups()
        if conv == "%":
            return "%"
        v = next(it, None)
        if v is None:
            return m.group(0)
        if conv in "di":
            v = v - (1 << 32) if v >= 1 << 31 else v
            return f"{v:{flags}{width}d}"
        if conv == "u":
            return f"{v:{flags}{width}d}"
        if conv in "xX":
            digits = max(int(prec or 0), int(width or 0))
            return f"{v:0{digits}{conv}}" if digits else f"{v:{conv}}"
        if conv == "c":
            return chr(v & 0xFF)
        return f"<{conv}:{v:#x}>"
    return SPEC.sub(sub, fmt)


EMM_TYPES = {
    0x41: "Attach Request", 0x42: "Attach Accept", 0x43: "Attach Complete",
    0x44: "Attach Reject", 0x45: "Detach Request", 0x46: "Detach Accept",
    0x48: "TAU Request", 0x49: "TAU Accept", 0x4A: "TAU Complete",
    0x4B: "TAU Reject", 0x4C: "Extended Service Request", 0x4E: "Service Reject",
    0x50: "GUTI Reallocation Command", 0x51: "GUTI Reallocation Complete",
    0x52: "Authentication Request", 0x53: "Authentication Response",
    0x54: "Authentication Reject", 0x5C: "Authentication Failure",
    0x55: "Identity Request", 0x56: "Identity Response",
    0x5D: "Security Mode Command", 0x5E: "Security Mode Complete",
    0x5F: "Security Mode Reject", 0x60: "EMM Status", 0x61: "EMM Information",
    0x62: "Downlink NAS Transport", 0x63: "Uplink NAS Transport",
    0x64: "CS Service Notification",
}
ESM_TYPES = {
    0xC1: "Activate Default Bearer Request", 0xC2: "Activate Default Bearer Accept",
    0xC3: "Activate Default Bearer Reject", 0xC5: "Activate Dedicated Bearer Request",
    0xC6: "Activate Dedicated Bearer Accept", 0xC7: "Activate Dedicated Bearer Reject",
    0xC9: "Modify Bearer Request", 0xCA: "Modify Bearer Accept",
    0xCB: "Modify Bearer Reject", 0xCD: "Deactivate Bearer Request",
    0xCE: "Deactivate Bearer Accept", 0xD0: "PDN Connectivity Request",
    0xD1: "PDN Connectivity Reject", 0xD2: "PDN Disconnect Request",
    0xD3: "PDN Disconnect Reject", 0xD9: "ESM Information Request",
    0xDA: "ESM Information Response", 0xE8: "ESM Status",
}
EMM_CAUSES = {
    2: "IMSI unknown in HSS", 3: "illegal UE", 5: "IMEI not accepted",
    6: "illegal ME", 7: "EPS services not allowed",
    8: "EPS and non-EPS services not allowed", 9: "UE identity cannot be derived",
    10: "implicitly detached", 11: "PLMN not allowed", 12: "tracking area not allowed",
    13: "roaming not allowed in this TA", 14: "EPS services not allowed in this PLMN",
    15: "no suitable cells in TA", 16: "MSC temporarily not reachable",
    17: "network failure", 18: "CS domain not available", 19: "ESM failure",
    22: "congestion", 25: "not authorized for this CSG",
    35: "requested service option not authorized", 39: "CS service temporarily unavailable",
    40: "no EPS bearer context activated", 111: "protocol error, unspecified",
}
ESM_CAUSES = {
    8: "operator determined barring", 26: "insufficient resources",
    27: "missing or unknown APN", 28: "unknown PDN type",
    29: "user authentication failed", 30: "rejected by serving/PDN gateway",
    31: "request rejected, unspecified", 32: "service option not supported",
    33: "requested service option not subscribed", 34: "service option temporarily out of order",
    36: "regular deactivation", 38: "network failure", 39: "reactivation requested",
    50: "PDN type IPv4 only allowed", 51: "PDN type IPv6 only allowed",
    54: "PDN connection does not exist",
    55: "multiple PDN connections for an APN not allowed",
    65: "maximum number of EPS bearers reached",
    66: "APN not supported in current RAT and PLMN",
}
LOG_NAMES = {0xB0E2: "ESM OTA in", 0xB0E3: "ESM OTA out",
             0xB0EC: "EMM OTA in", 0xB0ED: "EMM OTA out"}


def apn_text(value):
    """APN IE value: length-prefixed DNS labels."""
    out, i = [], 0
    while i < len(value):
        n = value[i]
        out.append(value[i + 1:i + 1 + n].decode(errors="replace"))
        i += 1 + n
    return ".".join(out)


def esm_details(pdu):
    kind = pdu[2] if len(pdu) > 2 else None
    name = ESM_TYPES.get(kind, f"ESM 0x{kind:02x}" if kind is not None else "ESM ?")
    extra = ""
    if kind in (0xC3, 0xC7, 0xCB, 0xCD, 0xD1, 0xD3, 0xE8) and len(pdu) > 3:
        extra = f" cause #{pdu[3]} {ESM_CAUSES.get(pdu[3], '')}"
    at = pdu.find(b"\x28", 4) if kind in (0xD0, 0xDA, 0xC1) else -1
    if at > 0 and at + 1 < len(pdu):
        extra += f" apn? {apn_text(pdu[at + 2:at + 2 + pdu[at + 1]])!r}"
    return name + extra


def nas_text(pdu):
    """Name an EMM or ESM PDU and pull out its cause."""
    if not pdu:
        return "empty NAS PDU"
    sec, pd = pdu[0] >> 4, pdu[0] & 0xF
    if pd == 7 and sec in (1, 2, 3, 4):     # integrity protected: skip MAC and SQN
        return nas_text(pdu[6:])
    if pd == 2:
        return esm_details(pdu)
    if pd != 7 or len(pdu) < 2:
        return f"NAS pd={pd} sec={sec} {pdu[:8].hex()}"
    kind = pdu[1]
    name = EMM_TYPES.get(kind, f"EMM 0x{kind:02x}")
    extra = ""
    if kind in (0x44, 0x4B, 0x4E, 0x54, 0x5F, 0x60) and len(pdu) > 2:
        extra = f" cause #{pdu[2]} {EMM_CAUSES.get(pdu[2], '')}"
    elif kind == 0x45 and len(pdu) > 2:
        extra = f" type {pdu[2] & 0x7}"
        at = pdu.find(b"\x53", 3)
        if at > 0 and at + 1 < len(pdu):
            extra += f" cause #{pdu[at + 1]} {EMM_CAUSES.get(pdu[at + 1], '')}"
    if kind in (0x41, 0x44, 0x42):          # ESM container rides inside attach
        at = pdu.find(b"\x02", 3)
        if 0 < at < len(pdu):
            extra += f" [inner: {esm_details(pdu[at:])}]"
    return name + extra


def modem_ms(ts):
    """CDMA-style timestamp: the upper 48 bits count 1.25 ms units."""
    return (ts >> 16) * 1.25


def decode_packet(p, db):
    """Yield (modem ms, ssid, level, where, text) for one DIAG packet."""
    cmd = p[0]
    if cmd == 0x98 and len(p) > 8:
        # Subscription wrapper: version, reserved, u32 subscription id.
        sub = struct.unpack_from("<I", p, 4)[0]
        for ms, ssid, mask, where, text in decode_packet(p[8:], db):
            yield ms, ssid, mask, where, f"[sub{sub}] {text}"
    elif cmd == 0x79 and len(p) >= 20:
        nargs = p[2]
        ts, line, ssid, mask = struct.unpack_from("<QHHI", p, 4)
        args = struct.unpack_from(f"<{nargs}I", p, 20)
        strings = p[20 + 4 * nargs:].split(b"\0")
        fmt = strings[0].decode(errors="replace")
        src = strings[1].decode(errors="replace") if len(strings) > 1 else "?"
        yield modem_ms(ts), ssid, mask, f"{src}:{line}", render(fmt, args)
    elif cmd == 0x99 and len(p) >= 18:
        spec = p[2]
        size, count = spec >> 4, spec & 0xF
        ts, key = struct.unpack_from("<QI", p, 4)
        body = p[18:]
        args = [int.from_bytes(body[k * size:(k + 1) * size], "little")
                for k in range(count)] if size else []
        e = db.get(key)
        if e:
            mask, ssid, line, src, fmt = e
            yield modem_ms(ts), ssid, mask, f"{src}:{line}", render(fmt, args)
        else:
            yield modem_ms(ts), -1, 0, "?", f"unknown QSR4 key {key} args {args}"
    elif cmd == 0x10 and len(p) >= 16:
        _more, _len, _len2, code, ts = struct.unpack_from("<BHHHQ", p, 1)
        body = p[16:]
        if code in LOG_NAMES:
            # Four version bytes precede the NAS PDU.
            yield modem_ms(ts), -2, 0, LOG_NAMES[code], nas_text(body[4:])
        else:
            yield modem_ms(ts), -2, 0, f"log 0x{code:04x}", body[:32].hex()
    elif cmd == 0x92 and len(p) >= 24:
        ts, line, ssid, mask, h = struct.unpack_from("<QHHII", p, 4)
        yield modem_ms(ts), ssid, mask, f"?:{line}", f"QSR hash {h:#010x}"


def decode(path, db):
    data = open(path, "rb").read()
    i = 0
    while i + 12 <= len(data):
        t, n = struct.unpack_from("<dI", data, i)
        p = data[i + 12:i + 12 + n]
        i += 12 + n
        if p:
            for rec in decode_packet(p, db):
                yield (t,) + rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("qdb", nargs="?", default=DEFAULT_QDB)
    ap.add_argument("--ssid", type=int, action="append")
    ap.add_argument("--grep")
    a = ap.parse_args()
    db = load_qdb(a.qdb)
    t0 = None
    for t, ms, ssid, mask, where, text in decode(a.capture, db):
        if a.ssid and ssid not in a.ssid:
            continue
        if a.grep and a.grep.lower() not in (where + text).lower():
            continue
        t0 = t if t0 is None else t0
        print(f"{t - t0:9.3f} {ms / 1000:12.3f} {ssid:5} {LEVELS.get(mask, mask):5} "
              f"{where}: {text}")


if __name__ == "__main__":
    main()
