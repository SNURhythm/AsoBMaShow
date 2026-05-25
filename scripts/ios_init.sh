#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IOS_DIR="${ROOT_DIR}/ios/Xcode/AsoBMaShow"
CACHE_ROOT="${IOS_DEPLOY_CACHE_ROOT:-${HOME}/Library/Caches/AsoBMaShow/ios-deploy}"

hash_stdin() {
  shasum | awk '{ print $1 }'
}

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

bgfx_cache_key() {
  {
    printf 'root=%s\n' "${ROOT_DIR}"
    git -C "${ROOT_DIR}" submodule status --recursive bgfx
    cmake --version | head -n 1
    xcodebuild -version
    printf 'iphoneos=%s\n' "$(xcrun --sdk iphoneos --show-sdk-path)"
  } | hash_stdin
}

prepare_bgfx_project() {
  local build_link="${ROOT_DIR}/bgfx/build"
  local cache_dir="${CACHE_ROOT}/bgfx-build/$(bgfx_cache_key)"

  link_cache_dir "${build_link}" "${cache_dir}"

  if [ -f "${build_link}/bgfx.xcodeproj/project.pbxproj" ]; then
    echo "Using cached bgfx Xcode project: ${cache_dir}"
    return
  fi

  echo "Configuring bgfx Xcode project: ${cache_dir}"
  cmake \
    -S "${ROOT_DIR}/bgfx" \
    -B "${build_link}" \
    -GXcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphoneos
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
  local pod_lock_hash
  local gem_lock_hash
  local pods_cache

  pod_lock_hash="$(shasum "${IOS_DIR}/Podfile.lock" | awk '{ print $1 }')"
  gem_lock_hash="$(shasum "${IOS_DIR}/Gemfile.lock" | awk '{ print $1 }')"
  pods_cache="${CACHE_ROOT}/pods/${pod_lock_hash}-${gem_lock_hash}"

  link_cache_dir "${IOS_DIR}/Pods" "${pods_cache}"

  cd "${IOS_DIR}"
  if [ -f "Pods/Manifest.lock" ] && cmp -s "Podfile.lock" "Pods/Manifest.lock"; then
    echo "Using cached CocoaPods install: ${pods_cache}"
  else
    bundle exec pod install --deployment
  fi
}

prepare_bgfx_project
install_gems
install_pods
