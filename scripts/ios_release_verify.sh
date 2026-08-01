#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${IOS_RELEASE_CMAKE_BUILD_DIR:-${ROOT_DIR}/cmake-build-debug}"
BUILD_JOBS="${IOS_RELEASE_BUILD_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 6)}"
DRY_RUN=0

usage() {
  cat <<'USAGE'
Usage: scripts/ios_release_verify.sh [--dry-run]

Runs release-critical native tests, iOS release-contract tests, and an unsigned
iOS build. It never archives, signs, or uploads a distribution artifact.
USAGE
}

if [ "${1:-}" = "--dry-run" ]; then
  DRY_RUN=1
  shift
fi
if [ "$#" -ne 0 ]; then
  usage >&2
  exit 2
fi

run() {
  if [ "${DRY_RUN}" -eq 1 ]; then
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    return 0
  fi
  "$@"
}

cd "${ROOT_DIR}"
if [ "${DRY_RUN}" -eq 1 ] || [ ! -f "${BUILD_DIR}/CMakeCache.txt" ]; then
  run cmake --preset debug
fi
run cmake --build "${BUILD_DIR}" --target \
  chart_repository_tests \
  ir_credential_store_tests \
  ir_credential_migration_tests \
  video_frame_layout_tests \
  video_decode_state_tests \
  decoded_image_cache_tests \
  image_decode_coordinator_tests \
  jukebox_restore_tests \
  -j "${BUILD_JOBS}"
run ctest --test-dir "${BUILD_DIR}" \
  -R '^(chart_repository_tests|ir_credential_store_tests|ir_credential_migration_tests|video_frame_layout_tests|video_decode_state_tests|decoded_image_cache_tests|image_decode_coordinator_tests|foundation_av_jukebox_restore)$' \
  --output-on-failure
run python3 tests/ios_build_setup_tests.py
run python3 tests/ios_release_workflow_tests.py
run scripts/ios_firebase_deploy.sh --build-only
