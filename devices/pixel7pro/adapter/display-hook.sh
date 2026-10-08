#!/bin/bash
set -euo pipefail
# Root's program (session/sudoers lets the desktop's account run it).
(( EUID == 0 )) && exec /usr/local/sbin/pixel-display-hook "$@"
exec sudo -n /usr/local/sbin/pixel-display-hook "$@"
