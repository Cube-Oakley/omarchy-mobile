#!/usr/bin/env python3
"""Guarded cellular trial: one Set Online, timed against modem sleep.

Opens the USIM provisioning session, puts the modem online and samples the
modem's SMEM sleep statistics (qcom_stats) every 0.2 s and NAS registration
every 2 s, printing JSON lines. It stops at the first modem crash or after
the given number of seconds.

Modem recovery stays disabled: on 2026-09-26 an enabled recovery panicked the
kernel while restarting the crashed modem. A crash therefore leaves the modem
(and Wi-Fi, whose firmware runs on it) down until a reboot. Without a crash
the radio is put in low-power mode at the end. Refuses to run unless rmtfs
keeps EFS writes in RAM (-r), the modem is running and offline, recovery is
disabled and qcom_stats is loaded.

With --cycle ON,OFF the radio goes to low-power ON seconds after going online,
stays there OFF seconds (long enough for the modem to power-collapse), then goes
online again: a fast way to exercise QLink's wake from sleep.

Install next to phone-qmi.py (used for the PDC check) and run on the phone:
  python3 /root/radio-bringup/cellular/phone-online-trial.py [seconds] [--cycle ON,OFF]
"""
import glob
import json
import os
import re
import subprocess
import sys
import time

USIM_AID = "A0:00:00:00:87:10:02:FF:FF:FF:FF:89:06:19:00:00"
STATS = "/sys/kernel/debug/qcom_stats/modem"
TMO_ID = "cb45c810b0532a8dd30b8f2324ba6b1967ecd55d"
PHONE_QMI = os.path.join(os.path.dirname(os.path.abspath(__file__)), "phone-qmi.py")

T0 = time.monotonic()


def emit(event, **kw):
    print(json.dumps({"t": round(time.monotonic() - T0, 2), "event": event, **kw}),
          flush=True)


def qmi(*args, timeout=12):
    r = subprocess.run(["qmicli", "-d", "qrtr://0", *args], capture_output=True,
                       text=True, timeout=timeout + 3)
    return r.returncode, r.stdout + r.stderr


def modem_rproc():
    for p in glob.glob("/sys/class/remoteproc/remoteproc*"):
        if open(f"{p}/name").read().strip() == "modem":
            return p
    sys.exit("no modem remoteproc")


def read_stats():
    s = dict(re.findall(r"^([A-Za-z ]+): (\d+)$", open(STATS).read(), re.M))
    return int(s["Count"]), int(s["Last Entered At"]), int(s["Last Exited At"])


def serving():
    rc, out = qmi("--nas-get-serving-system", timeout=6)
    get = lambda k: (re.search(rf"{k}: '([^']*)'", out) or [None, None])[1]
    return {"reg": get("Registration state"), "ps": get("PS"),
            "plmn": get("Description"), "rc": rc}


def fail(msg):
    emit("refused", reason=msg)
    sys.exit(1)


def main():
    args = sys.argv[1:]
    cycle = None
    if "--cycle" in args:
        i = args.index("--cycle")
        cycle = [float(x) for x in args[i + 1].split(",")]
        del args[i:i + 2]
    duration = float(args[0]) if args else 360
    rproc = modem_rproc()
    state = lambda: open(f"{rproc}/state").read().strip()

    # Preflight.
    rmtfs = subprocess.run(["pgrep", "-a", "rmtfs"], capture_output=True, text=True).stdout
    if not re.search(r"rmtfs .*-r\b", rmtfs):
        fail("rmtfs is not in RAM-shadow mode (-r)")
    if state() != "running":
        fail(f"modem is {state()}")
    if not os.path.exists(STATS):
        fail("qcom_stats is not loaded")
    rc, out = qmi("--dms-get-operating-mode")
    mode = (re.search(r"Mode: '([^']*)'", out) or [None, None])[1]
    if mode not in ("shutting-down", "low-power"):
        fail(f"DMS mode is {mode}")
    ipa = os.path.exists("/sys/module/ipa")
    emit("preflight", rproc=os.path.basename(rproc), ipa_loaded=ipa,
         kernel=os.uname().release, boot_id=open("/proc/sys/kernel/random/boot_id").read().strip())

    rc, out = qmi("--uim-change-provisioning-session=session-type=primary-gw-provisioning,"
                  f"activate=yes,slot=1,aid={USIM_AID}")
    emit("provisioning", rc=rc)
    time.sleep(3)
    rc, out = qmi("--uim-get-card-status")
    if "Application state: 'ready'" not in out:
        fail("USIM is not ready")
    pdc = subprocess.run([sys.executable, PHONE_QMI, "pdc-state"], capture_output=True,
                         text=True, timeout=90).stdout
    active = re.search(r"selected sub=0: active=(.*?) pending", pdc)
    active = active.group(1) if active else None
    emit("pdc", active=active, tmo=bool(active and TMO_ID[:8] in active))

    if open(f"{rproc}/recovery").read().strip() != "disabled":
        fail("modem recovery is enabled")
    count0, entered0, exited0 = read_stats()
    emit("baseline", count=count0, entered=entered0, exited=exited0)

    rc, out = qmi("--dms-set-operating-mode=online")
    emit("online", rc=rc)
    t_online = time.monotonic()

    last = (count0, entered0, exited0)
    next_nas = 0.0
    phase = "online"
    try:
        while time.monotonic() - t_online < duration:
            now = time.monotonic() - t_online
            if cycle and phase == "online" and now >= cycle[0]:
                rc, out = qmi("--dms-set-operating-mode=low-power")
                emit("low_power", rc=rc, since_online=round(now, 2))
                phase = "lpm"
            elif cycle and phase == "lpm" and now >= cycle[0] + cycle[1]:
                rc, out = qmi("--dms-set-operating-mode=online")
                emit("online_again", rc=rc, since_online=round(now, 2))
                phase = "online2"
            st = state()
            if st != "running":
                crashed_at = time.monotonic()
                emit("crash", state=st, since_online=round(crashed_at - t_online, 2),
                     last_stats=last)
                return
            cur = read_stats()
            if cur != last:
                emit("sleep", count=cur[0], entered=cur[1], exited=cur[2],
                     asleep=cur[1] > cur[2], since_online=round(time.monotonic() - t_online, 2))
                last = cur
            if time.monotonic() >= next_nas:
                next_nas = time.monotonic() + 2
                emit("nas", since_online=round(time.monotonic() - t_online, 2), **serving())
            time.sleep(0.2)
        rc, out = qmi("--dms-set-operating-mode=low-power")
        emit("low_power", rc=rc)
    finally:
        emit("done", modem=state())


if __name__ == "__main__":
    main()
