#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TYPE="${1:-debug}"
BUNDLE_APP="${2:-OFF}"
MACOS_DEPLOYMENT_TARGET="${ASOBMASHOW_MACOS_DEPLOYMENT_TARGET:-13.0}"
PRESET="user-${BUILD_TYPE}"
BUILD_DIR="${ASOBMASHOW_MACOS_BUILD_DIR:-${ROOT_DIR}/cmake-build-${BUILD_TYPE}}"

SDK_PATH="$(xcrun --show-sdk-path)"
export SDKROOT="${SDK_PATH}"
export MACOSX_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}"

if ! [[ "${MACOS_DEPLOYMENT_TARGET}" =~ ^[0-9]+([.][0-9]+){1,2}$ ]] ||
   ! awk -v target="${MACOS_DEPLOYMENT_TARGET}" \
     'BEGIN { exit !(target + 0 >= 13.0) }'; then
  echo "macOS deployment target must be a version at or above 13.0: ${MACOS_DEPLOYMENT_TARGET}" >&2
  exit 2
fi

# check if preset exists. if not, run generate_user_presets.sh
if [ ! -f "${ROOT_DIR}/CMakeUserPresets.json" ]; then
  "$(dirname "${BASH_SOURCE[0]}")/generate_user_presets.sh"
fi
# if BUNDLE_APP is ON, override output directory to cmake-build-${BUILD_TYPE}-bundle
if [ "${BUNDLE_APP}" == "ON" ] && [ -z "${ASOBMASHOW_MACOS_BUILD_DIR:-}" ]; then
  BUILD_DIR="${ROOT_DIR}/cmake-build-${BUILD_TYPE}-bundle"
fi
cmake --preset "${PRESET}" \
  -B "${BUILD_DIR}" \
  -DCMAKE_OSX_SYSROOT="${SDK_PATH}" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}" \
  -DVCPKG_TARGET_TRIPLET=arm64-osx-asobmashow \
  -DVCPKG_OVERLAY_TRIPLETS="${ROOT_DIR}/vcpkg-triplets" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DBUILD_MACOS_BUNDLE="${BUNDLE_APP}"

echo "Configured ${BUILD_TYPE} Ninja build in: ${BUILD_DIR}"
echo "SDK: ${SDK_PATH}"
echo "macOS deployment target: ${MACOS_DEPLOYMENT_TARGET}"
echo "Preset: ${PRESET}"
