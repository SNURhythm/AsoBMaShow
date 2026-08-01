#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IOS_DIR="${ROOT_DIR}/ios/Xcode/AsoBMaShow"
CACHE_ROOT="${IOS_DEPLOY_CACHE_ROOT:-${HOME}/Library/Caches/AsoBMaShow/ios-deploy}"

# shellcheck source=scripts/ios_pods_cache.sh
source "${ROOT_DIR}/scripts/ios_pods_cache.sh"

link_cache_dir() {
  local link_path="$1"
  local cache_dir="$2"

  mkdir -p "$(dirname "${link_path}")" "${cache_dir}"

  if [ -L "${link_path}" ]; then
    if [ "$(readlink "${link_path}")" != "${cache_dir}" ]; then
      rm "${link_path}"
      ln -s "${cache_dir}" "${link_path}"
    fi
  elif [ -e "${link_path}" ]; then
    rm -rf "${link_path}"
    ln -s "${cache_dir}" "${link_path}"
  else
    ln -s "${cache_dir}" "${link_path}"
  fi
}

prepare_bgfx_project() {
  local build_dir="${ROOT_DIR}/bgfx/build"

  if [ -L "${build_dir}" ]; then
    rm "${build_dir}"
  fi

  cmake \
    -S "${ROOT_DIR}/bgfx" \
    -B "${build_dir}" \
    -GXcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
}

install_gems() {
  local ruby_version
  local bundle_cache
  local bundle_jobs

  ruby_version="$(ruby -e 'print RUBY_VERSION')"
  bundle_cache="${CACHE_ROOT}/bundle/ruby-${ruby_version}"
  bundle_jobs="${BUNDLE_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

  mkdir -p "${IOS_DIR}/vendor"
  link_cache_dir "${IOS_DIR}/vendor/bundle" "${bundle_cache}"

  cd "${IOS_DIR}"
  if bundle check; then
    echo "Using cached Bundler gems: ${bundle_cache}"
  else
    bundle install --jobs "${bundle_jobs}" --retry 3
  fi
}

install_pods() {
  local pods_cache_key
  local pods_cache

  pods_cache_key="$(ios_pods_cache_key "${IOS_DIR}/Podfile" \
    "${IOS_DIR}/Podfile.lock" "${IOS_DIR}/Gemfile.lock")"
  pods_cache="${CACHE_ROOT}/pods/${pods_cache_key}"

  if ios_pods_cache_restore "${pods_cache}" "${IOS_DIR}/Pods" \
      "${IOS_DIR}/Podfile.lock"; then
    echo "Using cached CocoaPods install: ${pods_cache}"
    return 0
  fi

  rm -rf "${IOS_DIR}/Pods"
  cd "${IOS_DIR}"
  bundle exec pod install --deployment
  if ! ios_pods_cache_valid "${IOS_DIR}/Pods" "${IOS_DIR}/Podfile.lock"; then
    echo "CocoaPods installation is incomplete" >&2
    return 1
  fi
  if ! ios_pods_cache_store "${IOS_DIR}/Pods" "${pods_cache}" \
      "${IOS_DIR}/Podfile.lock"; then
    echo "Unable to refresh CocoaPods cache: ${pods_cache}" >&2
    return 1
  fi
}

prepare_bgfx_project
install_gems
install_pods
