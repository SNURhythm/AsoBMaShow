#!/usr/bin/env bash
set -euo pipefail
export LANG=en_US.UTF-8
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IOS_DIR="${ROOT_DIR}/ios/Xcode/AsoBMaShow"
SKIP_INIT=0
ENV_FILES=(
  "${ROOT_DIR}/.env"
  "${ROOT_DIR}/.env.local"
  "${IOS_DIR}/.env"
  "${IOS_DIR}/.env.local"
)

usage() {
  cat <<'USAGE'
Usage: scripts/ios_firebase_deploy.sh [options]

Runs the iOS Fastlane beta lane in its Firebase App Distribution mode.
Secrets are read from the current environment or optional shell-compatible .env files.

Options:
  --env-file PATH       Load an additional env file before running fastlane.
  --skip-init           Skip scripts/ios_init.sh.
  --build-number N      Set GITHUB_RUN_NUMBER for the Firebase build number.
  --groups CSV          Set FIREBASE_APP_DISTRIBUTION_GROUPS.
  --testers CSV         Set FIREBASE_APP_DISTRIBUTION_TESTERS.
  --release-notes TEXT  Set FIREBASE_APP_DISTRIBUTION_RELEASE_NOTES.
  -h, --help            Show this help.

Required signing env:
  APP_STORE_KEY
  KEYCHAIN_PASSWORD
  MATCH_PASSWORD
  GIT_BASIC_AUTHORIZATION

Optional App Store Connect env:
  APP_STORE_KEY_ID defaults to 7459U6UPUW.

Firebase auth must be available through one of:
  FIREBASE_SERVICE_CREDENTIALS_JSON, FIREBASE_CLI_TOKEN, FIREBASE_TOKEN,
  GOOGLE_APPLICATION_CREDENTIALS, cached Firebase CLI login, or gcloud ADC.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --env-file)
      [ "$#" -ge 2 ] || { echo "Missing value for --env-file" >&2; exit 2; }
      ENV_FILES+=("$2")
      shift 2
      ;;
    --skip-init)
      SKIP_INIT=1
      shift
      ;;
    --build-number)
      [ "$#" -ge 2 ] || { echo "Missing value for --build-number" >&2; exit 2; }
      export GITHUB_RUN_NUMBER="$2"
      shift 2
      ;;
    --groups)
      [ "$#" -ge 2 ] || { echo "Missing value for --groups" >&2; exit 2; }
      export FIREBASE_APP_DISTRIBUTION_GROUPS="$2"
      shift 2
      ;;
    --testers)
      [ "$#" -ge 2 ] || { echo "Missing value for --testers" >&2; exit 2; }
      export FIREBASE_APP_DISTRIBUTION_TESTERS="$2"
      shift 2
      ;;
    --release-notes)
      [ "$#" -ge 2 ] || { echo "Missing value for --release-notes" >&2; exit 2; }
      export FIREBASE_APP_DISTRIBUTION_RELEASE_NOTES="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

load_env_file() {
  local env_file="$1"
  [ -f "${env_file}" ] || return 0

  echo "Loading env file: ${env_file}"
  set -a
  # shellcheck disable=SC1090
  . "${env_file}"
  set +a
}

setup_project_ruby() {
  local ruby_version=""

  if [ -f "${IOS_DIR}/.ruby-version" ]; then
    ruby_version="$(tr -d '[:space:]' < "${IOS_DIR}/.ruby-version")"
  elif [ -f "${IOS_DIR}/.tool-versions" ]; then
    ruby_version="$(awk '$1 == "ruby" { print $2; exit }' "${IOS_DIR}/.tool-versions")"
  fi

  [ -n "${ruby_version}" ] || return 0

  if command -v ruby >/dev/null 2>&1 &&
     ruby -e 'exit RUBY_VERSION == ARGV.fetch(0) ? 0 : 1' "${ruby_version}" >/dev/null 2>&1; then
    return 0
  fi

  if [ -x "${HOME}/.asdf/installs/ruby/${ruby_version}/bin/ruby" ]; then
    export PATH="${HOME}/.asdf/installs/ruby/${ruby_version}/bin:${PATH}"
  elif command -v rbenv >/dev/null 2>&1; then
    export RBENV_VERSION="${ruby_version}"
    eval "$(rbenv init - bash)"
  fi
}

repo_name_from_origin() {
  local remote_url repo
  remote_url="$(git -C "${ROOT_DIR}" remote get-url origin 2>/dev/null || true)"
  case "${remote_url}" in
    git@*:*)
      repo="${remote_url#*:}"
      ;;
    ssh://git@*/*)
      repo="${remote_url#ssh://git@*/}"
      ;;
    https://github.com/*)
      repo="${remote_url#https://github.com/}"
      ;;
    *)
      repo="SNURhythm/AsoBMaShow"
      ;;
  esac
  repo="${repo%.git}"
  printf '%s\n' "${repo}"
}

require_env() {
  local missing=()
  local name

  for name in "$@"; do
    if [ -z "${!name:-}" ]; then
      missing+=("${name}")
    fi
  done

  if [ "${#missing[@]}" -gt 0 ]; then
    echo "Missing required env: ${missing[*]}" >&2
    echo "Load them in your shell or pass --env-file PATH." >&2
    exit 1
  fi
}

has_firebase_auth() {
  [ -n "${FIREBASE_SERVICE_CREDENTIALS_JSON:-}" ] && return 0
  [ -n "${FIREBASE_CLI_TOKEN:-}" ] && return 0
  [ -n "${FIREBASE_TOKEN:-}" ] && return 0
  [ -n "${GOOGLE_APPLICATION_CREDENTIALS:-}" ] && [ -f "${GOOGLE_APPLICATION_CREDENTIALS}" ] && return 0
  [ -f "${HOME}/.config/configstore/firebase-tools.json" ] && return 0
  [ -f "${HOME}/.config/gcloud/application_default_credentials.json" ] && return 0
  return 1
}

for env_file in "${ENV_FILES[@]}"; do
  load_env_file "${env_file}"
done

setup_project_ruby

export GITHUB_ACTIONS=true
export GITHUB_EVENT_NAME=pull_request
export GITHUB_BASE_REF="${GITHUB_BASE_REF:-develop}"
export GITHUB_HEAD_REF="${GITHUB_HEAD_REF:-$(git -C "${ROOT_DIR}" branch --show-current)}"
export GITHUB_SHA="${GITHUB_SHA:-$(git -C "${ROOT_DIR}" rev-parse HEAD)}"
export GITHUB_RUN_NUMBER="${GITHUB_RUN_NUMBER:-$(date -u +%Y%m%d%H%M)}"
export GITHUB_RUN_ID="${GITHUB_RUN_ID:-local-${GITHUB_RUN_NUMBER}}"
export GITHUB_SERVER_URL="${GITHUB_SERVER_URL:-https://github.com}"
export GITHUB_REPOSITORY="${GITHUB_REPOSITORY:-$(repo_name_from_origin)}"
export SPACESHIP_ONLY_ALLOW_INTERACTIVE_2FA="${SPACESHIP_ONLY_ALLOW_INTERACTIVE_2FA:-1}"

if [ -z "${FIREBASE_APP_DISTRIBUTION_RELEASE_NOTES:-}" ]; then
  export FIREBASE_APP_DISTRIBUTION_RELEASE_NOTES=$(
    printf 'Local Firebase build\nBranch: %s\nSHA: %s\nMachine: %s\n' \
      "${GITHUB_HEAD_REF}" \
      "${GITHUB_SHA}" \
      "$(hostname)"
  )
fi

require_env \
  APP_STORE_KEY \
  KEYCHAIN_PASSWORD \
  MATCH_PASSWORD \
  GIT_BASIC_AUTHORIZATION

if ! has_firebase_auth; then
  echo "Missing Firebase authentication." >&2
  echo "Set FIREBASE_SERVICE_CREDENTIALS_JSON, FIREBASE_CLI_TOKEN, FIREBASE_TOKEN, GOOGLE_APPLICATION_CREDENTIALS," >&2
  echo "or login with firebase-tools/gcloud before running this script." >&2
  exit 1
fi

if ! git -C "${ROOT_DIR}" diff --quiet || ! git -C "${ROOT_DIR}" diff --cached --quiet; then
  echo "Warning: deploying with uncommitted local changes." >&2
fi

if [ "${SKIP_INIT}" -eq 0 ]; then
  "${ROOT_DIR}/scripts/ios_init.sh"
fi

cd "${IOS_DIR}"
echo "Deploying Firebase iOS build ${GITHUB_RUN_NUMBER} from ${GITHUB_HEAD_REF} (${GITHUB_SHA})"
bundle exec fastlane ios beta --verbose
