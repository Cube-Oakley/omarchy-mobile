# First SIM: identity, carrier profile, LTE registration and a modem assertion — September 26, 2026

Phase 4 of the [cellular plan](cellular-plan-20260922.md), on kernel #194 with
the Tello SIM (T-Mobile network, physical nano-SIM) that the user activated
online. The phone had been running since before the card went in (hot insert).
Raw logs, with identifiers, are in ignored `out/cellular/sim-20260926/`. No
ICCID, IMSI, IMEI or phone number appears here.

**Result:** the SIM reads, the modem picks the right carrier profile by
itself, and it registers on LTE within three seconds of going online. About
100 seconds later the modem firmware asserts in its RF link manager, the same
assertion that stopped the OnePlus 7T Pro port. Data, SMS and calls are
blocked on that crash.

## The card

Slot 1 holds a USIM and an ISIM; slot 2 is empty (`no-atr-received`). The
firmware opens no provisioning session on its own, so both applications sit at
`detected` until one is opened; postmarketOS's `msm-modem-uim-selection` does
the same:

```sh
qmicli -d qrtr://0 --uim-change-provisioning-session=\
"session-type=primary-gw-provisioning,activate=yes,slot=1,aid=<USIM AID>"
```

The USIM is then `ready`, with PIN1 disabled (3/10 attempts, untouched) and
personalization `ready`: the phone is not SIM-locked against this card.

| Field | Value |
| --- | --- |
| ICCID issuer | `8901240` |
| IMSI home network | 310-240, MNC length 3 |
| Equivalent home networks (EHPLMN) | 310-260, 310-240 |
| GID1 | `6941` |
| SMS centre (EF_SMSP) | +12063130004 |
| MSISDN, SPN, GID2 | not on the card |
| Forbidden network | 310-830 |
| USIM services | SMS, SMS parameters, EPS mobility management; no SMS-over-IP bit |
| ISIM domain | `msg.pc.t-mobile.com` |
| ISIM identities | IMPI and one IMPU, both IMSI-based; no `tel:` URI |
| ISIM service table | `E2`: GBA, SMS storage and reports, **SMS over IP** |

The phone number is not stored anywhere on the card; IMS registration returns
it. The ISIM carries T-Mobile's own IMS domain, so VoLTE registers directly
with T-Mobile.

ISIM files need a UIM logical channel, and over QRTR each `qmicli` process is
its own client: the modem closes the channel when the process exits, so a
second `qmicli --uim-send-apdu` gets `InvalidArgument`.
`scripts/phone-qmi.py` is a small raw QMI-over-QRTR client (Python's
`AF_QIPCRTR`, no build step) that holds one client for the whole exchange.
Message layouts come from `qmicli --verbose` on this phone and from libqmi
1.38's service definitions. Run it on the phone with
`python3 - isim < scripts/phone-qmi.py`.

## Carrier profile: the modem chose it

Opening the USIM session was enough. The modem requested
`mcfg_sw/generic/na/tmo/commerci/mcfg_sw.mbn` (74,136 bytes) from
`tqftpserv`, stored it in place of the resident `CZ-VoLTE-Vodafone`, and made
it active on subscription 0:

```text
selected sub=0: active=Commercial-TMO v0x08010515 (cb45c810…) pending=-
selected sub=1: active=Free-VoLTE v0x08019a01 (a41c76a9…) pending=-
```

That is the profile the [baseline](cellular-baseline-20260922.md) identified,
by SHA-1. Its IIN list lacks this card's `8901240`, but its PLMN list has
310-240; the modem matched on the IMSI. No load, select or activate was sent.
`phone-qmi.py pdc-state` confirms that PDC's subscription field (TLV 0x11,
from the 7T Pro reference) is honoured here: subscription 1 still reports
`Free-VoLTE`.

The replacement lives only in rmtfs's RAM. With `-r`, rmtfs reads each EFS
partition once `O_RDONLY` into a buffer and closes it (`storage.c`); the
running daemon holds no descriptor on `/dev/sdf*`. After a reboot the modem
reads the unchanged partitions and selects the profile again when the USIM
session opens.

The profile brings T-Mobile's APNs: `fast.t-mobile.com` (default), `ims`,
`sos` (emergency), `tmus`, `h2g2`, `mms`. Mode preference is LTE first, voice
domain `ps-preferred`. The hardware reports LTE bands 1–5, 7, 8, 12, 13, 17–20,
25, 26, 28–30, 34, 38–41, 46, 48, 66 and 71, which covers T-Mobile's 2, 4, 12,
66 and 71.

## Online

`qmicli --dms-set-operating-mode=online` at 21:19:22. Three seconds later:

```text
Registration state: 'registered'   CS: 'attached'   PS: 'attached'
Radio interfaces: lte              Roaming: off
Current PLMN: 310-260 'Tello'      RSRP -90 dBm, RSRQ -13 dB, SNR 9.4 dB
```

CS attached over LTE means the network accepted a combined attach, so SMS over
SGs is a possibility alongside SMS over IMS. The kernel logged nothing.

## The assertion

At 21:21:05, 103 seconds after going online and with nothing else issued to
the modem:

```text
qcom_q6v5_pas 4080000.remoteproc: watchdog received:
rflm_diag_error.cc:368:RFLM@qsf_hl_seq.c:118 Assertion (rflm_qlnk_ls_retry_cnt < 2) failed
remoteproc remoteproc0: crash detected in modem: type watchdog
```

`ipa.ko` was loaded 37 seconds later (21:21:42), so IPA is not the trigger; it
then timed out awaiting the modem (`error -110 awaiting init driver
response`). Modem recovery is disabled, so the modem stayed down, and Wi-Fi
with it: WCN3990's firmware runs on the MPSS, and ath10k's restart failed
with mac80211 warnings.

This is the 7T Pro's assertion. There it fired right after online with a
PIN-locked card and no active software MCFG, and missing MCFG was the working
theory. Here the matching profile was active, the card ready and the phone
registered, so that theory does not hold. Every earlier modem watchdog in this
repository's logs is the generic `SFR Init: wdog or kernel error suspected`;
this is the first QLink failure on this phone.

`rflm_qlnk` is the RF link manager's QLink, the serial link from the baseband
to the SDR8150 transceiver. The assertion says re-establishing that link
failed twice. Registration worked, and the crash came after the connection
had time to go idle, which suggests the link fails when the RF wakes from a
low-power state rather than at first bring-up. That is an inference from
timing only.

Checked against the stock OnePlus kernel (`.work/lineage-kernel`) and the live
phone, all read-only. None explains it:

| Candidate | Finding |
| --- | --- |
| QLink sideband pins | GPIO 61 `qlink_request`, 62 `qlink_enable`: function 1, unclaimed by Linux; stock DT never touches them either |
| TLMM interrupt routing | GPIO 61/62 `INTR_CFG` 0xe2 (target 7, disabled), same as their neighbours |
| QLink clock reference | `GCC_RX2_QLINK_CLKREF_EN` (0x18c018) = 0x3, enabled; no mainline clock maps to it |
| Unused clocks, domains, regulators | Boot has `clk_ignore_unused pd_ignore_unused regulator_ignore_unused` |
| AP-side LDO modes | Stock initializes nearly every LDO to LPM too; the AP holds no HPM vote the modem could lean on |
| RF clocks | The AP holds no `rf_clk1` vote, but RPMh aggregates per processor, so the modem's own vote applies |
| LLCC modem slices | Mainline's SM8150 table matches stock for slices 7, 8, 9, 20, 21 and 29 (21 is `modem_paging` downstream, `MDMHPFX` in mainline) |

A LineageOS report shows the same failure family on an SDM845 phone under
Android ([LineageOS/issues#479](https://github.com/LineageOS/issues/issues/479),
"RF stuck in QLINK start state"), so it is not unique to mainline Linux.

## Recovery reboot incident

`phone-recovery-reboot.sh` stopped every Arch process, then refused to reboot:
ten `mount -o remount,ro /newroot` attempts returned `Resource busy`, with no
process rooted in `/newroot`, no open file there, no swap and no loop device.
Following the script's rule, it left the phone unrebooted (SSH gone, port 23
recovery shell up). From that shell: SysRq `s` then `u` (emergency read-only
remount), confirmed `/dev/sda19 /newroot ext4 ro`, then `busybox reboot -f`.
The next mount was clean (boot `a67e799f-d03b-4326-a8f2-ce17d8952cf6`): modem
running in `shutting-down`, radio ready after about 36 s, Wi-Fi HTTPS 200,
shell up. The kernel-side holder was not identified; the dead Wi-Fi or the
half-initialized IPA driver are the likely suspects.

Launching the script also needs care: it must run from the outer root
(`setsid -f chroot /proc/1/root /bin/busybox sh …`), and the upload has to be
checked, because a first `cat >` over SSH left an empty file.

## Second round: timing and the modem's own log

### Android is not a safe comparison

`/dev/sda19`, the Arch root, is Android's `userdata` partition (label
`archroot`, partition name `userdata`). Booting LineageOS on slot A would try
to mount it as encrypted Android data and could format it. The slot A test is
off the table until the Arch root is backed up or moved.

### Timed trials

`scripts/phone-online-trial.py` refuses to run unless rmtfs has `-r`, the modem
is running and offline, modem recovery is disabled and `qcom_stats` is loaded.
It opens the USIM session, checks the active profile with `phone-qmi.py`, goes
online, and samples the modem's SMEM sleep counters
(`/sys/kernel/debug/qcom_stats/modem`, from `out/sleep-stats/qcom_stats.ko`)
every 0.2 s and NAS every 2 s. Offline, the modem power-collapses constantly
(809 entries on one boot).

| Run | Boot | Registered | Assertion after online |
| --- | --- | --- | --- |
| 1 | up two days | at 3 s | 103 s |
| 2 | fresh | never | 1.3 s, while searching, 0.9 s after a sleep exit |
| 3 | fresh, DIAG capture | 2 s; data detached at 14 s, registration lost at 31 s | about 112 s |

All three are the same `rflm_qlnk_ls_retry_cnt < 2` assertion. Run 2 had modem
recovery enabled: the kernel died while `recovering modem` and the phone
rebooted (panic, nothing in ramoops). **Keep modem recovery disabled**; the
trial script now refuses otherwise.

### The modem's own log

The modem image ships its QShrink 4 string database,
`.work/guacamole-radio-firmware/image/qdsp6m.qdb` (zlib after a 64-byte
header; lines are `<key>:<level>:<ssid>:<line>:<file>:<format>`). Without any
capture it already explains the assertion. `rflm_qlnk.cpp` logs "QLNK fails
to start link in LS mode" / "StartLinkLS Failure!": LS is the low-speed link
start, before the high-speed gear. `rflm_qlnk_thread.c` logs "Starting rf qlink
cold start", "wakeup | scenario | is_quick_start" and "sleep", so the link is
restarted on RF wakeups. All QLink messages are SSID 14 (RF).

To record the log:

- linux-msm `diag` (`.work/diag`, commit `23c12c1`) is built on the phone as
  `/root/radio-bringup/src/diag/diag-router`, with libqrtr's sources compiled
  in. The modem makes its DIAG handshake only with a router present when it
  boots, so `guacamole-radio-start` (and `scripts/phone-radio-start.sh`)
  starts the router before rmtfs brings the modem up, **only when
  `/root/radio-bringup/cellular/diag-at-boot` exists**. Delete that file to
  turn it off. The previous script is kept as
  `/usr/local/sbin/guacamole-radio-start.before-diag`.
- `scripts/phone-diag-capture.py` connects to the router's abstract socket
  (`\0diag` padded to 108 bytes), sets ERROR/FATAL for every SSID range and
  MED and up for SSID 14, and records every packet. The router's "set all
  masks" command segfaults when the result is a partial mask (a NULL range
  reaches `diag_cmd_get_msg_mask`), so the client sets each range.
- `scripts/decode_diag.py` decodes `0x79` (text) and `0x99` (QShrink 4:
  timestamp, u32 qdb key, `0xc540` database id, then args packed as
  `(size << 4) | count`) against the qdb. `0x92` messages come from the WLAN
  firmware, whose database is not in the image.

Run 3's capture (`out/cellular/trial-20260926/capture-2.{bin,txt}`, 1509
QShrink 4 and 898 text messages) shows:

- the modem's GNSS engine running over QLink for the first seconds online
  (`GNSS_HS act 1`, QSleep programming), then LTE registration;
- `rflte_dispatch_exit_mode_req: Exiting LTE stack` at the data detach (about
  14 s) and no RF activity after that except a GNSS chain-off at about 77 s;
- the first modem power collapse since going online at 109 s, then an LTE
  search, another stack exit, and **`Starting rf qlink wakeup | scenario:1 |
  is_quick_start:0` twice, 0.55 s apart, then the assertion**. Those were the
  only two QLink wakeups in the run. The final `StartLinkLS Failure!` lines
  were not delivered before the crash;
- modem timer callbacks overrunning their budget around both wakeups
  (`timer_slaves.c`: 0x1c87 ticks against a 0x124f threshold);
- unrelated configuration complaints: "Invalid LTE Band 168", "LTE band 168 not
  present in SYS_BAND", "Unrecogonized ASDIV configuration 7", and
  `CA_MAX_TX_POWER_NV_DATA_TYPE Not parsed`.

With one wakeup succeeding (run 3 registered at 2 s) and another failing within
1.3 s (run 2), the link start looks probabilistic rather than always wrong.

### More candidates ruled out

| Candidate | Finding |
| --- | --- |
| SMEM project data | This bootloader writes OPLUS-format project info at item 135 (project 18821, RF 2, PCB 21), not the OnePlus item 136 `project_info.c` expects. The LineageOS kernel only reads it |
| RF test-cable state | Stock writes the cable state into item 135's `Feature[1]`; it reads 0 (no cable), the value stock would write |
| AP RF clock votes | Stock holds no `RF_CLK1` vote either (its `RF_CLK3` consumer is the WiGig driver) |
| Rail modes | Every enabled AP-controlled LDO is in normal (high-power) mode |

One difference from stock remains untested: rpmhpd never reaches `sync_state`
here, so the AP keeps its boot-time MSS/CX/MX votes, while stock drops its MSS
proxy vote after boot. Releasing MSS alone crashed the modem during suspend on
2026-09-17 ([MSS handoff](mss-handoff-test-20260917.md)).

### Incidents

- **A whole-region read of SMEM through `/dev/mem` rebooted the phone.**
  Partitions between two remote processors are firewalled; touching one aborts
  the CPU. `scripts/phone-smem.py` now maps only the partition table and the
  partitions that include APPS or the global heap, as mainline's driver does.
- After every modem crash the recovery reboot script hit `Resource busy` on
  the read-only remount with no userspace holder left. From the port 23 shell,
  SysRq `s` then `u`, a check that `/newroot` is `ro`, then `busybox reboot -f`
  worked each time, with clean mounts afterwards.
- `cat >` over SSH twice produced an empty file on the phone (the first reboot
  script upload and a backup copy). Check every upload's hash.

## Third round: data and IMS working

Everything below comes up by itself at boot (checked after a cold reboot,
`scripts/phone-cellular-status.sh --wait 240`): LTE registered on Tello,
mobile data over NetworkManager (IPv6 and IPv4 HTTPS 200), and IMS registered
with SMS and voice at full service. Call audio is not routed yet.

### 1. The QLink assertion was the AP's MSS vote

`rpmhpd` clamps every domain to its top corner until `sync_state`, so after
the modem's proxy vote is dropped the AP keeps voting `mss.lvl` at corner 9
(`turbo_l1`) for the whole boot (`rpmh_votes.ko` cache: `00030060 wake 9`).
Stock drops its vote. With it held, QLink could not restart after the modem
slept: `--cycle 25,45` (online, low-power, online) asserted 1.7 s after the
second online every time. `kernel/radio/mss_vote.c` sends one active-only
`mss.lvl` vote; at 0, the same cycle and 200-300 s runs passed with no crash.
`guacamole-radio-start` loads it and sets 0 once the modem runs
(`cellular/mss-release-at-boot`). A kernel fix (rpmhpd not clamping MSS, or
a real `sync_state`) should replace it.

### 2. The registration drop was the missing data path

Without IPA the modem dropped its default bearer 12-14 s after attaching and
lost registration at 31 s, with no network reject (NAS Network Reject
indications, `phone-qmi.py nas-watch`) and independent of usage setting or
RAT (`nas-usage data ... lte` changed nothing). REG then blocks the PLMN for
about 11 minutes ("backoff forbidden ... other causes"). With `ipa.ko` loaded
before the modem starts (`cellular/ipa-at-boot`), registration holds. Loaded
after the modem, IPA logs `unexpected init_completed response`.

### 3. Data needs DPM, WDA and IPv6

- WDS Bind Mux Data Port fails with Internal until DPM Open Port (service
  0x2f) tells the modem about the IPA hardware port: embedded/1, RX = modem TX
  endpoint (2), TX = modem RX endpoint (11), read from
  `/sys/class/net/rmnet_ipa0/device/modem/`, as ModemManager does.
- The port then defaults to raw IP without aggregation, and the modem drops
  QMAP frames. WDA Set Data Format to QMAPv4 both ways (IPA v4.1 uses MAPv4
  checksum offload) fixes it.
- `CONFIG_IPV6=m` and no module was installed. `ipv6.ko` is built from the
  #194 tree (`KBUILD_MODPOST_WARN=1`, only the netfilter modules fail) and
  loaded at boot; all its symbols resolve against `vmlinux.symvers`.

`phone-qmi.py data-up` does all of this by hand. ModemManager 1.24.2 (Arch)
does it itself for `ipa` ports and is now the owner: `guacamole-radio-start`
opens the USIM provisioning session, then starts ModemManager after the system
bus is up (`cellular/mm-at-boot`); D-Bus activation is disabled by
`/usr/local/share/dbus-1/system-services/org.freedesktop.ModemManager1.service`.
`udevadm` refuses to act in the chroot; `SYSTEMD_IGNORE_CHROOT=1` works
around that when tags are needed by hand. NetworkManager's `mobile`
connection (APN `fast.t-mobile.com`, route metric 700, below Wi-Fi's 600)
autoconnects. A first `nmcli c up` once saw ModemManager exit while bringing
`rmnet_ipa0` up; not seen since.

### 4. IMS needs the AP to serve IMS DCM

The modem's IMS stack asks the AP for its IMS PDN through QMI IMS DCM, service
0x302, where the AP is the server (openimsd.de; DiamaneOS
`hardware_diamaneos_ims` `docs/dcm-protocol.md`; libqmi
`qmi-service-imsdcm.json`). `phone-qmi.py ims-dcm` publishes it: within
seconds the modem sends its link address and state reports, then PDP Activate
for APN `ims`, IPv6, slot 1. The server starts that WDS call on mux 10 and
reports the address; IMSA (bound to subscription 0, TLV 0x10) then reports
IMS registered over WWAN with SMS and voice at full service. Started at boot
by `cellular/ims-dcm-at-boot`.

### Tooling added

- `scripts/phone-online-trial.py --cycle ON,OFF`, `scripts/run-cellular-trial.sh`
  (hash-checked deploy, boot-capture slice, automatic crash reboot),
  `scripts/phone-crash-reboot.sh`, `scripts/phone-cellular-status.sh`.
- DIAG: one capture per boot (`diag-boot.bin`, previous boot kept as
  `diag-boot.prev.bin`), `0x98` subscription packets decoded, and two
  `diag-router` patches in `adapter/radio/diag-router/`: disconnected clients
  no longer stall the modem's data, peripherals get log and event masks, and
  mask centralization is advertised. Log packets (NAS OTA) still do not flow.

## Next

Continued in [calling, texting and the phone apps](calling-20260927.md): SMS
works both ways, a VoLTE call connects, and call audio is in progress.
Still open from this round: modem recovery (a crash panics the kernel, and
with IPA loaded may freeze the phone), suspend with the cellular stack, and
NAS OTA log packets in DIAG.
