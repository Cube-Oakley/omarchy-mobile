#!/bin/bash
# The root half of a problem report (omarchy-mobile-report): what only root
# can read, as a tar archive on stdout. It takes no arguments and writes
# nothing outside a private directory it removes, so the desktop's account
# may run it through sudo (session/sudoers).
# Installed as /usr/local/sbin/pixel-report-root.
set -u
[[ $# -eq 0 ]] || { echo 'usage: pixel-report-root (no arguments)' >&2; exit 2; }
umask 077
dir=$(mktemp -d /run/pixel-report.XXXXXX) || exit 1
trap 'rm -rf "$dir"' EXIT
run() { local name=$1; shift; timeout 20 "$@" > "$dir/$name" 2>&1; }
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug 2>/dev/null
run dmesg.txt dmesg
tail -n 2000 /var/log/pixel-kernel-warnings.log > "$dir/kernel-warnings.txt" 2>&1
run modules.txt lsmod
run interrupts.txt cat /proc/interrupts
run wakeup-sources.txt cat /sys/kernel/debug/wakeup_sources
# Scanout's statistics read the display controller unlocked: never under the
# always-on display.
if ! compgen -G '/run/user/*/omarchy-mobile-ambient' >/dev/null; then
    for f in /sys/kernel/debug/dri/*/pixel_scanout; do [[ -r $f ]] && cat "$f" > "$dir/scanout.txt"; done
fi
for p in /sys/module/pixel_battery/parameters/* /sys/module/pixel_wdt_pm/parameters/*; do
    [[ -r $p ]] && echo "$p $(cat "$p" 2>/dev/null)"
done > "$dir/parameters.txt"
ls -la /run/pixel-* 2>/dev/null > "$dir/run-flags.txt"
run modem.txt python3 /usr/local/lib/omarchy-mobile/modem/manager.py status
# Android: its log and state (the container must not be frozen; lxc-attach
# waits on a frozen one, so it is skipped then).
lxc=(-P /var/lib/waydroid/lxc -n waydroid)
state=$(lxc-info "${lxc[@]}" -sH 2>/dev/null)
echo "container: ${state:-none}" > "$dir/android.txt"
if [[ $state == RUNNING ]]; then
    timeout 20 lxc-attach "${lxc[@]}" --clear-env -- /system/bin/logcat -d -t 3000 </dev/null > "$dir/logcat.txt" 2>&1
    timeout 20 lxc-attach "${lxc[@]}" --clear-env -- /system/bin/dumpsys activity activities </dev/null 2>&1 \
        | grep -E 'Resumed|mFocused|Task\{' | head -40 >> "$dir/android.txt"
fi
tail -n 400 /var/lib/waydroid/waydroid.log > "$dir/waydroid.log" 2>/dev/null
tar -C "$dir" -cf - .
