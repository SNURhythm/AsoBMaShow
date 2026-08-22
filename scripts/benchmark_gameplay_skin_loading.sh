#!/usr/bin/env bash
set -euo pipefail

baseline=develop
candidate=HEAD
maximum_regression_percent=10
samples=7
skin_path=

usage() {
  echo "usage: $0 [--baseline REF] [--candidate REF] [--maximum-regression-percent N] [--samples N] [--skin PATH]" >&2
}

while (($#)); do
  case "$1" in
    --baseline) baseline=${2-}; shift 2 ;;
    --candidate) candidate=${2-}; shift 2 ;;
    --maximum-regression-percent) maximum_regression_percent=${2-}; shift 2 ;;
    --samples) samples=${2-}; shift 2 ;;
    --skin) skin_path=${2-}; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done

if ! [[ $samples =~ ^[0-9]+$ ]] || ((samples < 7)); then
  echo "error: --samples must be an integer of at least 7" >&2
  exit 2
fi
if ! [[ $maximum_regression_percent =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "error: --maximum-regression-percent must be nonnegative" >&2
  exit 2
fi

repo=$(git rev-parse --show-toplevel)
git -C "$repo" rev-parse --verify "${baseline}^{commit}" >/dev/null
git -C "$repo" rev-parse --verify "${candidate}^{commit}" >/dev/null
vcpkg_toolchain=${CMAKE_TOOLCHAIN_FILE:-}
if [[ -z $vcpkg_toolchain && -n ${VCPKG_ROOT:-} ]]; then
  vcpkg_toolchain="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
fi
if [[ -z $vcpkg_toolchain && -f "$repo/cmake-build-debug/CMakeCache.txt" ]]; then
  vcpkg_toolchain=$(sed -n \
    's/^CMAKE_TOOLCHAIN_FILE:[^=]*=//p' \
    "$repo/cmake-build-debug/CMakeCache.txt" | head -1)
fi
if [[ ! -f $vcpkg_toolchain ]]; then
  echo "error: set VCPKG_ROOT or CMAKE_TOOLCHAIN_FILE to a valid vcpkg checkout" >&2
  exit 2
fi
if [[ -n $skin_path ]]; then
  skin_path=$(cd "$(dirname "$skin_path")" && pwd -P)/$(basename "$skin_path")
  if [[ ! -e $skin_path ]]; then
    echo "error: --skin path does not exist: $skin_path" >&2
    exit 2
  fi
fi

temporary=$(mktemp -d "${TMPDIR:-/tmp}/asobmashow-skin-loading.XXXXXX")
baseline_tree="$temporary/baseline"
candidate_tree="$temporary/candidate"

cleanup() {
  git -C "$repo" worktree remove --force "$candidate_tree" >/dev/null 2>&1 || true
  git -C "$repo" worktree remove --force "$baseline_tree" >/dev/null 2>&1 || true
  git -C "$repo" worktree prune >/dev/null 2>&1 || true
  rm -rf "$temporary"
}
trap cleanup EXIT INT TERM

git -C "$repo" worktree add --detach "$baseline_tree" "$baseline" >/dev/null
git -C "$repo" worktree add --detach "$candidate_tree" "$candidate" >/dev/null

link_submodules() {
  local tree=$1
  local submodule
  for submodule in SDL SDL_ttf bgfx yoga third_party/pcre2; do
    if [[ -d "$repo/$submodule" ]]; then
      rmdir "$tree/$submodule" 2>/dev/null || true
      if [[ ! -e "$tree/$submodule" ]]; then
        mkdir -p "$(dirname "$tree/$submodule")"
        ln -s "$repo/$submodule" "$tree/$submodule"
      fi
    fi
  done
}
link_submodules "$baseline_tree"
link_submodules "$candidate_tree"

# The feature benchmark target does not exist on develop. Install the exact
# same Lua-only harness into the isolated baseline tree; feature-only
# JSON/LR2 code is compiled out there. No baseline source escapes the temp
# worktree and both revisions execute the identical Lua session workload.
cp "$candidate_tree/tests/gameplay_skin_loading_benchmark_tests.cpp" \
   "$baseline_tree/tests/gameplay_skin_loading_benchmark_tests.cpp"
python3 - "$baseline_tree/CMakeLists.txt" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
fragment = r'''
if(TARGET play_skin_session_tests)
  get_target_property(_loading_benchmark_sources play_skin_session_tests SOURCES)
  list(REMOVE_ITEM _loading_benchmark_sources tests/play_skin_session_tests.cpp)
  add_executable(gameplay_skin_loading_benchmark_tests
    tests/gameplay_skin_loading_benchmark_tests.cpp
    ${_loading_benchmark_sources})
  target_include_directories(gameplay_skin_loading_benchmark_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
  target_compile_features(gameplay_skin_loading_benchmark_tests PRIVATE cxx_std_23)
  target_compile_definitions(gameplay_skin_loading_benchmark_tests PRIVATE
    ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
    ASOBMASHOW_PLAY_SKIN_SESSION_TESTING=1
    ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
    ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  get_target_property(_loading_benchmark_links play_skin_session_tests LINK_LIBRARIES)
  target_link_libraries(gameplay_skin_loading_benchmark_tests PRIVATE
    ${_loading_benchmark_links})
endif()
'''
path.write_text(path.read_text(encoding="utf-8") + fragment, encoding="utf-8")
PY

configure_and_build() {
  local tree=$1
  local build=$2
  cmake -S "$tree" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE="$vcpkg_toolchain" \
    -DASOBMASHOW_BUILD_TESTS=ON \
    -DBUILD_TESTING=ON \
    -DVCPKG_INSTALLED_DIR="$repo/cmake-build-debug/vcpkg_installed" >/dev/null
  cmake --build "$build" --target gameplay_skin_loading_benchmark_tests -j 6 >/dev/null
}

configure_and_build "$baseline_tree" "$temporary/build-baseline"
configure_and_build "$candidate_tree" "$temporary/build-candidate"

extra=(--format lua)
if [[ -n $skin_path ]]; then
  extra+=(--skin "$skin_path")
fi
requested=$((samples + 1))

run_mode() {
  local executable=$1
  local mode=$2
  "$executable" --benchmark --mode "$mode" --samples "$requested" "${extra[@]}"
}

baseline_cold=$(run_mode "$temporary/build-baseline/gameplay_skin_loading_benchmark_tests" cold)
baseline_warm=$(run_mode "$temporary/build-baseline/gameplay_skin_loading_benchmark_tests" warm)
candidate_cold=$(run_mode "$temporary/build-candidate/gameplay_skin_loading_benchmark_tests" cold)
candidate_warm=$(run_mode "$temporary/build-candidate/gameplay_skin_loading_benchmark_tests" warm)

python3 - "$baseline_cold" "$baseline_warm" "$candidate_cold" "$candidate_warm" \
  "$samples" "$maximum_regression_percent" <<'PY'
import json
import statistics
import sys

labels = ("baseline cold", "baseline warm", "candidate cold", "candidate warm")
records = [json.loads(value) for value in sys.argv[1:5]]
expected = int(sys.argv[5])
maximum = float(sys.argv[6])
medians = []
for label, record in zip(labels, records):
    values = record.get("samplesMicros", [])
    if len(values) != expected + 1:
        raise SystemExit(f"error: {label} returned {len(values)} samples")
    retained = values[1:]
    if len(retained) < 7 or any(value < 0 for value in retained):
        raise SystemExit(f"error: {label} returned invalid retained samples")
    medians.append(statistics.median(retained))

failed = False
for mode, base, candidate in (
    ("cold", medians[0], medians[2]),
    ("warm", medians[1], medians[3]),
):
    regression = 0.0 if base == 0 else (candidate - base) * 100.0 / base
    print(f"{mode}: baseline={base:.1f}us candidate={candidate:.1f}us regression={regression:.2f}%")
    failed = failed or regression > maximum
if failed:
    raise SystemExit(1)
PY
