#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"

if [ "$#" -gt 0 ]; then
  if [ "$#" -ne 2 ] || [ "$1" != "--root" ]; then
    echo "Usage: scripts/ios_derived_data_path.sh [--root PATH]" >&2
    exit 2
  fi
  ROOT_DIR="$2"
fi

if [ -n "${IOS_DERIVED_DATA_PATH:-}" ]; then
  printf '%s\n' "${IOS_DERIVED_DATA_PATH}"
  exit 0
fi

CANONICAL_ROOT="$(cd "${ROOT_DIR}" && pwd -P)"
ROOT_HASH="$(printf '%s' "${CANONICAL_ROOT}" | shasum -a 256 | awk '{ print substr($1, 1, 12) }')"
[ -n "${ROOT_HASH}" ] || { echo "Unable to hash repository root" >&2; exit 1; }

printf '%s\n' "${HOME}/Library/Developer/Xcode/DerivedData/AsoBMaShow-FirebaseCI-${ROOT_HASH}"
