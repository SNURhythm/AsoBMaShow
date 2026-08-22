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
benchmark_source=tests/gameplay_skin_loading_benchmark_tests.cpp
for ref in "$baseline" "$candidate"; do
  if ! git -C "$repo" cat-file -e "$ref:$benchmark_source" 2>/dev/null ||
     ! git -C "$repo" show "$ref:CMakeLists.txt" |
       grep -q 'add_executable(gameplay_skin_loading_benchmark_tests'; then
    echo "error: same benchmark target/workload is unavailable at $ref" >&2
    exit 1
  fi
  if ! git -C "$repo" show "$ref:$benchmark_source" |
       grep -q 'ASOBMASHOW_LOADING_WORKLOAD_V2'; then
    echo "error: same benchmark target/workload is unavailable at $ref" >&2
    exit 1
  fi
done
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

requested=$((samples + 1))

run_mode() {
  local executable=$1
  local mode=$2
  local format=$3
  local extra=(--format "$format")
  if [[ -n $skin_path ]]; then
    extra+=(--skin "$skin_path")
  fi
  "$executable" --benchmark --mode "$mode" --samples "$requested" "${extra[@]}"
}

records=()
for format in lua json lr2; do
  for mode in cold warm; do
    records+=("$(run_mode "$temporary/build-baseline/gameplay_skin_loading_benchmark_tests" "$mode" "$format")")
    records+=("$(run_mode "$temporary/build-candidate/gameplay_skin_loading_benchmark_tests" "$mode" "$format")")
  done
done

python3 - "$samples" "$maximum_regression_percent" "${records[@]}" <<'PY'
import json
import statistics
import sys

expected = int(sys.argv[1])
maximum = float(sys.argv[2])
records = [json.loads(value) for value in sys.argv[3:]]
labels = [
    f"{format_name} {mode} {revision}"
    for format_name in ("lua", "json", "lr2")
    for mode in ("cold", "warm")
    for revision in ("baseline", "candidate")
]
if len(records) != len(labels):
    raise SystemExit("error: benchmark result matrix is incomplete")
medians = {}
for label, record in zip(labels, records):
    values = record.get("samplesMicros", [])
    if len(values) != expected + 1:
        raise SystemExit(f"error: {label} returned {len(values)} samples")
    retained = values[1:]
    if len(retained) < 7 or any(value < 0 for value in retained):
        raise SystemExit(f"error: {label} returned invalid retained samples")
    if record.get("formats") != 1:
        raise SystemExit(f"error: {label} did not isolate one source format")
    medians[label] = statistics.median(retained)

failed = False
for format_name in ("lua", "json", "lr2"):
    for mode in ("cold", "warm"):
        base = medians[f"{format_name} {mode} baseline"]
        candidate = medians[f"{format_name} {mode} candidate"]
        regression = 0.0 if base == 0 else (candidate - base) * 100.0 / base
        print(f"{format_name} {mode}: baseline={base:.1f}us candidate={candidate:.1f}us regression={regression:.2f}%")
        failed = failed or regression > maximum
if failed:
    raise SystemExit(1)
PY
