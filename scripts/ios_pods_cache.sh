#!/usr/bin/env bash
set -euo pipefail

ios_pods_cache_key() {
  [ "$#" -eq 3 ] || return 2
  local podfile="$1"
  local pod_lock_file="$2"
  local gem_lock_file="$3"
  local podfile_hash
  local pod_lock_hash
  local gem_lock_hash

  podfile_hash="$(shasum "${podfile}" | awk '{ print $1 }')"
  pod_lock_hash="$(shasum "${pod_lock_file}" | awk '{ print $1 }')"
  gem_lock_hash="$(shasum "${gem_lock_file}" | awk '{ print $1 }')"
  printf '%s-%s-%s\n' "${podfile_hash}" "${pod_lock_hash}" "${gem_lock_hash}"
}

ios_pods_cache_valid() {
  [ "$#" -eq 2 ] || return 2
  local pods_dir="$1"
  local lock_file="$2"
  local configuration
  local aggregate_config

  for configuration in debug release; do
    aggregate_config="${pods_dir}/Target Support Files/Pods-AsoBMaShow/Pods-AsoBMaShow.${configuration}.xcconfig"
    [ -f "${aggregate_config}" ] || return 1
    # shellcheck disable=SC2016 # SRCROOT must stay literal in CocoaPods output.
    grep -Fqx 'PODS_ROOT = ${SRCROOT}/Pods' "${aggregate_config}" || return 1
  done

  [ -d "${pods_dir}" ] &&
    [ ! -L "${pods_dir}" ] &&
    [ -f "${pods_dir}/Manifest.lock" ] &&
    [ -f "${pods_dir}/Pods.xcodeproj/project.pbxproj" ] &&
    cmp -s "${lock_file}" "${pods_dir}/Manifest.lock"
}

ios_pods_cache_restore() {
  [ "$#" -eq 3 ] || return 2
  local cache_dir="$1"
  local pods_dir="$2"
  local lock_file="$3"
  local staging="${pods_dir}.restore.$$"

  ios_pods_cache_valid "${cache_dir}" "${lock_file}" || return 1
  rm -rf "${staging}"
  mkdir -p "${staging}"
  if ! rsync -a --delete "${cache_dir}/" "${staging}/" ||
     ! ios_pods_cache_valid "${staging}" "${lock_file}"; then
    rm -rf "${staging}"
    return 1
  fi

  rm -rf "${pods_dir}"
  mv "${staging}" "${pods_dir}"
}

ios_pods_cache_store() {
  [ "$#" -eq 3 ] || return 2
  local pods_dir="$1"
  local cache_dir="$2"
  local lock_file="$3"
  local staging="${cache_dir}.tmp.$$"
  local previous="${cache_dir}.previous.$$"

  ios_pods_cache_valid "${pods_dir}" "${lock_file}" || return 1
  mkdir -p "$(dirname "${cache_dir}")"
  rm -rf "${staging}" "${previous}"
  mkdir -p "${staging}"
  if ! rsync -a --delete "${pods_dir}/" "${staging}/" ||
     ! ios_pods_cache_valid "${staging}" "${lock_file}"; then
    rm -rf "${staging}"
    return 1
  fi

  if [ -e "${cache_dir}" ] || [ -L "${cache_dir}" ]; then
    mv "${cache_dir}" "${previous}"
  fi
  if mv "${staging}" "${cache_dir}"; then
    rm -rf "${previous}"
    return 0
  fi

  rm -rf "${staging}"
  if [ -e "${previous}" ] || [ -L "${previous}" ]; then
    mv "${previous}" "${cache_dir}"
  fi
  return 1
}
