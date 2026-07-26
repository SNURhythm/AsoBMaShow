# PR #82 Third Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Address the three approved replay review findings while intentionally deferring the Beatoraja slot-copy UI and preserving replay/result/IR decoupling.

**Architecture:** Treat replay files as optional profile attachments, give the schema-10 migration an optional chart-metadata key-mode resolver with a non-blocking fallback, and retain completed course attempts in the existing ResultScene retry-or-continue state. Keep migration conversion and cutover atomic, and keep course retry out of the chart-only IR path.

**Tech Stack:** C++20/C++23 tests, SQLite, Beatoraja `.brd` codec, CMake/CTest, Xcode iOS build script.

## Global Constraints

- Missing replay attachments must not block profile export, but present referenced files must still pass size and SHA-256 validation.
- Chart metadata lookup must be read-only and optional; unavailable, unmatched, ambiguous, or unsupported metadata falls back to observed-lane inference.
- Legacy lane conversion must support 5, 7, 9, 10, 14, 24, and 48-key modes.
- The schema-10-to-11 migration keeps its existing staged-file and SQLite transaction atomicity.
- Course persistence retry must retain the exact completed attempt and must not create automatic IR work.
- Do not add a production caller for Beatoraja slot copy, relocate occupied slots, or resolve that review thread.
- Do not reconstruct result or IR provenance from replay input.
- Do not upload an iOS build; use `scripts/ios_firebase_deploy.sh --build-only` only.

---

### Task 1: Export profiles with intentionally deleted replay attachments

**Files:**
- Modify: `tests/profile_archive_tests.cpp`
- Modify: `src/ProfileArchive.cpp:1690-1755`

**Interfaces:**
- Consumes: `ReplayRepository::ListReplayFileReferencesSnapshot(...)` and the existing `std::map<std::string, ReplayArchiveReference>` keyed by `replay/<filename>.brd`.
- Produces: profile archives that omit absent replay members while continuing to validate every referenced member that exists.

- [ ] **Step 1: Write the failing export regression**

Add this test beside `testExportRejectsReplayBytesThatDoNotMatchReference()` and call it from `main()`:

```cpp
void testExportOmitsDeletedReplayAttachment() {
  Fixture fixture;
  const PlayerProfilePaths source = fixture.manager.pathsFor(fixture.sourceId);
  std::error_code error;
  const bool removed = std::filesystem::remove(
      source.replayDirectory / std::string(kReplayFilename), error);
  expect(removed && !error, "referenced replay fixture is deleted");

  const auto destination = fixture.exchange.path() / "deleted-replay.zip";
  ProfileArchiveService service(fixture.manager);
  const auto exported = service.Export(fixture.sourceId, destination);
  expect(exported.ok(),
         "profile export accepts a deliberately deleted replay attachment: " +
             exported.message);

  std::string archiveError;
  auto members = readArchive(destination, archiveError);
  expect(archiveError.empty(), "deleted-replay export reads");
  expect(members.size() + 1 == kExpectedMembers.size(),
         "deleted replay removes exactly one archive member");
  expect(findMember(members,
                    "replay/" + std::string(kReplayFilename)) == nullptr,
         "deleted replay bytes are absent from the archive");
}
```

- [ ] **Step 2: Run the focused test to verify it fails**

Run:

```bash
cmake --build cmake-build-debug --target profile_archive_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_profile_archive$' --output-on-failure
```

Expected: `foundation_profile_archive` fails because export returns `ProfileError::IntegrityFailure` with `profile export is missing a referenced replay file`.

- [ ] **Step 3: Remove only the missing-reference rejection**

Delete the final post-enumeration check in `ProfileArchiveService::Export`:

```cpp
if (!replayReferences.empty()) {
  return failure(ProfileError::IntegrityFailure,
                 "profile export is missing a referenced replay file");
}
```

Keep the existing per-present-file `compressedSize` and SHA-256 checks unchanged. Do not delete database references or synthesize archive members.

- [ ] **Step 4: Run the profile archive test to verify both deletion and corruption behavior**

Run:

```bash
cmake --build cmake-build-debug --target profile_archive_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_profile_archive$' --output-on-failure
```

Expected: PASS, including `testExportOmitsDeletedReplayAttachment()` and the existing tampered-byte rejection.

- [ ] **Step 5: Commit the export fix**

```bash
git add src/ProfileArchive.cpp tests/profile_archive_tests.cpp
git commit -m "fix: allow deleted replay attachments in profile export"
```

### Task 2: Resolve legacy replay key mode from chart metadata

**Files:**
- Modify: `src/repositories/ReplayRepositoryReplayFileMigration.h`
- Modify: `src/repositories/ReplayRepositoryReplayFileMigration.cpp`
- Modify: `src/repositories/ReplayRepositoryInternal.h`
- Modify: `src/repositories/ReplayRepositorySchema.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepository.cpp`
- Modify: `src/AppDatabaseInitializer.h`
- Modify: `tests/replay_file_migration_tests.cpp`

**Interfaces:**
- Produces: `ReplayMigrationChartIdentity { chartPath, chartMd5, chartSha256 }`.
- Produces: `using ReplayMigrationKeyModeResolver = std::function<std::optional<int>(const ReplayMigrationChartIdentity &)>`.
- Produces: `ReplayMigrationKeyModeResolver makeChartDatabaseReplayKeyModeResolver(const std::filesystem::path &chartDatabasePath)`.
- Changes: `migrateReplaySchema10To11(..., ReplayMigrationFaults faults = {}, ReplayMigrationKeyModeResolver resolveKeyMode = {})`.
- Changes: `ReplayRepository::SetChartDatabasePath(std::filesystem::path)` and passes that path through schema creation/migration.

- [ ] **Step 1: Write a sparse-14K migration regression**

Extend the migration header contract assertions and add a test that builds a minimal read-only chart metadata database:

```cpp
void createChartMetadataDatabase(sqlite3 *database,
                                 const FixtureFacts &chart, int keyMode) {
  executeOrThrow(database,
      "CREATE TABLE chart_meta(path TEXT PRIMARY KEY,md5 TEXT NOT NULL,"
      "sha256 TEXT NOT NULL,keys INTEGER);"
      "INSERT INTO chart_meta(path,md5,sha256,keys) VALUES('BMS/test.bms','" +
          chart.md5 + "','" + chart.sha256 + "'," +
          std::to_string(keyMode) + ")");
}

void testChartMetadataPreservesSparseFourteenKeyMode() {
  TemporaryDirectory temporary;
  Database replayDatabase(temporary.path() / "replay.db");
  replay_schema10_fixture::createExactSchema(replayDatabase.get());
  const FixtureFacts chart = insertChartFixture(replayDatabase.get());
  executeOrThrow(replayDatabase.get(),
                 "UPDATE replays SET chart_path='BMS/test.bms' WHERE id=42");
  executeOrThrow(replayDatabase.get(),
                 "UPDATE pending_chart_score_writes SET "
                 "chart_path='BMS/test.bms' WHERE replay_id=42");

  Database chartDatabase(temporary.path() / "chart.db");
  createChartMetadataDatabase(chartDatabase.get(), chart, 14);
  replay::BeatorajaReplayCodec codec;
  replay::ReplayFileStore store(temporary.path());
  const auto resolver =
      replay_repository_detail::makeChartDatabaseReplayKeyModeResolver(
          temporary.path() / "chart.db");
  const auto outcome = replay_repository_detail::migrateReplaySchema10To11(
      replayDatabase.get(), temporary.path(), codec, store, {}, resolver);

  expect(outcome.status == replay_repository_detail::ReplayMigrationOutcome::
                               Status::Migrated,
         "sparse 14-key replay migrates with chart metadata");
  expect(integer(replayDatabase.get(),
                 "SELECT key_mode FROM chart_results WHERE id=42") == 14,
         "chart metadata, not observed lanes, owns migrated key mode");
}
```

The existing fixture observes only player-one lanes, so the current inference returns 7 and makes this assertion fail.

- [ ] **Step 2: Run the migration test to verify the missing resolver API/failing behavior**

Run:

```bash
cmake --build cmake-build-debug --target replay_file_migration_tests -j 6
```

Expected: compilation fails because the resolver types/factory/signature do not exist. After declaring only the interfaces, the runtime assertion must fail with stored `key_mode=7` until the resolver is applied.

- [ ] **Step 3: Add the optional resolver and exact identity rules**

Declare these in `ReplayRepositoryReplayFileMigration.h`:

```cpp
struct ReplayMigrationChartIdentity {
  std::string_view chartPath;
  std::string_view chartMd5;
  std::string_view chartSha256;
};

using ReplayMigrationKeyModeResolver =
    std::function<std::optional<int>(const ReplayMigrationChartIdentity &)>;

[[nodiscard]] ReplayMigrationKeyModeResolver
makeChartDatabaseReplayKeyModeResolver(
    const std::filesystem::path &chartDatabasePath);
```

Implement the factory in `ReplayRepositoryReplayFileMigration.cpp` with `sqlite3_open_v2(..., SQLITE_OPEN_READONLY, ...)`. Query `path`, lowercase `md5`, lowercase `sha256`, and `keys` from `chart_meta`. Accept a candidate only under these rules:

```cpp
const bool identityMatches =
    !identity.chartSha256.empty()
        ? rowSha256 == identity.chartSha256 &&
              (identity.chartMd5.empty() || rowMd5 == identity.chartMd5)
        : !identity.chartMd5.empty()
              ? rowMd5 == identity.chartMd5
              : rowPath == identity.chartPath;
```

Return a mode only when all accepted rows agree and it is one of `{5, 7, 9, 10, 14, 24, 48}`. Return `std::nullopt` for open/prepare/step failures, no match, conflicting rows, or unsupported values.

Pass the resolver into `readCharts`. After `readEvents` and before `readPendingResult`, resolve the chart identity and use its answer; otherwise call the existing `inferredKeyMode(chart)`. Pass that selected mode into `readPendingResult` rather than assigning inference inside that function.

- [ ] **Step 4: Wire the chart database path through ReplayRepository**

Add `chartDatabasePath` to `ReplayRepository::Impl`, defaulting to `Utils::GetDocumentsPath("db") / "chart.db"`. Add:

```cpp
void ReplayRepository::SetChartDatabasePath(
    std::filesystem::path chartDatabasePath) {
  std::lock_guard lock(impl_->sessionMutex);
  impl_->chartDatabasePath = std::move(chartDatabasePath);
}
```

Change `replay_repository_detail::MigrateSchema` and `CreateReplayTablesOnConnection` to accept the chart database path, and have `ReplayRepository` pass `impl_->chartDatabasePath` from binding, session reuse, and initial open. In `migrateReplayDatabaseSchema`, create the resolver with `makeChartDatabaseReplayKeyModeResolver(chartDatabasePath)` and pass it to `migrateReplaySchema10To11`.

In `initializeApplicationDatabases`, set the dependency after charts are ready and before replay initialization:

```cpp
[&] {
  replays.SetChartDatabasePath(charts.DatabasePath());
  return initializeReplayDatabase(replays);
}
```

Keep the path argument defaulted to an empty path on direct connection helpers so focused schema tests that have no chart database continue to exercise fallback inference.

- [ ] **Step 5: Run the metadata-aware migration regression**

Run:

```bash
cmake --build cmake-build-debug --target replay_file_migration_tests -j 6
ctest --test-dir cmake-build-debug -R '^replay_file_migration_tests$' --output-on-failure
```

Expected: PASS; the sparse fixture stores key mode 14, and existing migrations without a resolver remain green.

- [ ] **Step 6: Commit the resolver and production wiring**

```bash
git add src/AppDatabaseInitializer.h \
  src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepository.cpp \
  src/repositories/ReplayRepositoryInternal.h \
  src/repositories/ReplayRepositorySchema.cpp \
  src/repositories/ReplayRepositoryReplayFileMigration.h \
  src/repositories/ReplayRepositoryReplayFileMigration.cpp \
  tests/replay_file_migration_tests.cpp
git commit -m "fix: resolve legacy replay key mode from chart metadata"
```

### Task 3: Convert legacy physical lanes for every supported key mode

**Files:**
- Modify: `src/repositories/ReplayRepositoryReplayFileMigration.h`
- Modify: `src/repositories/ReplayRepositoryReplayFileMigration.cpp`
- Modify: `tests/replay_file_migration_tests.cpp`

**Interfaces:**
- Produces: `std::optional<replay::LogicalControl> legacyReplayControlForPhysicalLane(int physicalLane, int keyMode) noexcept` in `replay_repository_detail`.
- Consumes: the metadata-selected `keyMode` from Task 2.

- [ ] **Step 1: Write a table-driven mapping regression**

Add a table that distinguishes physical runtime lanes from Beatoraja key codes:

```cpp
void testMapsLegacyPhysicalLanesForEverySupportedMode() {
  using replay::LogicalControlKind;
  struct Case {
    int keyMode;
    int physicalLane;
    LogicalControlKind kind;
    int player;
    int logicalLane;
  };
  constexpr std::array cases{
      Case{5, 4, LogicalControlKind::Lane, 1, 4},
      Case{5, 7, LogicalControlKind::ScratchClockwise, 1, -1},
      Case{7, 6, LogicalControlKind::Lane, 1, 6},
      Case{9, 8, LogicalControlKind::Lane, 1, 8},
      Case{10, 8, LogicalControlKind::Lane, 2, 0},
      Case{10, 15, LogicalControlKind::ScratchClockwise, 2, -1},
      Case{14, 14, LogicalControlKind::Lane, 2, 6},
      Case{14, 15, LogicalControlKind::ScratchClockwise, 2, -1},
      Case{24, 25, LogicalControlKind::Lane, 1, 25},
      Case{48, 51, LogicalControlKind::Lane, 2, 25},
  };
  for (const auto &expected : cases) {
    const auto control =
        replay_repository_detail::legacyReplayControlForPhysicalLane(
            expected.physicalLane, expected.keyMode);
    expect(control.has_value() && control->kind == expected.kind &&
               control->player == expected.player &&
               control->lane == expected.logicalLane,
           "legacy physical lane maps for its exact key mode");
  }
  expect(!replay_repository_detail::legacyReplayControlForPhysicalLane(8, 7),
         "7-key migration rejects player-two and 9-key-only lane 8");
}
```

- [ ] **Step 2: Run the test to verify the mapping API is missing**

Run:

```bash
cmake --build cmake-build-debug --target replay_file_migration_tests -j 6
```

Expected: compilation fails because `legacyReplayControlForPhysicalLane` is undeclared.

- [ ] **Step 3: Implement physical-lane conversion and use it in migration**

Replace the anonymous `legacyControl` with the declared detail function. Use the runtime physical layout:

```text
5K:  P1 keys 0..4, P1 scratch 7
7K:  P1 keys 0..6, P1 scratch 7
9K:  P1 keys 0..8, no scratch
10K: P1 keys 0..4, scratch 7; P2 keys 8..12, scratch 15
14K: P1 keys 0..6, scratch 7; P2 keys 8..14, scratch 15
24K: P1 keys 0..25, no scratch
48K: P1 keys 0..25; P2 keys 26..51, no scratch
```

Scratch events map to `ScratchClockwise` because schema 10 did not preserve direction. Set `stockScratchDirectionBestEffort` when the returned control kind is either scratch kind. Invalid lanes remain skipped as before, and the legacy extension track remains unchanged.

- [ ] **Step 4: Run all migration tests**

Run:

```bash
cmake --build cmake-build-debug --target replay_file_migration_tests -j 6
ctest --test-dir cmake-build-debug -R '^replay_file_migration_tests$' --output-on-failure
```

Expected: PASS for all seven mode mappings, sparse metadata resolution, fallback inference, and atomic rollback fixtures.

- [ ] **Step 5: Commit the conversion fix**

```bash
git add src/repositories/ReplayRepositoryReplayFileMigration.h \
  src/repositories/ReplayRepositoryReplayFileMigration.cpp \
  tests/replay_file_migration_tests.cpp
git commit -m "fix: map legacy replay lanes by key mode"
```

### Task 4: Retain and retry failed course replay persistence

**Files:**
- Create: `src/scene/ResultCoursePersistence.h`
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `tests/remote_result_scene_tests.cpp`

**Interfaces:**
- Produces: `ResultPersistenceOptions::courseAttempt` as `std::shared_ptr<const result_persistence::CompletedCourseAttempt>`.
- Produces: `result_scene_detail::hasPersistenceAttempt(const ResultPersistenceOptions &) noexcept`.
- Produces: `result_scene_detail::persistenceAttemptId(const ResultPersistenceOptions &) noexcept`.
- Produces: `result_scene_detail::persistCourseAttempt(ResultPersistenceOptions &, CompletedCourseAttempt, const CoursePersistenceCallback &)`.
- Produces: `result_scene_detail::retryPersistenceAttempt(ResultPersistenceOptions &, std::span<const ir::IrOutboxDraft>, const ResultPersistenceRetryCallbacks &)`.
- Produces: `result_scene_detail::applyCoursePersistenceReceipt(const std::shared_ptr<const CompletedCourseAttempt> &, const SaveOutcome &, CoursePlaySession &) noexcept` in the focused course-persistence header.
- Consumes: `result_persistence::Coordinator::persistCourse(const CompletedCourseAttempt &)`, with no IR drafts.

- [ ] **Step 1: Write real attempt-policy, retry, and receipt regressions**

Add a focused behavior test to `remote_result_scene_tests.cpp`. The two callback outcomes are deliberately different, so choosing the wrong branch changes the observed `SaveOutcome` without asserting on the callback itself:

```cpp
void testCoursePersistenceAttemptParticipatesInRetryPolicy() {
  constexpr std::string_view attemptId =
      "123e4567-e89b-42d3-a456-426614174099";
  result_persistence::CompletedCourseAttempt course;
  course.result.attemptId = std::string(attemptId);
  ResultPersistenceOptions persistence;
  require(result_scene_detail::persistCourseAttempt(
              persistence, std::move(course),
              [](const result_persistence::CompletedCourseAttempt &) {
                return result_persistence::SaveOutcome{
                    .state = result_persistence::SaveState::Unstaged,
                    .userMessage = "Course replay was not saved.",
                };
              }) &&
              persistence.outcome.state ==
                  result_persistence::SaveState::Unstaged,
          "failed first course save retains its retryable attempt and outcome");

  require(result_scene_detail::hasPersistenceAttempt(persistence),
          "completed course attempt is available to persistence retry");
  require(result_scene_detail::persistenceAttemptId(persistence) ==
              attemptId,
          "course retry diagnostics use the retained course attempt ID");

  const ResultPersistenceRetryCallbacks callbacks{
      .persistChart =
          [](const result_persistence::CompletedChartAttempt &,
             std::span<const ir::IrOutboxDraft>) {
            return result_persistence::SaveOutcome{
                .state = result_persistence::SaveState::InvalidAttempt};
          },
      .persistCourse =
          [](const result_persistence::CompletedCourseAttempt &attempt) {
            return result_persistence::SaveOutcome{
                .state = result_persistence::SaveState::Saved,
                .receipt = result_persistence::StageReceipt{
                    .attemptId = *attempt.result.attemptId,
                    .resultId = 91,
                    .createdAt = "2026-07-26 01:02:03",
                }};
          },
  };
  require(result_scene_detail::retryPersistenceAttempt(
              persistence, {}, callbacks) &&
              persistence.outcome.state ==
                  result_persistence::SaveState::Saved,
          "course retry executes the course persistence branch");

  CoursePlaySession session;
  require(result_scene_detail::applyCoursePersistenceReceipt(
              persistence.courseAttempt, persistence.outcome, session) &&
              session.courseReplaySaved &&
              session.savedCourseReplayId == 91 &&
              session.courseReplayPlaybackData != nullptr,
          "saved course retry applies its receipt and replay to the session");

  persistence.attempt =
      std::make_shared<const result_persistence::CompletedChartAttempt>();
  require(!result_scene_detail::hasPersistenceAttempt(persistence),
          "ambiguous chart and course attempts fail closed");
  require(!result_scene_detail::retryPersistenceAttempt(
              persistence, {}, callbacks),
          "ambiguous persistence attempts cannot execute either branch");
}
```

- [ ] **Step 2: Run the focused scene test to verify it fails**

Run:

```bash
cmake --build cmake-build-debug --target remote_result_scene_tests -j 6
```

Expected: compilation fails because the retained course attempt and retry/receipt policy helpers do not exist.

- [ ] **Step 3: Add generic attempt availability and identity helpers**

Add `courseAttempt` beside the chart attempt in `ResultPersistenceOptions`. Implement the helpers so exactly one attempt is valid:

```cpp
[[nodiscard]] inline bool hasPersistenceAttempt(
    const ResultPersistenceOptions &persistence) noexcept {
  return (persistence.attempt != nullptr) !=
         (persistence.courseAttempt != nullptr);
}

[[nodiscard]] inline std::string_view persistenceAttemptId(
    const ResultPersistenceOptions &persistence) noexcept {
  if (!hasPersistenceAttempt(persistence)) {
    return {};
  }
  const auto &attemptId = persistence.attempt != nullptr
                              ? persistence.attempt->result.attemptId
                              : persistence.courseAttempt->result.attemptId;
  return attemptId ? std::string_view(*attemptId) : std::string_view{};
}
```

Use these helpers in `persistenceDecisionRequired`, status creation, Retry Save enablement, details generation, and `updateResultPersistencePresentation`. Existing chart behavior remains unchanged.

Add `ResultPersistenceRetryCallbacks` with one chart callback taking the attempt and automatic drafts, and one course callback taking only the course attempt. Implement `retryPersistenceAttempt` to fail closed unless exactly one attempt and its callback exist, invoke only that callback, and replace `persistence.outcome` with the returned value.

Add `CoursePersistenceCallback` and implement `persistCourseAttempt` to fail closed when a chart or course attempt is already attached, move the completed course attempt into `courseAttempt`, invoke the callback once, and retain both the attempt and returned outcome even when saving fails.

Create `ResultCoursePersistence.h` and implement `applyCoursePersistenceReceipt` there so `ResultScene.h` can retain its lightweight `CoursePlaySession` forward declaration. The helper requires a retained course attempt, `SaveState::Saved`, a positive receipt result ID, a non-empty created time, and an exact receipt/attempt ID match. On success it sets `courseReplaySaved`, `savedCourseReplayId`, and a copy of the retained `CourseReplayPlaybackData`; otherwise it leaves the session unchanged and returns false.

- [ ] **Step 4: Retain the first course attempt and apply successful receipts**

In `saveCourseReplay`, pass the completed attempt through `persistCourseAttempt`:

```cpp
auto &persistenceOptions = local->persistenceOptions;
result_scene_detail::persistCourseAttempt(
    persistenceOptions,
    {.result = std::move(result),
     .replay = session->recordedReplayPlayback},
    [this](const result_persistence::CompletedCourseAttempt &attempt) {
      return context.resultPersistence.persistCourse(attempt);
    });
const auto &courseAttempt = persistenceOptions.courseAttempt;
```

On a saved outcome with a receipt whose attempt ID matches `courseAttempt->result.attemptId`, set:

```cpp
session->savedCourseReplayId = persistenceOptions.outcome.receipt->resultId;
session->courseReplaySaved = true;
session->courseReplayPlaybackData =
    std::make_shared<replay::CourseReplayPlaybackData>(courseAttempt->replay);
```

On failure, log the retained outcome but do not discard `courseAttempt` or return before the result scene builds its persistence status UI.

- [ ] **Step 5: Branch Retry Save by attempt kind**

Update `retryResultPersistence()` to reject ambiguous/missing attempts through `hasPersistenceAttempt`. Keep chart automatic-draft behavior inside the chart branch. Add the course branch:

```cpp
if (persistenceOptions.courseAttempt != nullptr) {
  persistenceOptions.outcome = context.resultPersistence.persistCourse(
      *persistenceOptions.courseAttempt);
} else {
  std::vector<ir::IrOutboxDraft> automaticDrafts;
  if (persistenceOptions.irSnapshot) {
    automaticDrafts = context.irDrivers.buildAutomaticDrafts(
        context.settings.irProviders, *persistenceOptions.irSnapshot);
  }
  persistenceOptions.outcome = context.resultPersistence.persist(
      *persistenceOptions.attempt, automaticDrafts);
  if (persistenceOptions.outcome.saved() && !automaticDrafts.empty() &&
      context.irSubmissionService) {
    context.irSubmissionService->notifyOutboxChanged();
  }
}
```

Have `retryResultPersistence()` build automatic drafts only when the chart attempt is selected, then call `retryPersistenceAttempt` with callbacks bound to `context.resultPersistence.persist(...)` and `persistCourse(...)`. After the helper returns, apply the matching receipt. Course success calls `applyCoursePersistenceReceipt`; it never calls `buildAutomaticDrafts` or notifies the IR submission service. Continue Without Saving keeps the existing outcome and only dismisses the decision.

- [ ] **Step 6: Run scene and persistence regressions**

Run:

```bash
cmake --build cmake-build-debug --target remote_result_scene_tests result_persistence_integration_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^(remote_result_scene_tests|result_persistence_integration_tests)$' \
  --output-on-failure
```

Expected: PASS; chart retry contracts remain present, course attempt selection is unambiguous, and coordinator course idempotency still passes.

- [ ] **Step 7: Commit the course retry fix**

```bash
git add src/scene/ResultCoursePersistence.h src/scene/ResultScene.h \
  src/scene/ResultScene.cpp \
  tests/remote_result_scene_tests.cpp
git commit -m "fix: expose failed course replay saves for retry"
```

### Task 5: Verify and publish the PR update

**Files:**
- Verify: all files changed by Tasks 1-4
- Preserve: `docs/superpowers/specs/2026-07-26-pr-82-third-review-fixes-design.md`
- Preserve: `docs/superpowers/plans/2026-07-26-pr-82-third-review-fixes.md`

**Interfaces:**
- Consumes: all focused regressions from Tasks 1-4.
- Produces: a pushed `feature/file-based-replays` update for PR #82 without GitHub thread writes.

- [ ] **Step 1: Run all focused tests together**

```bash
cmake --build cmake-build-debug --target \
  profile_archive_tests replay_file_migration_tests \
  remote_result_scene_tests result_persistence_integration_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^(foundation_profile_archive|replay_file_migration_tests|remote_result_scene_tests|result_persistence_integration_tests)$' \
  --output-on-failure
```

Expected: all four CTest entries pass.

- [ ] **Step 2: Run the desktop application build**

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: target `main` builds successfully.

- [ ] **Step 3: Run the iOS compile-only verification**

```bash
scripts/ios_firebase_deploy.sh --build-only
```

Expected: the iOS build completes successfully. Do not run a Firebase upload.

- [ ] **Step 4: Inspect final scope and diff hygiene**

```bash
git status --short --branch
git diff --check origin/feature/file-based-replays...HEAD
git diff --stat origin/feature/file-based-replays...HEAD
```

Expected: only the approved three review areas plus their spec/plan are changed; no slot-copy UI or slot relocation appears.

- [ ] **Step 5: Confirm the approved spec and execution plan are committed**

```bash
git log --oneline -- \
  docs/superpowers/specs/2026-07-26-pr-82-third-review-fixes-design.md \
  docs/superpowers/plans/2026-07-26-pr-82-third-review-fixes.md
```

Expected: the design and plan each appear in branch history before push.

- [ ] **Step 6: Push the PR branch**

```bash
git push origin feature/file-based-replays
```

Expected: PR #82 advances to the final local commit. Do not reply to or resolve any GitHub review thread.
