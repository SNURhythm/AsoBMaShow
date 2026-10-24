#!/usr/bin/env bash
# Run actual APK text/atlas code on a connected arm64 Android device, without
# installing an app or changing its data. APK must match the current headers.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APK="${1:-${ROOT_DIR}/android/app/build/outputs/apk/restricted_file_access/release/app-restricted_file_access-release.apk}"
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}}"
NDK="${ANDROID_NDK_HOME:-${SDK}/ndk/28.2.13676358}"
BUILD_TOOLS="${SDK}/build-tools/36.0.0"
ANDROID_JAR="${SDK}/platforms/android-36/android.jar"
ADB="${SDK}/platform-tools/adb"
case "$(uname -s)" in
  Darwin)
    HOST_TAG=darwin-x86_64
    JDK="$(/usr/libexec/java_home -v 17)"
    ;;
  Linux)
    HOST_TAG=linux-x86_64
    JDK="${JAVA_HOME:?Select JDK 17 with JAVA_HOME}"
    ;;
  *) echo "Unsupported NDK host" >&2; exit 2 ;;
esac
CXX="${NDK}/toolchains/llvm/prebuilt/${HOST_TAG}/bin/aarch64-linux-android28-clang++"
if [[ ! -f "${APK}" || ! -x "${CXX}" || ! -f "${ANDROID_JAR}" ]]; then
  echo "Requires an assembled APK, NDK 28.2.13676358 and Android SDK 36." >&2
  exit 2
fi
if [[ "$("${ADB}" shell getprop ro.product.cpu.abi | tr -d '\r')" != arm64-v8a ]]; then
  echo "Select one connected arm64 device (ANDROID_SERIAL is supported)." >&2
  exit 2
fi
STAGE="$(mktemp -d /tmp/asobmashow-emoji-verify.XXXXXX)"
REMOTE="/data/local/tmp/$(basename "${STAGE}")"
OUTPUT="${ANDROID_EMOJI_TEST_OUTPUT:-$(mktemp -d /tmp/asobmashow-emoji-results.XXXXXX)}"
cleanup() {
  "${ADB}" shell rm -rf "${REMOTE}" >/dev/null 2>&1 || true
  rm -rf "${STAGE}"
}
trap cleanup EXIT
mkdir -p "${STAGE}/device" "${STAGE}/classes" "${OUTPUT}"
python3 - "${APK}" "${STAGE}/device" <<'PY'
from pathlib import Path
import re
import sys
import zipfile

with zipfile.ZipFile(sys.argv[1]) as apk, zipfile.ZipFile(Path(sys.argv[2]) / "app.jar", "w") as dex:
    for name in apk.namelist():
        if re.fullmatch(r"classes\d*\.dex", name):
            dex.writestr(name, apk.read(name))
        elif name.startswith("lib/arm64-v8a/") and name.endswith(".so"):
            (Path(sys.argv[2]) / Path(name).name).write_bytes(apk.read(name))
        elif name.startswith("assets/assets/fonts/") and not name.endswith("/"):
            target = Path(sys.argv[2]) / "assets/fonts" / Path(name).name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(apk.read(name))
PY
"${CXX}" -std=c++23 -shared -fPIC -D_LIBCPP_ENABLE_EXPERIMENTAL \
  -DASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1 \
  -I"${ROOT_DIR}/src" -I"${ROOT_DIR}/include" \
  -I"${ROOT_DIR}/SDL/include" -I"${ROOT_DIR}/SDL_ttf/include" \
  -I"${ROOT_DIR}/bgfx/bgfx/include" -I"${ROOT_DIR}/bgfx/bx/include" \
  -I"${ROOT_DIR}/bgfx/bimg/include" -I"${ROOT_DIR}/yoga" \
  "${ROOT_DIR}/tests/android_emoji_device_tests.cpp" \
  -L"${STAGE}/device" -lmain -lSDL3 -lSDL3_ttf \
  -o "${STAGE}/device/libemoji_device_tests.so"
"${JDK}/bin/javac" -cp "${ANDROID_JAR}" -d "${STAGE}/classes" \
  "${ROOT_DIR}/tests/android/EmojiDeviceTest.java"
JAVA_HOME="${JDK}" "${BUILD_TOOLS}/d8" --lib "${ANDROID_JAR}" --output "${STAGE}/device" \
  "${STAGE}/classes/com/snurhythm/asobmashow/EmojiDeviceTest.class"
"${ADB}" push "${STAGE}/device/." "${REMOTE}/" >/dev/null
"${ADB}" shell "CLASSPATH=${REMOTE}/classes.dex:${REMOTE}/app.jar LD_LIBRARY_PATH=${REMOTE} app_process /system/bin com.snurhythm.asobmashow.EmojiDeviceTest ${REMOTE}" \
  | tee "${OUTPUT}/results.txt"
"${ADB}" pull "${REMOTE}/builtin-emoji.bmp" "${OUTPUT}/builtin-emoji.bmp" >/dev/null
printf 'Emoji test evidence: %s\n' "${OUTPUT}"
