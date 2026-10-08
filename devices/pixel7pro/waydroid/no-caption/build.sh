#!/usr/bin/env bash
# On the laptop: build OmarchyNoCaption.apk, the static framework overlay
# that removes Android's caption bar from freeform windows.
#   build.sh FRAMEWORK_RES_APK BUILD_TOOLS_DIR [OUT_DIR]
# FRAMEWORK_RES_APK is the image's /system/framework/framework-res.apk (the
# overlay links against its private ids); BUILD_TOOLS_DIR has aapt2 and
# apksigner (Android build-tools 34). The signing key is made once in
# OUT_DIR: a preinstalled overlay needs a signature, not a particular one.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
framework="$1"
tools="$2"
out="${3:-$HOME/.cache/omarchy-waydroid/no-caption}"
mkdir -p "$out"
rm -rf "$out/compiled" && mkdir "$out/compiled"
"$tools/aapt2" compile --dir "$here/res" -o "$out/compiled"
"$tools/aapt2" link -o "$out/unsigned.apk" -I "$framework" \
    --manifest "$here/AndroidManifest.xml" --min-sdk-version 33 --target-sdk-version 33 \
    "$out"/compiled/*.flat
if [ ! -f "$out/key.jks" ]; then
    keytool -genkeypair -keystore "$out/key.jks" -storepass omarchy -keypass omarchy \
        -alias overlay -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Omarchy Mobile overlay" >/dev/null
fi
"$tools/apksigner" sign --ks "$out/key.jks" --ks-pass pass:omarchy \
    --out "$out/OmarchyNoCaption.apk" "$out/unsigned.apk"
"$tools/apksigner" verify "$out/OmarchyNoCaption.apk"
echo "$out/OmarchyNoCaption.apk"
