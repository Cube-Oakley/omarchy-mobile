#!/usr/bin/env bash
# Build-only: the sensor DSP's kernel pieces for the running native5 kernel
# (docs/sensors-20260924.md):
#   guacamole_slpi.ko   overlay that starts SLPI, only on kernel #193+
#   fastrpc.ko          the sensors domain's messages from the reserved pool;
#                       a waiting invoke survives system suspend
#   qcom_pd_mapper.ko   SM8150's table with SLPI's protection domains; it
#                       replaces the radio set's copy, which loads at boot
#   guacamole_smem_info.ko  read-only copies of the bootloader's project
#                       records (SMEM items 135, 136) in debugfs; diagnostic
# Deploys nothing.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KERNEL="$ROOT/.work/linux-sm8150-mmcx-sleep"
WORK="$ROOT/.work/sensor-modules"
OUT="$ROOT/out/sensors/modules"
SRC="$ROOT/kernel/sensors"
rm -rf "$WORK"
mkdir -p "$WORK/overlay" "$WORK/fastrpc" "$WORK/pdmapper" "$WORK/smem" "$OUT"
make_modules() {
    make -C "$KERNEL" ARCH=arm64 LLVM=1 M="$1" KBUILD_MODPOST_WARN=1 \
        KBUILD_EXTRA_SYMBOLS="$KERNEL/vmlinux.symvers ${2:-}" modules
}

cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I "$KERNEL/include" \
    "$SRC/guacamole-slpi.dts" "$WORK/overlay/overlay.dts"
dtc -@ -I dts -O dtb "$WORK/overlay/overlay.dts" -o "$OUT/guacamole_slpi.dtbo"
python3 - "$OUT/guacamole_slpi.dtbo" "$WORK/overlay/overlay.h" <<'PY'
from pathlib import Path
import sys
blob = Path(sys.argv[1]).read_bytes()
Path(sys.argv[2]).write_text('static const unsigned char overlay_dtbo[] __aligned(8) = {\n'
                             + ','.join(map(str, blob)) + '\n};\n')
PY
cp "$SRC/guacamole_slpi.c" "$WORK/overlay/"
printf 'obj-m += guacamole_slpi.o\n' > "$WORK/overlay/Makefile"
make_modules "$WORK/overlay"

cp "$KERNEL/drivers/misc/fastrpc.c" "$WORK/fastrpc/"
patch -s -p3 -d "$WORK/fastrpc" < "$SRC/fastrpc-sensors-pool.patch"
patch -s -p3 -d "$WORK/fastrpc" < "$SRC/fastrpc-freezable-wait.patch"
printf 'obj-m += fastrpc.o\n' > "$WORK/fastrpc/Makefile"
make_modules "$WORK/fastrpc"

cp "$KERNEL/drivers/soc/qcom/qcom_pd_mapper.c" "$KERNEL/drivers/soc/qcom/pdr_internal.h" "$WORK/pdmapper/"
patch -s -p4 -d "$WORK/pdmapper" < "$SRC/pd-mapper-sm8150-slpi.patch"
printf 'obj-m += qcom_pd_mapper.o\n' > "$WORK/pdmapper/Makefile"
make_modules "$WORK/pdmapper"

cp "$SRC/guacamole_smem_info.c" "$WORK/smem/"
printf 'obj-m += guacamole_smem_info.o\n' > "$WORK/smem/Makefile"
make_modules "$WORK/smem"

cp "$WORK/overlay/guacamole_slpi.ko" "$WORK/fastrpc/fastrpc.ko" "$WORK/pdmapper/qcom_pd_mapper.ko" \
    "$WORK/smem/guacamole_smem_info.ko" "$OUT/"
(cd "$OUT" && sha256sum ./*.ko ./*.dtbo > SHA256SUMS)
ls -l "$OUT"
