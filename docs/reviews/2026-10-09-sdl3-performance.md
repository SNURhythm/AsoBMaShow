# SDL2 → SDL3 performance measurements

Measured on 2026-10-09 against the application before migration (`1aa029e5`)
and the completed migration (`ef61b16f`).

## Result

The migration substantially reduces CPU text-rasterization cost in these
workloads, and improves CJK text updates. It is **not a uniform speedup**:
128-string Latin layout/update work regresses, while event-queue throughput and
cached view creation show no clear change. No overall FPS or physical
input-to-sound improvement was measured.

For the 128-string workload, CJK text updates fell **29.3%** (25.59 → 18.10 µs),
Latin rasterization fell **41.7%** (18.96 → 11.05 µs), and CJK rasterization fell
**61.8%** (58.83 → 22.48 µs). Latin text updates rose **94.4%**
(3.82 → 7.42 µs); width measurement alone rose **267.0%** (1.17 → 4.30 µs).
These operations overlap and must not be summed into an overall speedup.

Repeated CJK strings show much larger gains: width measurement falls 95.4%,
text updates 88.6%, and rasterization 88.1%. SDL3_ttf has an eight-entry
layout-position cache (`SDL_ttf/src/SDL_ttf.c`, `GetCachedGlyphPositions`);
the four-string workload fits, while cycling 128 strings exceeds that cache.
An update still repeats internal measurements of its current string, so the
128-string update workload does not make every internal access a cache miss.
The results are consistent with the cache benefiting repeated labels. They do
not isolate the cache's contribution from the other font-stack changes.

Synthetic touch push/watch/flush cost rose **28.0%**, from **71 ns to 91 ns**
per event, approximately **20 ns extra CPU work**. This is a throughput
microbenchmark, not a measurement of additional physical input latency.

## Controlled comparison

- Apple M1 Pro, 16 GiB RAM, macOS 27.0.1, arm64.
- Apple Clang 21.0.0; benchmark and sampled application sources compiled with
  `-O3 -DNDEBUG`. The retained SDL2 release libraries were built with the same
  compiler version and release optimization settings; SDL3 was freshly built
  in Release. No compilation overlapped retained timing runs.
- Baseline desktop dependencies: SDL **2.32.10**, SDL_ttf **2.24.0**, FreeType
  **2.13.3**, HarfBuzz **off**, from the existing release vcpkg installation.
  These are the dependency versions selected by the pre-migration desktop
  manifest, rather than the separate historical mobile fork versions.
- Candidate: SDL **3.4.19**, fork `3d22d98cf`; SDL_ttf **3.2.3**, fork `661ba00`;
  vendored FreeType **2.13.3** and HarfBuzz **8.5.0**, with HarfBuzz **on**.
  This measures the migrated dependency configuration, not an experiment
  isolating the SDL major version with every font feature held constant.
- Both binaries compile each revision's actual `TextView`, `View`,
  `SDLTouchInputSource`, and their supporting sources. Both share the same
  unchanged release bgfx/Yoga libraries. bgfx uses its Noop renderer: GPU
  upload, real drawing, presentation and display synchronization are excluded.
- Same bundled Noto Sans CJK font, normal weight, 24-point `TextView` input
  (the application's 2× CPU raster scale). The font and harness SHA-256 hashes
  are retained in the raw metadata.
- Latin and mixed Japanese/Korean strings use either four repeating labels or
  128 distinct strings generated from those labels plus numeric suffixes.
  This is a controlled cache-working-set comparison, not a sampled library
  of 128 real chart titles. Glyph/file caches are warm.
- 20 pairs / 40 retained process runs, with 10 SDL2→SDL3 and 10 SDL3→SDL2 pairs
  shuffled using a fixed seed. One whole-process warmup per version is
  excluded, as is one identical full-batch warmup inside each measured metric.
  No retained outliers were removed.
- Each run measures 640,000 queue events, 640,000 synthetic touch events,
  16,000 width/update operations per text workload, 1,000 raster operations per
  text workload, and 10,000 cached view constructions. Event/callback counts,
  positive text widths/surface sizes and output checksums are checked.
- Point estimates are medians across process runs. Percent change is
  `(SDL3 median / SDL2 median − 1) × 100`; negative is faster. Intervals are
  paired percentile bootstrap 95% intervals over 20,000 resamples of whole
  pairs. They describe run variability on this machine, not uncertainty over
  all devices or workloads. Intervals are exploratory and not adjusted for
  testing 15 metrics.

## Complete results

Times are **µs per operation**; lower is better. “Text update” calls the real
`TextView::setText` with texture materialization deferred. “Rasterize” calls
the real single-font text-surface rendering method, including font availability
checks, but excludes full multiline/fallback composition and GPU texture upload. The touch metric includes synthetic queue
insertion and flushing as well as the real app ingress callback.

| Workload | SDL2 | SDL3 | Time change | Paired 95% interval |
|---|---:|---:|---:|---:|
| SDL event queue, 64-event bursts | 0.0511 | 0.0515 | +0.8% | [-0.3%, +1.1%] |
| Synthetic touch push → app callback → flush | 0.0713 | 0.0913 | +28.0% | [+27.8%, +28.3%] |
| Measure width: LATIN, 4 repeated strings | 1.0695 | 0.9590 | -10.3% | [-11.1%, -10.0%] |
| Update text, deferred texture: LATIN, 4 repeated strings | 3.3414 | 3.4239 | +2.5% | [+1.9%, +3.2%] |
| Rasterize text to CPU surface: LATIN, 4 repeated strings | 16.9608 | 6.9097 | -59.3% | [-59.6%, -59.0%] |
| Measure width: CJK, 4 repeated strings | 16.9171 | 0.7698 | -95.4% | [-95.5%, -95.4%] |
| Update text, deferred texture: CJK, 4 repeated strings | 25.5727 | 2.9041 | -88.6% | [-88.7%, -88.4%] |
| Rasterize text to CPU surface: CJK, 4 repeated strings | 56.8498 | 6.7843 | -88.1% | [-88.2%, -88.0%] |
| Measure width: LATIN, 128 different strings | 1.1706 | 4.2958 | +267.0% | [+265.2%, +269.5%] |
| Update text, deferred texture: LATIN, 128 different strings | 3.8189 | 7.4236 | +94.4% | [+93.0%, +95.7%] |
| Rasterize text to CPU surface: LATIN, 128 different strings | 18.9633 | 11.0539 | -41.7% | [-41.8%, -41.4%] |
| Measure width: CJK, 128 different strings | 17.0757 | 15.9109 | -6.8% | [-9.7%, -5.7%] |
| Update text, deferred texture: CJK, 128 different strings | 25.5888 | 18.1029 | -29.3% | [-29.7%, -28.8%] |
| Rasterize text to CPU surface: CJK, 128 different strings | 58.8348 | 22.4850 | -61.8% | [-62.2%, -61.4%] |
| Create/destroy a view with its font cached | 1.4331 | 1.4304 | -0.2% | [-1.1%, +0.8%] |

## Timestamp precision and memory

In every retained run, 500 synthetic events were pushed at approximately
100 µs intervals. After expressing SDL event fields in microseconds,
SDL2 produced **50–51 distinct microsecond timestamps**; SDL3 produced
**500/500**. The SDL2 millisecond event field collapses nearby events; SDL3's
nanosecond event field retains them through the app's microsecond conversion.
This demonstrates timestamp distinguishability in this probe, not physical
sensor resolution, OS delivery latency or acoustic latency. The unchanged
native input backends do not automatically inherit a latency improvement from
this SDL event-field change. The baseline `SDLTouchInputSource` already used
high-resolution `steady_clock` receipt timestamps; the event-field result does
not demonstrate a corresponding improvement in app timestamp resolution.

Median peak RSS for the benchmark processes was 40.38 MiB (SDL2) versus
41.93 MiB (SDL3), approximately +1.55 MiB. This is the peak of this particular
probe, not application/gameplay memory usage. Per-version checksums are stable
across all 20 runs (519992968 versus 518577718). These aggregate checksums
include text widths and surface sizes, not pixel hashes. Their difference
indicates changed aggregate metrics, not identical output. Rendering correctness
was considered separately in the migration’s font golden review.

## Limits and interpretation

The strongest supported improvement is CPU text rasterization, with a meaningful
CJK update improvement even beyond the eight-entry text cache. Varied Latin
layout is a measurable regression and should be profiled in a real scrolling
library before claiming that every screen is faster.

No full-game frame-time, startup, cold chart/skin loading, Android/iOS runtime,
controller, physical touch, audio loopback or battery measurement was performed.
The retained bgfx renderer and custom audio engine are outside this comparison.
The probe also excludes GPU uploads and does not establish real-world frequency
of each workload. There is no justified aggregate “the app is X% faster” figure.

## Evidence and reproduction

- [Raw paired runs](evidence/2026-10-09-sdl3-performance/raw.json)
- [Computed medians and intervals](evidence/2026-10-09-sdl3-performance/summary.json)
- [Harness, build and run instructions](evidence/2026-10-09-sdl3-performance/README.md)
- [Binary fingerprints](evidence/2026-10-09-sdl3-performance/binary-provenance.json)

All 15 reported percentage changes were independently recomputed from the raw
samples using a separate median calculation. Independent review also checked
source revisions, compiler flags, library linkage, binary hashes, and a second
paired-bootstrap implementation; it found no material calculation or linkage
defect within this benchmark’s scope.
