# Slow test review — 2026-10-17

The subsequent [full-suite optimization pass](cold-suite-optimization-2026-10-17.md)
addresses cold compiler-cache performance and revisits the retry waits and
serialization retained in this earlier pass.

The preceding full `ctest -j 6` run passed 498 tests in 129.54 seconds. Its slowest entries guided this review; parallel-run durations include contention and are not directly comparable with isolated measurements. Temporary per-case timing was used to identify costs in the importer and progressive-audio suites, then removed.

## Reduced work

| Suite | Serial before | Serial after | Change and retained assertions |
| --- | ---: | ---: | --- |
| `bgfx_metal_readback_tests` | 11.48 s | 2.33 s | Four frames instead of 24 per case. Both pixel formats, odd rows, full export dimensions, canary bytes, resource replacement, and mip fallback remain. Four frames update each mip twice with changing contents, retaining stale-read detection. This reduces endurance repetition. |
| `difficulty_table_importer_tests` | 17.57 s | 13.01 s | Two seed-state tests use an eight-source, one-chart-per-source snapshot instead of repeatedly importing the entire catalog. Revision advancement, missing-file failure, shared database reopening and preservation of deleted tables remain. The separate packaged-snapshot test still imports the real catalog. |
| `progressive_music_source_tests` | 14.29 s | 11.03 s | Remove the 17-voice case and standalone 64-voice case; retain 32/33 at the cache boundary, repeated 64 voices, and 192 under a constrained descriptor limit. Ordinary fixture audio shrinks from 1–3 seconds to half a second, still crossing five 4096-frame decode blocks. Exact reference PCM comparisons and decoder-open/release assertions remain; the 192-voice fixture is unchanged. |

Combined serial time fell from 43.34 to 26.37 seconds (39%). Measurements are single local runs, not a statistical benchmark or a promise of the same wall-clock reduction under parallel CI.

## Expensive work retained

- `find_bms_ios_file_bridge_tests` took 32.72 seconds in the earlier parallel run and is serialized. It executes actual native download retries, range resume/restart, retry exhaustion, user recovery decisions, cancellation, disk capacity and injected file faults. Production retry waits account for substantial cost. Removing scenarios or changing extracted production backoff code would weaken this integration coverage; no change was made.
- `gameplay_scene_feature_off_compile_contract` took 19.49 seconds. It compiles four real translation units with gameplay skins disabled, a configuration ordinary enabled builds do not check. It remains useful despite compiler startup/header costs.
- The importer response-budget test alone took 7.37 seconds in profiling. Its actual 16 MiB boundaries for header and data downloads remain; tiny payloads would not test the production limit.
- The archive concurrency suite's 513 MiB case deliberately crosses the 512 MiB dispatch threshold and verifies all bytes. Its size is necessary to reach the regression path.
- Progressive audio's archived cold-seek, cancellation, lease cleanup and descriptor-limit cases remain. Their formats and boundary conditions exercise distinct behavior.

No production code or CTest registrations changed in this pass. All three affected targets rebuilt and their focused serial tests passed.

## Reusing compiled artifacts

Runtime-generated programs now consistently use the existing `FixtureCompiler` and configured compiler launcher in the UI-scale, main-menu tab, responsive-layout, and Foundation bridge suites. CTest also passes the launcher to the previously unwired settings, input-ingress and export-cancellation fixtures. Absolute temporary source paths in two existing fixtures were replaced by the helper's stable relative compilation names.

Foundation fixtures resolve the real macOS compiler and SDK with `xcrun`, compile through the launcher, then link directly. This retains compiler objects on disk through `ccache` when configured; it does not cache test results. Source/header/compiler/flag changes remain the compiler cache's responsibility. Runtime directories and linked helper executables remain fresh. Direct script runs still work without a cache launcher.

The initial reduced-work version passed all 498 tests in 130.44 seconds, versus 129.54 seconds before: no overall wall-clock improvement was demonstrated. A subsequent concurrency experiment overlapped an unrelated Android build and timed out progressive audio at its existing 30-second limit; 497 tests passed. Native bridge serialization was retained because its concurrent run approached the subprocess deadline. Neither timeouts nor production retry delays were relaxed.

After the Android build finished, all 15 affected fixture suites passed in both an empty isolated cache and a warm repeat, run serially:

| Cache state | Elapsed | Direct hits (cumulative) | Misses (cumulative) |
| --- | ---: | ---: | ---: |
| Empty at start | 49.07 s | 1 | 53 |
| Warm repeat | 8.59 s | 55 | 53 |

The warm run took 82.5% less time, with 54 new direct hits and no new misses or compiler failures. Cache storage was 3,232 KiB. The isolated benchmark cache was deleted afterward; normal CTest runs use the configured persistent cache. Existing GNU/MSVC/clang-cl command checks also pass. The final full suite passed all 498 tests with `ctest --test-dir cmake-build-debug --output-on-failure -j 6` in 120.33 seconds, using the normal persistent cache. This first full run still compiled newly cache-enabled helpers; it is not a fully warm whole-suite benchmark. The prior comparison run was 129.54 seconds. All affected C++ targets built, and `git diff --check` passed.
