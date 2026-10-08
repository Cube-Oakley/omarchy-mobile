#!/usr/bin/env bash
# On the laptop: build OmarchyKeyboard.apk, Android's input method that hands
# typing to the phone's own keyboard (see ../README.md).
#   build.sh ANDROID_JAR BUILD_TOOLS_DIR ECJ_JAR [OUT_DIR]
# ANDROID_JAR is the API 33 platform's android.jar; BUILD_TOOLS_DIR has aapt2,
# d8 and apksigner (build-tools 34); ECJ_JAR is the Eclipse Java compiler,
# which runs on a plain Java runtime (no JDK needed). The signing key is
# made once in OUT_DIR; Android only accepts updates signed with the same key.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
android_jar="$1"
tools="$2"
ecj="$3"
out="${4:-$HOME/.cache/omarchy-waydroid/keyboard-bridge}"
mkdir -p "$out"
rm -rf "$out/classes" "$out/dex" "$out/compiled" && mkdir -p "$out/classes" "$out/dex" "$out/compiled"
java -jar "$ecj" -source 1.8 -target 1.8 -nowarn -bootclasspath "$android_jar" -d "$out/classes" \
    $(find "$here/src" -name '*.java')
"$tools/d8" --release --min-api 33 --lib "$android_jar" --output "$out/dex" \
    $(find "$out/classes" -name '*.class')
"$tools/aapt2" compile --dir "$here/res" -o "$out/compiled"
"$tools/aapt2" link -o "$out/unsigned.apk" -I "$android_jar" \
    --manifest "$here/AndroidManifest.xml" --min-sdk-version 33 --target-sdk-version 33 \
    "$out"/compiled/*.flat
(cd "$out/dex" && zip -q -j "$out/unsigned.apk" classes.dex)
"$tools/zipalign" -f -p 4 "$out/unsigned.apk" "$out/aligned.apk"
if [ ! -f "$out/key.jks" ]; then
    keytool -genkeypair -keystore "$out/key.jks" -storepass omarchy -keypass omarchy \
        -alias keyboard -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Omarchy Mobile keyboard" >/dev/null
fi
"$tools/apksigner" sign --ks "$out/key.jks" --ks-pass pass:omarchy \
    --out "$out/OmarchyKeyboard.apk" "$out/aligned.apk"
echo "$out/OmarchyKeyboard.apk"
