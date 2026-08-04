#!/usr/bin/env bash
set -euo pipefail

EXPECTED_VERSION="0.0.1"
EXPECTED_MIN_OS="14.0"
EXPECTED_BUNDLE_ID="com.snurhythm.AsoBMaShow"
REQUIRE_SIGNATURE=0
TEMP_DIR=""

usage() {
  cat <<'USAGE'
Usage: scripts/ios_artifact_audit.sh [--require-signature] APP_OR_IPA

Audits an iOS .app or .ipa for the AsoBMaShow 0.0.1 / iOS 14 release contract.
A privacy manifest is neither required nor synthesized by this gate.
USAGE
}

fail() {
  echo "iOS artifact audit failed: $*" >&2
  exit 1
}

cleanup() {
  if [ -n "${TEMP_DIR}" ] && [ -d "${TEMP_DIR}" ]; then
    rm -rf "${TEMP_DIR}"
  fi
}
trap cleanup EXIT

if [ "${1:-}" = "--require-signature" ]; then
  REQUIRE_SIGNATURE=1
  shift
fi
if [ "$#" -ne 1 ]; then
  usage >&2
  exit 2
fi

ARTIFACT="$1"
[ -e "${ARTIFACT}" ] || fail "artifact does not exist: ${ARTIFACT}"
ARTIFACT_DIR="$(cd "$(dirname "${ARTIFACT}")" && pwd -P)"
ARTIFACT="${ARTIFACT_DIR}/$(basename "${ARTIFACT}")"

APP_PATH=""
case "${ARTIFACT}" in
  *.app)
    [ -d "${ARTIFACT}" ] || fail "app path is not a directory"
    APP_PATH="${ARTIFACT}"
    ;;
  *.ipa)
    TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/asobmashow-ios-audit.XXXXXX")"
    UNSAFE_ENTRY="$(/usr/bin/zipinfo -1 "${ARTIFACT}" | \
      awk '$0 ~ /(^|\/)\.\.($|\/)|^\// { print; exit }')"
    [ -z "${UNSAFE_ENTRY}" ] || fail "IPA contains an unsafe archive path: ${UNSAFE_ENTRY}"
    /usr/bin/unzip -q "${ARTIFACT}" -d "${TEMP_DIR}"
    APPS=()
    while IFS= read -r app; do
      APPS+=("${app}")
    done < <(find "${TEMP_DIR}/Payload" -mindepth 1 -maxdepth 1 -type d -name '*.app' -print 2>/dev/null)
    [ "${#APPS[@]}" -eq 1 ] || fail "IPA must contain exactly one Payload/*.app"
    APP_PATH="${APPS[0]}"
    ;;
  *)
    fail "expected an .app directory or .ipa file"
    ;;
esac

INFO_PLIST="${APP_PATH}/Info.plist"
[ -f "${INFO_PLIST}" ] || fail "Info.plist is missing"
plutil -lint "${INFO_PLIST}" >/dev/null || fail "Info.plist is invalid"

plist_raw() {
  plutil -extract "$1" raw -o - "${INFO_PLIST}" 2>/dev/null || true
}

VERSION="$(plist_raw CFBundleShortVersionString)"
[ "${VERSION}" = "${EXPECTED_VERSION}" ] || \
  fail "version must be ${EXPECTED_VERSION}, found ${VERSION:-missing}"

MINIMUM_OS="$(plist_raw MinimumOSVersion)"
[ "${MINIMUM_OS}" = "${EXPECTED_MIN_OS}" ] || \
  fail "minimum OS must be ${EXPECTED_MIN_OS}, found ${MINIMUM_OS:-missing}"

BUNDLE_ID="$(plist_raw CFBundleIdentifier)"
[ "${BUNDLE_ID}" = "${EXPECTED_BUNDLE_ID}" ] || \
  fail "bundle identifier must be ${EXPECTED_BUNDLE_ID}, found ${BUNDLE_ID:-missing}"

SDK_NAME="$(plist_raw DTSDKName)"
PLATFORM_NAME="$(plist_raw DTPlatformName)"
SUPPORTED_PLATFORMS="$(plutil -extract CFBundleSupportedPlatforms json -o - "${INFO_PLIST}" 2>/dev/null || true)"
[[ "${SDK_NAME}" =~ ^iphoneos[0-9]+([.][0-9]+)?$ ]] || \
  fail "SDK must be an iphoneos SDK, found ${SDK_NAME:-missing}"
[ "${PLATFORM_NAME}" = "iphoneos" ] || fail "SDK platform must be iphoneos"
[[ "${SUPPORTED_PLATFORMS}" == *'"iPhoneOS"'* ]] || \
  fail "supported SDK platform must include iPhoneOS"

DEVICE_FAMILY="$(plutil -extract UIDeviceFamily json -o - "${INFO_PLIST}" 2>/dev/null || true)"
[[ "${DEVICE_FAMILY}" =~ (^|[^0-9])1([^0-9]|$) ]] && \
  [[ "${DEVICE_FAMILY}" =~ (^|[^0-9])2([^0-9]|$) ]] || \
  fail "device family must include iPhone (1) and iPad (2)"

ICON_NAME="$(plist_raw CFBundleIcons.CFBundlePrimaryIcon.CFBundleIconName)"
[ -n "${ICON_NAME}" ] || fail "icon metadata is missing CFBundleIconName"
ICON_FILE="$(find "${APP_PATH}" -maxdepth 1 -type f -name "${ICON_NAME}*.png" -print -quit)"
[ -n "${ICON_FILE}" ] || fail "icon metadata has no matching compiled icon file"

for permission in \
  NSMotionUsageDescription \
  NSPhotoLibraryUsageDescription \
  NSPhotoLibraryAddUsageDescription; do
  [ -n "$(plist_raw "${permission}")" ] || fail "${permission} is missing or empty"
done

[ "$(plist_raw NSAppTransportSecurity.NSAllowsArbitraryLoads)" = "true" ] || \
  fail "NSAllowsArbitraryLoads must remain true for difficulty-table loading"

for files_access_key in \
  UIFileSharingEnabled \
  LSSupportsOpeningDocumentsInPlace \
  UISupportsDocumentBrowser; do
  [ "$(plist_raw "${files_access_key}")" = "true" ] || \
    fail "${files_access_key} must be true for Files-based skin management"
done

EXECUTABLE_NAME="$(plist_raw CFBundleExecutable)"
[ -n "${EXECUTABLE_NAME}" ] || fail "CFBundleExecutable is missing"
MAIN_EXECUTABLE="${APP_PATH}/${EXECUTABLE_NAME}"
[ -f "${MAIN_EXECUTABLE}" ] || fail "main executable is missing"

for stage in vs fs; do
  SKIN_SHADER="${APP_PATH}/shaders/metal/${stage}_skin_quad.bin"
  [ -f "${SKIN_SHADER}" ] || \
    fail "Metal skin shader is missing: shaders/metal/${stage}_skin_quad.bin"
  [ -s "${SKIN_SHADER}" ] || \
    fail "Metal skin shader is empty: shaders/metal/${stage}_skin_quad.bin"
done

BINARY_DESCRIPTION="$(file "${MAIN_EXECUTABLE}")"
[[ "${BINARY_DESCRIPTION}" == *"Mach-O"* ]] || fail "main executable is not Mach-O"
ARCHITECTURES="$(lipo -archs "${MAIN_EXECUTABLE}" 2>/dev/null || true)"
[ "${ARCHITECTURES}" = "arm64" ] || \
  fail "main executable architectures must be device arm64 only: ${ARCHITECTURES:-missing}"

LOAD_COMMANDS="$(otool -l "${MAIN_EXECUTABLE}")"
BUILD_PLATFORM="$(printf '%s\n' "${LOAD_COMMANDS}" | awk '
  $1 == "cmd" && $2 == "LC_BUILD_VERSION" { active = 1; next }
  active && $1 == "platform" { print $2; exit }
')"
BUILD_MIN_OS="$(printf '%s\n' "${LOAD_COMMANDS}" | awk '
  $1 == "cmd" && $2 == "LC_BUILD_VERSION" { active = 1; next }
  active && $1 == "minos" { print $2; exit }
')"
[ "${BUILD_PLATFORM}" = "2" ] || fail "Mach-O SDK platform is not iOS"
[ "${BUILD_MIN_OS}" = "${EXPECTED_MIN_OS}" ] || \
  fail "Mach-O minimum OS must be ${EXPECTED_MIN_OS}, found ${BUILD_MIN_OS:-missing}"

LOCAL_RPATH="$(printf '%s\n' "${LOAD_COMMANDS}" | awk '
  $1 == "cmd" && $2 == "LC_RPATH" { active = 1; next }
  active && $1 == "path" { print $2; active = 0 }
' | grep -E '^(/opt/homebrew|/usr/local|/Users/)' | head -n 1 || true)"
[ -z "${LOCAL_RPATH}" ] || fail "local build dependency path is embedded: ${LOCAL_RPATH}"

BINARIES=("${MAIN_EXECUTABLE}")
while IFS= read -r framework_plist; do
  framework_executable="$(plutil -extract CFBundleExecutable raw -o - "${framework_plist}" 2>/dev/null || true)"
  [ -n "${framework_executable}" ] || fail "framework CFBundleExecutable is missing: ${framework_plist}"
  framework_binary="$(dirname "${framework_plist}")/${framework_executable}"
  [ -f "${framework_binary}" ] || fail "framework executable is missing: ${framework_binary}"
  BINARIES+=("${framework_binary}")
done < <(find "${APP_PATH}/Frameworks" -type f -path '*.framework/Info.plist' -print 2>/dev/null)

for binary in "${BINARIES[@]}"; do
  binary_description="$(file "${binary}")"
  [[ "${binary_description}" == *"Mach-O"* ]] || \
    fail "embedded executable is not Mach-O: ${binary}"
  binary_architectures="$(lipo -archs "${binary}" 2>/dev/null || true)"
  [ "${binary_architectures}" = "arm64" ] || \
    fail "embedded binary architectures must be device arm64 only: ${binary} (${binary_architectures:-missing})"

  binary_load_commands="$(otool -l "${binary}")"
  binary_platform="$(printf '%s\n' "${binary_load_commands}" | awk '
    $1 == "cmd" && $2 == "LC_BUILD_VERSION" { active = 1; next }
    active && $1 == "platform" { print $2; exit }
  ')"
  [ "${binary_platform}" = "2" ] || \
    fail "embedded binary SDK platform is not iOS: ${binary}"
  binary_min_os="$(printf '%s\n' "${binary_load_commands}" | awk '
    $1 == "cmd" && $2 == "LC_BUILD_VERSION" { active = 1; next }
    active && $1 == "minos" { print $2; exit }
  ')"
  [ "${binary_min_os}" = "${EXPECTED_MIN_OS}" ] || \
    fail "embedded binary minimum OS must be ${EXPECTED_MIN_OS}: ${binary} (${binary_min_os:-missing})"
  binary_local_rpath="$(printf '%s\n' "${binary_load_commands}" | awk '
    $1 == "cmd" && $2 == "LC_RPATH" { active = 1; next }
    active && $1 == "path" { print $2; active = 0 }
  ' | grep -E '^(/opt/homebrew|/usr/local|/Users/)' | head -n 1 || true)"
  [ -z "${binary_local_rpath}" ] || \
    fail "local build dependency path is embedded: ${binary_local_rpath}"

  while IFS= read -r dependency; do
    case "${dependency}" in
      /System/Library/*|/usr/lib/*)
        ;;
      /opt/homebrew/*|/usr/local/*|/Users/*)
        fail "local build dependency is embedded: ${dependency}"
        ;;
      @rpath/*)
        relative="${dependency#@rpath/}"
        [ -e "${APP_PATH}/Frameworks/${relative}" ] || \
          fail "unresolved embedded dependency: ${dependency}"
        ;;
      @executable_path/*)
        relative="${dependency#@executable_path/}"
        [ -e "${APP_PATH}/${relative}" ] || \
          fail "unresolved executable dependency: ${dependency}"
        ;;
      @loader_path/*)
        relative="${dependency#@loader_path/}"
        [ -e "$(dirname "${binary}")/${relative}" ] || \
          fail "unresolved loader dependency: ${dependency}"
        ;;
      *)
        fail "unsupported dependency path: ${dependency}"
        ;;
    esac
  done < <(otool -L "${binary}" | tail -n +2 | awk '{ print $1 }')
done

UNWANTED="$(find "${APP_PATH}" \( \
  -name '.DS_Store' -o \
  -name '._*' -o \
  -name '.env' -o \
  -name '.env.*' -o \
  -name '*.p8' -o \
  -name '*.pem' \
\) -print -quit)"
[ -z "${UNWANTED}" ] || fail "unwanted file is embedded: ${UNWANTED}"

SECRET_PATTERN='BEGIN ([A-Z]+ )?PRIVATE KEY|APP_STORE_KEY[[:space:]:=]|MATCH_PASSWORD[[:space:]:=]|FIREBASE_(CLI_)?TOKEN[[:space:]:=]|Authorization:[[:space:]]*Bearer'
if LC_ALL=C grep -R -I -q -E "${SECRET_PATTERN}" "${APP_PATH}" 2>/dev/null; then
  fail "credential material is embedded in resources"
fi

for binary in "${BINARIES[@]}"; do
  if LC_ALL=C grep -a -q -E "${SECRET_PATTERN}" "${binary}"; then
    fail "credential material is embedded in binary: ${binary}"
  fi
done

if codesign -dv "${APP_PATH}" >/dev/null 2>&1; then
  codesign --verify --deep --strict "${APP_PATH}" >/dev/null 2>&1 || \
    fail "signature verification failed"
  echo "signature check passed"
elif [ "${REQUIRE_SIGNATURE}" -eq 1 ]; then
  fail "signature is required but the app is unsigned"
else
  echo "signature check skipped (unsigned build)"
fi

echo "iOS artifact audit passed: ${APP_PATH}"
