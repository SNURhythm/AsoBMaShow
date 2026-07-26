# PR #82 Fifth Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the three actionable review defects added against PR #82 commit `0ee381ac` without changing replay files, database schemas, or deferred slot relocation.

**Architecture:** Align the course-result action gate with the existing dual raw/legacy launcher, expose the combo seed already supported by `GameplaySimulation` through the raw replay materializer, and enforce the existing replay-directory durability barrier before validating any pre-existing final file. Each behavior is covered by a focused red/green regression before the production change.

**Tech Stack:** C++20, CMake/CTest, SQLite-independent replay storage, Xcode iOS build-only verification.

## Global Constraints

- Do not change the BRD codec format, Aso extension, replay path grammar, or database schema.
- Do not modify Beatoraja slot relocation, slot-copy selection UI, or its deferred review threads.
- Keep legacy judged replay playback isolated from raw replay materialization.
- Do not reply to or resolve GitHub review threads without explicit authorization.
- Use `scripts/ios_firebase_deploy.sh --build-only`; do not upload a build.

---

### Task 1: Raw Course Replay Action Gate

**Files:**
- Modify: `src/scene/ResultCoursePersistence.h`
- Modify: `src/scene/ResultScene.cpp:1731-1782`
- Test: `tests/remote_result_scene_tests.cpp`

**Interfaces:**
- Consumes: `CoursePlaySession::hasCourseReplayStage(std::size_t) const`.
- Produces: `result_scene_detail::courseReplayActionAvailable(const CoursePlaySession &) noexcept`.

- [ ] **Step 1: Write the failing action-availability regression**

Add a test that exercises empty, raw, and legacy session representations:

```cpp
void testCourseReplayActionAcceptsRawAndLegacyData() {
  CoursePlaySession session;
  require(!result_scene_detail::courseReplayActionAvailable(session),
          "empty course sessions hide replay");

  session.courseReplayPlaybackData =
      std::make_shared<replay::CourseReplayPlaybackData>();
  session.courseReplayPlaybackData->stages.emplace_back();
  require(result_scene_detail::courseReplayActionAvailable(session),
          "raw course replay data exposes replay");

  session.courseReplayPlaybackData.reset();
  session.courseReplayData = std::make_shared<JudgedCoursePlaybackData>();
  session.courseReplayData->stages.emplace_back();
  require(result_scene_detail::courseReplayActionAvailable(session),
          "legacy course replay data still exposes replay");
}
```

Register the function in `main()`.

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target remote_result_scene_tests -j 6
```

Expected: compilation fails because `courseReplayActionAvailable` does not yet exist.

- [ ] **Step 3: Implement the shared predicate and use it in the UI gate**

Add to `ResultCoursePersistence.h`:

```cpp
[[nodiscard]] inline bool
courseReplayActionAvailable(const CoursePlaySession &session) noexcept {
  return session.hasCourseReplayStage(0);
}
```

Change the final-course condition in `ResultScene::addCourseButtons()` to:

```cpp
if (isCourseFinalResult() && courseOptions.session != nullptr &&
    result_scene_detail::courseReplayActionAvailable(*courseOptions.session)) {
```

- [ ] **Step 4: Run the focused test and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target remote_result_scene_tests -j 6
ctest --test-dir cmake-build-debug -R '^remote_result_scene_tests$' --output-on-failure
```

Expected: build and test pass.

- [ ] **Step 5: Commit the action-gate fix**

```bash
git add src/scene/ResultCoursePersistence.h src/scene/ResultScene.cpp tests/remote_result_scene_tests.cpp
git commit -m "fix: expose raw course replay action"
```

---

### Task 2: Sequential Course Materialization Combo State

**Files:**
- Modify: `src/replay/ReplayPlaybackMaterializer.h`
- Modify: `src/replay/ReplayPlaybackMaterializer.cpp`
- Modify: `src/ReplayVideoExporter.cpp:4220-4290`
- Test: `tests/replay_playback_driver_tests.cpp`

**Interfaces:**
- Produces: `replay::ReplayMaterializationSeed { int carriedCombo; int carriedMaxCombo; }`.
- Extends: `materializeReplay(const ReplayPlaybackData &, const bms_parser::Chart &, const gameplay::GameplayRulesetPolicy &, ReplayMaterializationSeed = {})`.
- Consumes: `MaterializedReplay::attempt.combo` and `MaterializedReplay::attempt.maxCombo` as the next stage seed.

- [ ] **Step 1: Write the failing sequential materialization regression**

Extract or reuse a one-note chart/playback fixture, materialize it once, then request a second materialization seeded by the first result:

```cpp
const auto first = replay::materializeReplay(playback, chart, policy);
expect(first.materialized(), "first course stage materializes");
if (!first.value) {
  return;
}
const auto second = replay::materializeReplay(
    playback, chart, policy,
    {.carriedCombo = first.value->attempt.combo,
     .carriedMaxCombo = first.value->attempt.maxCombo});
expect(second.materialized() && !second.value->judgedEvents.empty() &&
           second.value->judgedEvents.front().combo == 2 &&
           second.value->attempt.combo == 2 &&
           second.value->attempt.maxCombo == 2,
       "second course stage continues combo and maximum");
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target replay_playback_driver_tests -j 6
```

Expected: compilation fails because `materializeReplay` does not accept the seed argument.

- [ ] **Step 3: Expose and apply the materialization seed**

Add to `ReplayPlaybackMaterializer.h`:

```cpp
struct ReplayMaterializationSeed {
  int carriedCombo = 0;
  int carriedMaxCombo = 0;
};
```

Default the new final argument so all chart-only callers remain source-compatible. In `ReplayPlaybackMaterializer.cpp`, normalize it before constructing the simulation:

```cpp
const int carriedCombo = std::max(0, seed.carriedCombo);
const int carriedMaxCombo =
    std::max(carriedCombo, seed.carriedMaxCombo);
```

Assign those values to `GameplayAttemptOptions::carriedCombo` and
`carriedMaxCombo`.

- [ ] **Step 4: Carry the seed through raw course video stages**

Create one `ReplayMaterializationSeed materializationSeed;` before the stage
loop. Pass it to each raw `materializeReplay()` call, then update it only after
a successful raw materialization:

```cpp
materializationSeed = {
    .carriedCombo = materialized.value->attempt.combo,
    .carriedMaxCombo = materialized.value->attempt.maxCombo,
};
```

- [ ] **Step 5: Run the focused test and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target replay_playback_driver_tests main -j 6
ctest --test-dir cmake-build-debug -R '^replay_playback_driver_tests$' --output-on-failure
```

Expected: build and test pass.

- [ ] **Step 6: Commit the combo-carry fix**

```bash
git add src/replay/ReplayPlaybackMaterializer.h src/replay/ReplayPlaybackMaterializer.cpp src/ReplayVideoExporter.cpp tests/replay_playback_driver_tests.cpp
git commit -m "fix: carry combo through course replay export"
```

---

### Task 3: Existing Replay Retry Durability

**Files:**
- Modify: `src/replay/ReplayFileStore.cpp:350-510`
- Test: `tests/replay_file_store_tests.cpp:240-280`

**Interfaces:**
- Consumes: `atomic_file::syncDirectory`, `validateFinal`, and the existing `directory-sync` fault-injection key.
- Produces: a private `syncAndValidateExistingFinal(...) -> FinalizeOutcome` helper.

- [ ] **Step 1: Strengthen the post-rename retry regression**

After the first `directory-sync` fault installs a reusable final file, retry
with a store that injects the same fault:

```cpp
replay::ReplayFileStore stillFaulty(
    profile.path,
    {.failAt = [](std::string_view point) {
      return point == "directory-sync";
    }});
const auto blockedRetry = stillFaulty.finalize(
    chartPath(), encoded, codec, chartIdentity(), "blocked_retry");
expect(!blockedRetry.metadata,
       "existing replay retry still requires directory durability");
```

Then keep the existing fault-free retry and require it to succeed as an
identical existing file.

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target replay_file_store_tests -j 6
ctest --test-dir cmake-build-debug -R '^replay_file_store_tests$' --output-on-failure
```

Expected: the new assertion fails because the existing-file branch skips the
injected directory-sync point and returns metadata.

- [ ] **Step 3: Add the sync-before-validation helper**

Add a private helper beside `validateFinal`:

```cpp
FinalizeOutcome syncAndValidateExistingFinal(
    const std::filesystem::path &finalPath,
    const std::filesystem::path &relativePath,
    std::span<const std::byte> expectedBytes,
    const BeatorajaReplayCodec &codec,
    const ExpectedReplayIdentity &expected,
    const ReplayFileStoreFaults &faults) {
  FinalizeOutcome outcome;
  if (failAt(faults, "directory-sync")) {
    outcome.diagnostic = "Injected replay directory-sync failure";
    return outcome;
  }
  if (!atomic_file::syncDirectory(finalPath.parent_path(),
                                  outcome.diagnostic)) {
    return outcome;
  }
  return validateFinal(finalPath, relativePath, expectedBytes, codec,
                       expected, faults, true);
}
```

Use it in both the early pre-existing-final branch and the no-replace
`DestinationExists` branch.

- [ ] **Step 4: Run the focused test and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target replay_file_store_tests -j 6
ctest --test-dir cmake-build-debug -R '^replay_file_store_tests$' --output-on-failure
```

Expected: test passes; the fault-free retry still recognizes an identical
existing replay.

- [ ] **Step 5: Commit the durability fix**

```bash
git add src/replay/ReplayFileStore.cpp tests/replay_file_store_tests.cpp
git commit -m "fix: resync replay directory on finalize retry"
```

---

### Task 4: Verification and PR Update

**Files:**
- Verify only; no planned production changes.

**Interfaces:**
- Consumes: the three completed fixes.
- Produces: verified branch commits pushed to PR #82.

- [ ] **Step 1: Run focused regressions together**

```bash
cmake --build cmake-build-debug --target remote_result_scene_tests replay_playback_driver_tests replay_file_store_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(remote_result_scene_tests|replay_playback_driver_tests|replay_file_store_tests)$' --output-on-failure
```

Expected: 3/3 tests pass and `main` builds.

- [ ] **Step 2: Run the complete desktop suite**

```bash
ctest --test-dir cmake-build-debug -j 6 --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 3: Run the iOS compile check**

```bash
scripts/ios_firebase_deploy.sh --build-only
```

Expected: `** BUILD SUCCEEDED **`; no archive or upload occurs.

- [ ] **Step 4: Audit deferred scope and worktree cleanliness**

```bash
git diff 0ee381ac...HEAD -- src/replay/ReplayFileActionService.cpp
git diff --check
git status --short
```

Expected: no slot-relocation diff, no whitespace errors, and no uncommitted
files.

- [ ] **Step 5: Push and inspect PR state read-only**

```bash
git push origin feature/file-based-replays
gh pr checks 82 || true
```

Do not reply to or resolve review threads.
