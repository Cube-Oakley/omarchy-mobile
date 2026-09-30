#!/usr/bin/env bash
# Build the q6voice modules (call audio: MVM/CVS/CVP sessions on the ADSP and
# the voice DAI) against the same tree and symbols as build_audio_modules.sh,
# so they load next to the running audio modules. Sources: kernel/audio/q6voice.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KERNEL="$ROOT/.work/linux-sm8150-mmcx-sleep"
AUDIO="$ROOT/.work/audio-modules"
WORK="$ROOT/.work/voice-modules"
OUT="$ROOT/out/audio-test/voice-modules"
[[ -f $AUDIO/Module.symvers ]] || { echo "Run build_audio_modules.sh first" >&2; exit 1; }
rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"
cp -a "$ROOT"/kernel/audio/q6voice/. "$WORK/"
# q6voice uses q6afe's port lookup; take the header from the tree that built q6afe.
cp "$KERNEL/sound/soc/qcom/qdsp6/q6afe.h" "$KERNEL/sound/soc/qcom/qdsp6/q6dsp-common.h" "$WORK/"
for patch in "$ROOT"/kernel/audio/q6voice-patches/*.patch; do
    [[ -f $patch ]] && patch -d "$WORK" -p1 < "$patch"
done
cat > "$WORK/Kbuild" <<'EOF'
ccflags-y += -I$(src)/include
obj-m += q6voice-common.o q6mvm.o q6cvs.o q6cvp.o q6voice.o q6voice-dai.o
EOF
make -C "$KERNEL" ARCH=arm64 LLVM=1 -j"${JOBS:-8}" M="$WORK" \
    KBUILD_EXTRA_SYMBOLS="$KERNEL/vmlinux.symvers $AUDIO/Module.symvers" modules
rm -f "$OUT"/*.ko
for module in q6voice-common q6mvm q6cvs q6cvp q6voice q6voice-dai; do
    llvm-strip --strip-debug -o "$OUT/$module.ko" "$WORK/$module.ko"
done

# Two overlay modules: the APR voice services (before the ADSP starts) and
# the VoiceMMode1 card link (after the card overlay). Checked by merging onto
# the microphone stage, the last stage the running card was built from.
overlay() {
    local name=$1 base=$2
    local dir="$WORK/overlay-$name"
    mkdir -p "$dir"
    cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I "$KERNEL/include" \
        -I "$ROOT/kernel/audio/q6voice/include" "$ROOT/kernel/audio/guacamole-$name.dts" "$dir/$name.dts"
    dtc -@ -I dts -O dtb "$dir/$name.dts" -o "$OUT/$name.dtbo"
    fdtoverlay -i "$base" -o "$OUT/$name-merged.dtb" "$OUT/$name.dtbo"
    local symbol=${name//-/_}
    cp "$ROOT/kernel/audio/${symbol}_overlay.c" "$dir/"
    python3 - "$OUT/$name.dtbo" "$dir/${symbol}_dtbo.h" "$symbol" <<'PY2'
from pathlib import Path
import sys
blob = Path(sys.argv[1]).read_bytes()
Path(sys.argv[2]).write_text(f'static const unsigned char {sys.argv[3]}_dtbo[] __aligned(8) = {{\n'
                             + ','.join(map(str, blob)) + '\n};\n')
PY2
    printf 'obj-m += guacamole_%s.o\nguacamole_%s-y := %s_overlay.o\n' "$symbol" "$symbol" "$symbol" > "$dir/Makefile"
    make -C "$KERNEL" ARCH=arm64 LLVM=1 M="$dir" KBUILD_EXTRA_SYMBOLS="$KERNEL/vmlinux.symvers" modules
    cp "$dir/guacamole_$symbol.ko" "$OUT/"
}
overlay voice-services "$ROOT/out/audio-test/microphone/merged.dtb"
overlay voice-link "$OUT/voice-services-merged.dtb"
(cd "$OUT" && sha256sum ./*.ko ./*.dtbo > SHA256SUMS)
cat "$OUT/SHA256SUMS"
