# Test Suite Performance Implementation Plan

> **For agentic workers:** Use systematic debugging and verification-before-completion. Root owns builds and full-suite measurements; independent fixture changes may be delegated without concurrent test runs.

**Goal:** Prune redundant execution, reduce measured full-suite bottlenecks, and repair test instability without losing distinct regression coverage.

**Architecture:** Keep the current CTest suite and its production paths. Remove provably duplicated cases, reuse identical fixture compilations within one test invocation, batch differential-oracle comparisons, and address observed timing failures at their cause. Measure the same parallel suite before and after.

**Tech Stack:** CMake/Ninja, CTest, C++, Python unittest, Java 17.

**Spec:** User request: “our full suite tests are too slow. first prune redundant tests, and optimize test bottlenecks, and fix unstable tests.”

## Constraints and review focus

- Use the current checkout and branch; no new worktree or deployment. Commit and push verified task changes.
- Preserve distinct behavioral cases, independent oracle results, both native regex backends, Lua enabled/disabled variants, and assertions.
- Fixture compilation reuse must retain per-case process and working-directory isolation; cache only within the current invocation.
- Batch output must be checked for exact case count, including invalid patterns and newline subjects; single-pair CLI remains supported.
- Audio stress must still exercise production policy limits and failure/cancellation paths. Keep the existing timeout; do not make a failing suite green by hiding failures.
- Measure with `ctest --test-dir cmake-build-debug --output-on-failure -j 6`. Repeat affected unstable tests and the final full suite.

## Tasks

- [x] Build all targets and capture baseline CTest timings/results and the 409-test inventory.
- [x] Remove `chart_repository_search_paging` registration: its three functions are already called by the default `chart_repository_tests` runner. Preserve its manual CLI selector.
- [x] Reuse compiled music-selection directory, course, replay, and error fixtures, reducing 33 equivalent compilations to five while retaining all 53 unittest methods and fresh subprocesses.
- [x] Batch the 115 Java regex comparisons per backend into one JVM and one native process, preserving individual comparisons and the single-pair CLI.
- [x] Optimize only the Debug test compilation of `ChartAudioRenderer.cpp` where supported. Verify application and unrelated test compile commands remain unchanged, assertions stay enabled, and the full 317,608,200-frame workload still executes.
- [x] Reduce explicit-budget archive payloads while preserving chunk/cancellation boundaries and real 513 MiB RAR5 coverage; store only oversized Jukebox padding while retaining other compressed fixtures.
- [x] Reuse native ledger proof through CTest fixtures so the gameplay and music-select audits validate the same current executions rather than rerunning 23 owners; test stale/failing/missing evidence and filtered CTest prerequisites.
- [x] Build affected targets, run focused checks, compare full-suite wall time, and repeat timing-sensitive checks under parallel load.
- [x] Obtain independent review, inspect the complete diff, and record measurements/limits.
- Deliver the verified changes on the current branch and update its existing PR.

## Evidence and decisions

Initial inspection: 409 CTests, no exact duplicate command lines. Three search-paging functions run through both a dedicated registration and the general runner. Music fixture duplication is compilation overhead rather than redundant assertions. Regex batching retains all 115 inputs for each backend. Audio foreground/background stress assertions cover different policy paths and remain intact.

Baseline all-target build passed. Full CTest passed 408/409 in 364.72 seconds; `chart_audio_renderer_tests` exceeded 30 seconds. Ten focused checks after the first changes passed in 39.07 seconds: music selection 39.05, archive 19.50, audio 1.84, Jukebox 4.55, regex backends 1.69/2.09 seconds. These are focused measurements, not the final full-suite comparison. Both timing-sensitive audio tests also passed eight consecutive runs (16 executions total). Compile-command comparison found exactly one change: the intended test renderer source gains `-O2`; the application and all other commands remain unchanged. All 53 music unittest names remain, and all four fixture bodies before `main` are byte-identical. Independent review of these changes found no issues.

Ledger integration: both real filtered audits automatically ran their 23 native prerequisites; all 26 selected checks (including the helper) passed in 15.41 seconds. Eight helper regressions passed, including failed-producer/stale-proof handling and filtered CTest scheduling. Review caught optional-target configuration rejection; fixed by explicitly disabling only the unavailable complete audit before installing dependencies. A real Lua-disabled configuration now succeeds. Normal configuration retains both enabled audits with 7/21 required owner fixtures. Inventory is 409: removed the duplicated paging registration and added the evidence fixture regression suite. A second independent review found no remaining issues.

## Final measurements (macOS arm64 Debug, 2026-09-30)

The all-target build passed. Baseline and first final runs used:

```sh
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

The baseline finished in **364.72 seconds**, with 408/409 passing and the audio renderer timing out at 30 seconds. The first final run passed **409/409 in 173.78 seconds**, a **52.4% wall-time reduction (2.10× faster)**. A second full run added `--schedule-random` and passed **409/409 in 69.98 seconds**. The second run benefits from warm executables/caches and a different schedule; it is a stability check, not a directly equivalent benchmark. Per-test parallel timings also reflect contention.

| Check | Baseline (s) | Final (s) | Randomized repeat (s) |
| --- | ---: | ---: | ---: |
| Music-selection behavior | 341.17 | 53.61 | 37.28 |
| Music-select ledger audit | 79.48 | 0.09 | 0.07 |
| Archive concurrency | 74.66 | 16.61 | 17.47 |
| Chart audio renderer | 30.06 (timeout) | 3.29 | 1.13 |
| Jukebox restore | 27.40 | 5.76 | 2.80 |
| Regex oracle (Lua backend) | 15.61 | 1.76 | 1.13 |
| Regex oracle (foundation backend) | 15.22 | 1.67 | 1.49 |

Coverage and limits:

- Removed one duplicate paging registration; its three behavioral cases still execute in the default repository runner. Added the ledger-fixture regression suite, leaving 409 CTests.
- Ledger audits now consume proof from the same invocation's 23 native setup owners, removing 28 repeated executions across the two audits. Failed owners block their audit; missing, malformed, or stale evidence fails. Standalone audits still run the native owners themselves. Filtered CTest audit selection automatically includes prerequisites.
- All 53 music-selection unittest methods and all 115 regex cases per backend remain. Shared fixture compilation retains a new process and working directory for each case. Removed one redundant compilation of a hand-written negative-control implementation, retaining the actual production behavior regression.
- Kept the complete 317,608,200-frame audio stress workload, assertions, and 30-second timeout. Audio renderer and Jukebox tests each passed eight consecutive focused runs, then both full runs.
- Kept the 513 MiB RAR5 production-threshold test and byte-for-byte validation. Reduced payloads only where the test injects its own budget or needs multiple cancellation chunks. Other compressed Jukebox fixtures remain.
- Comparing all 3,274 compile commands found exactly one change: `-O2` for the renderer source in its Debug test target. Application compilation is unchanged. This optimization respects `ENABLE_DEBUG_OPTIMIZED_BUILD` and is not applied to MSVC or Xcode; their timing behavior was not measured.
- A real Lua-disabled CMake configuration succeeded. Unavailable complete ledger audits are explicitly disabled in that configuration; the normal measured configuration enables both. Windows/mobile runtime execution was not part of this check.

Independent review found no remaining issues after correcting optional-owner configuration handling. `git diff --check` passed.
