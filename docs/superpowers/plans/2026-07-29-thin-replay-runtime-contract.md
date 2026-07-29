# Thin Replay Runtime Contract Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make locally produced BRDs reliably writable and playable while retaining only file-safety, structural, and ownership checks as runtime gates.

**Architecture:** Canonicalize raw input once at the producer boundary, treat the supported Aso extension as authoritative, and separate replay playability from exact result agreement. Preserve immutable result facts while permitting a one-way attachment recovery for exact retries.

**Tech Stack:** C++20, SQLite, nlohmann JSON, CMake/CTest, existing replay contract test executables.

## Global Constraints

- Keep bounded parsing, canonical paths, file hash/size checks, and chart identity checks.
- Do not reconstruct legacy replay detail or change legacy summary capabilities.
- Do not write event, touch, lane-cover, or course-stage playback rows to SQLite.
- Use TDD for every behavior change and commit independently reviewable boundaries.
- Do not deploy Firebase, TestFlight, or Google Play.

---

### Task 1: Canonical producer input normalization

**Files:**
- Modify: `src/replay/ReplayInputRecorder.h`
- Modify: `src/replay/ReplayInputRecorder.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Test: `tests/replay_input_recorder_tests.cpp`
- Test: `tests/chart_replay_persistence_tests.cpp`

**Interfaces:**
- Produces: `normalizeReplayInput(std::span<const InputTransition>, ReplayTimeBounds, std::string &, ReplayLimits) -> std::optional<std::vector<InputTransition>>`
- Consumes: `ReplayInputRecorder::finish` and `GamePlayScene::completeModernReplayCapture`

- [ ] **Step 1: Write failing normalization tests**

```cpp
require(recorder.recordSongTime(10, lane(), true, diagnostic), "press accepted");
require(recorder.recordSongTime(9, lane(), true, diagnostic), "late duplicate accepted");
require(recorder.recordSongTime(11, lane(), false, diagnostic), "release accepted");
const auto result = recorder.finish({.completionSongTimeMicros = 20}, diagnostic);
require(result == std::vector<InputTransition>{{9, lane(), true},
                                               {11, lane(), false}},
        "input is stable-ordered and redundant state is removed");
```

- [ ] **Step 2: Run the focused test and confirm the current fail-closed behavior fails it**

Run: `cmake --build cmake-build-debug --target replay_input_recorder_tests -j 6 && ctest --test-dir cmake-build-debug -R '^replay_input_recorder_tests$' --output-on-failure`

Expected: FAIL because decreasing time or the duplicate press invalidates the attachment.

- [ ] **Step 3: Implement one shared normalizer and route both capture paths through it**

```cpp
std::optional<std::vector<InputTransition>> normalizeReplayInput(
    std::span<const InputTransition> input, ReplayTimeBounds bounds,
    std::string &diagnostic, const ReplayLimits &limits = kReplayLimits);
```

The implementation stable-sorts by `songTimeMicros`, validates structural
controls/bounds/counts, and drops transitions that do not change logical state.
`ReplayInputRecorder` accumulates observer events and normalizes on `finish`.
Realtime completion normalizes `copyAcceptedReplayInputAfterStop()` through the
same function.

- [ ] **Step 4: Run recorder, realtime, playback, and chart-capture tests**

Run: `cmake --build cmake-build-debug --target replay_input_recorder_tests realtime_gameplay_worker_tests replay_playback_tests chart_replay_persistence_tests -j 6 && ctest --test-dir cmake-build-debug -R '^(replay_input_recorder_tests|realtime_gameplay_worker_tests|replay_playback_tests|chart_replay_persistence_tests)$' --output-on-failure`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/replay/ReplayInputRecorder.h src/replay/ReplayInputRecorder.cpp src/scene/play/GamePlayScene.cpp tests/replay_input_recorder_tests.cpp tests/chart_replay_persistence_tests.cpp
git commit -m "fix: normalize replay capture input"
```

### Task 2: Make Aso extension projection mismatches non-blocking

**Files:**
- Modify: `src/replay/BeatorajaReplayCodec.cpp`
- Test: `tests/beatoraja_replay_codec_tests.cpp`

**Interfaces:**
- Consumes: existing `BeatorajaReplayCodec::decode`
- Produces: supported Aso extension playback even when stock compatibility fields differ

- [ ] **Step 1: Change focused tests to require the supported Aso extension to remain authoritative**

```cpp
Json mismatch = outerJson(*encoded);
mismatch["doubleoption"] = 1;
const auto decoded = codec.decode(encodeJson(mismatch), context(source));
expect(decoded.chart.has_value() &&
           decoded.chart->playback.setup.doublePlayOption ==
               source.playback.setup.doublePlayOption,
       "supported Aso extension remains authoritative");
```

- [ ] **Step 2: Run the codec test and confirm it fails at stock/extension agreement**

Run: `cmake --build cmake-build-debug --target beatoraja_replay_codec_tests -j 6 && ctest --test-dir cmake-build-debug -R '^beatoraja_replay_codec_tests$' --output-on-failure`

Expected: FAIL because `stockProjectionAgrees` rejects the document.

- [ ] **Step 3: Remove runtime projection equality as a decode gate**

Decode and structurally validate both sections, but use the supported Aso
extension for Aso playback. Retain generated-file compatibility assertions in
the codec tests.

- [ ] **Step 4: Run codec and file-contract tests**

Run: `cmake --build cmake-build-debug --target beatoraja_replay_codec_tests replay_codec_store_contract_tests -j 6 && ctest --test-dir cmake-build-debug -R '^(beatoraja_replay_codec_tests|replay_codec_store_contract_tests)$' --output-on-failure`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/replay/BeatorajaReplayCodec.cpp tests/beatoraja_replay_codec_tests.cpp
git commit -m "fix: trust supported replay extension"
```

### Task 3: Separate replay playability from result agreement

**Files:**
- Modify: `src/replay/ReplayPlaybackMaterializer.h`
- Modify: `src/replay/ReplayPlaybackMaterializer.cpp`
- Modify: `src/replay/ChartReplayConsumer.cpp`
- Modify: `src/replay/CourseReplayConsumer.cpp`
- Test: `tests/replay_playback_driver_tests.cpp`
- Test: `tests/course_replay_consumer_tests.cpp`

**Interfaces:**
- Produces: `ReplayPlaybackMaterializationOutcome::playable() const noexcept`
- Consumes: chart and course replay consumers

- [ ] **Step 1: Add failing tests for playable result mismatch**

```cpp
auto outcome = ReplayPlaybackMaterializer::materializeForConsumers(
    document, ReplaySetupSource::AsoExtension, mismatchedSavedResult, chart);
expect(outcome.state == ReplayPlaybackMaterializationState::ResultMismatch &&
           outcome.playable() && outcome.replayData,
       "result disagreement is diagnostic but playback remains available");
```

- [ ] **Step 2: Run the focused materializer/consumer tests and confirm failure**

Run: `cmake --build cmake-build-debug --target replay_playback_driver_tests course_replay_consumer_tests -j 6 && ctest --test-dir cmake-build-debug -R '^(replay_playback_driver_tests|course_replay_consumer_tests)$' --output-on-failure`

Expected: FAIL because result mismatch returns before compatibility replay data is constructed.

- [ ] **Step 3: Construct replay data for matched and result-mismatched simulations**

```cpp
[[nodiscard]] bool playable() const noexcept {
  return (state == ReplayPlaybackMaterializationState::Matched ||
          state == ReplayPlaybackMaterializationState::ResultMismatch) &&
         replayData != nullptr;
}
```

Consumers accept `playable()` and carry `agreement.diagnostic` as a warning.
Structural, setup, chart identity, resource, and simulation-execution failures
remain blocking.

- [ ] **Step 4: Run all chart/course context, consumer, and materializer tests**

Run: `ctest --test-dir cmake-build-debug -R '(chart_replay|course_replay|replay_playback)' --output-on-failure`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/replay/ReplayPlaybackMaterializer.h src/replay/ReplayPlaybackMaterializer.cpp src/replay/ChartReplayConsumer.cpp src/replay/CourseReplayConsumer.cpp tests
git commit -m "fix: make replay result agreement diagnostic"
```

### Task 4: Recover replay attachment on exact result retry

**Files:**
- Modify: `src/replay/ChartReplayPersistence.cpp`
- Modify: `src/replay/CourseReplayPersistence.cpp`
- Modify: `src/repositories/ReplayRepositoryModernResults.cpp`
- Test: `tests/chart_replay_persistence_tests.cpp`
- Test: `tests/replay_repository_tests.cpp`

**Interfaces:**
- Consumes: `ReplayRepository::StageModernChartResult` and `StageModernCourseResult`
- Produces: one-way absent-to-present replay attachment transition for an otherwise exact stored result

- [ ] **Step 1: Add failing persistence and repository tests**

```cpp
// First save: exact result, no replay reference.
// Retry: same result/snapshot/drafts with a valid reserved attachment.
// Expect AlreadyStaged/SavedWithReplay and the stored record to own the file.
```

Also assert that a present attachment cannot be replaced and that a differing
result/snapshot/draft remains an integrity conflict.

- [ ] **Step 2: Run focused persistence/repository tests and confirm failure**

Run: `cmake --build cmake-build-debug --target chart_replay_persistence_tests replay_repository_tests -j 6 && ctest --test-dir cmake-build-debug -R '^(chart_replay_persistence_tests|replay_repository_tests)$' --output-on-failure`

Expected: FAIL because persistence suppresses the retry and repository staging requires attachment presence equality.

- [ ] **Step 3: Implement the one-way atomic attachment transition**

Remove `suppressNewReplay`. In the existing-result transaction, permit only
`stored replay absent && requested replay present`; verify the reservation and
ownership, insert `modern_replay_files`, delete the exact reservation, re-read
the record, and commit. Keep all other payloads immutable.

- [ ] **Step 4: Run persistence, repository, lifecycle, and file-association tests**

Run: `ctest --test-dir cmake-build-debug -R '(chart_replay_persistence|course_replay_persistence|replay_repository|replay_file_association|replay_file_lifecycle)' --output-on-failure`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/replay/ChartReplayPersistence.cpp src/replay/CourseReplayPersistence.cpp src/repositories/ReplayRepositoryModernResults.cpp tests/chart_replay_persistence_tests.cpp tests/replay_repository_tests.cpp
git commit -m "fix: recover missing replay attachments"
```

### Task 5: Preserve precise consumer diagnostics

**Files:**
- Modify: `src/scene/MainMenuScene.cpp`
- Test: `tests/replay_contract_boundary_tests.cpp`

**Interfaces:**
- Consumes: `ChartReplayConsumerOutcome::diagnostic` and course equivalent
- Produces: user-visible/export diagnostic and SDL log for failed replay loading

- [ ] **Step 1: Add a failing source-contract or focused presentation test**

```cpp
expect(!source.contains("message = \"No Replay\""),
       "video export preserves the replay consumer diagnostic");
```

- [ ] **Step 2: Run the focused test and confirm failure**

Run: `cmake --build cmake-build-debug --target replay_contract_boundary_tests -j 6 && ctest --test-dir cmake-build-debug -R '^replay_contract_boundary_tests$' --output-on-failure`

Expected: FAIL because Watch discards the diagnostic and export returns `No Replay`.

- [ ] **Step 3: Propagate diagnostics**

Log `loaded.diagnostic` for Watch/G-Battle and return it from video export,
falling back to `Replay is unavailable` only when empty.

- [ ] **Step 4: Run focused UI contract tests**

Run: `ctest --test-dir cmake-build-debug -R '^(replay_contract_boundary_tests|result_record_summary_tests|result_record_list_view_tests)$' --output-on-failure`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/scene/MainMenuScene.cpp tests
git commit -m "fix: expose replay load diagnostics"
```

### Task 6: Full verification and publication

**Files:**
- Verify: all modified files and branch diff

**Interfaces:**
- Consumes: completed Tasks 1-5
- Produces: verified PR update

- [ ] **Step 1: Run all focused replay, migration, file-state, and integration tests**

Run: `ctest --test-dir cmake-build-debug -R '(replay|modern_result|migration|result_record)' --output-on-failure`

- [ ] **Step 2: Run the full configured CTest suite**

Run: `ctest --test-dir cmake-build-debug --output-on-failure`

- [ ] **Step 3: Build desktop main**

Run: `cmake --build cmake-build-debug --target main -j 6`

- [ ] **Step 4: Run non-deploying iOS compile verification**

Run: `scripts/ios_firebase_deploy.sh --build-only`

- [ ] **Step 5: Review the complete diff against `origin/develop` and push**

Run: `git diff --check && git diff --stat origin/develop...HEAD && git status --short && git push origin feature/file-based-replays-v2`

Expected: no whitespace errors, only intended worktree changes, push succeeds.
