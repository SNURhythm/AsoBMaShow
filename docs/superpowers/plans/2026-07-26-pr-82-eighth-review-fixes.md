# PR 82 Eighth Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix all eight current PR 82 review findings while preserving stock Beatoraja BRD interoperability and atomic profile migration.

**Architecture:** Store replay-only initial gauge state in the optional BRD extension, store result-only adopted gauge type in replay schema 12, and carry the same gauge snapshot through course materialization. Tighten schema-10 conversion inputs, reclaim result-less reservations safely, inspect Records lazily, and align the provider-neutral IR snapshot bound with compact result storage.

**Tech Stack:** C++23, CMake/CTest, SQLite, nlohmann JSON, existing Beatoraja BRD codec and durable replay file store.

## Global Constraints

- Replay events remain exclusively in BRD files.
- Stock Beatoraja fields and `replay/<stem>[_history].brd` layout remain compatible.
- IR provenance and snapshots remain independent of raw replay input.
- Schema-10 source rows are deleted only after files and compact rows validate.
- GitHub review threads are not replied to or resolved without separate authorization.
- Every production behavior change follows a failing-test-first red/green cycle.
- iOS verification uses `scripts/ios_firebase_deploy.sh --build-only` and does not upload.

---

### Task 1: Preserve complete initial gauge state in raw BRDs

**Files:**
- Modify: `src/replay/ReplayPlaybackData.h`
- Modify: `src/replay/BeatorajaReplayCodec.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/play/GamePlayStartOptions.h`
- Modify: `src/replay/ReplayPlaybackMaterializer.cpp`
- Test: `tests/beatoraja_replay_codec_tests.cpp`
- Test: `tests/gameplay_playback_startup_tests.cpp`

**Interfaces:**
- Produce: `ChartPlaybackSetup::startingGaugeState`, an optional `GaugeStateSnapshot` stored only in the Aso extension.
- Consume: `RhythmState::gaugeSnapshot()` at recording start and `restoreGaugeState()` at playback/materialization start.

- [ ] Add a codec round-trip test whose BestClear snapshot contains groove 20%, survival 100%, distinct failure flags, and an active survival gauge.
- [ ] Run `cmake --build cmake-build-debug --target beatoraja_replay_codec_tests gameplay_playback_startup_tests -j 6` and the two matching CTests; confirm failure because the setup has no snapshot field.
- [ ] Encode/decode and validate the optional snapshot without changing the stock replay object or rejecting older extension-v2 files that omit it.
- [ ] Capture the configured state in `beginReplayRecording`, restore it in live raw playback, and pass it as `GameplayAttemptConfig::carriedGauge` during materialization. Use the scalar field only when no snapshot exists.
- [ ] Re-run the focused tests and commit the green change as `fix: preserve raw replay gauge state`.

### Task 2: Persist the adopted gauge type in compact results

**Files:**
- Modify: `src/ResultPersistenceModel.h`
- Modify: `src/ResultPersistenceModel.cpp`
- Modify: `src/ResultRecallBuilder.cpp`
- Modify: `src/repositories/ReplayRepositoryResultRecords.cpp`
- Modify: `src/repositories/ReplayRepositorySchema.cpp`
- Test: `tests/result_persistence_model_tests.cpp`
- Test: `tests/result_recall_builder_tests.cpp`
- Test: `tests/result_persistence_v11_integration_tests.cpp`

**Interfaces:**
- Produce: `PersistedChartResult::adoptedGaugeType` and `PersistedCourseStageResult::adoptedGaugeType`.
- Produce: replay schema 12 columns `chart_results.adopted_gauge_type` and `course_stage_results.adopted_gauge_type` with integer range checks.

- [ ] Add capture/recall tests proving a BestClear result whose active gauge differs from provenance recalls the adopted label, final value, and history.
- [ ] Add a schema-upgrade test proving a schema-11 profile migrates transactionally and backfills the recorded initial gauge type.
- [ ] Run the focused model, recall, and integration tests; confirm failures on the missing fields/schema.
- [ ] Capture `state.gaugeType`, validate it, include it in fingerprints, bind/read it in compact rows, and use it in `ResultRecallBuilder::stateFrom`.
- [ ] Advance the compact schema to 12 and implement an 11-to-12 transaction that adds/backfills both columns while preserving the existing 10-to-11 BRD cutover path.
- [ ] Re-run focused tests and commit as `fix: persist adopted gauge types`.

### Task 3: Carry gauge state through course raw materialization

**Files:**
- Modify: `src/replay/ReplayPlaybackMaterializer.h`
- Modify: `src/replay/ReplayPlaybackMaterializer.cpp`
- Modify: `src/ReplayVideoExporter.cpp`
- Test: `tests/replay_playback_driver_tests.cpp`

**Interfaces:**
- Extend: `ReplayMaterializationSeed` with `std::optional<GaugeStateSnapshot> carriedGauge`.
- Produce: each `MaterializedReplay::attempt.gaugeState` as the next stage seed.

- [ ] Add a two-stage materialization test where stage one changes the active auto-shift gauge and parallel values, then assert stage two starts from that full snapshot.
- [ ] Run the materializer CTest and confirm stage two currently resets gauge state.
- [ ] Pass `seed.carriedGauge` into `GameplayAttemptConfig`, and update course export to seed the next stage from `materialized.value->attempt.gaugeState` together with combo values.
- [ ] Re-run the focused test and commit as `fix: carry course replay gauge state`.

### Task 4: Make schema-10 migration authoritative and fail-closed

**Files:**
- Modify: `src/repositories/ReplayRepositoryReplayFileMigration.h`
- Modify: `src/repositories/ReplayRepositoryReplayFileMigration.cpp`
- Test: `tests/replay_file_migration_tests.cpp`

**Interfaces:**
- Extend: `ReplayMigrationChartMetadata` with `int totalNotes`.
- Consume: `chart_meta.total_notes` for fallback `maxScore`.

- [ ] Add a migration test with no pending score row, a truncated survival replay, and chart metadata whose total notes exceed recorded judgements; assert the compact result uses `total_notes * 2`.
- [ ] Add separate invalid action, touch action, judgement, and event gauge-type cases; assert `InvalidLegacyData`, schema version 10, and retained legacy rows.
- [ ] Run `replay_file_migration_tests` and confirm the max-score and enum cases fail for the reviewed reasons.
- [ ] Validate enum integers before casts, resolve `total_notes`, and require authoritative metadata when pending score facts are absent.
- [ ] Re-run the focused migration test and commit as `fix: validate legacy replay migration`.

### Task 5: Reclaim finalized result-less reservations

**Files:**
- Modify: `src/repositories/ReplayRepository.cpp`
- Modify: `src/ResultPersistenceCoordinator.h`
- Modify: `src/ResultPersistenceCoordinator.cpp`
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Test: `tests/replay_repository_v11_tests.cpp`
- Test: `tests/result_persistence_coordinator_v11_tests.cpp`

**Interfaces:**
- Produce: a coordinator abandonment operation that removes the safe finalized file before discarding the exact reservation.
- Consume: `SaveState::Unstaged` only; conflict and unfinalized states remain retryable and untouched.

- [ ] Add startup recovery tests for a missing orphan, a safe finalized orphan, and an unsafe path; only the first two reservations disappear and the safe file is removed.
- [ ] Add a coordinator/Result-scene model test proving Continue or cleanup abandons an unstaged attempt once but never abandons a durable or conflict outcome.
- [ ] Run the focused repository/coordinator tests and confirm finalized orphans currently survive.
- [ ] Extend startup recovery to remove safe result-less files before deleting reservation rows, and wire explicit unstaged abandonment into Continue and scene cleanup.
- [ ] Re-run focused tests and commit as `fix: reclaim finalized replay orphans`.

### Task 6: Inspect Records lazily and enlarge IR snapshots

**Files:**
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/ResultRecordSummary.cpp`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/ir/IrSubmissionSnapshot.h`
- Modify: `src/ir/IrSubmissionSnapshot.cpp`
- Test: `tests/result_record_summary_tests.cpp`
- Test: `tests/ir_submission_snapshot_tests.cpp`

**Interfaces:**
- Add: `ReplaySummary::ReplayFileState::Unchecked`, the default for local summaries.
- Add: `ir::kMaximumIrSubmissionSnapshotBytes = 16U * 1024U * 1024U`.

- [ ] Add a summary test proving unchecked records have no file-dependent capabilities until inspected.
- [ ] Add a snapshot round-trip test larger than 256 KiB and smaller than 16 MiB.
- [ ] Run the two focused tests and confirm the new expectations fail.
- [ ] Remove the all-results inspection loop from Records reload; inspect and rebuild only the selected local record, while retaining pre-action reinspection.
- [ ] Use the public 16 MiB constant for snapshot serialization and deserialization.
- [ ] Re-run focused tests and commit as `fix: defer replay inspection and align snapshot bounds`.

### Task 7: Verify and publish

**Files:**
- Verify all modified production, test, spec, and plan files.

- [ ] Run `cmake --build cmake-build-debug --target main -j 6`.
- [ ] Run every focused test target changed by Tasks 1-6 with `ctest --test-dir cmake-build-debug --output-on-failure -R '<focused-regex>'`.
- [ ] Run `ctest --test-dir cmake-build-debug --output-on-failure` and require zero failures.
- [ ] Run `scripts/ios_firebase_deploy.sh --build-only` and require `BUILD SUCCEEDED`; do not upload.
- [ ] Run `git diff --check`, inspect `git status --short`, and review the complete branch diff.
- [ ] Commit any verification-only adjustments, push `feature/file-based-replays`, and verify local, remote, and PR head SHAs match.
