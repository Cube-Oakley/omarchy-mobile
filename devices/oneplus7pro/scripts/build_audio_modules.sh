#!/usr/bin/env bash
# Select existing configured drivers; keep the running kernel/config untouched.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KERNEL="$ROOT/.work/linux-sm8150-mmcx-sleep"
WORK="$ROOT/.work/audio-modules"
OUT="$ROOT/out/audio-test/modules"
mkdir -p "$WORK" "$OUT"
for part in sound/core sound/soc drivers/slimbus drivers/soundwire drivers/base/regmap drivers/mfd drivers/gpio; do
    mkdir -p "$WORK/$part"
    cp -a --reflink=auto "$KERNEL/$part/." "$WORK/$part/"
done
cp "$KERNEL/sound/sound_core.c" "$WORK/sound/"
patch -d "$WORK" -p1 < "$ROOT/kernel/power/q6routing-probe-order.patch"
patch -d "$WORK" -p1 < "$ROOT/kernel/audio/sm8150-speakers.patch"
patch -d "$WORK" -p1 < "$ROOT/kernel/audio/sm8150-slimbus-every-link.patch"
patch -d "$WORK" -p1 < "$ROOT/kernel/audio/q6asm-dai-xlate-by-id.patch"
patch -d "$WORK" -p1 < "$ROOT/kernel/audio/slim-ngd-pd-up-race.patch"
python3 - "$WORK" <<'PY'
from pathlib import Path
import re,sys
work=Path(sys.argv[1])
def select(path, objects):
    p=work/path/'Makefile'
    # Retain compound-object definitions and compile flags, replace obj-* lists.
    text=p.read_text()
    text=re.sub(r'^obj-[^\n]*(?:\\\n[^\n]*)*\n', '', text, flags=re.M)
    p.write_text(text+'\nobj-m += '+' '.join(objects)+'\n')
(work/'Makefile').write_text('obj-m += sound/ drivers/slimbus/ drivers/soundwire/ drivers/base/regmap/ drivers/mfd/ drivers/gpio/\n')
(work/'sound/Makefile').write_text('soundcore-y := sound_core.o\nobj-m += soundcore.o core/ soc/\n')
select('sound/core',['snd.o','snd-timer.o','snd-pcm.o','snd-pcm-dmaengine.o','snd-compress.o'])
select('sound/soc',['snd-soc-core.o','codecs/','qcom/'])
select('sound/soc/codecs',['snd-soc-wcd-classh.o','snd-soc-wcd-mbhc.o','snd-soc-wcd934x.o'])
select('sound/soc/qcom',['snd-soc-sm8150.o','snd-soc-qcom-common.o','snd-soc-qcom-sdw.o','qdsp6/'])
select('drivers/slimbus',['slim-qcom-ngd-ctrl.o'])
(work/'drivers/base/regmap/Makefile').write_text('obj-m += regmap-slimbus.o regmap-sdw.o\n')
(work/'drivers/mfd/Makefile').write_text('obj-m += wcd934x.o\n')
(work/'drivers/gpio/Makefile').write_text('obj-m += gpio-wcd934x.o\n')
PY
make -C "$KERNEL" ARCH=arm64 LLVM=1 -j"${JOBS:-8}" M="$WORK" \
    KBUILD_EXTRA_SYMBOLS="$KERNEL/vmlinux.symvers $ROOT/.work/native5-radio-modules/remoteproc/Module.symvers" modules
find "$WORK" -name '*.ko' -exec cp '{}' "$OUT/" \;
(cd "$OUT" && sha256sum ./*.ko > SHA256SUMS)
tar -C "$OUT" -czf "$ROOT/out/audio-test/audio-modules.tar.gz" .
