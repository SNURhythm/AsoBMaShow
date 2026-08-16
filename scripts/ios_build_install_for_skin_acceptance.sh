#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
COMMIT=""
CONFIGURATION=""
DEVICE_ID=""
DEVELOPMENT_TEAM=""

fail() {
  echo "Skin acceptance install failed: $*" >&2
  exit 1
}

usage() {
  echo "Usage: scripts/ios_build_install_for_skin_acceptance.sh --commit HEX40 --configuration Release --device-id PRIVATE_ID --development-team TEAM" >&2
}

require_exact_clean_checkout() {
  local phase="$1"
  local git_root=""
  local canonical_git_root=""
  local head_commit=""
  local status=""

  if ! git_root="$(git -C "${ROOT_DIR}" rev-parse --show-toplevel 2>/dev/null)"; then
    fail "${phase}: source is not a Git repository root"
  fi
  if ! canonical_git_root="$(cd "${git_root}" && pwd -P)"; then
    fail "${phase}: unable to resolve Git repository root"
  fi
  [ "${canonical_git_root}" = "${ROOT_DIR}" ] || \
    fail "${phase}: source directory is not the repository root"
  if ! head_commit="$(git -C "${ROOT_DIR}" rev-parse --verify HEAD 2>/dev/null)"; then
    fail "${phase}: unable to resolve HEAD"
  fi
  [ "${head_commit}" = "${COMMIT}" ] || fail "${phase}: HEAD does not match --commit"
  if ! status="$(git -C "${ROOT_DIR}" status --porcelain --untracked-files=normal 2>/dev/null)"; then
    fail "${phase}: unable to inspect checkout cleanliness"
  fi
  [ -z "${status}" ] || fail "${phase}: checkout must be clean"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --commit)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      COMMIT="$2"
      shift 2
      ;;
    --configuration)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      CONFIGURATION="$2"
      shift 2
      ;;
    --device-id)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      DEVICE_ID="$2"
      shift 2
      ;;
    --development-team)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      DEVELOPMENT_TEAM="$2"
      shift 2
      ;;
    *)
      usage
      exit 2
      ;;
  esac
done

[[ "${COMMIT}" =~ ^[0-9a-f]{40}$ ]] || fail "--commit must be a lowercase 40-character commit"
[ "${COMMIT}" != "0000000000000000000000000000000000000000" ] || fail "--commit cannot be a placeholder"
[ "${CONFIGURATION}" = "Release" ] || fail "--configuration must be Release"
[ -n "${DEVICE_ID}" ] || fail "--device-id is required"
[ -n "${DEVELOPMENT_TEAM}" ] || fail "--development-team is required"

require_exact_clean_checkout "checkout preflight failed"

"${ROOT_DIR}/scripts/ios_init.sh"
require_exact_clean_checkout "checkout changed during iOS initialization"

ROOT_HASH="$(printf '%s' "${ROOT_DIR}" | shasum -a 256 | awk '{ print substr($1, 1, 12) }')"
[ -n "${ROOT_HASH}" ] || fail "unable to derive checkout build path"
DERIVED_DATA_PATH="${TMPDIR:-/tmp}/AsoBMaShow-SkinAcceptance-${ROOT_HASH}"
PROJECT="${ROOT_DIR}/ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj"

if ! xcodebuild \
  -project "${PROJECT}" \
  -scheme AsoBMaShow \
  -configuration "${CONFIGURATION}" \
  -destination "id=${DEVICE_ID}" \
  -derivedDataPath "${DERIVED_DATA_PATH}" \
  ASOBMASHOW_BUILD_COMMIT="${COMMIT}" \
  ASOBMASHOW_SOURCE_CLEAN=1 \
  DEVELOPMENT_TEAM="${DEVELOPMENT_TEAM}" \
  CODE_SIGN_STYLE=Automatic \
  build >/dev/null 2>&1; then
  fail "development-signed iOS build failed"
fi

require_exact_clean_checkout "checkout changed during iOS build"

APP_PATH="${DERIVED_DATA_PATH}/Build/Products/${CONFIGURATION}-iphoneos/AsoBMaShow.app"
[ -d "${APP_PATH}" ] || fail "built app is missing"
ASOBMASHOW_EXPECTED_BUILD_COMMIT="${COMMIT}" \
ASOBMASHOW_EXPECTED_BUILD_CONFIGURATION="${CONFIGURATION}" \
ASOBMASHOW_EXPECTED_SOURCE_CLEAN=1 \
  "${ROOT_DIR}/scripts/ios_artifact_audit.sh" --require-signature "${APP_PATH}" >/dev/null

if ! xcrun devicectl device install app --device "${DEVICE_ID}" "${APP_PATH}" \
  >/dev/null 2>&1; then
  fail "device install failed"
fi

echo "Skin acceptance app installed"
