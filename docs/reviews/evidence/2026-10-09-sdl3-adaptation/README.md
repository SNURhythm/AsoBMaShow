# Reproduce the SDL3 application adaptation comparison

This compares application code before and after the adaptation, with SDL3 and
its font dependencies held constant. Check out the commit containing this
report before following these instructions. The original
[benchmark requirements](../2026-10-09-sdl3-performance/README.md) still apply,
including the retained release bgfx/Yoga libraries and vcpkg installation.

The harness is the unchanged `probe.cpp` in the previous evidence directory.
Build its `probe3` target twice against the two application source snapshots.
The CMake project's legacy `probe2` target is not built or measured here.

```sh
benchmark_repo=$(pwd)
test "$(git -C SDL rev-parse HEAD)" = 3d22d98cf633b673aae4f278ea55775481ae9279 || exit 1
test "$(git -C SDL_ttf rev-parse HEAD)" = 661ba00eb142c060768b752c9420b50c961b909e || exit 1
benchmark_scratch=$(mktemp -d /tmp/asobmashow-sdl3-adaptation.XXXXXX)
benchmark_harness="$benchmark_repo/docs/reviews/evidence/2026-10-09-sdl3-performance"
benchmark_evidence="$benchmark_repo/docs/reviews/evidence/2026-10-09-sdl3-adaptation"
mkdir -p "$benchmark_scratch/before" "$benchmark_scratch/after"
git archive 53f52b9e src | tar -x -C "$benchmark_scratch/before"
git archive HEAD src | tar -x -C "$benchmark_scratch/after"
cmake -S "$benchmark_harness" -B "$benchmark_scratch/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DREPO="$benchmark_repo" \
  -DBASELINE="$benchmark_scratch/before" \
  -DCANDIDATE="$benchmark_scratch/before"
cmake --build "$benchmark_scratch/build" --target probe3 -j 6
cp "$benchmark_scratch/build/probe3" "$benchmark_scratch/probe-before"
cmake -S "$benchmark_harness" -B "$benchmark_scratch/build" \
  -DCANDIDATE="$benchmark_scratch/after"
cmake --build "$benchmark_scratch/build" --target probe3 -j 6
cp "$benchmark_scratch/build/probe3" "$benchmark_scratch/probe-after"
python3 "$benchmark_evidence/compare.py" \
  --before "$benchmark_scratch/probe-before" \
  --after "$benchmark_scratch/probe-after" \
  --output "$benchmark_scratch/raw.json" --pairs 20
```

Finish all compilation and other performance-intensive work before timing.
Both binaries must report SDL major version 3. The runner checks metric names,
operation counts, positive results, and matching aggregate checksums. It saves
all 20 balanced, shuffled pairs and computes paired bootstrap intervals. Never
replace the committed raw data with a selectively chosen rerun.

Source revision/build labels are metadata declarations, not automatic proof of
binary provenance. The retained run separately verified compiler flags and the
before executable's hash against the original reviewed SDL3 binary. The runner
records both executable hashes, the font/harness hash, and the candidate
TextView source hash. `measured-changes.patch.gz` preserves the tracked working-tree
diff at measurement time; the decompressed patch hash is in `raw.json`. It includes supporting
changes not exercised by the text benchmark, and excludes then-untracked files.
Use the committed tree for full code/test reproduction.

The measured candidate differs from the baseline's sampled application sources
only in `TextView.cpp`. Both link the same Release SDL3/SDL3_ttf archives and
have HarfBuzz enabled. The two underlying fork commits and every nested
submodule pin are recorded in the [adaptation report](../../2026-10-09-sdl3-adaptation.md).
The recorded aggregate checksum covers text widths/surface sizes, not pixels.
The native wrapped-size path, pointer fixes and Metal cleanup have separate
correctness tests; these 15 timing metrics do not quantify their runtime cost.

`candidate-build.json` records the verified candidate compile commands, source
fingerprints and link command from the retained measurement build.
