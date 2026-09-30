#!/usr/bin/env bash
# Build qrtr-smd.ko with kernel/radio/qrtr-smd-wake.patch (an incoming call or
# text wakes the phone from s2idle) against the running #194 tree. Only
# qrtr-smd.ko is meant to replace the radio set's copy; qrtr.ko is rebuilt
# alongside for its symbols. Deploys nothing.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KERNEL="${KERNEL_TREE:-$ROOT/.work/linux-sm8150-194}"
WORK="$ROOT/.work/qrtr-wake"
OUT="$ROOT/out/cellular/modules"
rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"
cp -a "$KERNEL/net/qrtr/." "$WORK/"
patch -s -p3 -d "$WORK" < "$ROOT/kernel/radio/qrtr-smd-wake.patch"
make -C "$KERNEL" ARCH=arm64 LLVM=1 M="$WORK" KBUILD_MODPOST_WARN=1 \
    KBUILD_EXTRA_SYMBOLS="$KERNEL/vmlinux.symvers" CONFIG_QRTR_MHI=n modules
cp "$WORK/qrtr-smd.ko" "$OUT/"
modinfo -F vermagic "$OUT/qrtr-smd.ko"
(cd "$OUT" && sha256sum qrtr-smd.ko > qrtr-smd.sha256)
