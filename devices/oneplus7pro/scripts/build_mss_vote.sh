#!/usr/bin/env bash
# Build kernel/radio/mss_vote.c against the running #194 tree.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KERNEL="${KERNEL_TREE:-$ROOT/.work/linux-sm8150-194}"
WORK="$ROOT/.work/mss-vote"
OUT="$ROOT/out/cellular/modules"
mkdir -p "$WORK" "$OUT"
cp "$ROOT/kernel/radio/mss_vote.c" "$WORK/"
printf 'obj-m += mss_vote.o\n' > "$WORK/Makefile"
make -C "$KERNEL" ARCH=arm64 LLVM=1 M="$WORK" \
    KBUILD_EXTRA_SYMBOLS="$KERNEL/vmlinux.symvers" modules
cp "$WORK/mss_vote.ko" "$OUT/"
modinfo -F vermagic "$OUT/mss_vote.ko"
(cd "$OUT" && sha256sum mss_vote.ko > mss_vote.sha256)
