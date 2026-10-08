#!/bin/bash
# Deep sleep under the always-on display, then power the display pipeline up
# and off again right after the wake (as a minute tick's redraw does), N
# times. Stops at the first wake that is not the alarm or the modem.
n=${1:-500}
user=cube; uid=$(id -u $user); home=/home/$user; runtime=/run/user/$uid
display() { setpriv --reuid=$uid --regid=$(id -g $user) --init-groups env -i HOME=$home USER=$user PATH=/usr/local/sbin:/usr/local/bin:/usr/bin XDG_RUNTIME_DIR=$runtime WAYLAND_DISPLAY=wayland-1 HYPRLAND_INSTANCE_SIGNATURE=$(ls $runtime/hypr | head -1) $home/.local/bin/omarchy-mobile-display "$@" >/dev/null 2>&1; }
policy=$(pgrep -f "^python3 $home/.local/bin/omarchy-mobile-sleep run$" | head -1)
p=/sys/module/pixel_scanout/parameters/ambient
log=/root/tools/aod-resume-loop.log
: > $log
[[ -n $policy ]] && kill -STOP $policy
trap '[[ -n $policy ]] && kill -CONT $policy; display on; echo "aod-resume: end" > /dev/kmsg' EXIT
display ambient
sleep 3
echo "aod-resume: start $n" > /dev/kmsg
sync
for ((i = 1; i <= n; i++)); do
    r=$(OMARCHY_MOBILE_WAKE_PURPOSE=ambient /usr/local/sbin/pixel-suspend --keep-dark --allow-charging --wake-after 5 2>&1 | tail -1)
    woke=$(python3 -c 'import json, sys; d = json.loads(sys.argv[1]); print(d.get("woke", "?"), d.get("sleep", ""))' "$r" 2>/dev/null || echo "? ?")
    echo "$i $woke" >> $log
    case ${woke%% *} in
        alarm|rtc|modem|unknown) ;;
        *) echo "aod-resume: stop at $i on $woke" > /dev/kmsg; break ;;
    esac
    sleep 0.15
    echo 0 > $p
    echo 1 > $p
    sleep 0.4
    (( i % 50 == 0 )) && { echo "aod-resume: $i" > /dev/kmsg; sync; }
done
