#!/bin/sh
# Run inside the native OnePlus 13 initramfs AFTER extracting Arch to /run/arch.
# Only RAM/virtual filesystems are used. Public key arrives over trusted serial.
set -eu
hostname oneplus13-linux
ROOT=/run/arch
[ "$(uname -m)" = aarch64 ]
grep -q ' /run tmpfs ' /proc/mounts
[ -x "$ROOT/usr/bin/bash" ]
[ -s /run/transfer/authorized_keys ]
# Give the chroot a real mount root so mountinfo/space checks resolve '/' correctly.
mountpoint -q "$ROOT" || mount --bind "$ROOT" "$ROOT"
mkdir -p "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/run" "$ROOT/root/.ssh"
# A recursive device bind also makes the existing devpts mount available.
mountpoint -q "$ROOT/dev" || mount --rbind /dev "$ROOT/dev"
mountpoint -q "$ROOT/proc" || mount -t proc proc "$ROOT/proc"
mountpoint -q "$ROOT/sys" || mount -t sysfs sysfs "$ROOT/sys"
# Separate /run avoids binding an ancestor of ROOT back inside itself.
mountpoint -q "$ROOT/run" || mount -t tmpfs -o mode=0755 tmpfs "$ROOT/run"
mkdir -p /dev/shm
mountpoint -q /dev/shm || mount -t tmpfs -o mode=1777 tmpfs /dev/shm
# The shm mount was added after the recursive bind above.
mountpoint -q "$ROOT/dev/shm" || mount --bind /dev/shm "$ROOT/dev/shm"
for pair in fd:/proc/self/fd stdin:/proc/self/fd/0 stdout:/proc/self/fd/1 stderr:/proc/self/fd/2; do
    name=${pair%%:*}; target=${pair#*:}
    [ -L "/dev/$name" ] || ln -s "$target" "/dev/$name"
done
cp /run/transfer/authorized_keys "$ROOT/root/.ssh/authorized_keys"
chmod 700 "$ROOT/root/.ssh"
chmod 600 "$ROOT/root/.ssh/authorized_keys"
mkdir -p "$ROOT/run/sshd"
# No systemd or stock Arch sshd configuration is started. Restrict to this link.
cat > "$ROOT/etc/ssh/sshd_config.oneplus13" <<'EOF'
Port 22
ListenAddress 10.77.13.1
HostKey /etc/ssh/ssh_host_ed25519_key.oneplus13
PidFile /run/sshd-oneplus13.pid
PermitRootLogin prohibit-password
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
AuthenticationMethods publickey
UsePAM no
AllowUsers root
AllowTcpForwarding no
X11Forwarding no
PermitTunnel no
Subsystem sftp internal-sftp
EOF
if [ ! -f "$ROOT/etc/ssh/ssh_host_ed25519_key.oneplus13" ]; then
    chroot "$ROOT" /usr/bin/ssh-keygen -q -t ed25519 -N '' -f /etc/ssh/ssh_host_ed25519_key.oneplus13
fi
chroot "$ROOT" /usr/bin/sshd -t -f /etc/ssh/sshd_config.oneplus13
chroot "$ROOT" /usr/bin/sshd -f /etc/ssh/sshd_config.oneplus13 -E /run/sshd-oneplus13.log
echo ONEPLUS13_ARCH_SSH_HOST_KEY
cat "$ROOT/etc/ssh/ssh_host_ed25519_key.oneplus13.pub"
echo ONEPLUS13_ARCH_SSH_READY
