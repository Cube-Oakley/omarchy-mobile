#!/usr/bin/env bash
# Connect to persistent Arch, with the host key pinned during USB provisioning.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KNOWN_HOSTS="$ROOT/out/network-test/known_hosts"
IDENTITY=${PHONE_SSH_IDENTITY:-$HOME/.ssh/id_ed25519}
[[ -s $KNOWN_HOSTS ]] || {
    echo 'Missing pinned phone host key; provision SSH over the trusted USB link first.' >&2
    exit 1
}
# PHONE_HOST reaches the phone another way; the host key is still checked
# against the one pinned over USB. When USB does not answer, the phone's Wi-Fi
# address (out/network-test/wifi-host, kept out of the repository) is used.
host=${PHONE_HOST:-}
if [[ -z $host ]]; then
    host=172.16.42.1
    if ! ping -c1 -W1 "$host" >/dev/null 2>&1 && [[ -s $ROOT/out/network-test/wifi-host ]]; then
        host=$(tr -d '[:space:]' < "$ROOT/out/network-test/wifi-host")
        echo "USB is down; using Wi-Fi $host" >&2
    fi
fi
exec ssh -F /dev/null -i "$IDENTITY" -o IdentitiesOnly=yes \
    -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$KNOWN_HOSTS" \
    -o HostKeyAlias=172.16.42.1 -o ConnectTimeout=5 root@"$host" "$@"
