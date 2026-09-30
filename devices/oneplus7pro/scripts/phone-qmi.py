#!/usr/bin/env python3
"""Raw QMI over QRTR for requests libqmi 1.38 cannot express.

One process holds each client, so state the modem ties to a client (a UIM
logical channel, a PDC indication token) lives as long as the command. The
message layouts were read from `qmicli --verbose` on this phone; anything else
is marked where it is used. Every request has a timeout and closing the socket
releases the modem-side client.

Run on the phone:  python3 - isim < scripts/phone-qmi.py
Output with identities goes to stdout; keep it in ignored out/cellular/.
"""
import select
import socket
import struct
import sys

QRTR_PORT_CTRL = 0xFFFFFFFE
QRTR_TYPE_NEW_SERVER = 4
QRTR_TYPE_NEW_LOOKUP = 10
TIMEOUT = 10.0

SVC_UIM = 0x0B


class QmiError(Exception):
    def __init__(self, msg_id, error):
        super().__init__(f"message 0x{msg_id:04x} failed: QMI error {error}")
        self.error = error


def lookup(service):
    """Return (node, port) of the first QRTR server for a QMI service."""
    s = socket.socket(socket.AF_QIPCRTR, socket.SOCK_DGRAM)
    try:
        node, _ = s.getsockname()
        s.sendto(struct.pack("<5I", QRTR_TYPE_NEW_LOOKUP, service, 0, 0, 0),
                 (node, QRTR_PORT_CTRL))
        while True:
            r, _, _ = select.select([s], [], [], TIMEOUT)
            if not r:
                raise TimeoutError(f"no QRTR server for service {service}")
            cmd, svc, _inst, snode, sport = struct.unpack("<5I", s.recv(64)[:20])
            if cmd != QRTR_TYPE_NEW_SERVER or not svc:
                raise LookupError(f"QMI service {service} is not published")
            if svc == service:
                return snode, sport
    finally:
        s.close()


def tlv(t, value):
    return struct.pack("<BH", t, len(value)) + value


def parse_tlvs(data):
    out, i = {}, 0
    while i + 3 <= len(data):
        t, n = struct.unpack_from("<BH", data, i)
        out[t] = data[i + 3:i + 3 + n]
        i += 3 + n
    return out


class Client:
    def __init__(self, service):
        self.addr = lookup(service)
        self.sock = socket.socket(socket.AF_QIPCRTR, socket.SOCK_DGRAM)
        self.txn = 0
        self.indications = []

    def close(self):
        self.sock.close()

    def _receive(self, what):
        r, _, _ = select.select([self.sock], [], [], TIMEOUT)
        if not r:
            raise TimeoutError(f"no {what} within {TIMEOUT:.0f} s")
        data = self.sock.recv(65536)
        kind, txn, mid, n = struct.unpack_from("<BHHH", data)
        return kind, txn, mid, parse_tlvs(data[7:7 + n])

    def request(self, msg_id, tlvs=b"", check=True):
        self.txn = (self.txn + 1) & 0xFFFF or 1
        self.sock.sendto(struct.pack("<BHHH", 0, self.txn, msg_id, len(tlvs)) + tlvs,
                         self.addr)
        while True:
            kind, txn, mid, tl = self._receive(f"reply to 0x{msg_id:04x}")
            if kind == 4:
                self.indications.append((mid, tl))
            elif kind == 2 and txn == self.txn and mid == msg_id:
                result, error = struct.unpack("<HH", tl.get(0x02, b"\0\0\0\0"))
                if check and result:
                    raise QmiError(msg_id, error)
                return tl

    def indication(self, msg_id, token=None):
        """Wait for an indication, matched by its PDC-style token (TLV 0x10)."""
        def match(mid, tl):
            return mid == msg_id and (token is None or
                                      tl.get(0x10) == struct.pack("<I", token))
        while True:
            for i, (mid, tl) in enumerate(self.indications):
                if match(mid, tl):
                    return self.indications.pop(i)[1]
            kind, _, mid, tl = self._receive(f"indication 0x{msg_id:04x}")
            if kind == 4:
                self.indications.append((mid, tl))


class UimChannel:
    """A UIM logical channel to one application, closed on exit."""

    def __init__(self, slot, aid):
        self.slot = slot
        self.uim = Client(SVC_UIM)
        tl = self.uim.request(0x0042, tlv(0x01, bytes([slot])) +
                              tlv(0x10, bytes([len(aid)]) + aid))
        self.channel = tl[0x10][0]

    def close(self):
        try:
            self.uim.request(0x003F, tlv(0x01, bytes([self.slot])) +
                             tlv(0x11, bytes([self.channel])), check=False)
        finally:
            self.uim.close()

    def apdu(self, cla, ins, p1, p2, data=b"", le=None):
        # ISO 7816-4: logical channels 1-3 are the low CLA bits.
        cmd = bytes([cla | self.channel, ins, p1, p2])
        if data:
            cmd += bytes([len(data)]) + data
        if le is not None:
            cmd += bytes([le & 0xFF])
        tl = self.uim.request(0x003B, tlv(0x01, bytes([self.slot])) +
                              tlv(0x02, struct.pack("<H", len(cmd)) + cmd) +
                              tlv(0x10, bytes([self.channel])))
        raw = tl.get(0x10, b"")
        resp = raw[2:2 + struct.unpack_from("<H", raw)[0]] if len(raw) >= 2 else b""
        body, sw1, sw2 = resp[:-2], resp[-2], resp[-1]
        if sw1 == 0x61:                                    # more data waiting
            return self.apdu(0x00, 0xC0, 0, 0, le=sw2)
        if sw1 == 0x6C and le is not None:                 # wrong Le, card says
            return self.apdu(cla, ins, p1, p2, data, le=sw2)
        return body, (sw1 << 8) | sw2

    def select(self, fid):
        body, sw = self.apdu(0x00, 0xA4, 0x00, 0x04, struct.pack(">H", fid), le=0)
        if sw != 0x9000:
            return None
        fcp = parse_ber(body[2:]) if body[:1] == b"\x62" else {}
        info = {"size": int.from_bytes(fcp.get(0x80, b""), "big") or None}
        if 0x82 in fcp and len(fcp[0x82]) >= 5:
            info["reclen"] = int.from_bytes(fcp[0x82][2:4], "big")
            info["records"] = fcp[0x82][4]
        return info

    def read_binary(self, size):
        out = b""
        while len(out) < size:
            n = min(size - len(out), 0xFF)
            body, sw = self.apdu(0x00, 0xB0, len(out) >> 8, len(out) & 0xFF, le=n)
            if sw != 0x9000:
                break
            out += body
        return out

    def read_record(self, number, length):
        body, sw = self.apdu(0x00, 0xB2, number, 0x04, le=length)
        return body if sw == 0x9000 else None


def parse_ber(data):
    out, i = {}, 0
    while i + 2 <= len(data):
        tag, n = data[i], data[i + 1]
        out.setdefault(tag, data[i + 2:i + 2 + n])
        i += 2 + n
    return out


def tlv_string(data):
    """ISIM identities are a 0x80 tag holding a UTF-8 string."""
    if data and data[0] == 0x80 and len(data) >= 2:
        return data[2:2 + data[1]].decode("utf-8", "replace")
    return None


ISIM_AID_PREFIX = bytes.fromhex("A0000000871004")


def find_isim(slot):
    """Return the ISIM AID reported by UIM Get Card Status (message 0x002F)."""
    uim = Client(SVC_UIM)
    try:
        status = uim.request(0x002F).get(0x10, b"")
    finally:
        uim.close()
    # Card status: index/lengths, then per slot a card header and applications.
    # The AID is length-prefixed; locate it by the ISIM RID/app code.
    at = status.find(ISIM_AID_PREFIX)
    if at < 1:
        return None
    return status[at:at + status[at - 1]]


def cmd_isim(slot=1):
    aid = find_isim(slot)
    if not aid:
        print("no ISIM application on the card")
        return 1
    ch = UimChannel(slot, aid)
    try:
        print(f"isim channel {ch.channel}")
        for name, fid in (("IMPI", 0x6F02), ("DOMAIN", 0x6F03), ("IST", 0x6F07)):
            info = ch.select(fid)
            if not info:
                print(f"{name}: not present")
                continue
            data = ch.read_binary(info["size"] or 0)
            text = tlv_string(data)
            print(f"{name}: {text if text is not None else data.hex()}")
        for name, fid in (("IMPU", 0x6F04), ("PCSCF", 0x6F09)):
            info = ch.select(fid)
            if not info or "reclen" not in info:
                print(f"{name}: not present")
                continue
            for n in range(1, info["records"] + 1):
                rec = ch.read_record(n, info["reclen"])
                if rec is None or set(rec) <= {0xFF}:
                    continue
                if name == "PCSCF" and len(rec) > 3 and rec[0] == 0x80:
                    print(f"{name}[{n}]: type {rec[2]} "
                          f"{rec[3:2 + rec[1]].decode('utf-8', 'replace')}")
                else:
                    print(f"{name}[{n}]: {tlv_string(rec) or rec.hex()}")
    finally:
        ch.close()
    return 0


SVC_PDC = 0x24
PDC_SOFTWARE = 1


class Pdc:
    """Persistent Device Configuration (carrier MCFG) client.

    Layouts are libqmi 1.38's qmi-service-pdc.json. The subscription TLVs are
    not in libqmi: 0x11 on Get/Set Selected and Deactivate, 0x12 on Activate,
    as the 7T Pro reference read them from OxygenOS's RIL.
    """

    def __init__(self):
        self.qmi = Client(SVC_PDC)
        self.token = 0x6F70  # arbitrary; incremented per request
        self.qmi.request(0x20, tlv(0x10, b"\x01"))  # enable reporting

    def close(self):
        self.qmi.close()

    def _call(self, msg_id, tlvs):
        self.token += 1
        self.qmi.request(msg_id, tlvs + tlv(0x10, struct.pack("<I", self.token)))
        ind = self.qmi.indication(msg_id, self.token)
        result = struct.unpack("<H", ind.get(0x01, b"\0\0"))[0]
        if result:
            raise QmiError(msg_id, result)
        return ind

    @staticmethod
    def _sub(t, sub):
        return tlv(t, struct.pack("<I", sub)) if sub is not None else b""

    @staticmethod
    def _type_id(config_id):
        return tlv(0x01, struct.pack("<IB", PDC_SOFTWARE, len(config_id)) + config_id)

    def selected(self, sub=None):
        ind = self._call(0x22, tlv(0x01, struct.pack("<I", PDC_SOFTWARE)) +
                         self._sub(0x11, sub))
        ids = []
        for t in (0x11, 0x12):
            v = ind.get(t)
            ids.append(v[1:1 + v[0]] if v else None)
        return ids  # active, pending

    def configs(self):
        ind = self._call(0x24, tlv(0x11, struct.pack("<I", PDC_SOFTWARE)))
        raw, out, i = ind.get(0x11, b"\0"), [], 1
        for _ in range(raw[0]):
            n = raw[i + 4]
            out.append(raw[i + 5:i + 5 + n])
            i += 5 + n
        return out

    def info(self, config_id):
        ind = self._call(0x28, self._type_id(config_id))
        desc = ind.get(0x12, b"\0")
        return {"size": struct.unpack("<I", ind[0x11])[0] if 0x11 in ind else None,
                "name": desc[1:1 + desc[0]].decode("latin1"),
                "version": struct.unpack("<I", ind[0x13])[0] if 0x13 in ind else None}

    def limits(self):
        self.token += 1
        tl = self.qmi.request(0x29, tlv(0x01, struct.pack("<I", PDC_SOFTWARE)) +
                              tlv(0x10, struct.pack("<I", self.token)))
        if 0x11 not in tl:
            tl = self.qmi.indication(0x29, self.token)
        return (struct.unpack("<Q", tl[0x11])[0] if 0x11 in tl else None,
                struct.unpack("<Q", tl[0x12])[0] if 0x12 in tl else None)


def describe(pdc, config_id):
    if not config_id:
        return "-"
    try:
        i = pdc.info(config_id)
        return f"{i['name']} v0x{i['version']:08x} ({config_id.hex()[:8]}…)"
    except QmiError as e:
        return f"{config_id.hex()[:8]}… (info: {e})"


def cmd_pdc_state():
    pdc = Pdc()
    try:
        try:
            print("limits (max, used):", pdc.limits())
        except (QmiError, TimeoutError) as e:
            print("limits:", e)
        for sub in (None, 0, 1):
            try:
                active, pending = pdc.selected(sub)
                print(f"selected sub={sub}: active={describe(pdc, active)} "
                      f"pending={describe(pdc, pending)}")
            except (QmiError, TimeoutError) as e:
                print(f"selected sub={sub}: {e}")
        configs = pdc.configs()
        print(f"resident software configs: {len(configs)}")
        for c in configs:
            print(f"  {c.hex()}")
    finally:
        pdc.close()
    return 0


SVC_NAS = 0x03
RADIO = {0: "none", 1: "cdma1x", 2: "hdr", 4: "gsm", 5: "umts", 8: "lte", 9: "tdscdma", 12: "5gnr"}
DOMAIN = {0: "none", 1: "cs", 2: "ps", 3: "cs-ps", 4: "camped"}
REG = {0: "not-registered", 1: "registered", 2: "searching", 3: "denied", 4: "unknown"}


def cmd_nas_watch(seconds="300"):
    """Print network rejects (with cause) and registration changes as they
    happen. Layouts: libqmi qmi-service-nas.json (Register Indications 0x0003,
    Network Reject 0x0068, Serving System 0x0024)."""
    import time
    nas = Client(SVC_NAS)
    try:
        nas.request(0x0003, tlv(0x13, b"\x01") + tlv(0x21, b"\x01\x00"))
        end = time.monotonic() + float(seconds)
        print("watching", flush=True)
        while time.monotonic() < end:
            try:
                kind, _, mid, tl = nas._receive("indication")
            except TimeoutError:
                continue
            stamp = time.strftime("%H:%M:%S")
            if kind == 4 and mid == 0x0068:
                plmn = ""
                if 0x10 in tl:
                    mcc, mnc, pcs = struct.unpack("<HHB", tl[0x10][:5])
                    plmn = f" plmn {mcc}-{mnc:0{3 if pcs else 2}d}"
                print(f"{stamp} REJECT rat={RADIO.get(tl[0x01][0], tl[0x01][0])} "
                      f"domain={DOMAIN.get(tl[0x02][0], tl[0x02][0])} "
                      f"cause=#{tl[0x03][0]}{plmn}", flush=True)
            elif kind == 4 and mid == 0x0024 and 0x01 in tl:
                reg, cs, ps, net, nif = struct.unpack_from("<BBBBB", tl[0x01])
                rats = [RADIO.get(b, b) for b in tl[0x01][5:5 + nif]]
                print(f"{stamp} serving reg={REG.get(reg, reg)} cs={cs} ps={ps} rats={rats}",
                      flush=True)
    finally:
        nas.close()
    return 0


def cmd_nas_usage(usage, duration="power-cycle", modes=None):
    """Set the UE usage setting (TS 24.301): a voice-centric UE without voice
    service on LTE disables LTE and looks for 2G/3G; a data-centric one stays.
    NAS Set System Selection Preference 0x0033: TLV 0x21 usage (1 voice-centric,
    2 data-centric), TLV 0x17 change duration (0 until power cycle, 1 permanent),
    optional TLV 0x11 mode preference ("lte" = LTE only, bit 4)."""
    value = {"voice": 1, "data": 2}[usage]
    dur = {"power-cycle": 0, "permanent": 1}[duration]
    nas = Client(SVC_NAS)
    try:
        extra = tlv(0x11, struct.pack("<H", 1 << 4)) if modes == "lte" else b""
        nas.request(0x0033, tlv(0x21, struct.pack("<I", value)) + tlv(0x17, bytes([dur])) + extra)
        print(f"usage preference set to {usage}-centric ({duration})"
              + (", LTE only" if extra else ""))
    finally:
        nas.close()
    return 0


SVC_WDS = 0x01
EMBEDDED_ENDPOINT, IPA_INTERFACE, TETHERED = 4, 1, 1   # as ModemManager uses for ipa


def ipv6_text(b):
    return socket.inet_ntop(socket.AF_INET6, b[:16])


def ipv4_text(v):
    return socket.inet_ntoa(struct.pack(">I", struct.unpack("<I", v)[0]))


def wds_start(family, apn, mux):
    """Bind a WDS client to the IPA mux and start a call; return (client,
    handle, settings). Layouts: libqmi qmi-service-wds.json (Bind Mux Data Port
    0x00A2, Set IP Family 0x004D, Start Network 0x0020, Get Current Settings
    0x002D)."""
    wds = Client(SVC_WDS)
    wds.request(0x00A2, tlv(0x10, struct.pack("<II", EMBEDDED_ENDPOINT, IPA_INTERFACE)) +
                tlv(0x11, bytes([mux])) + tlv(0x13, struct.pack("<I", TETHERED)))
    wds.request(0x004D, tlv(0x01, bytes([family])))
    start = tlv(0x19, bytes([family]))
    if apn:
        start += tlv(0x14, apn.encode())
    tl = wds.request(0x0020, start, check=False)
    result, error = struct.unpack("<HH", tl[0x02])
    if result:
        reason = struct.unpack("<H", tl[0x10])[0] if 0x10 in tl else None
        verbose = struct.unpack("<Hh", tl[0x11]) if 0x11 in tl else None
        wds.close()
        raise QmiError(0x0020, f"{error} (call end {reason}, verbose type/reason {verbose})")
    handle = struct.unpack("<I", tl[0x01])[0]
    cur = wds.request(0x002D, tlv(0x10, struct.pack("<I", 0xFFFF)))
    s = {"mtu": struct.unpack("<I", cur[0x29])[0] if 0x29 in cur else None}
    if 0x14 in cur:
        s["apn"] = cur[0x14][1:1 + cur[0x14][0]].decode(errors="replace") \
            if cur[0x14][:1] and cur[0x14][0] < len(cur[0x14]) else cur[0x14].decode(errors="replace")
    if 0x1E in cur:
        s["ipv4"] = ipv4_text(cur[0x1E])
        s["gw4"] = ipv4_text(cur[0x20]) if 0x20 in cur else None
        s["mask4"] = ipv4_text(cur[0x21]) if 0x21 in cur else None
        s["dns4"] = [ipv4_text(cur[t]) for t in (0x15, 0x16) if t in cur]
    if 0x25 in cur:
        s["ipv6"] = ipv6_text(cur[0x25])
        s["prefix6"] = cur[0x25][16]
        s["gw6"] = ipv6_text(cur[0x26]) if 0x26 in cur else None
        s["dns6"] = [ipv6_text(cur[t]) for t in (0x27, 0x28) if t in cur]
    return wds, handle, s


SVC_DPM = 0x2F


def dpm_open_ipa_port():
    """Tell the modem about the IPA hardware data port, as ModemManager does
    for "ipa" (mm-port-qmi.c dpm_open_port): DPM Open Port 0x0020, TLV 0x11
    hardware data ports {endpoint type, interface, RX endpoint, TX endpoint},
    where the port's RX is the modem TX endpoint and its TX the modem RX one,
    read from the IPA driver's sysfs. Without it WDS Bind Mux Data Port fails
    with Internal. Returns the DPM client, which is kept for the session."""
    base = "/sys/class/net/rmnet_ipa0/device/modem/"
    tx = int(open(base + "tx_endpoint_id").read())
    rx = int(open(base + "rx_endpoint_id").read())
    dpm = Client(SVC_DPM)
    dpm.request(0x0020, tlv(0x11, bytes([1]) +
                              struct.pack("<IIII", EMBEDDED_ENDPOINT, IPA_INTERFACE, tx, rx)))
    print(f"DPM port open: embedded/{IPA_INTERFACE}, rx {tx}, tx {rx}", flush=True)
    return dpm


SVC_WDA = 0x1A
RAW_IP, QMAPV4 = 2, 8


def wda_set_qmap():
    """Match the modem's embedded data port to IPA's framing, as
    ModemManager's data-format setup does: raw IP with QMAPv4 (QMAP plus MAPv4
    checksum offload, which IPA v4.1 uses) both ways. Out of the box the port is
    raw IP without aggregation, and the modem drops QMAP uplink frames.
    WDA Set Data Format 0x0020: 0x11 link layer, 0x12/0x13 UL/DL aggregation,
    0x15/0x16 DL max datagrams/size (IPA RX buffers are 8 KiB), 0x17 endpoint."""
    wda = Client(SVC_WDA)
    try:
        tl = wda.request(0x0020, tlv(0x11, struct.pack("<I", RAW_IP)) +
                         tlv(0x12, struct.pack("<I", QMAPV4)) +
                         tlv(0x13, struct.pack("<I", QMAPV4)) +
                         tlv(0x15, struct.pack("<I", 32)) +
                         tlv(0x16, struct.pack("<I", 8192)) +
                         tlv(0x17, struct.pack("<II", EMBEDDED_ENDPOINT, IPA_INTERFACE)))
        got = {t: struct.unpack("<I", v)[0] for t, v in tl.items() if t in (0x11, 0x12, 0x13, 0x15, 0x16) and len(v) == 4}
        print(f"WDA data format: {got}", flush=True)
    finally:
        wda.close()


def sh(cmd):
    import subprocess
    print("+", cmd, flush=True)
    subprocess.run(cmd, shell=True, check=False)


def cmd_data_up(families="6", apn="", seconds="600", configure="configure"):
    """Start data calls (families "4", "6" or "46") on IPA mux 1 and, unless
    told "noconfigure", give rmnet_data0 the addresses with policy routing
    (table 200) so Wi-Fi and USB routes stay untouched. Holds the calls for
    SECONDS or until SIGTERM, then stops them."""
    import signal
    import time
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    calls = []
    dpm = dpm_open_ipa_port()
    wda_set_qmap()
    try:
        for fam in families:
            wds, handle, s = wds_start(int(fam), apn, 1)
            calls.append((wds, handle))
            print(f"IPv{fam} call up, handle {handle:#x}: {s}", flush=True)
            if configure != "configure":
                continue
            sh("ip link set rmnet_ipa0 up")
            sh("ip link show rmnet_data0 >/dev/null 2>&1 || ip link add link rmnet_ipa0 "
               "name rmnet_data0 type rmnet mux_id 1 ingress-deaggregation on "
               "ingress-mapv4-checksum on egress-mapv4-checksum on")
            sh(f"ip link set rmnet_data0 mtu {s['mtu'] or 1500} up")
            if fam == "4":
                sh(f"ip addr add {s['ipv4']}/32 dev rmnet_data0")
                sh("ip route replace default dev rmnet_data0 table 200")
                sh(f"ip rule add from {s['ipv4']} lookup 200")
            else:
                sh(f"ip -6 addr add {s['ipv6']}/{s['prefix6']} dev rmnet_data0 nodad")
                sh("ip -6 route replace default dev rmnet_data0 table 200")
                sh(f"ip -6 rule add from {s['ipv6']} lookup 200")
        end = time.monotonic() + float(seconds)
        while not stop and time.monotonic() < end:
            time.sleep(1)
    finally:
        for wds, handle in calls:
            try:
                wds.request(0x0021, tlv(0x01, struct.pack("<I", handle)), check=False)
            finally:
                wds.close()
        dpm.close()
        print("calls stopped", flush=True)
    return 0


IMSDCM_SERVICE = 0x302
QRTR_TYPE_NEW_SERVER_CMD, QRTR_TYPE_DEL_SERVER_CMD = 4, 5


def dcm_frame(kind, txn, msg_id, tlvs):
    return struct.pack("<BHHH", kind, txn, msg_id, len(tlvs)) + tlvs


def dcm_timezone(txn, pdp, sequence):
    """0x32 WLAN timezone reply, as DiamaneOS documents the stock one: eight
    u16 local fields (sec, min, hour, day, month, year, weekday from Sunday,
    standard offset west of UTC in 15-minute units), then u64 UTC seconds."""
    import time
    now = time.time()
    lt = time.localtime(now)
    west = -((-time.timezone) // 900)
    fields = [lt.tm_sec, lt.tm_min, lt.tm_hour, lt.tm_mday, lt.tm_mon, lt.tm_year,
              (lt.tm_wday + 1) % 7, west & 0xFFFF]
    return dcm_frame(2, txn, 0x32, tlv(0x02, b"\0\0\0\0") + tlv(0x03, bytes([pdp])) +
                     tlv(0x04, struct.pack("<I", sequence)) +
                     tlv(0x05, struct.pack("<8HQ", *fields, int(now))))


def cmd_ims_dcm(seconds="0"):
    """Serve QMI IMS DCM (service 0x302) to the modem's IMS stack, which asks
    the AP for its IMS PDN instead of opening it itself (AP server, modem
    client; openimsd.de). Activation (0x20): acknowledge, start a WDS call on
    the requested profile and family on a private IPA mux, then report the
    address in an indication. Layouts: libqmi qmi-service-imsdcm.json and
    DiamaneOS hardware_diamaneos_ims docs/dcm-protocol.md."""
    import time
    s = socket.socket(socket.AF_QIPCRTR, socket.SOCK_DGRAM)
    node, port = s.getsockname()
    s.sendto(struct.pack("<5I", QRTR_TYPE_NEW_SERVER_CMD, IMSDCM_SERVICE, 1, 0, 0),
             (node, QRTR_PORT_CTRL))
    print(f"IMS DCM published on {node}:{port}", flush=True)
    sessions = {}          # pdp id -> (peer, wds client, handle, activation)
    sequence = {}
    next_pdp, next_mux = 1, 10
    end = time.monotonic() + float(seconds) if float(seconds) else None

    def indicate(peer, pdp, a, address):
        sequence[peer] = sequence.get(peer, 0) + 1
        body = tlv(0x02, b"\0\0" + (b"\0\0" if address else b"\x0d\0")) + tlv(0x01, bytes([pdp]))
        if a["cookie"] is not None:
            body += tlv(0x10, struct.pack("<I", a["cookie"]))
        if address:
            fam = 0 if ":" not in address else 1
            body += tlv(0x11, struct.pack("<IB", fam, len(address)) + address.encode())
        body += tlv(0x12, struct.pack("<I", a["instance"] or 0))
        s.sendto(dcm_frame(4, sequence[peer], 0x20, body), peer)

    try:
        while end is None or time.monotonic() < end:
            r, _, _ = select.select([s], [], [], 1)
            if not r:
                continue
            data, peer = s.recvfrom(65536)
            if peer[1] == QRTR_PORT_CTRL or len(data) < 7:
                continue
            kind, txn, mid, n = struct.unpack_from("<BHHH", data)
            if kind != 0:
                continue
            tl = parse_tlvs(data[7:7 + n])
            u32 = lambda t: struct.unpack("<I", tl[t])[0] if t in tl else None
            stamp = time.strftime("%H:%M:%S")
            if mid == 0x20:
                p = tl.get(0x01, b"")
                ln = p[0] if p else 0
                apn = p[1:1 + ln].decode(errors="replace")
                apn_type, rat, family, profile = struct.unpack_from("<4I", p, 1 + ln)
                a = {"cookie": u32(0x10), "instance": u32(0x13)}
                print(f"{stamp} activate apn={apn!r} type={apn_type} rat={rat} "
                      f"family={'v6' if family else 'v4'} profile={profile} "
                      f"seq={a['cookie']} sub={u32(0x11)} slot={u32(0x12)} inst={a['instance']}",
                      flush=True)
                pdp, next_pdp = next_pdp, next_pdp + 1
                resp = tlv(0x02, b"\0\0\0\0") + tlv(0x10, bytes([pdp])) + \
                    tlv(0x11, struct.pack("<I", a["cookie"] or 0))
                if a["instance"] is not None:
                    resp += tlv(0x12, struct.pack("<I", a["instance"]))
                s.sendto(dcm_frame(2, txn, 0x20, resp), peer)
                try:
                    wds, handle, st = wds_start(6 if family else 4, apn, next_mux)
                    next_mux += 1
                    address = st.get("ipv6" if family else "ipv4")
                    sessions[pdp] = (peer, wds, handle, a)
                    print(f"{stamp}   call up pdp {pdp}: {st}", flush=True)
                    indicate(peer, pdp, a, address)
                except (QmiError, TimeoutError, OSError) as e:
                    print(f"{stamp}   call failed: {e}", flush=True)
                    indicate(peer, pdp, a, None)
            elif mid == 0x21:
                pdp = tl.get(0x01, b"\0")[0]
                inst = u32(0x10)
                found = sessions.pop(pdp, None)
                print(f"{stamp} deactivate pdp {pdp} ({'known' if found else 'unknown'})", flush=True)
                resp = tlv(0x02, b"\0\0\0\0" if found else b"\x01\0\x1c\0") + tlv(0x10, bytes([250]))
                if inst is not None:
                    resp += tlv(0x11, struct.pack("<I", inst))
                s.sendto(dcm_frame(2, txn, 0x21, resp), peer)
                if found:
                    _, wds, handle, _ = found
                    wds.request(0x0021, tlv(0x01, struct.pack("<I", handle)), check=False)
                    wds.close()
            elif mid == 0x22:
                print(f"{stamp} get-ip request (no reply, as stock)", flush=True)
            elif mid in (0x23, 0x2E, 0x33, 0x34):
                print(f"{stamp} request 0x{mid:02x} acknowledged", flush=True)
                s.sendto(dcm_frame(2, txn, mid, tlv(0x02, b"\0\0\0\0")), peer)
            elif mid == 0x32:
                pdp = tl.get(0x01, b"\0")[0]
                s.sendto(dcm_timezone(txn, pdp, u32(0x02) or 0), peer)
                print(f"{stamp} timezone request answered", flush=True)
            else:
                code = b"\x01\0\0\0" if mid in range(0x25, 0x2B) or mid in (0x2C, 0x2D, 0x31) \
                    else b"\x01\0\x3a\0"
                print(f"{stamp} request 0x{mid:02x} refused", flush=True)
                s.sendto(dcm_frame(2, txn, mid, tlv(0x02, code)), peer)
    finally:
        for peer, wds, handle, _ in sessions.values():
            wds.request(0x0021, tlv(0x01, struct.pack("<I", handle)), check=False)
            wds.close()
        s.sendto(struct.pack("<5I", QRTR_TYPE_DEL_SERVER_CMD, IMSDCM_SERVICE, 1, 0, 0),
                 (node, QRTR_PORT_CTRL))
        s.close()
    return 0


COMMANDS = {"isim": cmd_isim, "pdc-state": cmd_pdc_state, "nas-watch": cmd_nas_watch,
            "nas-usage": cmd_nas_usage, "data-up": cmd_data_up, "ims-dcm": cmd_ims_dcm}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        sys.exit(f"usage: phone-qmi.py {{{','.join(COMMANDS)}}} [args]")
    sys.exit(COMMANDS[sys.argv[1]](*sys.argv[2:]))
