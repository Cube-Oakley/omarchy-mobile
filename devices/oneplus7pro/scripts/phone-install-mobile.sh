#!/usr/bin/env bash
# Install the working tree's mobile userspace (overlay/mobile and the OnePlus
# adapter) on the phone with install.sh, then restart the shell so it loads
# the new files. install.sh backs up the configuration it replaces.
#   scripts/phone-install-mobile.sh [--no-restart]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
restart=1
[[ ${1:-} == --no-restart ]] && restart=0
bundle=$(mktemp --suffix=.tgz)
trap 'rm -f "$bundle"' EXIT
tar czf "$bundle" --exclude=__pycache__ --exclude='*.pyc' \
    -C "$REPO/overlay" mobile -C "$ROOT" adapter
hash=$(sha256sum "$bundle" | cut -d' ' -f1)
remote=$(timeout 180 bash "$ROOT/scripts/phone-ssh.sh" \
    'rm -rf /root/omarchy-mobile-src && mkdir -p /root/omarchy-mobile-src && cat > /root/omarchy-mobile-src/src.tgz && sha256sum /root/omarchy-mobile-src/src.tgz' < "$bundle")
[[ $remote == "$hash"* ]] || { echo "Upload damaged (sent $hash, got $remote)" >&2; exit 1; }
timeout 900 bash "$ROOT/scripts/phone-ssh.sh" 'set -e; cd /root/omarchy-mobile-src && tar xzf src.tgz
    export XDG_RUNTIME_DIR=/run/user/0
    bash mobile/install.sh /root/omarchy-mobile-src/adapter 2>&1 | grep -vE "^\s*$|CMake|cmake|Qt6QmlMacros|qt_add_qml_module|^  " | tail -3'
if [[ $restart == 1 ]]; then
    timeout 90 bash "$ROOT/scripts/phone-ssh.sh" 'python3 -' < "$ROOT/scripts/phone-shell-restart.py"
fi
