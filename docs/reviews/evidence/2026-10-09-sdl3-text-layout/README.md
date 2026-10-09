# Reproduce dropdown and wrapping reuse measurements

Build **only `probe3`** twice against application snapshots. This harness uses
SDL3 on both sides; the shared CMake project's legacy `probe2` is not applicable.
The [original build prerequisites](../2026-10-09-sdl3-performance/README.md) apply,
including retained Release bgfx/Yoga libraries and vcpkg SDL2 package metadata.
Dependency pins and font fingerprints are recorded in the raw/build metadata.

From this report's committed checkout:

```sh
benchmark_repo=$(pwd)
benchmark_scratch=$(mktemp -d /tmp/asobmashow-text-layout.XXXXXX)
benchmark_harness="$benchmark_repo/docs/reviews/evidence/2026-10-09-sdl3-text-layout/probe.cpp"
mkdir -p "$benchmark_scratch/before" "$benchmark_scratch/after"
git archive abaefa1c src | tar -x -C "$benchmark_scratch/before"
git archive HEAD src | tar -x -C "$benchmark_scratch/after"
cmake -S docs/reviews/evidence/2026-10-09-sdl3-performance \
  -B "$benchmark_scratch/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DREPO="$benchmark_repo" -DBASELINE="$benchmark_scratch/before" \
  -DCANDIDATE="$benchmark_scratch/before" \
  -DPROBE_SOURCE="$benchmark_harness" -DPROBE_DROPDOWNS=ON
cmake --build "$benchmark_scratch/build" --target probe3 -j 6
cp "$benchmark_scratch/build/probe3" "$benchmark_scratch/probe-before"
cmake -S docs/reviews/evidence/2026-10-09-sdl3-performance \
  -B "$benchmark_scratch/build" -DCANDIDATE="$benchmark_scratch/after"
cmake --build "$benchmark_scratch/build" --target probe3 -j 6
cp "$benchmark_scratch/build/probe3" "$benchmark_scratch/probe-after"
python3 docs/reviews/evidence/2026-10-09-sdl3-adaptation/compare.py \
  --baseline abaefa1c --harness "$benchmark_harness" \
  --before "$benchmark_scratch/probe-before" \
  --after "$benchmark_scratch/probe-after" \
  --output "$benchmark_scratch/raw.json" --pairs 20
```

Finish all builds/tests first. The runner excludes process and full-batch
warmups, retains every sample, checks matching metric names/operation counts and
aggregate checksums, and uses balanced shuffled pair order. The runner's new
`--harness` option records the actual source fingerprint; its original default
still points to the original 15-metric migration probe. It now accepts any
nonempty set of uniquely named metrics, with before/after consistency checks.

This retained run used an existing Release build serially to avoid duplicating
its dependency artifacts. The before source snapshot came from `git archive`;
the candidate used the working tree. `build.json` records the four changed
application source/header fingerprints, primary/icon font fingerprints and
candidate compile commands. Binary hashes are in the raw metadata.
`measured-changes.patch.gz` captures the tracked diff at measurement time;
`probe.cpp` is recorded separately because it was then untracked.

Raw results and the patch are losslessly gzip-compressed. The summary retains
all metrics, including changed-label/resized-width cache-miss controls. Reused
manual wrapping is a protected helper measurement that consumes a const reference
to either the baseline temporary or candidate cache. Composed raster includes
manual wrapping plus CPU surface work.
Neither measures native left-aligned primary-font wrapping, GPU work, frame
rate, physical input latency or mobile performance.


The initial complete 20-pair run is retained in `value-returning-probe/`, with
its exact harness and raw/summary/build records. Its helper returned a vector
by value, adding a copy to the candidate's reference-returning API on a cache
miss (the baseline could move its return value). After observing a +3.4% mixed
resize delta, this mismatch was identified and the helper corrected to consume
both results through a const reference within one scope, as production does.
The final run uses this corrected identical harness on both revisions. No
production change was made between runs and no samples were removed from either.
