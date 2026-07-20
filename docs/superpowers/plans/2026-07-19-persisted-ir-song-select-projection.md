# Persisted IR Song Select Projection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the slow runtime IR chart-list overlay with source-flagged IR scores persisted through the normal score-save and SQLite summary path.

**Architecture:** `ir_remote_scores` remains the durable detailed mirror. `ScoreRepository` transactionally materializes each scoped mirror generation into source-tagged `scores` rows using the shared score insert primitive, while existing summary tables feed the original paginated Song Select SQL queries.

**Tech Stack:** C++23, SQLite, CMake/CTest, SDL logging

## Global Constraints

- Preserve durable IR receipts, remote Records, and recalled remote results.
- Imported IR rows are read-only and must never become uploadable local attempts.
- API keys remain outside score rows and outbox payloads.
- The remote lamp is authoritative; imported placeholder break data must not infer FULL COMBO.
- Song Select must use the pre-`142a685` count plus lazy 128-row page query path.
- Review and execute this plan as one plan, not with task-by-task approval pauses.

---

### Task 1: Imported score schema and repository transaction

**Files:**
- Modify: `src/repositories/ScoreRepository.h`
- Modify: `src/repositories/ScoreRepositoryModels.h`
- Modify: `src/repositories/ScoreRepositoryInternal.h`
- Modify: `src/repositories/ScoreRepositorySchema.cpp`
- Modify: `src/repositories/ScoreRepositoryQueries.cpp`
- Create: `src/repositories/ScoreRepositoryIrImport.cpp`
- Test: `tests/ir_score_import_projection_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Consumes: `ir::IrRemoteScore`, `result_persistence::ChartScoreWrite`, and the existing score insert binding path.
- Produces: `ScoreRepository::ReplaceImportedIrScores(providerId, serverOrigin, syncGeneration, scores)` and `ScoreRepository::ImportedIrScoresAreCurrent(providerId, serverOrigin, syncGeneration, scoreCount)`.

- [ ] **Step 1: Write failing repository tests**

Add tests that request the new API and assert:

```cpp
const auto applied = scores.ReplaceImportedIrScores(
    "tachi", "https://boku.tachi.ac", 1000, remote);
expect(applied.status == ImportedIrScoreProjectionStatus::Applied,
       "remote snapshot is persisted");
expect(scores.ImportedIrScoresAreCurrent(
           "tachi", "https://boku.tachi.ac", 1000, remote.size()),
       "projection generation is current");
```

Also assert four playable LN summary rows, exact remote lamp preservation when
`combo_break` is a placeholder, replacement of absent remote IDs, origin
isolation, rollback on an invalid score, imported-source result semantics, and
unchanged local `SaveProjectedScore` behavior.

- [ ] **Step 2: Run the focused test and verify RED**

Run: `cmake --build cmake-build-debug --target ir_score_import_projection_tests -j 6 && ./cmake-build-debug/ir_score_import_projection_tests`

Expected: compilation fails because the imported-score repository API and test target do not exist.

- [ ] **Step 3: Add schema version 10 and source identity**

Add source columns with local defaults and a partial unique index for imported
identity. Validate the complete v10 shape in `CurrentSchemaIsValid`, migrate v9
inside the repository transaction, and rebuild summary tables after migration.

Use these source values consistently:

```cpp
enum class ScoreStorageSource : int {
  LocalGameplay = 0,
  ImportedIr = 1,
};
```

- [ ] **Step 4: Implement transactional snapshot replacement**

Validate provider, normalized origin, generation, and every remote score before
opening the write transaction. Delete only the selected imported scope, map
remote fields to `ChartScoreWrite`, insert through the shared low-level binder,
then rebuild summaries before commit. Increment the score revision once per
changed snapshot.

- [ ] **Step 5: Implement wildcard and authoritative-lamp summaries**

For imported `ln_mode = -1`, make the insert trigger and summary rebuild emit
summary keys `0`, `1`, `2`, and `3`. Make `fullComboClearRankExpr` return
`clear_type` directly for `ImportedIr`; preserve the local expression exactly.

- [ ] **Step 6: Run the focused test and verify GREEN**

Run: `cmake --build cmake-build-debug --target ir_score_import_projection_tests -j 6 && ./cmake-build-debug/ir_score_import_projection_tests`

Expected: exit 0 with every imported-score assertion passing.

### Task 2: Reconciliation and activation lifecycle

**Files:**
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryIrRemoteScores.cpp`
- Modify: `src/ir/IrSubmissionService.h`
- Modify: `src/ir/IrSubmissionService.cpp`
- Modify: `src/context.h`
- Modify: `src/scene/SettingsSceneIr.cpp`
- Test: `tests/replay_repository_tests.cpp`
- Test: `tests/ir_submission_service_tests.cpp`

**Interfaces:**
- Consumes: Task 1's imported-score replacement/currentness APIs.
- Produces: a lightweight scoped mirror-state read and an `IrSubmissionServiceOptions::remoteSnapshotApplied` callback.

- [ ] **Step 1: Write failing lifecycle tests**

Assert that mirror state exposes a consistent generation/count, successful
reconciliation invokes exactly one projection callback with the committed
snapshot, callback failure publishes a failed reconciliation status, and an
empty snapshot still invokes the callback so stale imported rows can be
deleted.

- [ ] **Step 2: Run focused lifecycle tests and verify RED**

Run: `cmake --build cmake-build-debug --target replay_repository_tests ir_submission_service_tests -j 6 && ./cmake-build-debug/replay_repository_tests && ./cmake-build-debug/ir_submission_service_tests`

Expected: compilation fails because mirror state and projection callbacks are absent.

- [ ] **Step 3: Expose durable mirror state and callback**

Return the single validated `sync_generation` plus row count for a provider and
origin. After `ApplyIrRemoteSnapshot` commits, invoke:

```cpp
std::function<bool(std::string_view profileId,
                   std::string_view providerId,
                   std::string_view serverOrigin,
                   std::int64_t syncGeneration,
                   std::span<const IrRemoteScore> scores,
                   std::string &diagnostic)> remoteSnapshotApplied;
```

Do not publish reconciliation success until it returns true.

- [ ] **Step 4: Wire application activation and identity clearing**

On service activation, compare mirror state with score projection state and
load the full local mirror only when they differ. Wire fresh reconciliation
snapshots directly to `ReplaceImportedIrScores`. Clear provider-scoped imported
rows when credential/provider evidence is invalidated.

- [ ] **Step 5: Run focused lifecycle tests and verify GREEN**

Run: `cmake --build cmake-build-debug --target replay_repository_tests ir_submission_service_tests -j 6 && ./cmake-build-debug/replay_repository_tests && ./cmake-build-debug/ir_submission_service_tests`

Expected: both executables exit 0.

### Task 3: Remove the mistaken runtime overlay and restore pagination

**Files:**
- Delete: `src/ir/IrScoreHistoryProjection.h`
- Delete: `src/ir/IrScoreHistoryProjection.cpp`
- Delete: `tests/ir_score_history_projection_tests.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `src/repositories/ScoreRepositoryModels.h`
- Modify: `src/repositories/ScoreRepositoryQueries.cpp`
- Modify: `src/repositories/ChartRepositoryQueries.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `tests/chart_repository_tests.cpp`
- Modify: `tests/main_menu_library_tests.cpp`
- Modify: `scripts/check_main_menu_chart_list_lifecycle.py`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: persisted score summaries from Tasks 1 and 2.
- Produces: one Song Select query path using `CountChartMeta`, lazy `QueryChartMeta` pages, and `FindChartMetaIndex`.

- [ ] **Step 1: Strengthen the static regression check and verify RED**

Require that `reloadChartList` contains no
`chartMetaQueryUsesProjectedScores`, `projectedScoreQueryIndices`,
`resetOwned`, or `resetReferenced`, and does contain the original count/reset
sequence. Run:

`python3 scripts/check_main_menu_chart_list_lifecycle.py`

Expected: failure while the runtime projection branch remains.

- [ ] **Step 2: Remove overlay-only models and source files**

Delete imported SHA/MD5 overlay maps and merge helpers from score cache models,
remove the projection module and its CMake entries, and revert folder clear
aggregation to the persisted SHA summary lookup.

- [ ] **Step 3: Restore the single paginated chart-list cache**

Remove owned/referenced records and projected metadata cache state. Keep the
`releasePages()` pause fix, but restore:

```cpp
const int databaseCount = chartSession->CountChartMeta(query);
chartListCache.reset(*chartSession, query, databaseCount,
                     std::move(leadingRecord));
```

Path restoration must call `chartSession->FindChartMetaIndex(query, path)` with
the course-leading offset exactly as before `142a685`.

- [ ] **Step 4: Remove projection refresh state**

Delete `projectActiveIrScoreMirror`, projection diagnostics/revisions, and
`refreshIrScoreProjectionIfNeeded`. `reloadScoreClearRanks` loads persisted
local-plus-imported score summaries once and passes the same chart ranks plus
local-only course ranks to folder aggregation.

- [ ] **Step 5: Run focused regressions and verify GREEN**

Run: `python3 scripts/check_main_menu_chart_list_lifecycle.py && cmake --build cmake-build-debug --target chart_repository_tests main_menu_library_tests -j 6 && ./cmake-build-debug/chart_repository_tests && ./cmake-build-debug/main_menu_library_tests`

Expected: script and both test executables exit 0.

### Task 4: Whole-plan verification and publication

**Files:**
- Review: all files changed by Tasks 1–3

**Interfaces:**
- Consumes: all prior tasks.
- Produces: verified commit on `feature/bokutachi-ir`, pushed to its configured remote.

- [ ] **Step 1: Inspect cleanup completeness**

Run: `rg -n "IrScoreHistoryProjection|projectActiveIrScoreMirror|chartMetaQueryUsesProjectedScores|projectedChartMetadataCache|resetReferenced|resetOwned" src tests CMakeLists.txt`

Expected: no matches.

- [ ] **Step 2: Run the full test suite**

Run: `ctest --test-dir cmake-build-debug --output-on-failure`

Expected: 100% tests passed, 0 failed.

- [ ] **Step 3: Run the desktop build**

Run: `cmake --build cmake-build-debug --target main -j 6`

Expected: exit 0.

- [ ] **Step 4: Review the final diff**

Run: `git diff --check && git status --short && git diff --stat && git diff`

Expected: no whitespace errors; changes are limited to persisted IR projection,
its lifecycle, obsolete-overlay cleanup, tests, and these design documents.

- [ ] **Step 5: Commit and push**

Run:

```bash
git add CMakeLists.txt src tests scripts docs/superpowers
git commit -m "fix: persist imported IR scores for song select"
git push
```

Expected: the branch advances on the configured remote without force-push.
