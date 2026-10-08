#!/bin/sh
# sudo with the lock screen's PIN or password (docs/user-session-plan-
# 20261002.md), put on the phone by the user from the host:
#
#   sh devices/pixel7pro/session/install-lock-sudo.sh [WIFI-ADDRESS]
#
# Installs lock-secret-auth, the sudoers drop-in (its prompt) and
# /etc/pam.d/sudo, after checking them (visudo, python3, the session
# account's password field locked: Arch's pam_unix takes an empty one, and
# PAM falls through to it after a wrong PIN). The previous files are kept in
# /root/*.before-lock. NOPASSWD entry points are unaffected; root SSH stays
# the way back.
set -eu
cd "$(dirname "$0")/.."
keys=out/checkpoints/20260925-persistence-power/arch-session-f
ssh=$keys/ssh
[ -x "$ssh" ] || { echo "missing $ssh" >&2; exit 1; }
if [ $# -ge 1 ]; then
    wifi=$1
    ssh() { /usr/bin/ssh -F /dev/null -i "$keys/id_ed25519" -o IdentitiesOnly=yes \
        -o UserKnownHostsFile="$keys/known_hosts" -o HostKeyAlias=10.77.7.1 \
        -o StrictHostKeyChecking=yes -o BatchMode=yes -o ConnectTimeout=15 "root@$wifi" "$@"; }
    ssh=ssh
fi
"$ssh" 'cat > /root/lock-secret-auth.new' < session/lock-secret-auth.py
"$ssh" 'cat > /root/pam-sudo.new' < session/pam-sudo
"$ssh" 'cat > /root/sudoers.new' < session/sudoers
"$ssh" 'set -e
user=$(sed -n "s/^user=//p" /etc/omarchy-mobile/session)
[ -n "$user" ] || { echo "no session user" >&2; exit 1; }
field=$(getent shadow "$user" | cut -d: -f2)
case $field in
    "!"*|"*"*) ;;
    *) echo "$user has a usable or empty password; lock it first (passwd -l $user)" >&2; exit 1 ;;
esac
python3 -m py_compile /root/lock-secret-auth.new
visudo -cf /root/sudoers.new >/dev/null
keep() { [ ! -e "$1" ] || [ -e "$2" ] || cp -a "$1" "$2"; }
keep /etc/pam.d/sudo /root/pam-sudo.before-lock
keep /etc/sudoers.d/omarchy-mobile /root/sudoers.before-lock
install -o root -g root -m 0755 /root/lock-secret-auth.new /usr/local/lib/omarchy-mobile/lock-secret-auth
install -o root -g root -m 0440 /root/sudoers.new /etc/sudoers.d/omarchy-mobile.new
mv /etc/sudoers.d/omarchy-mobile.new /etc/sudoers.d/omarchy-mobile
install -o root -g root -m 0644 /root/pam-sudo.new /etc/pam.d/sudo.new
mv /etc/pam.d/sudo.new /etc/pam.d/sudo
sync
echo "installed; try: sudo -k; sudo true (as $user, in a terminal)"'
echo "To undo: put /root/pam-sudo.before-lock back as /etc/pam.d/sudo."
