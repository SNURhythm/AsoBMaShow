# Summary-Only Replay Schema Cutover Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the legacy SQLite replay format with atomic read-only chart/course summaries, remove every runtime raw-row producer and legacy playback/reconstruction consumer, and leave modern BRD/result/IR behavior as the only active path.

**Architecture:** Replay database schema v14 copies only explicitly selected header facts into nullable legacy summary tables, re-parents historical receipts, preserves independently durable outbox payloads, and drops every raw playback table in the same transaction. Repository and UI code project those rows through a dedicated summary type with Records-only capabilities; all replay-dependent and IR-producing paths consume modern results, stored snapshots, and verified BRDs.

**Tech Stack:** C++20, SQLite C API, SDL, CMake/CTest, existing replay/result/IR contract modules.

## Global Constraints

- Implement only delivery Slice 7 of `docs/superpowers/specs/2026-07-27-contract-first-file-replays-restart-design.md` and the focused design in `docs/superpowers/specs/2026-07-27-summary-only-replay-schema-cutover-design.md`.
- The target replay database schema version is exactly 14.
- Migration performs no replay-file I/O and never reads `replay_events`, `replay_touch_samples`, `replay_lane_cover_events`, or `course_replay_stages`.
- Preserve every legacy chart/course header row independently; malformed row values become unknown/partial and do not block other rows.
- Do not convert legacy replay details, reconstruct modern results or IR snapshots, or derive max combo/course identity from detail rows.
- Drop legacy chart/course header and detail tables plus `pending_chart_score_writes` in the same transaction that preserves summaries and receipt ownership.
- Legacy summaries expose Records only: no View Result, Watch, Retry Same, G-Battle, practice ghost, video, share, delete, or new IR action.
- Modern result history, recall, provenance, receipts, snapshots, outbox work, and BRD file ownership remain independent.
- Never mutate `~/Downloads/profiles`; any real-profile migration check uses a temporary copy.
- Do not deploy to Firebase, TestFlight, or Google Play.
- Use TDD for every behavior change and commit each red/green or independently reviewable boundary.

---

## File Map

- Create `src/repositories/ReplayRepositoryLegacyMigration.h`: exact v14 summary/receipt schema inspection and the header-only cutover entrypoint.
- Create `src/repositories/ReplayRepositoryLegacyMigration.cpp`: nullable header decoding, provenance validation, row copy, outbox retirement, raw-table deletion, and final verification.
- Create `src/repositories/ReplayRepositoryLegacySummaries.cpp`: bounded read-only chart/course summary queries.
- Create `src/repositories/LegacyResultSummary.h`: optional chart/course legacy header models and read outcomes.
- Modify `src/repositories/ReplayRepositorySchema.cpp`: remove detail-derived historical backfills, route v13 and older databases through the v14 cutover, and stop current/fresh schema creation from recreating raw tables.
- Modify `src/repositories/ReplayRepository.{h,cpp}` and `ReplayRepositoryInternal.h`: expose summary reads, retire raw mutation/hydration APIs, and keep current schema validation fail-closed.
- Modify `src/repositories/ReplayRepositoryRecords.cpp`: remove raw replay inserts, event hydration, legacy result staging, legacy course recovery, and legacy IR reconstruction reads; retain only code still owned here or move it to focused files.
- Modify `src/repositories/ReplayRepositoryIrRemoteScores.cpp` and `ReplayRepositoryIrOutbox.cpp`: use legacy-summary or modern-result receipt ownership and modern result agreement.
- Modify `src/ir/IrScoreReconciliation.{h,cpp}`, `IrUploadCandidates.{h,cpp}`, and related views/controllers: identify candidates by modern result/attempt and consume stored snapshots.
- Modify `src/ResultRecordSummary.{h,cpp}`, `ReplayRecordFilters.h`, `ReplaySummaryFormatting.h`, and `view/ResultRecordListView.h`: project honest nullable legacy facts and Records-only capabilities.
- Modify `src/context.h`, `ApplicationResultRecovery.h`, `ProfileSessionCoordinator.{h,cpp}`, `scene/ResultScene.{h,cpp}`, `scene/MainMenuScene.{h,cpp}`, `scene/ChartViewerScene.{h,cpp}`, `scene/IrUploadsScene.{h,cpp}`, `scene/play/GamePlayScene.cpp`, and replay/video/result helpers: remove legacy producers and consumers.
- Modify `src/PlayerProfileManager.cpp` and `src/ProfileArchive.cpp`: validate migrated summary-only databases without requiring legacy replay bytes.
- Add `tests/replay_legacy_migration_tests.cpp`, `tests/legacy_result_summary_tests.cpp`, and focused modern IR/cutover cases; update or retire tests whose asserted legacy reconstruction behavior is intentionally removed.
- Modify `CMakeLists.txt`, `src/CMakeLists.txt`, and `docs/replay/file-replay-contract-matrix.md`.

---

### Task 1: Pin the Header-Only Atomic v14 Migration

**Files:**
- Create: `tests/replay_legacy_migration_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `replay_repository_detail::MigrateSchema(sqlite3 *)` and `ReplayRepository::EnsureSchema()`.
- Produces: executable expectations for schema v14, summary row preservation, detail-read denial, raw-table absence, historical receipt/outbox survival, and rollback.

- [ ] **Step 1: Write a minimal version-2/v13-compatible fixture builder**

Create only the six legacy playback tables and their header/detail rows, set
`PRAGMA user_version=2`, and include:

```cpp
exec(db,
     "INSERT INTO replays(id,chart_path,chart_md5,chart_sha256,chart_title,"
     "chart_artist,ln_mode,gauge_type,gauge_auto_shift,final_score,max_combo,"
     "final_gauge,clear_type,created_at) VALUES"
     "(11,'BMS/kept.bms','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
     "'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',"
     "'Kept','Artist',1,0,0,1234,456,72.5,300,'2026-07-01 02:03:04')");
exec(db,
     "INSERT INTO replay_events(replay_id,event_index,action,lane,"
     "note_time_micros,song_time_micros,judge_time_micros,judgement,"
     "diff_micros,gauge,gauge_type,combo,score)"
     " VALUES(11,0,0,1,1,2,3,0,0,99.0,0,9999,999999)");
```

The deliberately contradictory event proves migration does not use it.

- [ ] **Step 2: Write failing success and no-detail-read tests**

Install an SQLite authorizer that returns `SQLITE_DENY` for `SQLITE_READ` on
the four detail tables. Assert `EnsureSchema()` still succeeds, version is 14,
the chart summary has score 1234/max combo 456 rather than event values, the
partial course keeps its header counts, and these tables are absent:

```cpp
constexpr std::array rawTables{
    "replays", "replay_events", "replay_touch_samples",
    "replay_lane_cover_events", "course_replays", "course_replay_stages",
    "pending_chart_score_writes"};
for (const char *table : rawTables) {
  assert(!tableExists(db, table));
}
```

- [ ] **Step 3: Write failing malformed-row and receipt/outbox tests**

Use SQLite dynamic typing to store one malformed header value. Assert the row
is copied, the value is `NULL`, and `partial=1`. Add a legacy receipt and ready
outbox payload; assert receipt ID/evidence and ready work survive. Add inactive
legacy work and assert it is retained with `local_result_ready=0`, a blocked
state, and `last_error_code='legacy_result_cutover'`.

- [ ] **Step 4: Write the rollback fault matrix**

For increasing progress-handler interruption thresholds, copy the pristine
fixture, capture the database-family bytes, return nonzero once at the selected
VM callback, and call `MigrateSchema`. For every injected failure assert the
database family, schema, rows, and user version equal the before snapshot. Stop
after the first threshold that completes successfully and assert at least one
failure was exercised in each broad phase (create, copy, receipt rebuild,
drop, verify/version).

- [ ] **Step 5: Run the test to verify RED**

```bash
cmake -S . -B cmake-build-debug -DASOBMASHOW_BUILD_TESTS=ON
cmake --build cmake-build-debug --target replay_legacy_migration_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^replay_legacy_migration_tests$'
```

Expected: failure because schema version is 13 and raw tables remain.

- [ ] **Step 6: Commit the failing contract**

```bash
git add CMakeLists.txt tests/replay_legacy_migration_tests.cpp
git commit -m "test: pin atomic legacy summary cutover"
```

---

### Task 2: Implement the v14 Summary and Receipt Transition

**Files:**
- Create: `src/repositories/ReplayRepositoryLegacyMigration.h`
- Create: `src/repositories/ReplayRepositoryLegacyMigration.cpp`
- Modify: `src/repositories/ReplayRepositorySchema.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Test: `tests/replay_legacy_migration_tests.cpp`

**Interfaces:**
- Produces: `bool replay_repository_legacy::migrateToSummarySchema(sqlite3 *, int sourceVersion)` and `bool replay_repository_legacy::inspectCurrentSchema(sqlite3 *)`.
- Guarantees: exact v14 schema, header-only reads, all-or-nothing cutover, and no raw tables on current/fresh databases.

- [ ] **Step 1: Define exact summary schemas**

Use nullable fact columns and required `partial`:

```cpp
constexpr std::string_view kLegacyChartSummaryTableSql =
    "CREATE TABLE legacy_chart_result_summaries("
    "legacy_replay_id INTEGER PRIMARY KEY,chart_path TEXT,chart_md5 TEXT,"
    "chart_sha256 TEXT,chart_title TEXT,chart_artist TEXT,long_note_mode INTEGER,"
    "final_score INTEGER,max_combo INTEGER,final_gauge REAL,clear_type INTEGER,"
    "created_at TEXT,ruleset_version INTEGER,eligibility INTEGER,"
    "provenance_json TEXT,partial INTEGER NOT NULL CHECK(partial IN(0,1)))";
```

Define the course table and four lookup indexes with equally exact SQL.

- [ ] **Step 2: Add typed nullable header copying**

Select only named header columns. Bind a destination value only when its source
storage type and range are valid. Validate provenance with
`deserializeScoreProvenance`, canonical reserialization, and agreement with
the indexed fields. Pass `sourceVersion` so pre-v2 max combo and pre-v3
provenance remain unknown.

- [ ] **Step 3: Rebuild receipt ownership and retire inactive legacy work**

Create the v14 receipt table with:

```sql
FOREIGN KEY(replay_id)
  REFERENCES legacy_chart_result_summaries(legacy_replay_id)
  ON DELETE CASCADE
```

Copy every existing receipt unchanged. Before dropping legacy pending writes,
mark only inactive outbox rows whose attempt belongs to a legacy replay and no
modern chart result as blocked historical work with bounded diagnostic fields.

- [ ] **Step 4: Drop raw tables and verify exact state**

Drop children before parents, compare source and summary counts captured before
the drops, run `PRAGMA foreign_key_check`, verify exact summary/receipt schema
and raw absence, then let the caller set version 14 and commit.

- [ ] **Step 5: Remove detail-derived older migrations**

Delete `backfillReplayMaxCombo`, `backfillCompleteCourseReplayKeys`, and their
stage/event queries. Older versions may add missing header columns, but no
upgrade step may inspect detail content.

- [ ] **Step 6: Make current and fresh schema creation raw-free**

Read `user_version` before legacy bootstrap. Create the old tables only for a
pre-v14 migration input. A current v14 `EnsureSchema` validates summary/modern
objects without `CREATE TABLE IF NOT EXISTS replays`; a fresh version-0
database may create them inside the uncommitted migration but must finish with
none present.

- [ ] **Step 7: Run the focused migration test to verify GREEN**

```bash
cmake --build cmake-build-debug --target replay_legacy_migration_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^replay_legacy_migration_tests$'
```

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt src/CMakeLists.txt src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositorySchema.cpp \
  src/repositories/ReplayRepositoryLegacyMigration.* \
  tests/replay_legacy_migration_tests.cpp
git commit -m "feat: migrate legacy replays to summaries"
```

---

### Task 3: Add the Honest Legacy Summary Read Model

**Files:**
- Create: `src/repositories/LegacyResultSummary.h`
- Create: `src/repositories/ReplayRepositoryLegacySummaries.cpp`
- Create: `tests/legacy_result_summary_tests.cpp`
- Modify: `src/repositories/ReplayRepository.{h,cpp}`
- Modify: `src/repositories/ReplayRepositoryInternal.h`
- Modify: `src/ResultRecordSummary.{h,cpp}`
- Modify: `src/ReplayRecordFilters.h`
- Modify: `src/view/ResultRecordListView.h`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Produces: `ListLegacyChartSummaries(const bms_parser::ChartMeta &, size_t)` and `ListLegacyCourseSummaries(const CourseReplayLookup &, size_t)`.
- Produces: `makeLegacyChartResultRecord` / `makeLegacyCourseResultRecord` with explicit legacy identities and Records-only capabilities.

- [ ] **Step 1: Write failing repository read tests**

After migrating the fixture, assert optional facts are preserved, malformed
facts stay `std::nullopt`, lookup uses stored SHA-256/MD5/path or course
key/legacy ID, order is newest-first, and the limit is enforced.

- [ ] **Step 2: Write failing projection and UI tests**

Create partial chart/course summaries and assert:

```cpp
const auto record = makeLegacyChartResultRecord(summary);
assert(record.capabilities == ResultRecordCapabilities{});
assert(record.legacyChart.has_value());
assert(!record.localReplayId().has_value());
assert(record.stableKey() == "lc:11");
```

Bind the record list row and assert unknown score/rank/lamp are rendered as an
unavailable label and the detail says `Legacy summary` rather than inventing a
gauge, option, or score maximum.

- [ ] **Step 3: Run RED**

```bash
cmake --build cmake-build-debug --target legacy_result_summary_tests result_record_summary_tests result_record_list_view_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(legacy_result_summary_tests|result_record_summary_tests|result_record_list_view_tests)$'
```

- [ ] **Step 4: Implement optional summary decoding and projection**

Decode only the two summary tables. Give legacy chart/course distinct identity
variants. Derive capability flags exclusively through
`replay::capabilitiesFor({.origin=LegacyChartSummary/LegacyCourseSummary})`.
Add presence flags or optionals to the generic row projection so unknown facts
never render/filter/sort as real zero values.

- [ ] **Step 5: Verify GREEN and commit**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R '^(legacy_result_summary_tests|result_record_summary_tests|result_record_list_view_tests|replay_record_filters_tests)$'
git add CMakeLists.txt src/CMakeLists.txt src/repositories/LegacyResultSummary.h \
  src/repositories/ReplayRepositoryLegacySummaries.cpp \
  src/repositories/ReplayRepository.h src/repositories/ReplayRepository.cpp \
  src/repositories/ReplayRepositoryInternal.h src/ResultRecordSummary.* \
  src/ReplayRecordFilters.h src/view/ResultRecordListView.h \
  tests/legacy_result_summary_tests.cpp tests/result_record_summary_tests.cpp \
  tests/result_record_list_view_tests.cpp tests/replay_record_filters_tests.cpp
git commit -m "feat: expose legacy records as summaries only"
```

---

### Task 4: Remove Legacy Producers and Pending-Score Adapters

**Files:**
- Modify: `tests/replay_repository_tests.cpp`
- Modify: `tests/application_startup_tests.cpp`
- Modify: `src/repositories/ReplayRepositoryRecords.cpp`
- Modify: `src/repositories/ReplayRepository.{h,cpp}`
- Modify: `src/repositories/ReplayRepositoryInternal.h`
- Modify: `src/context.h`
- Modify: `src/ApplicationResultRecovery.h`
- Modify: `src/ProfileSessionCoordinator.{h,cpp}`
- Modify: `src/scene/ResultScene.{h,cpp}`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: modern `ChartReplayPersistence::recoverAll`, chart persistence, and course persistence.
- Removes: legacy `SaveReplay`, `SaveCourseReplay`, `StageChartResult`, legacy pending-score acknowledgement/recovery, and course replay fallback behavior.

- [ ] **Step 1: Write failing raw-write cutover tests**

Assert a fresh database has none of the raw tables before and after modern
chart/course persistence. Exercise app recovery and ResultScene routing through
test dependencies and assert only modern recovery/persistence callbacks run.

- [ ] **Step 2: Run RED**

```bash
cmake --build cmake-build-debug --target replay_repository_tests application_startup_tests result_persistence_integration_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(replay_repository_tests|application_startup_tests|result_persistence_integration_tests)$'
```

- [ ] **Step 3: Delete raw mutation/hydration SQL and runtime routes**

Remove insert helpers for `replays`, events, touches, lane cover, course
replays/stages, legacy pending score writes, and their public/internal APIs.
Remove the old coordinator member and recovery call from `ApplicationContext`.
Keep modern recovery as the single application/profile-switch recovery path.

- [ ] **Step 4: Remove ResultScene fallbacks**

Delete `saveCourseReplay`, legacy course score/replay fallback, legacy chart
persistence retry, and receipt-to-`ReplayData` adaptation. A live attempt that
cannot construct a modern payload reports the modern failure; it never writes
a legacy row.

- [ ] **Step 5: Retire obsolete tests/targets and verify GREEN**

Remove configured targets whose sole contract is legacy row persistence or
event-derived reconstruction, retaining model tests still used by modern score
projection. Add focused modern equivalents before removing any coverage.

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R '^(replay_legacy_migration_tests|replay_repository_modern_chart_tests|replay_repository_modern_course_tests|chart_replay_persistence_tests|course_replay_persistence_tests|application_startup_tests)$'
```

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src tests
git commit -m "refactor: remove legacy replay persistence"
```

---

### Task 5: Cut Every Consumer Over to Summaries or Modern BRDs

**Files:**
- Modify: `src/scene/MainMenuScene.{h,cpp}`
- Modify: `src/scene/ChartViewerScene.{h,cpp}`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/ResultPresentationUtils.h`
- Modify: `src/ReplayVideoExporter.cpp`
- Modify: `src/ResultImageExporter.cpp`
- Modify: relevant scene/consumer tests

**Interfaces:**
- Consumes: legacy summary list APIs for Records and modern chart/course consumers for every replay-dependent action.
- Removes: every call that hydrates legacy `ReplayData`/`CourseReplayData` or reconstructs a result/ghost/pacemaker from legacy events.

- [ ] **Step 1: Strengthen action-selection tests**

Add legacy chart and course records to the selection matrix and assert no
action callback can dispatch even when IDs collide with modern or autoplay
identities. Add a modern verified record beside them and assert its existing
actions still dispatch.

- [ ] **Step 2: Run RED**

```bash
cmake --build cmake-build-debug --target result_record_summary_tests result_record_list_view_tests chart_replay_context_tests course_replay_context_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(result_record_summary_tests|result_record_list_view_tests|chart_replay_context_tests|course_replay_context_tests)$'
```

- [ ] **Step 3: Replace Records loading and action branches**

MainMenu loads legacy summaries only into Records, merges modern results and
remote rows, and treats autoplay separately. Remove legacy Watch, G-Battle,
View Result, video, share/delete, Retry Same, and IR functions. Action dispatch
must branch on typed modern identity plus capability, never on a positive
legacy integer ID.

- [ ] **Step 4: Remove legacy ghost and pacemaker reconstruction**

ChartViewer lists only verified modern BRDs as saved ghosts. GamePlay and
exporters may use static independently stored best-score targets, but must not
query legacy replay summaries or events for score progression.

- [ ] **Step 5: Verify and commit**

```bash
rg -n 'LoadReplay\(|LoadCourseReplay\(|LoadReplayResult\(|LoadLatestReplay\(' src
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(result_record_.*|chart_replay_.*|course_replay_.*|replay_capabilities_tests)$'
git add src tests
git commit -m "refactor: remove legacy replay consumers"
```

Expected `rg`: no runtime match.

---

### Task 6: Make IR Reconciliation and Manual Upload Snapshot-Only

**Files:**
- Modify: `src/ir/IrScoreReconciliation.{h,cpp}`
- Modify: `src/ir/IrUploadCandidates.{h,cpp}`
- Modify: `src/repositories/ReplayRepositoryIrRemoteScores.cpp`
- Modify: `src/repositories/ReplayRepositoryIrOutbox.cpp`
- Modify: `src/repositories/ReplayRepositoryModernResults.cpp`
- Modify: `src/scene/IrUploadsScene.{h,cpp}`
- Modify: `src/view/IrUploadCandidateListView.{h,cpp}`
- Modify: IR repository/model/service/controller tests

**Interfaces:**
- Produces: modern reconciliation candidates with `modernChartResultId` and stored snapshot-backed manual candidates with stable attempt IDs.
- Guarantees: no legacy summary can authorize a new IR receipt or submission; historical legacy receipts remain durable evidence only.

- [ ] **Step 1: Write failing modern-owner reconciliation tests**

Stage a modern result with snapshot and no BRD, reconcile it with a matching
remote score, and assert the new receipt has `replayId==0` and the exact
`modernChartResultId`. Add a legacy summary/receipt and assert it is preserved
but never returned as a local reconciliation candidate.

- [ ] **Step 2: Write failing manual candidate tests**

Assert a stored modern snapshot remains selectable and enqueueable after its
BRD is missing/user-deleted. Assert a legacy summary with identical hashes and
score is omitted. Selection keys are canonical attempt IDs, not replay IDs.

- [ ] **Step 3: Run RED**

```bash
cmake --build cmake-build-debug --target ir_score_reconciliation_tests ir_upload_candidates_tests replay_repository_tests ir_submission_service_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(ir_score_reconciliation_tests|ir_upload_candidates_tests|replay_repository_tests|ir_submission_service_tests)$'
```

- [ ] **Step 4: Generalize owner identity and modernize repository reads**

Replace `IrLocalReceiptCandidate::replayId` with exact modern result ownership.
Build candidates from `modern_chart_results`, `ir_submission_snapshots`,
`ir_outbox`, and `ir_submission_receipts` under one read snapshot. Validate
result/snapshot/receipt/outbox agreement before returning a candidate.

- [ ] **Step 5: Switch the uploads scene to stored snapshots**

Project display facts from the modern result and use the already validated
snapshot submission during preparation. Remove chart hydration,
`ResultRecallBuilder`, and historical replay-based IR reconstruction.

- [ ] **Step 6: Verify and commit**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R '^(ir_.*|modern_result_recall_tests|replay_repository_modern_chart_tests)$'
git add src tests CMakeLists.txt
git commit -m "refactor: make saved-result IR snapshot-only"
```

---

### Task 7: Preserve Summary History Across Profiles and Close the Contract

**Files:**
- Modify: `tests/player_profile_manager_tests.cpp`
- Modify: `tests/profile_archive_tests.cpp`
- Modify: `src/PlayerProfileManager.cpp`
- Modify: `src/ProfileArchive.cpp`
- Modify: `docs/replay/file-replay-contract-matrix.md`

**Interfaces:**
- Consumes: v14 schema validation, legacy summary tables, and agreed modern replay inventory.
- Guarantees: summary-only history duplicates/archives without a BRD and invalid modern ownership still fails atomically.

- [ ] **Step 1: Add failing profile history tests**

Migrate a temporary source profile containing chart/course legacy headers,
duplicate it, export/import it, and assert both summary tables and partial flags
survive while no raw table or legacy replay member exists. Keep the existing
verified/missing/user-deleted/corrupt/mismatched modern BRD matrix.

- [ ] **Step 2: Run RED**

```bash
cmake --build cmake-build-debug --target player_profile_manager_tests profile_archive_tests profile_switch_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(foundation_profile_manager|foundation_profile_archive|foundation_profile_switch)$'
```

- [ ] **Step 3: Fix only shared profile/schema boundaries**

Make snapshot/open validation require exact v14 summaries and agreed modern
inventory. Do not add a file exception for legacy history; summaries have no
file reference by design.

- [ ] **Step 4: Update the contract matrix and commit**

Mark legacy summary schema, migration, Records projection, and modern-only IR
and consumer routes active; remove the temporary legacy bridge row.

```bash
git add src tests docs/replay/file-replay-contract-matrix.md
git commit -m "docs: complete the summary-only replay cutover"
```

---

### Task 8: Slice Review, Full Verification, and Publication

**Files:**
- Review all Slice 7 files and the complete branch diff against `origin/develop`.

**Interfaces:**
- Produces: a verified, pushed branch and ready non-draft PR targeting `develop`.

- [ ] **Step 1: Run focused contract and audit commands**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j 6 -R '(replay|result|ir|profile|migration)'
rg -n 'INSERT INTO (replays|replay_events|replay_touch_samples|replay_lane_cover_events|course_replays|course_replay_stages)|SELECT .* FROM (replay_events|replay_touch_samples|replay_lane_cover_events|course_replay_stages)' src
rg -n 'LoadReplay\(|LoadCourseReplay\(|LoadReplayResult\(|LoadLatestReplay\(|SaveReplay\(|SaveCourseReplay\(' src
```

Expected audits: no runtime raw write, detail read, legacy hydration, or legacy
mutation match. Schema migration may contain only `DROP TABLE` references to
the retired raw tables.

- [ ] **Step 2: Test a temporary copy of any available real profiles**

Copy `~/Downloads/profiles` to a `mktemp -d` directory using read-only source
access. Run schema migration against copied replay databases only. Record
successes and malformed structural blockers; never write the source directory.

- [ ] **Step 3: Run full verification**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j 6
cmake --build cmake-build-debug --target main -j 6
scripts/ios_firebase_deploy.sh --build-only
```

- [ ] **Step 4: Review the complete diff**

```bash
git diff --check
git diff --stat origin/develop...HEAD
git diff origin/develop...HEAD
```

Search for the same setup, limits, identity agreement, result agreement, file
ownership, course continuation, summary capability, or migration rule being
implemented differently across branches. Fix findings at the shared boundary
with a regression test and commit each fix.

- [ ] **Step 5: Push and open a ready PR**

```bash
git push -u origin feature/file-based-replays-v2
gh pr create --base develop --head feature/file-based-replays-v2 \
  --title "Implement contract-first Beatoraja file replays" \
  --body "Implements the approved seven-slice contract-first BRD architecture. See the committed design and plan documents for architecture, atomic v14 migration and rollback, legacy safety, compatibility, and the complete verification record."
```

The description covers architecture, v14 atomic migration and rollback,
legacy safety, BRD compatibility, file actions/profile transfer, test results,
desktop/iOS verification, and the fact that PR #82 was reference-only.

- [ ] **Step 6: Wait for initial checks and fix in-scope failures**

Use `gh pr checks --watch` or the GitHub checks tools. Reproduce each in-scope
failure locally, add or strengthen a regression test, commit, push, and wait
again. Leave the PR open, ready for review, and non-draft.
