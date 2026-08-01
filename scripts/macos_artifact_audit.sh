#!/usr/bin/env bash
set -euo pipefail

EXPECTED_VERSION="${ASOBMASHOW_MACOS_VERSION:-0.0.1}"
EXPECTED_MIN_OS="${ASOBMASHOW_MACOS_DEPLOYMENT_TARGET:-13.0}"
EXPECTED_BUNDLE_ID="${ASOBMASHOW_MACOS_BUNDLE_ID:-com.SNURhythm.AsoBMaShow}"
EXPECTED_ARCHS="${ASOBMASHOW_MACOS_ARCHS:-arm64}"
REQUIRE_SIGNATURE=0
REQUIRE_GATEKEEPER=0

usage() {
  cat <<'USAGE'
Usage: scripts/macos_artifact_audit.sh [options] APP

Audits a macOS AsoBMaShow.app without publishing it.

Options:
  --require-signature   Require a hardened Developer ID Application signature.
  --require-gatekeeper  Require a valid notarization ticket and Gatekeeper approval.
  -h, --help            Show this help.
USAGE
}

fail() {
  echo "macOS artifact audit failed: $*" >&2
  exit 1
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --require-signature)
      REQUIRE_SIGNATURE=1
      shift
      ;;
    --require-gatekeeper)
      REQUIRE_SIGNATURE=1
      REQUIRE_GATEKEEPER=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      break
      ;;
  esac
done

[ "$#" -eq 1 ] || { usage >&2; exit 2; }
APP_PATH="$1"
[ -d "${APP_PATH}" ] || fail "app bundle does not exist: ${APP_PATH}"
case "${APP_PATH}" in
  *.app) ;;
  *) fail "expected an .app directory" ;;
esac
APP_PARENT="$(cd "$(dirname "${APP_PATH}")" && pwd -P)"
APP_PATH="${APP_PARENT}/$(basename "${APP_PATH}")"

for command_name in plutil file lipo otool vtool codesign strings; do
  command -v "${command_name}" >/dev/null 2>&1 || \
    fail "required tool is unavailable: ${command_name}"
done

INFO_PLIST="${APP_PATH}/Contents/Info.plist"
[ -f "${INFO_PLIST}" ] || fail "Contents/Info.plist is missing"
plutil -lint "${INFO_PLIST}" >/dev/null || fail "Info.plist is invalid"

plist_raw() {
  plutil -extract "$1" raw -o - "${INFO_PLIST}" 2>/dev/null || true
}

VERSION="$(plist_raw CFBundleShortVersionString)"
[ "${VERSION}" = "${EXPECTED_VERSION}" ] || \
  fail "version must be ${EXPECTED_VERSION}, found ${VERSION:-missing}"

BUILD_VERSION="$(plist_raw CFBundleVersion)"
[ -n "${BUILD_VERSION}" ] || fail "CFBundleVersion is missing"

MINIMUM_OS="$(plist_raw LSMinimumSystemVersion)"
[ "${MINIMUM_OS}" = "${EXPECTED_MIN_OS}" ] || \
  fail "minimum OS must be ${EXPECTED_MIN_OS}, found ${MINIMUM_OS:-missing}"

BUNDLE_ID="$(plist_raw CFBundleIdentifier)"
[ "${BUNDLE_ID}" = "${EXPECTED_BUNDLE_ID}" ] || \
  fail "bundle identifier must be ${EXPECTED_BUNDLE_ID}, found ${BUNDLE_ID:-missing}"

ICON_NAME="$(plist_raw CFBundleIconFile)"
[ -n "${ICON_NAME}" ] || fail "CFBundleIconFile is missing"
case "${ICON_NAME}" in
  *.icns) ;;
  *) ICON_NAME="${ICON_NAME}.icns" ;;
esac
[ -f "${APP_PATH}/Contents/Resources/${ICON_NAME}" ] || \
  fail "bundle icon is missing: Contents/Resources/${ICON_NAME}"
[ -d "${APP_PATH}/Contents/Resources/assets" ] || fail "assets are missing"
[ -d "${APP_PATH}/Contents/Resources/shaders/metal" ] || \
  fail "Metal shaders are missing"

EXECUTABLE_NAME="$(plist_raw CFBundleExecutable)"
[ -n "${EXECUTABLE_NAME}" ] || fail "CFBundleExecutable is missing"
MAIN_EXECUTABLE="${APP_PATH}/Contents/MacOS/${EXECUTABLE_NAME}"
[ -f "${MAIN_EXECUTABLE}" ] || fail "main executable is missing"

BINARIES=("${MAIN_EXECUTABLE}")
if [ -d "${APP_PATH}/Contents/Frameworks" ]; then
  while IFS= read -r candidate; do
    if file "${candidate}" | grep -q 'Mach-O'; then
      BINARIES+=("${candidate}")
    fi
  done < <(find "${APP_PATH}/Contents/Frameworks" -type f -print)
fi

for binary in "${BINARIES[@]}"; do
  DESCRIPTION="$(file "${binary}")"
  [[ "${DESCRIPTION}" == *"Mach-O"* ]] || fail "binary is not Mach-O: ${binary}"

  ARCHITECTURES="$(lipo -archs "${binary}" 2>/dev/null || true)"
  [ "${ARCHITECTURES}" = "${EXPECTED_ARCHS}" ] || \
    fail "binary architectures must be ${EXPECTED_ARCHS}: ${binary} (${ARCHITECTURES:-missing})"

  BUILD_INFO="$(vtool -show-build "${binary}" 2>/dev/null || true)"
  PLATFORM="$(printf '%s\n' "${BUILD_INFO}" | awk '$1 == "platform" { print $2; exit }')"
  BINARY_MIN_OS="$(printf '%s\n' "${BUILD_INFO}" | awk '$1 == "minos" { print $2; exit }')"
  [ "${PLATFORM}" = "MACOS" ] || \
    fail "binary platform must be macOS: ${binary} (${PLATFORM:-missing})"
  [ "${BINARY_MIN_OS}" = "${EXPECTED_MIN_OS}" ] || \
    fail "binary minimum OS must be ${EXPECTED_MIN_OS}: ${binary} (${BINARY_MIN_OS:-missing})"

  LOCAL_RPATH="$(otool -l "${binary}" | awk '
    $1 == "cmd" && $2 == "LC_RPATH" { active = 1; next }
    active && $1 == "path" { print $2; active = 0 }
  ' | grep -E '^(/opt/homebrew|/usr/local|/Users/)' | head -n 1 || true)"
  [ -z "${LOCAL_RPATH}" ] || fail "local build rpath is embedded: ${LOCAL_RPATH}"

  while IFS= read -r dependency; do
    case "${dependency}" in
      /System/Library/*|/usr/lib/*)
        ;;
      /opt/homebrew/*|/usr/local/*|/Users/*)
        fail "local build dependency is embedded: ${dependency}"
        ;;
      @rpath/*)
        relative="${dependency#@rpath/}"
        [ -e "${APP_PATH}/Contents/Frameworks/${relative}" ] || \
          fail "unresolved embedded dependency: ${dependency}"
        ;;
      @executable_path/*)
        relative="${dependency#@executable_path/}"
        [ -e "${APP_PATH}/Contents/MacOS/${relative}" ] || \
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
  -name '*.pem' -o \
  -name 'CMakeFiles' \
\) -print -quit)"
[ -z "${UNWANTED}" ] || fail "unwanted file is embedded: ${UNWANTED}"

SECRET_PATTERN='BEGIN ([A-Z]+ )?PRIVATE KEY|APP_STORE_KEY[[:space:]:=]+[^[:space:]%]{20,}|MATCH_PASSWORD[[:space:]:=]+[^[:space:]%]{8,}|FIREBASE_(CLI_)?TOKEN[[:space:]:=]+[^[:space:]%]{20,}|Authorization:[[:space:]]*Bearer[[:space:]]+[A-Za-z0-9._~+/-]{20,}'
SECRET_MATCH="$(LC_ALL=C grep -R -I -n -E "${SECRET_PATTERN}" \
  "${APP_PATH}/Contents/Resources" 2>/dev/null | head -n 1 || true)"
[ -z "${SECRET_MATCH}" ] || fail "credential material is embedded: ${SECRET_MATCH}"
for binary in "${BINARIES[@]}"; do
  if LC_ALL=C strings -a "${binary}" | grep -q -E "${SECRET_PATTERN}"; then
    fail "credential material is embedded in binary: ${binary}"
  fi
done

if codesign -dv "${APP_PATH}" >/dev/null 2>&1; then
  codesign --verify --deep --strict "${APP_PATH}" >/dev/null 2>&1 || \
    fail "signature verification failed"
  SIGNATURE_DETAILS="$(codesign -dvvv "${APP_PATH}" 2>&1)"
  if [ "${REQUIRE_SIGNATURE}" -eq 1 ]; then
    printf '%s\n' "${SIGNATURE_DETAILS}" | \
      grep -q '^Authority=Developer ID Application:' || \
      fail "a Developer ID Application signature is required"
    printf '%s\n' "${SIGNATURE_DETAILS}" | \
      grep -Eq '^TeamIdentifier=[A-Z0-9]+$' || fail "a signing team is required"
    printf '%s\n' "${SIGNATURE_DETAILS}" | \
      grep -Eq '^CodeDirectory .*flags=.*runtime' || \
      fail "the hardened runtime is required"
  fi
  echo "signature check passed"
elif [ "${REQUIRE_SIGNATURE}" -eq 1 ]; then
  fail "a Developer ID Application signature is required but the app is unsigned"
else
  echo "signature check skipped (unsigned build)"
fi

if [ "${REQUIRE_GATEKEEPER}" -eq 1 ]; then
  command -v spctl >/dev/null 2>&1 || fail "spctl is unavailable"
  command -v xcrun >/dev/null 2>&1 || fail "xcrun is unavailable"
  xcrun stapler validate "${APP_PATH}" >/dev/null 2>&1 || \
    fail "a valid stapled notarization ticket is required"
  spctl --assess --type execute --verbose=2 "${APP_PATH}" >/dev/null 2>&1 || \
    fail "Gatekeeper assessment failed"
  echo "notarization and Gatekeeper checks passed"
fi

echo "macOS artifact audit passed: ${APP_PATH}"
