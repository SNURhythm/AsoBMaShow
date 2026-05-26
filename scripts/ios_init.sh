#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IOS_DIR="${ROOT_DIR}/ios/Xcode/AsoBMaShow"
CACHE_ROOT="${IOS_DEPLOY_CACHE_ROOT:-${HOME}/Library/Caches/AsoBMaShow/ios-deploy}"
CLEAN_PROJECT_ONLY=0

for arg in "$@"; do
  case "${arg}" in
    --clean-project-only)
      CLEAN_PROJECT_ONLY=1
      ;;
    *)
      echo "Unknown argument: ${arg}" >&2
      exit 2
      ;;
  esac
done

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

clean_project() {
  echo "Cleaning iOS generated project and Xcode build state"
  rm -rf "${ROOT_DIR}/bgfx/build"
  rm -rf "${CACHE_ROOT}/bgfx-build"
  rm -rf "${IOS_DIR}/build"
  rm -f "${IOS_DIR}"/*.ipa "${IOS_DIR}"/*.dSYM.zip

  if [ -d "${HOME}/Library/Developer/Xcode/DerivedData" ]; then
    find "${HOME}/Library/Developer/Xcode/DerivedData" \
      -maxdepth 1 \
      -name 'AsoBMaShow-*' \
      -exec rm -rf {} +
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

build_bgfx_libraries() {
  local build_dir="${ROOT_DIR}/bgfx/build"
  local expected_libraries=(
    "${build_dir}/cmake/bgfx/Release-iphoneos/libbgfx.a"
    "${build_dir}/cmake/bimg/Release-iphoneos/libbimg.a"
    "${build_dir}/cmake/bimg/Release-iphoneos/libbimg_decode.a"
    "${build_dir}/cmake/bimg/Release-iphoneos/libbimg_encode.a"
    "${build_dir}/cmake/bx/Release-iphoneos/libbx.a"
  )
  local library
  local missing=0

  for library in "${expected_libraries[@]}"; do
    if [ ! -f "${library}" ] || [ -L "${library}" ]; then
      missing=1
    fi
  done

  if [ "${missing}" -eq 0 ] && [ "${BGFX_FORCE_BUILD:-0}" != "1" ]; then
    echo "Using cached bgfx iOS static libraries"
    return
  fi

  echo "Building bgfx iOS static libraries"
  for library in "${expected_libraries[@]}"; do
    [ ! -L "${library}" ] || rm "${library}"
  done

  xcodebuild \
    -project "${build_dir}/bgfx.xcodeproj" \
    -configuration Release \
    -sdk iphoneos \
    -destination 'generic/platform=iOS' \
    -target bgfx \
    -target bimg \
    -target bimg_decode \
    -target bimg_encode \
    -target bx \
    build
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

if [ "${CLEAN_PROJECT_ONLY}" -eq 1 ]; then
  clean_project
  exit 0
fi

prepare_bgfx_project
build_bgfx_libraries
setup_project_ruby
install_gems
install_pods
