#!/bin/sh
# On the phone, as root: start or stop Waydroid's container manager.
#   waydroid-container.sh start|stop|status
# The Android session itself is then `waydroid session start` as cube.
#
# What a stock host gets from systemd and udev, done by hand here:
# - cgroup2 on /sys/fs/cgroup (LXC needs it; nothing else on the phone does);
# - the modules from pack-modules.sh: kernel.modprobe points at the
#   initramfs helper, so nothing is loaded on demand, and Android's netd
#   inside the container needs its iptables and ip6tables tables, matches
#   and targets loaded beforehand;
# - a real root for LXC. The initramfs mounts the Arch root at /run/arch
#   and chroots into it, so the mount namespace's root is still the
#   initramfs. LXC's pivot_root then leaves Android's paths under /run/arch,
#   and zygote aborts ("Not allowlisted: /run/arch/apex/..."). The manager
#   runs in its own mount namespace where the Arch root is moved onto /, as
#   switch_root does; the phone's own mounts are not touched.
set -eu
busybox=/proc/1/root/bin/busybox
manager='^/usr/bin/python3 /usr/bin/waydroid container'

case "${1:-}" in
start)
    if pgrep -f "$manager" > /dev/null; then
        echo 'container manager already running'
        exit 0
    fi
    mountpoint -q /sys/fs/cgroup || mount -t cgroup2 cgroup2 /sys/fs/cgroup
    # binderfs here, not in the manager's namespace: the session (cube, in
    # this namespace) opens /dev/binder -> /dev/binderfs/binder too. Mounted
    # by the manager instead, it was invisible here, and the session's
    # Android services spun at 100% CPU retrying.
    mkdir -p /dev/binderfs
    mountpoint -q /dev/binderfs || mount -t binder binder /dev/binderfs
    # And the /dev links Waydroid looks for. Without them (a fresh /dev after
    # every boot) the container start mounted a binderfs of its own in the
    # manager's namespace: Android ran there while the session's tools
    # waited for its service manager on this one, so no app would open
    # (October 6).
    for node in binder vndbinder hwbinder; do
        [ -e "/dev/$node" ] || ln -sf "binderfs/$node" "/dev/$node"
    done
    release=$(uname -r)
    find "/lib/modules/$release/kernel" -name '*.ko' | while read -r m; do
        name=$(basename "$m" .ko)
        [ -d "/sys/module/$(echo "$name" | tr - _)" ] || modprobe "$name" 2> /dev/null || true
    done
    setsid -f "$busybox" chroot /proc/1/root /bin/busybox unshare -m /bin/busybox sh -c \
        'mount --make-rprivate / && cd /run/arch && mount --move . / &&
         exec /bin/busybox chroot . /usr/bin/python3 /usr/bin/waydroid container start' \
        > /var/log/waydroid-container.log 2>&1 < /dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        if pgrep -f "$manager" > /dev/null; then
            setsid -f "$0" watch >> /var/log/waydroid-container.log 2>&1 < /dev/null
            echo 'container manager started'
            exit 0
        fi
        sleep 1
    done
    echo 'container manager did not start; see /var/log/waydroid-container.log' >&2
    exit 1
    ;;
watch)
    # Android runs in Waydroid's single-window mode: each app it shows gets
    # a window of its own (app ID waydroid.PACKAGE), but only the app in
    # front of Android is live; the others keep a picture of their last frame
    # and are in Android's background, as on a phone, so they notify. (In
    # multi-window mode every Android window stayed "on screen" to Android:
    # WhatsApp parked with a chat open neither notified nor held back read
    # receipts, October 6.) Focusing an app's window brings it to the front
    # in Android (the hwcomposer's setFocusedTask).
    #
    # The watcher also keeps Android's density at the shell's scale (3x, 480
    # dpi) and its time zone at the phone's, passes the keyboard bridge's
    # shown/hidden to the shell ($run/android-keyboard), and puts Android's
    # screen to sleep or wakes it as the shell asks ($run/android-power,
    # "awake" while an Android app is in front of a lit screen, else
    # "asleep"), so Android apps are in the background and Android dozes
    # while its apps are out of sight. The same FIFO carries replies typed
    # in the notification shade to the bridge's ReplyReceiver. It waits on
    # LXC's monitor, Android's log and that FIFO, so it costs no wakeups.
    set +e
    pgrep -f "^/bin/sh $0 watch" | grep -v "^$$\$" | grep -q . && exit 0
    lxc='-P /var/lib/waydroid/lxc -n waydroid'
    in_android() { timeout 10 lxc-attach $lxc --clear-env -- "$@"; }
    run=/run/user/1001/omarchy-mobile
    # The phone's photos and videos for Android apps' pickers (WhatsApp,
    # Signal): each file under ~/Pictures and ~/Videos hard-linked into
    # Android's Pictures/Phone and Movies/Phone. It is the same file system,
    # so nothing is copied, and links whose file is gone go too. (A bind
    # mount there stayed invisible: Android's media provider works in a mount
    # namespace of its own.) Then Android's media index scans its storage in
    # the background. `content` is an app_process tool: it needs the paths
    # and zygote's class path that lxc-attach's clear environment lacks.
    share_photos() {
        share_folder /home/cube/Pictures /home/cube/.local/share/waydroid/data/media/0/Pictures/Phone
        share_folder /home/cube/Videos /home/cube/.local/share/waydroid/data/media/0/Movies/Phone
    }
    share_folder() {
        src=$1
        dst=$2
        [ -d "$src" ] && [ -d "${dst%/*}" ] || return 0
        owner=$(stat -c %u:%g "${dst%/*}")
        (cd "$src" && find . -type d) | while read -r d; do
            mkdir -p "$dst/$d" && chown "$owner" "$dst/$d" && chmod 2770 "$dst/$d"
        done
        (cd "$src" && find . -type f) | while read -r f; do
            [ "$src/$f" -ef "$dst/$f" ] || ln -f "$src/$f" "$dst/$f"
        done
        (cd "$dst" && find . -type f) | while read -r f; do
            [ -e "$src/$f" ] || rm -f "$dst/$f"
        done
    }
    media_scan() {
        share_photos
        timeout 120 lxc-attach $lxc --clear-env -- /system/bin/sh -c '
            export PATH=/system/bin:/apex/com.android.art/bin:/apex/com.android.runtime/bin
            export ANDROID_DATA=/data ANDROID_ROOT=/system ANDROID_ART_ROOT=/apex/com.android.art
            export ANDROID_I18N_ROOT=/apex/com.android.i18n ANDROID_TZDATA_ROOT=/apex/com.android.tzdata
            export BOOTCLASSPATH=$(tr "\0" "\n" < /proc/$(pidof zygote64)/environ | sed -n "s/^BOOTCLASSPATH=//p")
            content call --uri content://media --method scan_volume --arg external_primary' \
            < /dev/null > /dev/null 2>&1 &
    }
    last_scan=0
    power_requests() {
        fifo=$run/android-power
        while :; do
            mkdir -p "$run" && chown cube:cube "$run"
            if [ ! -p "$fifo" ]; then
                rm -f "$fifo"
                mkfifo -m 600 "$fifo"
            fi
            chown cube:cube "$fifo"
            # Blocks until the shell writes; a writer closing ends the loop.
            # lxc-attach gets /dev/null for input: it hands its input to the
            # container's root (fchown), and with the FIFO there the shell
            # could no longer write to it.
            while read -r want package title text; do
                case $want in
                # An Android app in front: photos taken since the last scan
                # show in its pickers (at most a scan a minute).
                awake)
                    in_android /system/bin/input keyevent KEYCODE_WAKEUP < /dev/null
                    now=$(date +%s)
                    if [ $((now - last_scan)) -ge 60 ]; then
                        media_scan
                        last_scan=$now
                    fi
                    ;;
                # Android's screen-off black (ColorFade) is drawn into the
                # window of the app in front of it: the keyboard bridge's
                # blank SleepActivity goes in front first, so Waydroid keeps
                # the app's window at its last frame.
                asleep)
                    in_android /system/bin/am start -W --activity-no-animation \
                        -n org.omarchy.mobile.keyboard/.SleepActivity < /dev/null > /dev/null
                    sleep 0.3
                    in_android /system/bin/input keyevent KEYCODE_SLEEP < /dev/null
                    ;;
                # An Android app with its keyboard up left the screen: close
                # the keyboard bridge only (HideReceiver), so the app draws
                # itself at full height for the switcher.
                hide-keyboard)
                    in_android /system/bin/am broadcast -n org.omarchy.mobile.keyboard/.HideReceiver \
                        < /dev/null > /dev/null
                    ;;
                # A reply from the shade: "reply PACKAGE TITLE TEXT", the
                # last two UTF-8 in base64 (keyboard-bridge's ReplyReceiver).
                reply)
                    echo "$package" | grep -qE '^[A-Za-z0-9_.]+$' || continue
                    echo "$title$text" | grep -qE '^[A-Za-z0-9+/=]*$' || continue
                    in_android /system/bin/am broadcast -n org.omarchy.mobile.keyboard/.ReplyReceiver \
                        --es package "$package" --es title64 "$title" --es text64 "$text" < /dev/null > /dev/null
                    ;;
                # The media card's buttons, from omarchy-mobile-android-media:
                # "media CMD [POSITION_MS]" (keyboard-bridge's MediaReceiver).
                media)
                    case $package in play|pause|toggle|next|previous|seek|test|test-end) ;; *) continue ;; esac
                    echo "${title:--1}" | grep -qE '^(-1|[0-9]+)$' || continue
                    in_android /system/bin/am broadcast -n org.omarchy.mobile.keyboard/.MediaReceiver \
                        --es cmd "$package" --el position "${title:--1}" < /dev/null > /dev/null
                    ;;
                esac
            done < "$fifo"
        done
    }
    keyboard_state() {
        # Written in place: Quickshell's file watch does not follow a rename.
        [ -f "$run/android-keyboard" ] || { : > "$run/android-keyboard"; chown cube:cube "$run/android-keyboard"; }
        printf '%s\n' "$1" > "$run/android-keyboard"
    }
    # What Android plays (keyboard-bridge's MediaBridge logs it as JSON) for
    # omarchy-mobile-android-media, the MPRIS player the media card shows;
    # its artwork copied out of the bridge's files as the session's user.
    media_art=0
    media_state() {
        [ -f "$run/android-media.json" ] || { : > "$run/android-media.json"; chown cube:cube "$run/android-media.json"; }
        art=$(printf '%s' "$1" | sed -n 's/.*"art":\([0-9][0-9]*\).*/\1/p')
        if [ -n "$art" ] && [ "$art" != 0 ] && [ "$art" != "$media_art" ]; then
            cp /home/cube/.local/share/waydroid/data/data/org.omarchy.mobile.keyboard/files/media-art.png \
                "$run/android-media-art.png" 2> /dev/null && chown cube:cube "$run/android-media-art.png"
            media_art=$art
        fi
        printf '%s\n' "$1" > "$run/android-media.json"
    }
    # Android's system server starting again inside a running container (its
    # watchdog killed it, or it crashed) leaves Waydroid half alive: no app
    # gets a window any more, each `waydroid app launch` waits on the old
    # binder for good (October 7), and once Android had no network after it
    # (October 6). The session stops instead, so the next Android app opened
    # from the shell starts it afresh, and a notification says why. The log
    # is followed from after boot, so the first system server is never seen:
    # any start it shows is a restart.
    as_cube() {
        setpriv --reuid=1001 --regid=1001 --init-groups env HOME=/home/cube XDG_RUNTIME_DIR=/run/user/1001 \
            DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1001/bus "$@" < /dev/null > /dev/null 2>&1
    }
    android_restarted() {
        echo "$(date '+%F %T') Android's system server restarted: session stopped" >&2
        as_cube gdbus call --session --dest org.freedesktop.Notifications \
            --object-path /org/freedesktop/Notifications --method org.freedesktop.Notifications.Notify -- \
            Android 0 dialog-warning 'Android restarted' \
            'Android stopped after a fault. Open an Android app to start it again.' '[]' '{}' -1
        as_cube timeout 60 waydroid session stop
    }
    while pgrep -f "$manager" > /dev/null; do
        lxc-wait $lxc -s RUNNING -t 600 || continue
        for _ in $(seq 150); do
            [ "$(in_android /system/bin/getprop sys.boot_completed 2> /dev/null)" = 1 ] && break
            sleep 2
        done
        in_android /system/bin/setprop persist.waydroid.multi_windows false
        in_android /system/bin/setprop persist.sys.timezone "$(readlink /etc/localtime | sed 's#.*/zoneinfo/##')"
        in_android /system/bin/wm density 480
        # The keyboard bridge's height in dp: the phone keyboard's keys plus
        # its suggestion strip (44) and navigation gap (24), as keyboard/shell.qml.
        keyboard=$(python3 -c '
import json, sys
try: h = json.load(open(sys.argv[1]))["device"]["keyboardHeight"]
except Exception: h = 280
print((h if isinstance(h, int) and 160 <= h <= 500 else 280) + 68)' \
            /home/cube/.local/state/omarchy-mobile/palette.json)
        in_android /system/bin/settings put global omarchy_keyboard_height "$keyboard"
        # The keyboard bridge's reply listener (replies from the shade) needs
        # notification access.
        in_android /system/bin/cmd notification allow_listener \
            org.omarchy.mobile.keyboard/org.omarchy.mobile.keyboard.ReplyService > /dev/null 2>&1
        # microG (Cloud Messaging) holds the one push connection for every
        # app; Android's battery saving must leave it alone while it dozes.
        in_android /system/bin/sh -c 'pm path com.google.android.gms > /dev/null &&
            dumpsys deviceidle whitelist +com.google.android.gms > /dev/null'
        mkdir -p "$run" && chown cube:cube "$run"
        keyboard_state hidden
        media_scan
        power_requests &
        power=$!
        lxc-attach $lxc --clear-env -- /system/bin/logcat -T 1 -s OmarchyKeyboard:I OmarchyMedia:I SystemServer:I |
        while read -r line; do
            case $line in
            *OmarchyKeyboard*shown*) keyboard_state shown ;;
            *OmarchyKeyboard*hidden*) keyboard_state hidden ;;
            *'OmarchyMedia: {'*) media_state "${line#*OmarchyMedia: }" ;;
            *'SystemServer: Entered the Android system server'*) android_restarted ;;
            esac
        done
        # logcat ended: the container stopped.
        kill "$power" 2> /dev/null
        keyboard_state hidden
        media_state '{"state":"none"}'
        sleep 2
    done
    ;;
stop)
    lxc-stop -P /var/lib/waydroid/lxc -n waydroid -k 2> /dev/null || true
    pkill -f "$manager" || true
    pkill -f "^/bin/sh $0 watch" || true
    echo 'container manager stopped'
    ;;
status)
    pgrep -f "$manager" > /dev/null && echo 'container manager running' || echo 'container manager stopped'
    lxc-info -P /var/lib/waydroid/lxc -n waydroid -s 2> /dev/null || true
    ;;
*)
    echo 'usage: waydroid-container.sh start|stop|status' >&2
    exit 2
    ;;
esac
