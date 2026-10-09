#!/usr/bin/env bash
set -euo pipefail
export LANG=en_US.UTF-8
INHERITED_GITHUB_RUN_NUMBER="${GITHUB_RUN_NUMBER:-}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_DIR="${ROOT_DIR}/android"
LANE="play_beta"
BUILD_ONLY=0
SKIP_BUILD=0
ENV_FILES=("${ROOT_DIR}/.env" "${ROOT_DIR}/.env.local" "${ANDROID_DIR}/.env" "${ANDROID_DIR}/.env.local")

while [ "$#" -gt 0 ]; do
  case "$1" in
    --build-only)
      LANE="build_bundle"
      BUILD_ONLY=1
      shift
      ;;
    --skip-build)
      LANE="upload_beta"
      SKIP_BUILD=1
      shift
      ;;
    -h|--help)
      echo "Usage: scripts/android_play_deploy.sh [--build-only | --skip-build]"
      echo "Builds a signed AAB and uploads a Google Play public beta draft. --build-only skips upload."
      echo "--skip-build uploads the existing restricted-file release AAB without rebuilding."
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [ "${BUILD_ONLY}" -eq 1 ] && [ "${SKIP_BUILD}" -eq 1 ]; then
  echo "--build-only and --skip-build cannot be combined." >&2
  exit 2
fi

for env_file in "${ENV_FILES[@]}"; do
  if [ -f "${env_file}" ]; then
    set -a
    # shellcheck disable=SC1090
    . "${env_file}"
    set +a
  fi
done

# Keep the Actions counter intact before Fastlane invokes the shared build helper.
if [ -n "${INHERITED_GITHUB_RUN_NUMBER}" ]; then
  export GITHUB_RUN_NUMBER="${INHERITED_GITHUB_RUN_NUMBER}"
fi

# Local test builds may reuse 1, but uploads must deliberately select a code.
if [ "${BUILD_ONLY}" -eq 0 ]; then
  play_version_code="${ANDROID_VERSION_CODE:-${GITHUB_RUN_NUMBER:-}}"
  if [ -z "${play_version_code}" ]; then
    echo "Local Play uploads require an explicit, unused ANDROID_VERSION_CODE; use the Play workflow for its automatic GITHUB_RUN_NUMBER, or --build-only for a local test build." >&2
    exit 1
  fi
  if ! [[ "${play_version_code}" =~ ^[1-9][0-9]{0,9}$ ]] ||
     [ "${play_version_code}" -gt 2100000000 ]; then
    echo "Play upload version code (ANDROID_VERSION_CODE or GITHUB_RUN_NUMBER) must be an integer from 1 to 2100000000." >&2
    exit 1
  fi
fi

ruby_version="$(tr -d '[:space:]' < "${ANDROID_DIR}/.ruby-version")"
if ! ruby -e 'exit RUBY_VERSION == ARGV.fetch(0) ? 0 : 1' "${ruby_version}" >/dev/null 2>&1; then
  if [ -x "${HOME}/.asdf/installs/ruby/${ruby_version}/bin/ruby" ]; then
    export PATH="${HOME}/.asdf/installs/ruby/${ruby_version}/bin:${PATH}"
  elif command -v rbenv >/dev/null 2>&1; then
    export RBENV_VERSION="${ruby_version}"
    eval "$(rbenv init - bash)"
  fi
fi
if ! ruby -e 'exit RUBY_VERSION == ARGV.fetch(0) ? 0 : 1' "${ruby_version}" >/dev/null 2>&1; then
  echo "Ruby ${ruby_version} is required; install/select android/.ruby-version first." >&2
  exit 1
fi

cd "${ANDROID_DIR}"
export BUNDLE_GEMFILE="${ANDROID_DIR}/Gemfile"
export BUNDLE_PATH="${ANDROID_PLAY_BUNDLE_PATH:-${HOME}/Library/Caches/AsoBMaShow/android-play/bundle/ruby-${ruby_version}}"
export BUNDLE_FROZEN=true
export FASTLANE_SKIP_UPDATE_CHECK=1
export FASTLANE_HIDE_CHANGELOG=1
bundle check || bundle install --jobs 4 --retry 3
exec bundle exec fastlane android "${LANE}"
