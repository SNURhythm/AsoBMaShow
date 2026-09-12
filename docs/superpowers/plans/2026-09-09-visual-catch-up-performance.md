# Visual Catch-up Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove decoder contention during historical visual catch-up while preserving every visual command and final frame behavior.

**Architecture:** A scoped, nestable VideoPlayer admission gate quiesces decoding without retaining the video mutex. Jukebox gates affected players for multi-event catch-up and releases them after its existing ordered commands. Normal zero/single-event progression remains cheap.

**Tech Stack:** C++23, FFmpeg, bgfx, SDL, CMake/CTest; existing Python fixture generation.

**Spec:** `docs/superpowers/specs/2026-09-09-visual-catch-up-performance-design.md`

## Global Constraints

- Work on the existing checkout and branch. No commits, pushes, worktrees, or distribution.
- Preserve every ordered seek/play command, including unsuccessful seeks; do not coalesce events.
- Keep audio authority and eager visual readiness; no event-time file/archive I/O or decoder initialization.
- Keep application suspension independent of batching; nested scopes and exception unwinding must release correctly.
- Use actual decoder and Jukebox behavior in tests, with test-only boundary instrumentation where needed.
- No dependencies, parser changes, whole-file formatting, or production-only-for-test accessors.
- Build in `cmake-build-debug` with `-j 6`; full CTest also uses `-j 6`.
- The user waived further approval checkpoints; resolve ordinary design details autonomously and document material deviations.

---

### Task 1: Scoped decoder admission and Jukebox integration

**Files:**
- Modify: `src/video/VideoPlayer.h`, `src/video/VideoPlayer.cpp` (nestable admission gate and worker checks).
- Modify: `src/audio/Jukebox.h`, `src/audio/Jukebox.cpp` (multi-event catch-up scopes and locked activation helper).
- Modify: `CMakeLists.txt` (focused behavior-test registration).
- Create: `tests/visual_catch_up_tests.cpp`, `tests/visual_catch_up_video_fixture.h`, `tests/visual_catch_up_video_fixture.cpp` (real decoder boundary observation and catch-up tests).
- Reuse or narrowly extend: `tests/skin_movie_clock_tests.py` and its existing marker generation, or add `tests/visual_catch_up_tests.py` if a separate harness keeps responsibilities clearer.

**Interfaces:**
- Consumes: `VideoPlayer::seek(int64_t)`, `playFrom(int64_t)`, `setDecodeSuspended(bool)` and Jukebox's existing catch-up entry points.
- Produces: a public production-used `VideoPlayer::DecodeBatch` RAII guard constructed from `VideoPlayer&`, non-copyable with a noexcept move constructor if stored in a vector; destructor releases its own nesting contribution. No move assignment is required.
- Produces: private `Jukebox::activateVisualAtLocked(int visualId, long long elapsedMicros)`, requiring `videoPlayerTableMutex` to be held. Existing activation delegates while acquiring that lock.
- A private shared catch-up helper may remove duplicated loops if it preserves reset/stopwatch semantics. Avoid unrelated class extraction.

- [x] **Step 1: Add behavioral regression and run RED.**

Build a dedicated test from existing Jukebox test support and a fixture translation unit that includes production VideoPlayer.cpp after wrapping FFmpeg boundaries. A test-only wrapper around actual wait predicates is also allowed to observe a positive rejected-admission witness; it must preserve the real predicate and condition-variable behavior. Keep decoder behavior real. At the boundary, record seek positions, flushes, decoder actions, and inject exactly one selected seek failure. The behavioral test must fail if workers are admitted during a multi-event catch-up; synchronization/barriers must make this deterministic, not a scheduling lottery. No wall-clock performance assertion belongs in CTest.

The independent expected command sequence for a 1,000,000 µs catch-up is:

```cpp
const std::vector<std::int64_t> expectedSeekMicros{
    900'000, 700'000, 500'000, 800'000, 200'000};
```

Use base events at 100,000/300,000/500,000 and layer events at 200,000/800,000 µs; they deliberately preserve base-before-layer rather than global chronological ordering. Include shared IDs and a missing ID separately. Record the expected failing assertion and exact build/run commands before writing production behavior.

- [x] **Step 2: Implement the smallest gate and integration.**

The gate's decision must compose two independent inputs:

```cpp
const bool canDecode = !decodeSuspended.load(std::memory_order_acquire) &&
                       decodeBatchDepth.load(std::memory_order_acquire) == 0;
```

Use that condition in the worker's output wait predicate and repeat it under `videoMutex` immediately before decoder work. Entering a batch synchronizes through `videoMutex` to finish any previously admitted action; it releases the mutex before any caller invokes `seek`. The final guard release notifies the existing worker condition variables without toggling `decodeSuspended`.

For Jukebox, determine due range ends first. If fewer than two events are due, keep the existing direct activation path. Otherwise collect distinct existing video IDs from those ranges, keep `videoPlayerTableMutex` locked for guard lifetime, create one guard per affected player, then run the original base/layer loops through the locked helper. Missing assets still do not change the current ID. Guard containers must unwind before releasing the table lock; reserve before acquiring guards where practical.

- [x] **Step 3: Run GREEN and extend boundary coverage.**

Assert the literal seek sequence, injected failure behavior, no decoder action inside the scoped batch, and eventual real marker-frame decoding after release. Add nested scope and independent suspension tests, missing/image fallback, same-file separate IDs, shared base/layer ID, backward/rapid seek, EOF/shutdown, and zero/single-event coverage. Failures are addressed at their root cause. Run focused existing Jukebox and skin movie clock tests as well.

```sh
cmake --build cmake-build-debug --target visual_catch_up_tests jukebox_restore_tests skin_movie_clock_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -j 6 -R 'visual_catch_up|foundation_av_jukebox_restore|skin_movie_clock|video_decode_state'
git diff --check
```

- [x] **Step 4: Self-review and independent task review.**

Write the implementation report with RED/GREEN evidence, complete changed-file list, actual lock order, scope lifetime and failure semantics. Freeze source for the controller's review. Do not commit. The controller packages the entire delta including new test files and obtains explicit spec-compliance and quality verdicts before final validation.

### Task 2: Measure and validate the converged change

**Files:**
- Create: `docs/reviews/2026-09-09-visual-catch-up-performance.md` (reproducible workload, results, limitations, review and validation).
- Update: this plan's checkboxes and status as work completes.
- Generated evidence: this plan's ignored coordination directory; preserve the prior baseline probe directory.

**Interfaces:**
- Consumes the frozen Task 1 code and existing generated real-FFmpeg probes under `.superpowers/sdd/2026-09-09-performance`.
- Produces a before/after table and final verification record. No production behavior changes in this task without returning to regression-first correction and review.

- [x] **Step 1: Rebuild the scratch probes against converged production objects.**

The baseline executable and raw samples remain intact. Use the probe builder's compile-command/link-command replacement mechanism to create distinct after binaries without overwriting baseline binaries. Run sequentially with the same 30-second clip, target 20,000,000 µs, four IDs, and 4/100/1,000 events. Use eight samples and discard the first from each run. Confirm every intended seek remains, every seek succeeds, and retain the uninstrumented control.

- [x] **Step 2: Check measurable acceptance.**

Compute medians directly from JSONL `seekMicros` values with `sample > 0`; show minimum/maximum and command counts separately. Accept a 1,000-event median at most 321.975 ms and a four-event median no more than 1 ms above its matching baseline. Investigate a miss; do not silently reduce scope or present an unmeasured speedup.

- [x] **Step 3: Run converged verification and final review.**

```sh
cmake --build cmake-build-debug -j 6
ctest --test-dir cmake-build-debug --output-on-failure -j 6
IOS_RELEASE_BUILD_JOBS=6 scripts/ios_release_verify.sh
CMAKE_BUILD_PARALLEL_LEVEL=6 scripts/android_firebase_deploy.sh --build-only
git diff --check
```

Serialize native builds/benchmarks to avoid memory pressure and invalid timing. Run the desktop `main` target as part of the all-target build. Obtain a final independent review of the complete task delta, not the unrelated preceding PR work. Preserve unrelated untracked review inputs.

- [x] **Step 4: Publish the local report and finish.**

Write exact measured results and validation outcomes to the report; distinguish synthetic debug-build measurements from mobile/gameplay performance. Keep changes uncommitted, do not upload builds, and do not ask the absent user for routine approval or integration choices.
