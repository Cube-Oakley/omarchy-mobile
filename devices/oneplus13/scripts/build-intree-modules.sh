#!/bin/sh
# Rebuild every in-tree module the port uses in one pass, so the kernel tree's
# Module.symvers lists all their exports (a single-target build rewrites it
# with only that target). Out-of-tree modules then link against all of them.
# qcom-cpucp-mbox.ko is built separately: it lacks MODULE_LICENSE in 7.3-rc2.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
make -C "$here/out/linux-7.3-rc2" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    $(grep -v '^#' "$here/kernel/intree-modules.txt") "$@"
