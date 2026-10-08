#!/bin/bash
# Runs a program inside a running fingerprint HAL lab (fp-hal-lab.sh): in its
# mount and PID namespaces and its chroot, without capabilities, like the
# lab's own programs.
#   fp-hal-exec.sh PROGRAM [ARGS...]   e.g. /system/bin/fp-client list
#   fp-hal-exec.sh stop                ends the lab early, with its clean-up
set -euo pipefail
L=/run/fp-hal-logs
if [[ ${1:-} == stop ]]; then
    touch "$L/stop"
    exit 0
fi
pid=$(cat "$L/lab.pid" 2>/dev/null || true)
[[ -n $pid ]] && kill -0 "$pid" 2>/dev/null || { echo 'no lab is running' >&2; exit 1; }
# -r: the lab's root; a mount namespace's own root is the initramfs (switch_root).
exec nsenter -t "$pid" -m -p -r -- /run/fp-hal/enter-gnss /run/fp-hal "$@"
