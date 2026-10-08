#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TEST_DIR=$(mktemp -d /tmp/omarchy-mobile-qml.XXXXXX)
trap 'rm -rf "$TEST_DIR"' EXIT
# Isolate the pure Qt input/motion components from Quickshell's application-only
# plugins and the real desktop's theme, keyboard and compositor services.
cp "$ROOT"/overlay/mobile/{DrawerMotion,PreviewGestures,EdgeGestures,LeftBack,HeldPreview,DrawerPull,TouchTextField,KeyboardDismiss,NotificationGroups,CrtPhases,SnapPager,CardList,CardThrow,VolumeGroupSlider}.qml "$TEST_DIR/"
cp "$ROOT"/overlay/mobile/kit/{CoverFlowPicker,MenuOverlay,ImagePreloader}.qml "$TEST_DIR/"
cp "$ROOT"/overlay/mobile/apps/calculator/calc.js "$TEST_DIR/"
cp "$ROOT"/tests/qml/tst_*.qml "$TEST_DIR/"
QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME=basic QT_QUICK_BACKEND=software \
    /usr/lib/qt6/bin/qmltestrunner -input "$TEST_DIR" "$@"
