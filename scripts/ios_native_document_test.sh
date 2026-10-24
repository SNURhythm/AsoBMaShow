#!/usr/bin/env bash
set -euo pipefail

# Uses an already booted simulator; never builds or installs the actual app.
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SIMULATOR="${1:-booted}"
TEST_ROOT="$(mktemp -d /tmp/asobmashow-ios-document.XXXXXX)"
TEST_BUNDLE="com.snurhythm.NativeDocumentTests"
TEST_APP="$TEST_ROOT/NativeDocumentTests.app"
cleanup() {
  xcrun simctl uninstall "$SIMULATOR" "$TEST_BUNDLE" >/dev/null 2>&1 || true
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT
mkdir -p "$TEST_APP"
cat > "$TEST_APP/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.snurhythm.NativeDocumentTests</string>
<key>CFBundleExecutable</key><string>NativeDocumentTests</string>
<key>CFBundleName</key><string>NativeDocumentTests</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>LSRequiresIPhoneOS</key><true/><key>UILaunchScreen</key><dict/>
<key>UIApplicationSceneManifest</key><dict><key>UISceneConfigurations</key><dict>
<key>UIWindowSceneSessionRoleApplication</key><array><dict>
<key>UISceneConfigurationName</key><string>Native document tests</string>
<key>UISceneDelegateClassName</key><string>NativeDocumentTestScene</string>
</dict></array></dict></dict>
</dict></plist>
PLIST
xcrun --sdk iphonesimulator clang++ -std=c++23 -fobjc-arc \
  -target "$(uname -m)-apple-ios17.0-simulator" \
  -isysroot "$(xcrun --sdk iphonesimulator --show-sdk-path)" \
  -I"$ROOT_DIR/src" -I"$ROOT_DIR/SDL/include" \
  -DASOBMASHOW_NATIVE_DOCUMENT_TEST_STUB \
  "$ROOT_DIR/tests/native_document_ios_tests.mm" \
  "$ROOT_DIR/src/platform/NativeDocumentApple.mm" \
  "$ROOT_DIR/src/platform/NativeDocument.cpp" "$ROOT_DIR/src/view/BmsSyntax.cpp" \
  -framework UIKit -framework Foundation -framework CoreGraphics -framework QuartzCore \
  -o "$TEST_APP/NativeDocumentTests"
codesign -s - --force "$TEST_APP"
xcrun simctl install "$SIMULATOR" "$TEST_APP"
# simctl can return zero after an application assertion, so require its marker.
python3 - "$SIMULATOR" "$TEST_BUNDLE" <<'PY'
import subprocess
import sys
result = subprocess.run(["xcrun", "simctl", "launch", "--terminate-running-process", "--console",
                         sys.argv[1], sys.argv[2]], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True, timeout=60)
print(result.stdout, end="")
sys.exit(0 if result.returncode == 0 and "PASS native document UIKit:" in result.stdout
         and "FAIL" not in result.stdout else 1)
PY
