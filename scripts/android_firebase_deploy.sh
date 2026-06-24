#!/usr/bin/env bash
set -euo pipefail
export LANG=en_US.UTF-8

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_DIR="${ROOT_DIR}/android"
GRADLEW="${ROOT_DIR}/SDL/android-project/gradlew"
BUILD_ONLY=0
SKIP_BUILD=0
VARIANT="firebaseRelease"
APK_PATH=""
FIREBASE_CLI_BIN="${FIREBASE_CLI_BIN:-firebase}"
SERVICE_CREDENTIALS_FILE=""
CLI_ANDROID_VERSION_CODE=""
CLI_ANDROID_VERSION_NAME=""
CLI_GITHUB_RUN_NUMBER=""
ANDROID_VERSION_NAME_FIXED=0
ENV_FILES=(
  "${ROOT_DIR}/.env"
  "${ROOT_DIR}/.env.local"
  "${ANDROID_DIR}/.env"
  "${ANDROID_DIR}/.env.local"
)

usage() {
  cat <<'USAGE'
Usage: scripts/android_firebase_deploy.sh [options]

Builds the Android app and deploys the APK to Firebase App Distribution.
With --build-only, runs a plain Gradle assemble task and skips upload.
Secrets are read from the current environment or optional shell-compatible .env files.

Options:
  --env-file PATH       Load an additional env file.
  --build-only          Build only; do not upload.
  --skip-build          Upload an existing APK from --apk.
  --variant NAME        Gradle build variant to assemble. Default: firebaseRelease.
  --apk PATH            APK to upload. Defaults to android/app/build/outputs/apk/<variant>/app-<variant>.apk.
  --build-number N      Override automatic versionCode with N.
  --version-code N      Override automatic ANDROID_VERSION_CODE with N.
  --version-name TEXT   Override automatic ANDROID_VERSION_NAME.
  --groups CSV          Set FIREBASE_APP_DISTRIBUTION_GROUPS.
  --testers CSV         Set FIREBASE_APP_DISTRIBUTION_TESTERS.
  --release-notes TEXT  Set FIREBASE_APP_DISTRIBUTION_RELEASE_NOTES.
  --app-id ID           Set FIREBASE_ANDROID_APP_ID.
  --project ID          Set FIREBASE_PROJECT.
  --android-home PATH   Set ANDROID_HOME and ANDROID_SDK_ROOT.
  --vcpkg-root PATH     Set VCPKG_ROOT.
  --firebase-cli PATH   Firebase CLI executable. Default: firebase.
  -h, --help            Show this help.

Required env for deployment:
  FIREBASE_ANDROID_APP_ID

Required env for release builds unless --skip-build is used:
  ANDROID_KEYSTORE_PATH, ANDROID_KEYSTORE_PASSWORD, ANDROID_KEY_ALIAS,
  ANDROID_KEY_PASSWORD

Firebase auth for deployment must be available through one of:
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
    --build-only)
      BUILD_ONLY=1
      shift
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --variant)
      [ "$#" -ge 2 ] || { echo "Missing value for --variant" >&2; exit 2; }
      VARIANT="$2"
      shift 2
      ;;
    --apk)
      [ "$#" -ge 2 ] || { echo "Missing value for --apk" >&2; exit 2; }
      APK_PATH="$2"
      shift 2
      ;;
    --build-number)
      [ "$#" -ge 2 ] || { echo "Missing value for --build-number" >&2; exit 2; }
      CLI_GITHUB_RUN_NUMBER="$2"
      CLI_ANDROID_VERSION_CODE="$2"
      shift 2
      ;;
    --version-code)
      [ "$#" -ge 2 ] || { echo "Missing value for --version-code" >&2; exit 2; }
      CLI_ANDROID_VERSION_CODE="$2"
      shift 2
      ;;
    --version-name)
      [ "$#" -ge 2 ] || { echo "Missing value for --version-name" >&2; exit 2; }
      CLI_ANDROID_VERSION_NAME="$2"
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
    --app-id)
      [ "$#" -ge 2 ] || { echo "Missing value for --app-id" >&2; exit 2; }
      export FIREBASE_ANDROID_APP_ID="$2"
      shift 2
      ;;
    --project)
      [ "$#" -ge 2 ] || { echo "Missing value for --project" >&2; exit 2; }
      export FIREBASE_PROJECT="$2"
      shift 2
      ;;
    --android-home)
      [ "$#" -ge 2 ] || { echo "Missing value for --android-home" >&2; exit 2; }
      export ANDROID_HOME="$2"
      export ANDROID_SDK_ROOT="$2"
      shift 2
      ;;
    --vcpkg-root)
      [ "$#" -ge 2 ] || { echo "Missing value for --vcpkg-root" >&2; exit 2; }
      export VCPKG_ROOT="$2"
      shift 2
      ;;
    --firebase-cli)
      [ "$#" -ge 2 ] || { echo "Missing value for --firebase-cli" >&2; exit 2; }
      FIREBASE_CLI_BIN="$2"
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

cleanup() {
  if [ -n "${SERVICE_CREDENTIALS_FILE}" ] && [ -f "${SERVICE_CREDENTIALS_FILE}" ]; then
    rm -f "${SERVICE_CREDENTIALS_FILE}"
  fi
}
trap cleanup EXIT

load_env_file() {
  local env_file="$1"
  [ -f "${env_file}" ] || return 0

  echo "Loading env file: ${env_file}"
  set -a
  # shellcheck disable=SC1090
  . "${env_file}"
  set +a
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

is_positive_int() {
  case "$1" in
    ''|*[!0-9]*)
      return 1
      ;;
    *)
      [ "$1" -gt 0 ]
      ;;
  esac
}

is_release_variant() {
  case "${VARIANT}" in
    *Release)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

setup_android_signing_env() {
  local resolved_path

  if [ "${SKIP_BUILD}" -eq 1 ] || ! is_release_variant; then
    return 0
  fi

  require_env \
    ANDROID_KEYSTORE_PATH \
    ANDROID_KEYSTORE_PASSWORD \
    ANDROID_KEY_ALIAS \
    ANDROID_KEY_PASSWORD

  resolved_path="${ANDROID_KEYSTORE_PATH}"
  case "${resolved_path}" in
    "~/"*)
      resolved_path="${HOME}/${resolved_path#~/}"
      ;;
    /*)
      ;;
    *)
      resolved_path="${ROOT_DIR}/${resolved_path}"
      ;;
  esac
  export ANDROID_KEYSTORE_PATH="${resolved_path}"

  if [ ! -f "${ANDROID_KEYSTORE_PATH}" ]; then
    echo "ANDROID_KEYSTORE_PATH does not point to a file: ${ANDROID_KEYSTORE_PATH}" >&2
    exit 1
  fi
}

android_timestamp_version_code() {
  # YY + day-of-year + HHMM stays monotonic enough for local builds and below
  # Android's 2,100,000,000 versionCode ceiling.
  date -u +%y%j%H%M
}

apply_cli_overrides() {
  if [ -n "${CLI_GITHUB_RUN_NUMBER}" ]; then
    export GITHUB_RUN_NUMBER="${CLI_GITHUB_RUN_NUMBER}"
  fi
  if [ -n "${CLI_ANDROID_VERSION_CODE}" ]; then
    export ANDROID_VERSION_CODE="${CLI_ANDROID_VERSION_CODE}"
  fi
  if [ -n "${CLI_ANDROID_VERSION_NAME}" ]; then
    export ANDROID_VERSION_NAME="${CLI_ANDROID_VERSION_NAME}"
  fi
}

capture_version_fixed_flags() {
  if [ -n "${ANDROID_VERSION_NAME:-}" ]; then
    ANDROID_VERSION_NAME_FIXED=1
  fi
}

validate_android_version_code() {
  if ! is_positive_int "${ANDROID_VERSION_CODE}" ||
     [ "${ANDROID_VERSION_CODE}" -gt 2100000000 ]; then
    echo "ANDROID_VERSION_CODE must be an integer from 1 to 2100000000; got '${ANDROID_VERSION_CODE}'." >&2
    exit 1
  fi
}

set_android_version_code() {
  export ANDROID_VERSION_CODE="$1"
  validate_android_version_code
  if [ "${ANDROID_VERSION_NAME_FIXED}" -eq 0 ]; then
    export ANDROID_VERSION_NAME="1.0.${ANDROID_VERSION_CODE}"
  fi
}

setup_android_env() {
  local ndk_candidate=""

  if [ -z "${ANDROID_HOME:-}" ] &&
     [ -n "${ANDROID_SDK_ROOT:-}" ] &&
     [ -d "${ANDROID_SDK_ROOT}" ]; then
    export ANDROID_HOME="${ANDROID_SDK_ROOT}"
  fi
  if [ -z "${ANDROID_HOME:-}" ]; then
    for candidate in \
      "/opt/homebrew/share/android-commandlinetools" \
      "${HOME}/Library/Android/sdk"; do
      if [ -d "${candidate}" ]; then
        export ANDROID_HOME="${candidate}"
        break
      fi
    done
  fi
  if [ -n "${ANDROID_HOME:-}" ]; then
    if [ -z "${ANDROID_SDK_ROOT:-}" ] || [ ! -d "${ANDROID_SDK_ROOT}" ]; then
      export ANDROID_SDK_ROOT="${ANDROID_HOME}"
    fi
  fi
  if [ -z "${ANDROID_NDK_HOME:-}" ] ||
     [ ! -f "${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" ]; then
    if [ -n "${ANDROID_HOME:-}" ] &&
       [ -f "${ANDROID_HOME}/ndk-bundle/build/cmake/android.toolchain.cmake" ]; then
      ndk_candidate="${ANDROID_HOME}/ndk-bundle"
    elif [ -n "${ANDROID_HOME:-}" ] && [ -d "${ANDROID_HOME}/ndk" ]; then
      ndk_candidate="$(
        find "${ANDROID_HOME}/ndk" -mindepth 1 -maxdepth 1 -type d -name '[0-9]*' 2>/dev/null |
          sort -r |
          head -n 1
      )"
    fi
    if [ -n "${ndk_candidate}" ]; then
      export ANDROID_NDK_HOME="${ndk_candidate}"
    fi
  fi
  if [ -n "${ANDROID_NDK_HOME:-}" ]; then
    export ANDROID_NDK_ROOT="${ANDROID_NDK_HOME}"
  fi
  if [ -z "${VCPKG_ROOT:-}" ] && [ -d "/Users/xf/vcpkg" ]; then
    export VCPKG_ROOT="/Users/xf/vcpkg"
  fi

  require_env ANDROID_HOME ANDROID_SDK_ROOT ANDROID_NDK_HOME VCPKG_ROOT

  if [ ! -f "${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" ]; then
    echo "ANDROID_NDK_HOME does not contain build/cmake/android.toolchain.cmake: ${ANDROID_NDK_HOME}" >&2
    exit 1
  fi
}

java_major_for_binary() {
  local java_bin="$1"
  local version major
  version="$("${java_bin}" -version 2>&1 | awk -F '"' '/version/ { print $2; exit }')"
  [ -n "${version}" ] || return 1
  case "${version}" in
    1.*)
      major="$(printf '%s' "${version}" | cut -d. -f2)"
      ;;
    *)
      major="$(printf '%s' "${version}" | cut -d. -f1)"
      ;;
  esac
  is_positive_int "${major}" || return 1
  printf '%s\n' "${major}"
}

is_supported_gradle_java_major() {
  local major="$1"
  is_positive_int "${major}" &&
    [ "${major}" -ge 17 ] &&
    [ "${major}" -le 21 ]
}

setup_java_env() {
  local java_bin="${JAVA_HOME:+${JAVA_HOME}/bin/java}"
  local major=""

  if [ -z "${java_bin}" ] || [ ! -x "${java_bin}" ]; then
    java_bin="$(command -v java 2>/dev/null || true)"
  fi
  if [ -n "${java_bin}" ]; then
    major="$(java_major_for_binary "${java_bin}" || true)"
    if is_supported_gradle_java_major "${major}"; then
      return 0
    fi
  fi

  if [ -x /usr/libexec/java_home ]; then
    local requested candidate candidate_major
    for requested in 17 21; do
      candidate="$(/usr/libexec/java_home -v "${requested}" 2>/dev/null || true)"
      if [ -n "${candidate}" ] && [ -x "${candidate}/bin/java" ]; then
        candidate_major="$(java_major_for_binary "${candidate}/bin/java" || true)"
        if is_supported_gradle_java_major "${candidate_major}"; then
          export JAVA_HOME="${candidate}"
          export PATH="${JAVA_HOME}/bin:${PATH}"
          echo "Using JAVA_HOME=${JAVA_HOME}"
          return 0
        fi
      fi
    done
  fi

  if [ -n "${major}" ]; then
    echo "Warning: current Java major version ${major} may be unsupported by Gradle." >&2
  fi
}

setup_build_metadata() {
  local default_version_code
  default_version_code="$(android_timestamp_version_code)"

  export GITHUB_ACTIONS=true
  export GITHUB_EVENT_NAME=pull_request
  export GITHUB_BASE_REF="${GITHUB_BASE_REF:-develop}"
  export GITHUB_HEAD_REF="${GITHUB_HEAD_REF:-$(git -C "${ROOT_DIR}" branch --show-current)}"
  export GITHUB_SHA="${GITHUB_SHA:-$(git -C "${ROOT_DIR}" rev-parse HEAD)}"
  export GITHUB_RUN_NUMBER="${GITHUB_RUN_NUMBER:-${default_version_code}}"
  export GITHUB_RUN_ID="${GITHUB_RUN_ID:-local-${GITHUB_RUN_NUMBER}}"
  export GITHUB_SERVER_URL="${GITHUB_SERVER_URL:-https://github.com}"
  export GITHUB_REPOSITORY="${GITHUB_REPOSITORY:-$(repo_name_from_origin)}"

  if [ -z "${ANDROID_VERSION_CODE:-}" ]; then
    set_android_version_code "${default_version_code}"
  else
    validate_android_version_code
    if [ -z "${ANDROID_VERSION_NAME:-}" ]; then
      export ANDROID_VERSION_NAME="1.0.${ANDROID_VERSION_CODE}"
    fi
  fi

  if [ -z "${FIREBASE_APP_DISTRIBUTION_RELEASE_NOTES:-}" ]; then
    export FIREBASE_APP_DISTRIBUTION_RELEASE_NOTES=$(
      printf 'Local Android Firebase build\nBranch: %s\nSHA: %s\nMachine: %s\n' \
        "${GITHUB_HEAD_REF}" \
        "${GITHUB_SHA}" \
        "$(hostname)"
    )
  fi
}

variant_task_name() {
  local first rest
  first="$(printf '%s' "${VARIANT:0:1}" | tr '[:lower:]' '[:upper:]')"
  rest="${VARIANT:1}"
  printf 'assemble%s%s\n' "${first}" "${rest}"
}

artifact_path_for_variant() {
  if [ -n "${APK_PATH}" ]; then
    if [[ "${APK_PATH}" = /* ]]; then
      printf '%s\n' "${APK_PATH}"
    else
      printf '%s/%s\n' "${ROOT_DIR}" "${APK_PATH}"
    fi
    return 0
  fi

  local variant_dir="${VARIANT}"
  local apk_variant="${VARIANT}"
  case "${VARIANT}" in
    *Debug)
      local flavor="${VARIANT%Debug}"
      local flavor_lower
      flavor_lower="$(printf '%s' "${flavor}" | tr '[:upper:]' '[:lower:]')"
      variant_dir="${flavor_lower}/debug"
      apk_variant="${flavor_lower}-debug"
      ;;
    *Release)
      local flavor="${VARIANT%Release}"
      local flavor_lower
      flavor_lower="$(printf '%s' "${flavor}" | tr '[:upper:]' '[:lower:]')"
      variant_dir="${flavor_lower}/release"
      apk_variant="${flavor_lower}-release"
      ;;
  esac

  printf '%s/app/build/outputs/apk/%s/app-%s.apk\n' \
    "${ANDROID_DIR}" \
    "${variant_dir}" \
    "${apk_variant}"
}

run_gradle_build() {
  local task
  task="$(variant_task_name)"
  echo "Building Android ${VARIANT} APK with versionCode=${ANDROID_VERSION_CODE}, versionName=${ANDROID_VERSION_NAME}"
  "${GRADLEW}" -p "${ANDROID_DIR}" ":app:${task}" --no-daemon
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

json_value_from_google_services() {
  local key_path="$1"
  local google_services="${ANDROID_DIR}/app/google-services.json"
  [ -f "${google_services}" ] || return 0
  command -v python3 >/dev/null 2>&1 || return 0

  python3 - "${google_services}" "${key_path}" <<'PY'
import json
import sys

path, key_path = sys.argv[1], sys.argv[2]
with open(path, "r", encoding="utf-8") as f:
    value = json.load(f)
for part in key_path.split("."):
    if part.isdigit():
        value = value[int(part)]
    else:
        value = value[part]
print(value)
PY
}

setup_firebase_app_defaults() {
  if [ -z "${FIREBASE_ANDROID_APP_ID:-}" ]; then
    FIREBASE_ANDROID_APP_ID="$(
      json_value_from_google_services "client.0.client_info.mobilesdk_app_id" || true
    )"
    export FIREBASE_ANDROID_APP_ID
  fi
  if [ -z "${FIREBASE_PROJECT:-}" ]; then
    FIREBASE_PROJECT="$(
      json_value_from_google_services "project_info.project_id" || true
    )"
    export FIREBASE_PROJECT
  fi
}

setup_firebase_service_credentials() {
  if [ -z "${FIREBASE_SERVICE_CREDENTIALS_JSON:-}" ] ||
     [ -n "${GOOGLE_APPLICATION_CREDENTIALS:-}" ]; then
    return 0
  fi

  SERVICE_CREDENTIALS_FILE="$(mktemp "${TMPDIR:-/tmp}/asobmashow-firebase-credentials.XXXXXX.json")"
  printf '%s' "${FIREBASE_SERVICE_CREDENTIALS_JSON}" > "${SERVICE_CREDENTIALS_FILE}"
  chmod 600 "${SERVICE_CREDENTIALS_FILE}"
  export GOOGLE_APPLICATION_CREDENTIALS="${SERVICE_CREDENTIALS_FILE}"
}

run_firebase_deploy() {
  local apk_path="$1"
  local token_arg=""
  local firebase_args=(
    appdistribution:distribute
    "${apk_path}"
    --app
    "${FIREBASE_ANDROID_APP_ID}"
    --release-notes
    "${FIREBASE_APP_DISTRIBUTION_RELEASE_NOTES}"
  )

  if [ -n "${FIREBASE_APP_DISTRIBUTION_GROUPS:-}" ]; then
    firebase_args+=(--groups "${FIREBASE_APP_DISTRIBUTION_GROUPS}")
  fi
  if [ -n "${FIREBASE_APP_DISTRIBUTION_TESTERS:-}" ]; then
    firebase_args+=(--testers "${FIREBASE_APP_DISTRIBUTION_TESTERS}")
  fi
  if [ -n "${FIREBASE_PROJECT:-}" ]; then
    firebase_args+=(--project "${FIREBASE_PROJECT}")
  fi
  if [ -n "${FIREBASE_CLI_TOKEN:-}" ]; then
    token_arg="${FIREBASE_CLI_TOKEN}"
  elif [ -n "${FIREBASE_TOKEN:-}" ]; then
    token_arg="${FIREBASE_TOKEN}"
  fi
  if [ -n "${token_arg}" ]; then
    firebase_args+=(--token "${token_arg}")
  fi

  echo "Deploying Android Firebase build ${ANDROID_VERSION_CODE} from ${GITHUB_HEAD_REF} (${GITHUB_SHA})"
  "${FIREBASE_CLI_BIN}" "${firebase_args[@]}"
}

for env_file in "${ENV_FILES[@]}"; do
  load_env_file "${env_file}"
done

apply_cli_overrides
capture_version_fixed_flags
setup_android_env
setup_java_env
setup_build_metadata
setup_android_signing_env

if [ "${BUILD_ONLY}" -eq 0 ]; then
  setup_firebase_app_defaults
  require_env FIREBASE_ANDROID_APP_ID

  if ! has_firebase_auth; then
    echo "Missing Firebase authentication." >&2
    echo "Set FIREBASE_SERVICE_CREDENTIALS_JSON, FIREBASE_CLI_TOKEN, FIREBASE_TOKEN, GOOGLE_APPLICATION_CREDENTIALS," >&2
    echo "or login with firebase-tools/gcloud before running this script." >&2
    exit 1
  fi

  if ! command -v "${FIREBASE_CLI_BIN}" >/dev/null 2>&1; then
    echo "Firebase CLI not found: ${FIREBASE_CLI_BIN}" >&2
    echo "Install firebase-tools or pass --firebase-cli PATH." >&2
    exit 1
  fi

  setup_firebase_service_credentials
fi

if ! git -C "${ROOT_DIR}" diff --quiet || ! git -C "${ROOT_DIR}" diff --cached --quiet; then
  if [ "${BUILD_ONLY}" -eq 1 ]; then
    echo "Warning: building with uncommitted local changes." >&2
  else
    echo "Warning: deploying with uncommitted local changes." >&2
  fi
fi

if [ "${SKIP_BUILD}" -eq 0 ]; then
  run_gradle_build
fi

if [ "${BUILD_ONLY}" -eq 1 ]; then
  exit 0
fi

APK_PATH="$(artifact_path_for_variant)"
if [ ! -f "${APK_PATH}" ]; then
  echo "APK not found: ${APK_PATH}" >&2
  echo "Build first or pass --apk PATH." >&2
  exit 1
fi

run_firebase_deploy "${APK_PATH}"
