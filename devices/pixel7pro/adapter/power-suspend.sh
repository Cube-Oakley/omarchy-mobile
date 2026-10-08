#!/bin/bash
set -euo pipefail
# Root's program (session/sudoers lets the desktop's account run it, and keeps
# the sleep policy's OMARCHY_MOBILE_* choices, which it checks).
(( EUID == 0 )) && exec /usr/local/sbin/pixel-suspend "$@"
exec sudo -n /usr/local/sbin/pixel-suspend "$@"
