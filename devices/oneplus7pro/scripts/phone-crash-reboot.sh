#!/usr/bin/env bash
# Reboot the phone (after a modem crash, or a normal one), then wait for the
# radio to be ready.
#
# After a modem crash the recovery reboot script's read-only remount fails
# with EBUSY although no userspace process holds the root. This runs in the
# outer BusyBox root instead: stop Arch's processes, sync, SysRq emergency
# read-only remount, check that /newroot is read-only, and only then reboot.
# See docs/cellular-sim-20260926.md.
#
# Over USB it uses the unauthenticated port 23 recovery shell. When USB is
# down (the first boot after a hard reset) it stages the same steps in the
# outer root over Wi-Fi SSH and starts them detached, because stopping Arch
# ends the SSH session.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
USB=172.16.42.1

# The radio daemons stay up: stopping rmtfs under a running modem crashes it,
# and on 2026-09-27 that froze the whole phone (IPA loaded). SysRq-u does not
# need them gone to remount read-only.
STOP='for s in TERM KILL; do for r in /proc/[0-9]*/root; do
  [ "$(/bin/busybox readlink $r 2>/dev/null)" = /newroot ] || continue
  p=${r%/root}; case "$(/bin/busybox cat $p/comm 2>/dev/null)" in rmtfs|pd-mapper|tqftpserv|qrtr-ns) continue ;; esac
  /bin/busybox kill -$s ${p##*/} 2>/dev/null; done; /bin/busybox sleep 2; done
/bin/busybox sync; echo s > /proc/sysrq-trigger; /bin/busybox sleep 2
echo u > /proc/sysrq-trigger; /bin/busybox sleep 3
/bin/busybox grep "^/dev/sda19 /newroot ext4 ro," /proc/mounts && echo READ_ONLY_OK'
REBOOT='/bin/busybox grep -q "^/dev/sda19 /newroot ext4 ro," /proc/mounts && /bin/busybox reboot -f'
# With a crashed remoteproc the kernel's own reboot path can stall
# (2026-09-17, 2026-09-27). The root is read-only by then, so SysRq-b is safe.
FORCE='/bin/busybox grep -q "^/dev/sda19 /newroot ext4 ro," /proc/mounts && /bin/busybox sync && echo b > /proc/sysrq-trigger'

# nc waits ${3:-5} s after sending before it quits: longer than the command runs.
shell() { printf '%s\n' "$1" | timeout "${2:-20}" nc -q "${3:-5}" "$USB" 23 | tr -d '\r'; }

old=$(timeout 10 bash "$ROOT/scripts/phone-ssh.sh" 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null || true)
if ping -c1 -W1 "$USB" >/dev/null 2>&1; then
    PHONE=$USB
    shell "$STOP" 40 25 | tee /dev/stderr | grep -q READ_ONLY_OK ||
        { echo 'root is not read-only; not rebooting' >&2; exit 1; }
    shell "$REBOOT" 8 || true
else
    PHONE=$(tr -d '[:space:]' < "$ROOT/out/network-test/wifi-host")
    echo "USB is down; rebooting over Wi-Fi ($PHONE)" >&2
    printf '%s\n' "$STOP" '[ -z "$(/bin/busybox grep "^/dev/sda19 /newroot ext4 ro," /proc/mounts)" ] && exit 1' \
        "$REBOOT" '/bin/busybox sleep 45' "$FORCE" |
        timeout 20 bash "$ROOT/scripts/phone-ssh.sh" 'cat > /proc/1/root/tmp/guacamole-reboot.sh'
    timeout 20 bash "$ROOT/scripts/phone-ssh.sh" 'sync; setsid chroot /proc/1/root /bin/busybox sh /tmp/guacamole-reboot.sh > /proc/1/root/tmp/guacamole-reboot.log 2>&1 < /dev/null & echo launched' || true
fi

down() { for _ in $(seq 1 "$1"); do ping -c1 -W1 "$PHONE" >/dev/null 2>&1 || return 0; sleep 1; done; return 1; }
if ! down 75; then
    if [[ $PHONE == "$USB" ]]; then
        echo 'reboot -f stalled; SysRq-b' >&2
        shell "$FORCE" 8 || true
        down 30 || { echo 'phone did not go down' >&2; exit 1; }
    else
        echo 'phone did not go down' >&2; exit 1
    fi
fi
for _ in $(seq 1 90); do ping -c1 -W1 "$PHONE" >/dev/null 2>&1 && break; sleep 2; done
for _ in $(seq 1 60); do
    timeout 8 bash "$ROOT/scripts/phone-ssh.sh" 'test -e /run/guacamole-radio.ready' 2>/dev/null && break
    sleep 4
done
new=$(timeout 10 bash "$ROOT/scripts/phone-ssh.sh" 'cat /proc/sys/kernel/random/boot_id')
[[ $new != "$old" ]] || { echo "boot ID unchanged ($new)" >&2; exit 1; }
echo "rebooted: $new"
timeout 10 bash "$ROOT/scripts/phone-ssh.sh" \
    'for p in /sys/class/remoteproc/*; do echo "$(cat $p/name) $(cat $p/state)"; done'
