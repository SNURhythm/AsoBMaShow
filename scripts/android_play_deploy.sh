#!/usr/bin/env bash
set -euo pipefail
export LANG=en_US.UTF-8
INHERITED_GITHUB_RUN_NUMBER="${GITHUB_RUN_NUMBER:-}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_DIR="${ROOT_DIR}/android"
LANE="play_beta"
ENV_FILES=("${ROOT_DIR}/.env" "${ROOT_DIR}/.env.local" "${ANDROID_DIR}/.env" "${ANDROID_DIR}/.env.local")

while [ "$#" -gt 0 ]; do
  case "$1" in
    --build-only)
      LANE="build_bundle"
      shift
      ;;
    -h|--help)
      echo "Usage: scripts/android_play_deploy.sh [--build-only]"
      echo "Builds a signed AAB and uploads a Google Play public beta draft. --build-only skips upload."
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

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
