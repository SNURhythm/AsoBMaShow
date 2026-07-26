# PR #82 Fourth Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the nine accepted replay review findings while keeping raw replay events file-based and preserving Beatoraja-compatible stock replay behavior.

**Architecture:** Extend the existing codec, migration, replay preparation, saved-course recall, logical input, summary repository, and result capability boundaries with focused helpers. Aso-only replay facts remain in the BRD extension; stock Beatoraja data receives a compatible projection. No slot-copy or slot-relocation UI work is included.

**Tech Stack:** C++23, nlohmann/json, zlib/gzip, SQLite, CMake/CTest, Xcode/iOS build scripts.

## Global Constraints

- Follow red-green-refactor for every behavior change: write the focused failing test, run it and confirm the expected failure, make the smallest production change, then rerun it.
- Preserve exact `ASSIGN:` options and unmodified logical input inside the AsoBMaShow extension.
- Do not restore raw replay events to SQLite or couple replay files to IR provenance.
- Do not implement Beatoraja replay-slot copy, slot selection, or occupied-slot relocation.
- Do not reply to or resolve GitHub review threads without separate user authorization.
- Do not run an upload or deployment command. The only iOS verification command is `scripts/ios_firebase_deploy.sh --build-only`.
- Preserve unrelated user changes in the worktree.

---

## Task 1: Encode manual lane assignments as a stock-compatible projection

**Files:**

- Modify: `tests/beatoraja_replay_codec_tests.cpp`
- Modify: `src/replay/BeatorajaReplayCodec.cpp`

- [ ] **Step 1: Add a failing manual-assignment codec test**

Add a 7-key replay fixture whose first option is `ASSIGN:S2134567`. Record transitions on assigned destination lane 0 and on the scratch destination. Assert all of the following:

- encoding succeeds;
- the outer stock JSON stores `randomoption == 0` (`NORMAL`);
- decoding the full BRD through `BeatorajaReplayCodec` restores the exact `ASSIGN:S2134567` setup and the original logical input;
- inflating the stock `keyinput` shows the assigned destination transitions remapped to the original source Beatoraja key codes; and
- the extension does not contain the remapped stock projection in place of the original input.

Add table cases proving malformed, duplicate, missing-lane, wrong-key-mode, and manual-assignment-on-unsupported-key-mode options are rejected.

- [ ] **Step 2: Run the codec test and confirm the current rejection**

```bash
cmake --build cmake-build-debug --target beatoraja_replay_codec_tests -j 6
ctest --test-dir cmake-build-debug -R '^beatoraja_replay_codec_tests$' --output-on-failure
```

Expected failure: encoding the valid `ASSIGN:` fixture returns no BRD because `optionIndex()` only accepts Beatoraja stock options.

- [ ] **Step 3: Implement the stock option and input projection**

In `BeatorajaReplayCodec.cpp`, add small internal helpers that:

1. recognize `ASSIGN:<notation>` independently for `playOption` and `playOption2`;
2. enumerate the standard physical lanes in assignment notation order for 5-, 7-, 10-, and 14-key modes;
3. validate exact notation length, legal symbols, and a one-to-one destination-to-source mapping;
4. map each assigned destination physical lane back to its original source physical lane;
5. translate remapped physical lanes back to stock logical lane/scratch controls; and
6. return stock option index `0` for a valid manual assignment while leaving ordinary stock options unchanged.

Apply remapping only inside `stockKeyRecords()`. Keep `encodeSetupExtension()` and `encodeInputExtension()` unchanged so they retain the exact Aso replay. Preserve Start and Select controls. When a source physical lane is scratch, use the existing clockwise stock scratch representation. Fail closed if a logical control cannot be mapped coherently.

Update `validateSetup()` and `encodeStage()` to accept validated manual assignments and write `NORMAL` to the corresponding stock option field.

- [ ] **Step 4: Run the focused tests and inspect the literal stock records**

```bash
cmake --build cmake-build-debug --target beatoraja_replay_codec_tests -j 6
ctest --test-dir cmake-build-debug -R '^beatoraja_replay_codec_tests$' --output-on-failure
```

Expected: valid manual assignment round-trips through the extension, its stock option is `NORMAL`, stock key codes target the original chart lanes, and invalid assignments fail.

- [ ] **Step 5: Commit the codec change**

```bash
git add src/replay/BeatorajaReplayCodec.cpp tests/beatoraja_replay_codec_tests.cpp
git commit -m "fix: project manual assignments into stock replays"
```

---

## Task 2: Preserve empty legacy replay input during migration

**Files:**

- Modify: `tests/replay_file_migration_tests.cpp`
- Modify: `src/repositories/ReplayRepositoryReplayFileMigration.cpp`

- [ ] **Step 1: Add a failing empty-input migration assertion**

Create or extend a schema-10 all-miss chart fixture with zero replay events. Migrate it, decode the resulting BRD, and assert both the decoded Aso input and the inflated stock key record stream are empty.

- [ ] **Step 2: Run the migration suite and confirm fake transitions appear**

```bash
cmake --build cmake-build-debug --target replay_file_migration_tests -j 6
ctest --test-dir cmake-build-debug -R '^replay_file_migration_tests$' --output-on-failure
```

Expected failure: the decoded replay contains the migration's synthetic lane-0 press/release pair.

- [ ] **Step 3: Remove synthetic event insertion**

Delete the `chart.playback.input.empty()` branch that inserts a press at timestamp 0 and a release at timestamp 1. Let the codec encode its already-supported empty stock stream.

- [ ] **Step 4: Re-run the migration and codec suites**

```bash
cmake --build cmake-build-debug --target replay_file_migration_tests beatoraja_replay_codec_tests -j 6
ctest --test-dir cmake-build-debug -R '^(replay_file_migration_tests|beatoraja_replay_codec_tests)$' --output-on-failure
```

- [ ] **Step 5: Commit the empty-input fix**

```bash
git add src/repositories/ReplayRepositoryReplayFileMigration.cpp tests/replay_file_migration_tests.cpp
git commit -m "fix: preserve empty legacy replay input"
```

---

## Task 3: Resolve coherent chart metadata for replay migration

**Files:**

- Modify: `src/repositories/ReplayRepositoryReplayFileMigration.h`
- Modify: `src/repositories/ReplayRepositoryReplayFileMigration.cpp`
- Modify: `src/repositories/ReplayRepositorySchema.cpp`
- Modify: `tests/replay_file_migration_tests.cpp`

- [ ] **Step 1: Add failing defined/undefined-LN migration tests**

Extend the chart-database fixture with:

- a chart with a positive long-note count and `ln_mode == 1`;
- a chart with a positive long-note count and `ln_mode == 0`;
- a chart with no long-note/backspin notes; and
- ambiguous identity rows whose metadata disagrees.

Assert the resolver returns both key mode and undefined-LN status for coherent matches, returns no value for ambiguous matches, and drives canonical paths as follows:

- defined-LN and no-LN charts receive no `C`/`H` prefix;
- undefined-LN charts receive the prefix dictated by the legacy replay LN mode; and
- course canonical naming aggregates the corrected per-stage undefined-LN facts.

- [ ] **Step 2: Run the migration suite and confirm all records are treated as undefined-LN**

```bash
cmake --build cmake-build-debug --target replay_file_migration_tests -j 6
ctest --test-dir cmake-build-debug -R '^replay_file_migration_tests$' --output-on-failure
```

Expected failure: the key-mode-only resolver cannot expose the LN fact and `buildPlayback()` forces `hasUndefinedLongNotes = true`.

- [ ] **Step 3: Replace the key-mode-only resolver contract**

In `ReplayRepositoryReplayFileMigration.h`, introduce:

```cpp
struct ReplayMigrationChartMetadata {
  int keyMode = 0;
  bool hasUndefinedLongNotes = false;
};

using ReplayMigrationChartMetadataResolver =
    std::function<std::optional<ReplayMigrationChartMetadata>(
        const ReplayMigrationChartIdentity &)>;
```

Rename `makeChartDatabaseReplayKeyModeResolver()` to `makeChartDatabaseReplayMetadataResolver()` and update `migrateReplayFilesFromLegacySchema()` to receive the new resolver type. Update the schema caller accordingly.

Change the chart-database query to fetch `keys`, `ln_mode`, `total_long_notes`, and `total_backspin_notes`. Define undefined long notes as:

```cpp
(total_long_notes + total_backspin_notes) > 0 && ln_mode == 0
```

Require all matching rows selected by the existing identity rules to agree on both returned fields. Return no metadata for unsupported keys, malformed values, database failures, or disagreement.

- [ ] **Step 4: Carry resolved metadata through chart and course construction**

Add `hasUndefinedLongNotes` to the legacy chart's resolved migration state. In `readCharts()`, use the resolver's pair when available. Otherwise retain the current event-based key-mode fallback and the conservative legacy undefined-LN fallback.

In `buildPlayback()`, copy the chart's resolved boolean into `ReplaySetup`. In `buildCourse()`, derive `pathInput.hasUndefinedLongNotes` from the referenced stages rather than assigning `true` unconditionally.

- [ ] **Step 5: Re-run migration and path suites**

```bash
cmake --build cmake-build-debug --target replay_file_migration_tests beatoraja_replay_path_tests -j 6
ctest --test-dir cmake-build-debug -R '^(replay_file_migration_tests|beatoraja_replay_path_tests)$' --output-on-failure
```

- [ ] **Step 6: Audit the resolver rename**

```bash
rg -n 'ReplayMigrationKeyModeResolver|makeChartDatabaseReplayKeyModeResolver|hasUndefinedLongNotes = true' src/repositories tests/replay_file_migration_tests.cpp
```

Expected: no old resolver names remain, and any remaining literal `true` is an intentional test fixture or documented fallback rather than unconditional resolved metadata.

- [ ] **Step 7: Commit migration metadata handling**

```bash
git add src/repositories/ReplayRepositoryReplayFileMigration.h src/repositories/ReplayRepositoryReplayFileMigration.cpp src/repositories/ReplayRepositorySchema.cpp tests/replay_file_migration_tests.cpp
git commit -m "fix: resolve replay migration chart metadata"
```

---

## Task 4: Reject replay playback against a different raw chart

**Files:**

- Modify: `src/PlayOptionUtils.h`
- Modify: `tests/result_recall_builder_tests.cpp`

- [ ] **Step 1: Add failing real-parser identity tests**

In `result_recall_builder_tests.cpp`, create a minimal temporary BMS chart, parse it to obtain its actual SHA-256 and MD5, and call the raw-path overload of `play_options::prepareReplayChart()` with:

1. the matching SHA-256/MD5 setup;
2. the legacy MD5-only fallback SHA-256 plus matching MD5; and
3. a mismatching SHA-256/MD5 setup.

Assert the first two calls return a prepared chart and the third returns null. Keep the replay option `NORMAL` so the test isolates identity validation.

- [ ] **Step 2: Run the recall suite and confirm mismatch is accepted**

```bash
cmake --build cmake-build-debug --target result_recall_builder_tests -j 6
ctest --test-dir cmake-build-debug -R '^result_recall_builder_tests$' --output-on-failure
```

Expected failure: the mismatched setup still returns the chart because the helper applies options immediately after parsing.

- [ ] **Step 3: Add the identity gate before replay options**

Include `replay/LegacyReplayIdentity.h` in `PlayOptionUtils.h`. After parsing and cancellation checks, compare replay setup identity to `chart->Meta.SHA256` and `chart->Meta.MD5` with `replay::storedChartIdentityMatches()`. Return null before calling `applyReplayPlayOptions()` when the identity is malformed or mismatched.

- [ ] **Step 4: Re-run the recall suite**

```bash
cmake --build cmake-build-debug --target result_recall_builder_tests -j 6
ctest --test-dir cmake-build-debug -R '^result_recall_builder_tests$' --output-on-failure
```

- [ ] **Step 5: Commit the playback identity gate**

```bash
git add src/PlayOptionUtils.h tests/result_recall_builder_tests.cpp
git commit -m "fix: validate chart identity before replay setup"
```

---

## Task 5: Restore persisted aggregate facts in saved-course recall

**Files:**

- Modify: `src/CoursePlaySession.h`
- Modify: `src/ResultRecallBuilder.cpp`
- Modify: `src/scene/result/ResultScene.cpp`
- Modify: `tests/result_recall_builder_tests.cpp`

- [ ] **Step 1: Add failing saved-course aggregate tests**

Build a valid recalled course result whose `finalGauge` differs from a freshly initialized gauge and whose persisted `clearType` cannot be inferred reliably from the completed-stage prefix. Assert the reconstructed session carries the exact final gauge and saved clear rank.

Build an incomplete saved course with `totalCharts == 3` and one persisted stage. Assert:

- `entries.size() == 3`;
- `completedResults.size() == 1`;
- only one chart is owned/parsed;
- placeholder entries cannot become recalled navigable stages; and
- the persisted final gauge and clear rank survive aggregate result presentation.

- [ ] **Step 2: Run the recall suite and confirm aggregate facts are absent**

```bash
cmake --build cmake-build-debug --target result_recall_builder_tests -j 6
ctest --test-dir cmake-build-debug -R '^result_recall_builder_tests$' --output-on-failure
```

Expected failure: `carriedGauge` is unset, `entries.size()` equals completed stages, and no saved clear-rank override exists.

- [ ] **Step 3: Add saved-result-only course state**

Add this saved-result-only optional field to `CoursePlaySession`:

```cpp
std::optional<int> recalledCourseClearTypeRank;
```

The field is populated only by saved-result reconstruction; live course sessions remain unset.

- [ ] **Step 4: Reconstruct final gauge, clear rank, and total entries**

In `ResultRecallBuilder::buildCourseResult()`:

- keep parsing only `result.stages` into owned charts and completed results;
- append default presentation-only `CoursePlayEntry` values until `entries.size()` equals `result.totalCharts`;
- create `carriedGauge` from the persisted course gauge configuration and `finalGauge`, ensuring the selected gauge's current value and stored gauge-value slot equal `finalGauge`; and
- assign `result.clearType` to `recalledCourseClearTypeRank`.

Reject internally inconsistent results rather than truncating when completed stages exceed `totalCharts`.

- [ ] **Step 5: Make saved aggregate presentation authoritative**

In `courseResultStateForSession()` in `ResultScene.cpp`, retain the live-session behavior when the optional clear rank is absent. For recalled saved results:

- do not force the final gauge to zero merely because presentation placeholders make the session incomplete; and
- after aggregating completed stages, restore the persisted clear rank through the existing read-only clear-type restoration API.

Continue to bound saved-stage navigation by `completedResults.size()` rather than `entries.size()`.

- [ ] **Step 6: Re-run recall and result-scene-adjacent suites**

```bash
cmake --build cmake-build-debug --target result_recall_builder_tests remote_result_scene_tests -j 6
ctest --test-dir cmake-build-debug -R '^(result_recall_builder_tests|remote_result_scene_tests)$' --output-on-failure
```

- [ ] **Step 7: Commit saved-course recall facts**

```bash
git add src/CoursePlaySession.h src/ResultRecallBuilder.cpp src/scene/result/ResultScene.cpp tests/result_recall_builder_tests.cpp
git commit -m "fix: restore saved course aggregate state"
```

---

## Task 6: Balance replay notifications across scratch ownership handoffs

**Files:**

- Modify: `src/input/LogicalGameplayInputAdapter.h`
- Modify: `src/input/LogicalGameplayInputAdapter.cpp`
- Modify: `tests/logical_gameplay_input_tests.cpp`

- [ ] **Step 1: Add failing overlap tests for both handoff directions**

Using the existing applied-transition recorder, cover these complete sequences:

1. counter-clockwise directional press, digital scratch-lane press, directional release, digital release;
2. digital scratch-lane press, counter-clockwise directional press, digital release, directional release.

For each sequence assert:

- the rhythm control receives one physical press at the start of the shared hold and one physical release at the end;
- no physical press/release pair is emitted merely for logical ownership handoff; and
- replay callbacks form balanced press/release pairs in order, including a counter-clockwise release before a clockwise digital press when ownership passes to the digital binding.

- [ ] **Step 2: Run the logical gameplay input suite and confirm the unmatched direction**

```bash
cmake --build cmake-build-debug --target logical_gameplay_input_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_input_gameplay$' --output-on-failure
```

Expected failure: one sequence records a counter-clockwise press followed by a clockwise release.

- [ ] **Step 3: Track and hand off the recorded logical scratch owner**

Extend the per-scratch-lane state with the minimum state needed to distinguish the physically held lane from the logical control currently reported to replay capture. Route scratch replay notifications through a helper that:

- does nothing if the effective logical owner is unchanged;
- emits release for the previous recorded owner;
- emits press for the new recorded owner at the same input transition; and
- updates recorded ownership only after callbacks are emitted.

Use this helper from both digital `applyLane()` transitions and directional scratch transitions. Preserve the existing physical reference-count/held-state behavior and scratch reversal ordering.

- [ ] **Step 4: Run all foundation input tests**

```bash
cmake --build cmake-build-debug --target logical_gameplay_input_tests input_binding_resolver_tests -j 6
ctest --test-dir cmake-build-debug -R '^(foundation_input_gameplay|foundation_input_resolver)$' --output-on-failure
```

- [ ] **Step 5: Commit balanced scratch handoffs**

```bash
git add src/input/LogicalGameplayInputAdapter.h src/input/LogicalGameplayInputAdapter.cpp tests/logical_gameplay_input_tests.cpp
git commit -m "fix: balance replay scratch ownership handoffs"
```

---

## Task 7: Apply compact-summary limits after validation

**Files:**

- Modify: `src/repositories/ReplayRepositoryResultRecords.cpp`
- Modify: `tests/result_persistence_v11_integration_tests.cpp`

- [ ] **Step 1: Add failing chart and course summary tests**

For chart results, persist at least three attempts for the same lookup, corrupt the newest row through direct SQLite mutation, then request two summaries. Assert the two older valid summaries are returned newest-first.

For course results, persist at least two attempts for the same course lookup, corrupt the newest aggregate row, then request one summary. Assert the next valid summary is returned.

Add a bounded-scan case with more corrupt chart candidates than `replay_summary_scan::kCorruptCandidateAllowance` before an older valid row. Assert a positive-limit query does not reach that row. Keep `limit <= 0` coverage proving the unbounded mode can scan all candidates.

- [ ] **Step 2: Run the persistence integration suite and confirm corrupt rows consume SQL LIMIT**

```bash
cmake --build cmake-build-debug --target result_persistence_integration_tests -j 6
ctest --test-dir cmake-build-debug -R '^result_persistence_integration_tests$' --output-on-failure
```

Expected failure: positive-limit chart/course queries return fewer valid summaries than requested because candidate IDs are limited before validation.

- [ ] **Step 3: Implement bounded keyset candidate scanning**

In both `ListCompactChartResultsOnConnection()` and `ListCompactCourseResultsOnConnection()`:

- remove the requested result limit from the single initial candidate query;
- fetch IDs in `replay_summary_scan::kChunkSize` pages ordered by `played_at_unix_ms DESC, id DESC`;
- use the last timestamp/id as a keyset cursor for the next page;
- validate candidates with the existing detail readers;
- append only valid compact summaries;
- stop after collecting the requested positive limit;
- for positive limits, stop after inspecting at most `limit + kCorruptCandidateAllowance` candidates; and
- retain unlimited scanning for `limit <= 0`.

Keep the read lock/connection scope and current fail-closed SQLite stepping behavior. Do not use OFFSET pagination.

- [ ] **Step 4: Re-run persistence and summary suites**

```bash
cmake --build cmake-build-debug --target result_persistence_integration_tests replay_summary_list_tests -j 6
ctest --test-dir cmake-build-debug -R '^(result_persistence_integration_tests|replay_summary_list_tests)$' --output-on-failure
```

- [ ] **Step 5: Commit validated summary scanning**

```bash
git add src/repositories/ReplayRepositoryResultRecords.cpp tests/result_persistence_v11_integration_tests.cpp
git commit -m "fix: limit replay summaries after validation"
```

---

## Task 8: Expose deletion for corrupt regular replay files

**Files:**

- Modify: `src/ResultRecordSummary.cpp`
- Modify: `tests/result_record_summary_tests.cpp`

- [ ] **Step 1: Change the corrupt-file capability expectation first**

Update the corrupt regular replay case to require `deleteReplayFile == true`. In the same case assert Watch, G-Battle, video export, and sharing remain false. Keep autoplay, missing, unsafe, and I/O-failed cases non-deletable.

- [ ] **Step 2: Run the summary capability suite and confirm Delete is hidden**

```bash
cmake --build cmake-build-debug --target result_record_summary_tests -j 6
ctest --test-dir cmake-build-debug -R '^result_record_summary_tests$' --output-on-failure
```

- [ ] **Step 3: Separate availability from deletability**

In `ResultRecordSummary.cpp`, keep `replayAvailable` for playback/export/share actions. Add `replayDeletable` for non-autoplay files in `Available` or `Corrupt` state, and use it only for `deleteReplayFile`.

- [ ] **Step 4: Re-run capability and file-action service tests**

```bash
cmake --build cmake-build-debug --target result_record_summary_tests replay_file_store_tests -j 6
ctest --test-dir cmake-build-debug -R '^(result_record_summary_tests|replay_file_store_tests)$' --output-on-failure
```

- [ ] **Step 5: Commit corrupt replay deletion capability**

```bash
git add src/ResultRecordSummary.cpp tests/result_record_summary_tests.cpp
git commit -m "fix: allow deleting corrupt replay files"
```

---

## Task 9: Run cross-cutting audits and full verification

**Files:**

- Verify all files changed by Tasks 1-8
- Update only tests or production code directly implicated by a discovered regression

- [ ] **Step 1: Run the focused regression set together**

```bash
cmake --build cmake-build-debug --target beatoraja_replay_codec_tests replay_file_migration_tests result_recall_builder_tests logical_gameplay_input_tests result_persistence_integration_tests result_record_summary_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(beatoraja_replay_codec_tests|replay_file_migration_tests|result_recall_builder_tests|foundation_input_gameplay|result_persistence_integration_tests|result_record_summary_tests)$' --output-on-failure
```

- [ ] **Step 2: Run source-policy audits**

```bash
rg -n 'ReplayMigrationKeyModeResolver|makeChartDatabaseReplayKeyModeResolver' src tests
rg -n 'input\.empty\(\)' src/repositories/ReplayRepositoryReplayFileMigration.cpp
rg -n 'ASSIGN:|recalledCourseClearTypeRank|kCorruptCandidateAllowance|ReplayFileState::Corrupt' src tests
git diff --check
git status --short
```

Expected: old migration resolver symbols and synthetic empty-input insertion are absent; the new behavior has direct production and test coverage; the diff has no whitespace errors.

- [ ] **Step 3: Run the complete desktop test suite**

```bash
ctest --test-dir cmake-build-debug -j 6 --output-on-failure
```

- [ ] **Step 4: Rebuild the desktop application target**

```bash
cmake --build cmake-build-debug --target main -j 6
```

- [ ] **Step 5: Run the iOS build-only verification**

```bash
scripts/ios_firebase_deploy.sh --build-only
```

This wrapper performs the repository's local compile check without archive, signing, or upload. Capture the exact failing compiler output if the local environment prevents completion.

- [ ] **Step 6: Review the final diff against scope**

```bash
git diff 595f18f5...HEAD --stat
git diff 595f18f5...HEAD -- src/replay/ReplayFileActionService.cpp src/scene/main/MainMenuScene.cpp
git log --oneline 595f18f5..HEAD
git status --short
```

Expected: no slot-copy or slot-relocation implementation appears, each accepted review item has a focused commit, and the worktree is clean.

- [ ] **Step 7: Commit only if verification required a direct regression fix**

If verification required a scoped code/test correction, rerun its focused red-green cycle and commit it with a message naming the corrected behavior. Do not create an empty verification commit.
