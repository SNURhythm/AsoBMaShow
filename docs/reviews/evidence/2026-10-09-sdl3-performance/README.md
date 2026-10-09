# Reproduce the SDL migration comparison

This macOS-only probe compares CPU-side application code, not a complete game.
See [the report](../../2026-10-09-sdl3-performance.md) for interpretation and limits.

Requirements: the repository/submodules at the recorded revisions; Xcode/Clang,
CMake, Ninja, pkg-config and Python 3; the retained `cmake-build-release` vcpkg
SDL2/SDL2_ttf dependencies and release bgfx/Yoga static libraries. The baseline
packages are SDL2 2.32.10, SDL2_ttf 2.24.0 with HarfBuzz disabled, and FreeType
2.13.3. Building SDL2_ttf with another feature set changes the comparison.
`binary-provenance.json` records the exact libraries and executable hashes used.

The CMake project deliberately uses the baseline's pkg-config static library
paths rather than a global `Freetype::Freetype` CMake target, which SDL3_ttf uses
for its vendored dependency. This prevents accidentally giving the old binary
the new font backend. The SDL3_ttf static archive contains its vendored objects.

From the repository root:

```sh
benchmark_repo=$(pwd)
test "$(git -C SDL rev-parse HEAD)" = 3d22d98cf633b673aae4f278ea55775481ae9279 || exit 1
test "$(git -C SDL_ttf rev-parse HEAD)" = 661ba00eb142c060768b752c9420b50c961b909e || exit 1
benchmark_scratch=$(mktemp -d /tmp/asobmashow-sdl-comparison.XXXXXX)
benchmark_evidence="$benchmark_repo/docs/reviews/evidence/2026-10-09-sdl3-performance"
mkdir -p "$benchmark_scratch/baseline" "$benchmark_scratch/candidate"
git archive 1aa029e5 src | tar -x -C "$benchmark_scratch/baseline"
git archive ef61b16f src | tar -x -C "$benchmark_scratch/candidate"
cmake -S "$benchmark_evidence" -B "$benchmark_scratch/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DREPO="$benchmark_repo" \
  -DBASELINE="$benchmark_scratch/baseline" \
  -DCANDIDATE="$benchmark_scratch/candidate"
cmake --build "$benchmark_scratch/build" --target probe2 probe3 -j 6
python3 "$benchmark_evidence/run.py" --build "$benchmark_scratch/build" \
  --output "$benchmark_scratch/raw.json" --pairs 20
```

`run.py` records revision/build labels but does not independently validate supplied
binaries. Use fresh snapshots and a fresh build directory as above; the committed
run’s source revisions, compile flags, linkage and fingerprints were checked
separately. Keep the machine otherwise idle while timing, and finish compilation first.
`run.py` writes every retained process result and `summary.json` alongside the
requested raw output. Reruns intentionally use a new output path instead of
replacing the committed measurement. It uses balanced, shuffled pair order and
fixed seeds for reproducibility. Do not select only favorable reruns.

`probe.cpp` checks event and ingress counts, positive text measurements and
surfaces, and records a checksum. Each metric has an excluded identical batch
warmup. The process-level warmups are also excluded before the 20 retained pairs.
No physical events are injected: all touch events are synthetic and remain
inside the benchmark process. No audio output or distribution action is used.

The saved results were collected after two discarded calibration passes used
to verify the harness and choose sufficient batch lengths. Inspection of
SDL3_ttf's eight-entry layout cache motivated including both four-string and
128-string workloads before the retained series began. All 40 retained process
runs are present, including their ranges and order.
