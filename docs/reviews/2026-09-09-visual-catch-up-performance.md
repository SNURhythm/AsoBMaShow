# Visual catch-up performance — 2026-09-09

Status: **complete — measured improvement, independent reviews and desktop/iOS/Android verification passed**.

## Change

Historical BGA catch-up previously woke decoder workers between successive seeks. Workers decoded intermediate positions that the next event immediately invalidated, while the calling thread repeatedly waited on each decoder's mutex.

`VideoPlayer::DecodeBatch` now temporarily gates decoding and synchronizes with already admitted work. Jukebox applies scoped gates to the affected players during multi-event catch-up, then resumes decoding after all existing commands have run. The gate composes with application suspension, supports nesting and unwinding, and rechecks admission under the decoder mutex.

Every seek and its failure behavior remains in the existing base-then-layer order. This is **not event coalescing**, video-ID merging, lazy loading, or an audio/global scheduler rewrite. Zero/single-event progression avoids batch containers. Audio-clock authority, BGA offsets, and eager visual readiness remain unchanged.

## Results

Paired runs used the preserved baseline executable built from `267ff6974a1ab579619162a9a41b086ad2f4f03d` and an after executable linked against the changed production objects. Each cell is the median of seven samples after discarding one warmup. All runs were sequential, without another native build or benchmark running concurrently.

| Video IDs | Due events | Before (ms) | After (ms) | Actual seek/flush calls |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 0.332 | 0.222 | 1 / 1 |
| 4 | 4 | 0.541 | 0.601 | 4 / 4 |
| 4 | 100 | 64.381 | 4.014 | 100 / 100 |
| 4 | 1,000 | 633.713 | 30.061 | 1,000 / 1,000 |
| 16 | 1,000 | 472.619 | 13.364 | 1,000 / 1,000 |

An independent **uninstrumented** four-ID/1,000-event control improved from **645.125 ms to 28.436 ms**, a **95.6% reduction** (22.7× faster for this operation). Its retained sample ranges were 633.237–656.449 ms before and 27.046–29.733 ms after. The instrumented four-ID/1,000-event ranges were 632.604–647.554 ms and 13.297–31.410 ms. Variation remains; the table is not a worst-case latency guarantee.

The small four-event case increased by 0.060 ms, within the 1 ms absolute nonregression allowance. The large case exceeded the required 50% reduction. Instrumented calls all succeeded and all commanded seeks/flushes remained present. Worker counts did not change: three, six and eighteen loaded process threads for one, four and sixteen video IDs respectively. No memory reduction is claimed.

## Workload and limitations

- Apple M1 Pro, eight CPU cores, 16 GiB RAM, macOS 26.5.1, Apple clang 17.
- Existing CMake **Debug** build (`-g`), real production Jukebox and FFmpeg decoder code, a fake audio backend, and bgfx Noop rendering.
- Generated H.264 video: 640×360, 30 fps, 30 seconds, 900 frames, GOP 60. Independent video IDs reference the same file but retain separate players.
- Event timestamps are `ordinal * 20,000,000 / eventCount` microseconds. Catch-up targets 20,000,000 microseconds.
- Every sample constructs and eagerly loads the players, waits a disclosed 100 ms, then measures only `seekVisualsToSongTime`. This delay is not a first-frame readiness assertion. Fixture generation and loading are outside the measured operation.
- These are local synthetic, cache-warm measurements, not release-build/mobile frame-rate, real audio/input jitter, or physical cold-storage results. Correct final frames are established separately by regression tests.

The initial broad probes also found expensive selection graph construction. Large-library samples showed busy parsing workers rather than evidence for a global scheduling rewrite. Those independent areas remain outside this focused correction.

## Correctness evidence

The new `visual_catch_up` integration test exercises real decoding and observes actual FFmpeg calls, wait predicates, and uploaded timestamp-marker pixels. Its pre-fix behavioral failure was:

```text
visual_catch_up_tests: multi-event catch-up must reject decoder admission at the worker barrier
```

The test waits for a positive rejection or actual decoder action; a missing-witness timeout fails and is never accepted as proof of exclusion. A separate mutation removing the under-mutex admission check also failed the dedicated race case.

Eight cases cover ordered catch-up and paused restoration, shared base/layer IDs, separate same-file players, final seek failure without an extra flush, retained decoded position, nested/moved guards and unwinding, independent suspension, admission races, zero/single events, missing/image fallback, backward/rapid seeks, eager reuse after deleting the input pathname, EOF recovery, and cancellation/reload. Seven focused CTests passed; the new integration test additionally passed ten consecutive runs.

The test-only fixture observes real wait predicates in addition to FFmpeg calls. This small expansion of instrumentation was chosen to obtain a deterministic rejection witness without adding a production testing API. Its cost is additional fixture complexity, checked during independent review.

## Reproduction on this checkout

Correctness tests are repository files:

```sh
cmake --build cmake-build-debug --target visual_catch_up_tests jukebox_restore_tests skin_movie_clock_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -j 6 -R 'visual_catch_up|foundation_av_jukebox_restore|skin_movie_clock|video_decode_state'
```

Local investigation artifacts are intentionally retained, untracked and ignored:

- `.superpowers/sdd/2026-09-09-performance/`: original probes, generated clip, raw original baselines, and sampling traces.
- `.superpowers/sdd/2026-09-09-visual-catch-up-performance/`: distinct after-probe builder, paired runner, raw paired JSONL/logs, summaries, source/baseline hashes, review packages, and verification logs.

With those local artifacts intact, rerun the comparison sequentially:

```sh
bash .superpowers/sdd/2026-09-09-visual-catch-up-performance/run_comparison.sh
```

The runner rebuilds after probes from current CMake objects and verifies that the original baseline binaries, original samples and generated clip remain unchanged. Rebuild `jukebox_restore_tests` before running it after source changes. The ignored investigation artifacts are not a standalone benchmark shipped by a fresh clone; the workload definition and portable regression fixture are recorded here separately.

## Final verification

- Independent Task 1 review: spec PASS, quality APPROVED, no critical/important/minor findings. The reviewer inspected lock ordering, lifetime, actual boundary instrumentation, preserved failed-seek behavior and test evidence without rerunning native workloads.
- Desktop all-target build, including `main`: passed. Full parallel CTest: **353/353 passed**, 86.31 seconds.
- `IOS_RELEASE_BUILD_JOBS=6 scripts/ios_release_verify.sh`: passed. Its 66 native tests, Python build/release/artifact/documentation checks (49/20/16/3 tests), unsigned Release iOS build and artifact audit all succeeded. No upload occurred.
- `CMAKE_BUILD_PARALLEL_LEVEL=6 scripts/android_firebase_deploy.sh --build-only`: Firebase Release build passed in 3 minutes 57 seconds, using the automatic version code. No upload occurred. Android runtime/device performance was not tested in this pass.
- Final whole-change review accepted production integration, independently recomputed all 96 paired raw samples, and confirmed the measured claims and platform evidence. It found one minor bug in the local rerun script: the summary output matched its raw-input glob. The input selection was corrected; an isolated behavioral RED/GREEN check and two successive summary reruns preserved all twelve numeric results and all original evidence hashes. Scoped re-review marked the finding addressed, with no remaining findings or new breakage.

The implementation and validation pass made no commits, pushes, merges, deployments, new worktrees, parser changes, dependency changes, or whole-file formatting. The user subsequently requested committing and pushing the verified changes. The five original September 8 review inputs remain untouched and are excluded from that commit.
