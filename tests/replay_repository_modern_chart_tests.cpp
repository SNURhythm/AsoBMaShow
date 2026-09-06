#include "repositories/ReplayRepository.h"

#include "ir/IrSubmissionSnapshot.h"
#include "repositories/SqliteRAII.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("asobmashow-modern-chart-repository-" + std::to_string(stamp));
    assert(std::filesystem::create_directories(path));
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

SqliteConnectionHandle openDatabase(const std::filesystem::path &path) {
  sqlite3 *raw = nullptr;
  assert(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
  return SqliteConnectionHandle(raw);
}

void exec(sqlite3 *database, const std::string &sql) {
  char *error = nullptr;
  if (sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error) !=
      SQLITE_OK) {
    std::cerr << (error != nullptr ? error : "SQLite failure") << '\n';
    sqlite3_free(error);
    std::abort();
  }
}

int queryInt(sqlite3 *database, const std::string &sql) {
  SqliteStatementHandle statement;
  assert(prepareSqliteStatement(database, sql, statement) == SQLITE_OK);
  assert(sqlite3_step(statement.get()) == SQLITE_ROW);
  return sqlite3_column_int(statement.get(), 0);
}

bool tableExists(sqlite3 *database, std::string_view table) {
  SqliteStatementHandle statement;
  assert(prepareSqliteStatement(
             database,
             "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
             statement) == SQLITE_OK);
  assert(sqlite3_bind_text(statement.get(), 1, table.data(),
                           static_cast<int>(table.size()),
                           SQLITE_TRANSIENT) == SQLITE_OK);
  return sqlite3_step(statement.get()) == SQLITE_ROW;
}

bool columnExists(sqlite3 *database, std::string_view table,
                  std::string_view column) {
  SqliteStatementHandle statement;
  assert(prepareSqliteStatement(
             database, "SELECT 1 FROM pragma_table_info(?) WHERE name=?",
             statement) == SQLITE_OK);
  assert(sqlite3_bind_text(statement.get(), 1, table.data(),
                           static_cast<int>(table.size()),
                           SQLITE_TRANSIENT) == SQLITE_OK);
  assert(sqlite3_bind_text(statement.get(), 2, column.data(),
                           static_cast<int>(column.size()),
                           SQLITE_TRANSIENT) == SQLITE_OK);
  return sqlite3_step(statement.get()) == SQLITE_ROW;
}

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

std::string attemptId(int suffix) {
  char value[37]{};
  std::snprintf(value, sizeof(value), "123e4567-e89b-42d3-a456-426614174%03d",
                suffix);
  return value;
}

result_persistence::ModernChartResult result(int suffix, char sha = 'a') {
  result_persistence::ModernChartResult value;
  value.attemptId = attemptId(suffix);
  value.score.chartPath = "library/chart.bms";
  value.score.chartMd5 = repeated(sha == 'a' ? 'b' : sha, 32);
  value.score.chartSha256 = repeated(sha, 64);
  value.score.chartTitle = "Title";
  value.score.chartArtist = "Artist";
  value.score.longNoteMode = 1;
  value.score.score = 7;
  value.score.maxScore = 10;
  value.score.maxCombo = 4;
  value.score.comboBreak = 1;
  value.score.pGreat = 3;
  value.score.great = 1;
  value.score.good = 1;
  value.score.finalGauge = 82.5F;
  value.score.clearType = kClearTypeNormalClearRank;
  value.score.provenance = ScoreProvenance::Legacy();
  value.keyMode = 7;
  value.adoptedGaugeType = GaugeType::Normal;
  value.adoptedGaugeHistory = {20.0F, 48.5F, 82.5F};
  value.playedAtUnixMillis = 1'700'000'000'000LL + suffix;
  value.resultFingerprint = result_persistence::modernResultFingerprint(value);
  std::string diagnostic;
  assert(result_persistence::validateModernChartResult(value, diagnostic));
  return value;
}

ir::IrSubmissionSnapshot
snapshot(const result_persistence::ModernChartResult &value) {
  std::string diagnostic;
  auto captured = ir::captureIrSubmissionSnapshot(value, diagnostic);
  assert(captured.has_value());
  return *captured;
}

ir::IrOutboxDraft draft(const ir::IrSubmissionSnapshot &value) {
  std::string diagnostic;
  const auto payload = ir::serializeIrSubmissionSnapshot(value, diagnostic);
  assert(payload.has_value());
  return {.providerId = "fake",
          .attemptId = value.submission.attemptId,
          .chartMd5 = value.submission.chartMd5,
          .chartSha256 = value.submission.chartSha256,
          .payloadJson = *payload,
          .rulesetProof = {.rulesetId = "test-rules",
                           .rulesetRevision = 1,
                           .validationFingerprint = repeated('d', 64)},
          .createdAtUnixMillis = value.submission.playedAtUnixMillis};
}

ModernReplayFileAttachment
attachment(const ModernReplayPathReservation &reservation, char hash = 'c') {
  return {.identity = reservation.identity,
          .metadata = {.relativePath = reservation.identity.relativePath,
                       .sha256 = repeated(hash, 64),
                       .compressedSize = 123,
                       .codecVersion =
                           replay::BeatorajaReplayCodec::kCodecVersion}};
}

void testCurrentSchemaAddsSelectorMetricsTable() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replay.db";
  {
    ReplayRepository repository(databasePath);
    assert(repository.EnsureSchema());
  }
  {
    auto database = openDatabase(databasePath);
    exec(database.get(), "DROP TABLE modern_chart_score_metrics");
    assert(queryInt(database.get(), "PRAGMA user_version") ==
           ReplayRepository::kCurrentSchemaVersion);
  }
  {
    ReplayRepository repository(databasePath);
    assert(repository.EnsureSchema());
  }
  auto database = openDatabase(databasePath);
  assert(tableExists(database.get(), "modern_chart_score_metrics"));
}

void testSchemaReservationAtomicStageAndExactRetry() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replay.db";
  ReplayRepository repository(databasePath);
  assert(repository.GetResolvedProfileRoot() == temporary.path);
  assert(repository.EnsureSchema());

  auto database = openDatabase(databasePath);
  assert(queryInt(database.get(), "PRAGMA user_version") ==
         ReplayRepository::kCurrentSchemaVersion);
  for (const std::string_view table :
       {"modern_chart_results", "modern_replay_files",
        "modern_replay_file_reservations", "modern_replay_stem_sequences",
        "ir_submission_snapshots", "modern_pending_chart_score_writes",
        "modern_chart_score_metrics", "ir_submission_receipts"}) {
    assert(tableExists(database.get(), table));
  }
  assert(columnExists(database.get(), "ir_submission_receipts",
                      "modern_chart_result_id"));
  assert(columnExists(database.get(), "modern_replay_files",
                      "user_deleted"));
  for (const std::string_view table :
       {"replays", "replay_events", "replay_touch_samples",
        "replay_lane_cover_events", "course_replays",
        "course_replay_stages"}) {
    assert(!tableExists(database.get(), table));
  }

  const auto completed = result(1);
  const auto savedSnapshot = snapshot(completed);
  const auto outboxDraft = draft(savedSnapshot);
  const auto reserved = repository.ReserveModernReplayPath(
      completed.attemptId, completed.score.chartSha256,
      completed.playedAtUnixMillis);
  assert(reserved.status == ModernReplayReservationStatus::Reserved &&
         reserved.reservation.has_value() &&
         reserved.reservation->identity.historyIndex == 0 &&
         reserved.reservation->identity.relativePath ==
             std::filesystem::path("replay") /
                 (completed.score.chartSha256 + ".brd"));
  const auto reservationInventory =
      repository.ListModernReplayPathReservations();
  assert(reservationInventory.status ==
             ModernReplayFileInventoryStatus::Loaded &&
         reservationInventory.reservations ==
             std::vector{*reserved.reservation});
  const auto repeatedReservation = repository.ReserveModernReplayPath(
      completed.attemptId, completed.score.chartSha256,
      completed.playedAtUnixMillis);
  assert(repeatedReservation.status ==
             ModernReplayReservationStatus::AlreadyReserved &&
         repeatedReservation.reservation == reserved.reservation);
  assert(repository
             .ReserveModernReplayPath(completed.attemptId,
                                      completed.score.chartSha256,
                                      completed.playedAtUnixMillis + 1)
             .status == ModernReplayReservationStatus::IntegrityConflict);
  assert(repository
             .ReserveModernReplayPath(completed.attemptId, repeated('e', 64),
                                      completed.playedAtUnixMillis)
             .status == ModernReplayReservationStatus::IntegrityConflict);

  const auto file = attachment(*reserved.reservation);
  const replay::ReplayFileOwnershipReceipt ownership{
      .attemptToken = completed.attemptId, .metadata = file.metadata};
  const auto recorded = repository.RecordModernReplayInstallIntent(
      *reserved.reservation, ownership);
  assert(recorded.status == ModernReplayOwnershipRecordStatus::Recorded &&
         recorded.reservation &&
         recorded.reservation->ownedFile == file.metadata);
  const auto repeatedOwnership =
      repository.RecordModernReplayInstallIntent(*recorded.reservation,
                                                  ownership);
  assert(repeatedOwnership.status ==
             ModernReplayOwnershipRecordStatus::AlreadyRecorded &&
         repeatedOwnership.reservation == recorded.reservation);
  auto futureFile = file;
  ++futureFile.metadata.codecVersion;
  assert(repository
             .StageModernChartResult(completed, savedSnapshot, futureFile,
                                     std::vector{outboxDraft})
             .status == ModernChartStageStatus::Invalid);
  assert(queryInt(database.get(),
                  "SELECT COUNT(*) FROM modern_chart_results") == 0 &&
         queryInt(database.get(),
                  "SELECT COUNT(*) FROM modern_replay_file_reservations") ==
             1);
  auto mismatchedOwnedFile = file;
  mismatchedOwnedFile.metadata.sha256 = repeated('9', 64);
  assert(repository
             .StageModernChartResult(completed, savedSnapshot,
                                     mismatchedOwnedFile,
                                     std::vector{outboxDraft})
             .status == ModernChartStageStatus::IntegrityConflict);
  const std::vector drafts{outboxDraft};
  const auto staged =
      repository.StageModernChartResult(completed, savedSnapshot, file, drafts);
  assert(staged.status == ModernChartStageStatus::Staged &&
         staged.receipt.has_value() && staged.receipt->resultId > 0);
  assert(
      queryInt(database.get(), "SELECT COUNT(*) FROM modern_chart_results") ==
          1 &&
      queryInt(database.get(), "SELECT COUNT(*) FROM modern_replay_files") ==
          1 &&
      queryInt(database.get(),
               "SELECT COUNT(*) FROM ir_submission_snapshots") == 1 &&
      queryInt(database.get(),
               "SELECT COUNT(*) FROM modern_pending_chart_score_writes") == 1 &&
      queryInt(database.get(),
               "SELECT COUNT(*) FROM ir_outbox WHERE local_result_ready=1") ==
          1 &&
      queryInt(database.get(),
               "SELECT COUNT(*) FROM modern_replay_file_reservations") == 0);
  assert(repository.ListModernReplayPathReservations().reservations.empty());
  for (const std::string_view table :
       {"replays", "replay_events", "replay_touch_samples",
        "replay_lane_cover_events", "course_replays",
        "course_replay_stages"}) {
    assert(!tableExists(database.get(), table));
  }

  const auto loaded =
      repository.LoadModernChartResultByAttempt(completed.attemptId);
  assert(loaded.status == ModernChartResultReadStatus::Loaded &&
         loaded.record.has_value() &&
         loaded.record->result.resultId == staged.receipt->resultId &&
         loaded.record->result.score == completed.score &&
         loaded.record->result.resultFingerprint ==
             completed.resultFingerprint &&
         loaded.record->replayFile.has_value() &&
         loaded.record->replayFile->identity == file.identity &&
         loaded.record->replayFile->metadata == file.metadata);
  const auto loadedSnapshot =
      repository.LoadModernIrSubmissionSnapshot(completed.attemptId);
  assert(loadedSnapshot.status == ModernIrSnapshotReadStatus::Loaded &&
         loadedSnapshot.snapshot == savedSnapshot);

  const auto deleted = repository.MarkModernReplayFileUserDeleted(
      ModernReplayOwnerKind::ChartResult, completed.attemptId,
      *loaded.record->replayFile);
  assert(deleted.status == ModernReplayFileMutationStatus::Changed);
  const auto afterDelete =
      repository.LoadModernChartResultByAttempt(completed.attemptId);
  assert(afterDelete.status == ModernChartResultReadStatus::Loaded &&
         afterDelete.record && afterDelete.record->replayFile &&
         afterDelete.record->replayFile->userDeleted &&
         afterDelete.record->result == loaded.record->result);
  const auto snapshotAfterDelete =
      repository.LoadModernIrSubmissionSnapshot(completed.attemptId);
  assert(snapshotAfterDelete.status == ModernIrSnapshotReadStatus::Loaded &&
         snapshotAfterDelete.snapshot == savedSnapshot);
  assert(repository
             .MarkModernReplayFileUserDeleted(
                 ModernReplayOwnerKind::ChartResult, completed.attemptId,
                 *loaded.record->replayFile)
             .status == ModernReplayFileMutationStatus::AlreadyChanged);

  auto wrongReference = *loaded.record->replayFile;
  wrongReference.metadata.sha256 = repeated('9', 64);
  assert(repository
             .MarkModernReplayFileUserDeleted(
                 ModernReplayOwnerKind::ChartResult, completed.attemptId,
                 wrongReference)
             .status == ModernReplayFileMutationStatus::IntegrityConflict);

  const auto inventory = repository.ListModernReplayFileReferences();
  assert(inventory.status == ModernReplayFileInventoryStatus::Loaded &&
         inventory.entries.size() == 1 &&
         inventory.entries.front().owner ==
             ModernReplayOwnerKind::ChartResult &&
         inventory.entries.front().attemptId == completed.attemptId &&
         inventory.entries.front().reference.userDeleted);

  const auto retried =
      repository.StageModernChartResult(completed, savedSnapshot, file, drafts);
  assert(retried.status == ModernChartStageStatus::AlreadyStaged &&
         retried.receipt == staged.receipt);
  auto conflictingFile = file;
  conflictingFile.metadata.sha256 = repeated('f', 64);
  assert(repository
             .StageModernChartResult(completed, savedSnapshot, conflictingFile,
                                     drafts)
             .status == ModernChartStageStatus::IntegrityConflict);

  const auto outbox =
      repository.LoadIrOutbox(outboxDraft.providerId, completed.attemptId);
  const auto queuedRecords = repository.ListIrUploadRecords(
      outboxDraft.providerId, "https://example.invalid");
  assert(queuedRecords.status == ir::IrUploadRecordReadStatus::Loaded &&
         queuedRecords.records.size() == 1 &&
         queuedRecords.records.front().attemptId == completed.attemptId &&
         queuedRecords.records.front().resolvedState() ==
             ir::IrRecordState::Queued);
  assert(outbox.status == ir::IrOutboxReadStatus::Found && outbox.entry &&
         repository
                 .ClaimIrOutbox(outbox.entry->id, ir::IrOutboxState::Pending,
                                completed.playedAtUnixMillis + 1)
                 .status == ir::IrOutboxClaimStatus::Claimed);
  const auto delivered = repository.ApplyIrOutboxDelivery({
      .rowId = outbox.entry->id,
      .nextState = ir::IrOutboxState::Succeeded,
      .updatedAtUnixMillis = completed.playedAtUnixMillis + 2,
      .completedAtUnixMillis = completed.playedAtUnixMillis + 2,
      .successfulReceipt =
          ir::IrSuccessfulReceiptDraft{
              .serverOrigin = "https://example.invalid",
              .remoteUserId = 42,
              .remoteScoreId = "remote-score",
              .confirmedAtUnixMillis = completed.playedAtUnixMillis + 2,
          },
  });
  assert(delivered.status == ir::IrOutboxMutationStatus::Updated);
  const auto receipt = repository.LoadIrSubmissionReceipt(
      outboxDraft.providerId, "https://example.invalid", completed.attemptId);
  assert(receipt.status == ir::IrReceiptReadStatus::Found && receipt.receipt &&
         receipt.receipt->replayId == 0 &&
         receipt.receipt->modernChartResultId == staged.receipt->resultId);
  const auto uploadedRecords = repository.ListIrUploadRecords(
      outboxDraft.providerId, "https://example.invalid");
  assert(uploadedRecords.status == ir::IrUploadRecordReadStatus::Loaded &&
         uploadedRecords.records.size() == 1 &&
         uploadedRecords.records.front().resolvedState(
             ir::IrRecordActivity::Polling) == ir::IrRecordState::Uploaded);

  char *ownershipError = nullptr;
  const std::string ambiguousOwner =
      "INSERT INTO ir_submission_receipts(provider_id,server_origin,replay_id,"
      "modern_chart_result_id,attempt_id,chart_sha256,confirmation_source,"
      "confirmed_at_ms) VALUES('fake2','https://example.invalid',1," +
      std::to_string(staged.receipt->resultId) + ",'" + completed.attemptId +
      "','" + completed.score.chartSha256 + "',0,1)";
  assert(sqlite3_exec(database.get(), ambiguousOwner.c_str(), nullptr, nullptr,
                      &ownershipError) != SQLITE_OK);
  sqlite3_free(ownershipError);

  auto newer = result(5);
  newer.playedAtUnixMillis = completed.playedAtUnixMillis + 100;
  newer.resultFingerprint = result_persistence::modernResultFingerprint(newer);
  assert(
      repository.StageModernChartResult(newer, std::nullopt, std::nullopt, {})
          .status == ModernChartStageStatus::Staged);
  const auto history =
      repository.ListModernChartResults(completed.score.chartSha256, 2);
  assert(history.status == ModernChartHistoryReadStatus::Loaded &&
         history.records.size() == 2 &&
         history.records[0].result.attemptId == newer.attemptId &&
         history.records[1].result.attemptId == completed.attemptId);
  assert(repository.ListModernChartResults(completed.score.chartSha256, 0)
             .status == ModernChartHistoryReadStatus::Invalid);

  const auto wrongIdentity = result(6, 'f');
  const auto wrongReservation = repository.ReserveModernReplayPath(
      wrongIdentity.attemptId, repeated('e', 64),
      wrongIdentity.playedAtUnixMillis);
  assert(wrongReservation.reservation &&
         repository
                 .StageModernChartResult(
                     wrongIdentity, std::nullopt,
                     attachment(*wrongReservation.reservation), {})
                 .status == ModernChartStageStatus::Invalid);

  const auto next = result(2, 'e');
  const auto nextReservation = repository.ReserveModernReplayPath(
      next.attemptId, completed.score.chartSha256, next.playedAtUnixMillis);
  assert(nextReservation.status == ModernReplayReservationStatus::Reserved &&
         nextReservation.reservation->identity.historyIndex == 1);

  const auto cleared = repository.ClearIrAccountEvidence(
      outboxDraft.providerId, "https://example.invalid");
  assert(cleared.status == ir::IrOutboxMutationStatus::Updated &&
         queryInt(database.get(), "SELECT COUNT(*) FROM ir_outbox") == 0 &&
         queryInt(database.get(),
                  "SELECT COUNT(*) FROM ir_submission_receipts") == 0 &&
         repository.LoadModernChartResult(staged.receipt->resultId).status ==
             ModernChartResultReadStatus::Loaded);
}

void testRollbackAndReplayOptionality() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replay.db";
  ReplayRepository repository(databasePath);
  assert(repository.EnsureSchema());
  auto database = openDatabase(databasePath);

  const auto completed = result(3, 'c');
  const auto savedSnapshot = snapshot(completed);
  const std::vector drafts{draft(savedSnapshot)};
  const auto reserved = repository.ReserveModernReplayPath(
      completed.attemptId, completed.score.chartSha256,
      completed.playedAtUnixMillis);
  assert(reserved.reservation.has_value());
  exec(database.get(),
       "CREATE TRIGGER fail_modern_snapshot BEFORE INSERT ON "
       "ir_submission_snapshots BEGIN SELECT RAISE(ABORT,'injected'); END");
  const auto failed = repository.StageModernChartResult(
      completed, savedSnapshot, attachment(*reserved.reservation), drafts);
  assert(failed.status == ModernChartStageStatus::StorageFailure);
  for (const std::string_view table :
       {"modern_chart_results", "modern_replay_files",
        "ir_submission_snapshots", "modern_pending_chart_score_writes"}) {
    assert(queryInt(database.get(),
                    "SELECT COUNT(*) FROM " + std::string(table)) == 0);
  }
  assert(queryInt(database.get(),
                  "SELECT COUNT(*) FROM modern_replay_file_reservations") == 1);
  exec(database.get(), "DROP TRIGGER fail_modern_snapshot");

  const auto resultOnly = result(4, 'd');
  const auto staged = repository.StageModernChartResult(
      resultOnly, std::nullopt, std::nullopt, {});
  assert(staged.status == ModernChartStageStatus::Staged);
  const auto loaded =
      repository.LoadModernChartResultByAttempt(resultOnly.attemptId);
  assert(loaded.status == ModernChartResultReadStatus::Loaded &&
         loaded.record.has_value() && !loaded.record->replayFile.has_value());
  assert(
      repository.LoadModernIrSubmissionSnapshot(resultOnly.attemptId).status ==
      ModernIrSnapshotReadStatus::NotFound);
  assert(!tableExists(database.get(), "replay_events"));

  const auto snapshotOnly = result(10, 'e');
  const auto storedSnapshot = snapshot(snapshotOnly);
  const auto snapshotStaged = repository.StageModernChartResult(
      snapshotOnly, storedSnapshot, std::nullopt, {});
  assert(snapshotStaged.status == ModernChartStageStatus::Staged &&
         snapshotStaged.receipt);
  exec(database.get(),
       "INSERT INTO legacy_chart_result_summaries(legacy_replay_id,"
       "chart_md5,chart_sha256,final_score,clear_type,partial) VALUES(901,'" +
           snapshotOnly.score.chartMd5 + "','" +
           snapshotOnly.score.chartSha256 + "'," +
           std::to_string(snapshotOnly.score.score) + "," +
           std::to_string(snapshotOnly.score.clearType) + ",0)");

  const auto manual = repository.ListIrUploadCandidates(
      "tachi", "https://boku.tachi.ac");
  assert(manual.status == ir::IrUploadCandidateReadStatus::Loaded &&
         manual.candidates.empty());
  const auto records = repository.ListIrUploadRecords(
      "tachi", "https://boku.tachi.ac");
  assert(records.status == ir::IrUploadRecordReadStatus::Loaded &&
         records.records.size() == 1 &&
         records.records.front().modernChartResultId ==
             snapshotStaged.receipt->resultId &&
         records.records.front().attemptId == snapshotOnly.attemptId &&
         records.records.front().resolvedState() ==
             ir::IrRecordState::Hidden);

  const auto reconciliation = repository.LoadIrReconciliationCandidates(
      "tachi", "https://boku.tachi.ac");
  assert(reconciliation.status ==
             ir::IrReconciliationReadOutcome::Status::Loaded &&
         reconciliation.candidates.size() == 1 &&
         reconciliation.candidates.front().modernChartResultId ==
             snapshotStaged.receipt->resultId &&
         reconciliation.candidates.front().attemptId ==
             snapshotOnly.attemptId &&
         !reconciliation.candidates.front().eligible);

  const auto newerSnapshotResult = result(11, 'f');
  const auto newerSnapshot = snapshot(newerSnapshotResult);
  const auto newerSnapshotStage = repository.StageModernChartResult(
      newerSnapshotResult, newerSnapshot, std::nullopt, {});
  assert(newerSnapshotStage.status == ModernChartStageStatus::Staged &&
         newerSnapshotStage.receipt);
  const auto newestPage = repository.ListIrUploadRecords(
      "tachi", "https://boku.tachi.ac", std::nullopt, 1);
  assert(newestPage.status == ir::IrUploadRecordReadStatus::Loaded &&
         newestPage.records.size() == 1 &&
         newestPage.records.front().attemptId == newerSnapshotResult.attemptId &&
         newestPage.nextBeforeModernChartResultId.has_value());
  const auto olderPage = repository.ListIrUploadRecords(
      "tachi", "https://boku.tachi.ac",
      newestPage.nextBeforeModernChartResultId, 1);
  assert(olderPage.status == ir::IrUploadRecordReadStatus::Loaded &&
         olderPage.records.size() == 1 &&
         olderPage.records.front().attemptId == snapshotOnly.attemptId &&
         !olderPage.nextBeforeModernChartResultId.has_value());
}

void testReservationReleaseAndModernPendingLifecycle() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path / "replay.db");
  assert(repository.EnsureSchema());
  const auto completed = result(7, 'e');
  const auto reserved = repository.ReserveModernReplayPath(
      completed.attemptId, completed.score.chartSha256,
      completed.playedAtUnixMillis);
  assert(reserved.reservation);
  auto wrong = *reserved.reservation;
  wrong.createdAtUnixMillis += 1;
  assert(repository.ReleaseModernReplayPathReservation(wrong).status ==
         ModernReplayReservationReleaseStatus::IntegrityConflict);
  assert(repository.ReleaseModernReplayPathReservation(*reserved.reservation)
             .status == ModernReplayReservationReleaseStatus::Released);
  assert(repository.ReleaseModernReplayPathReservation(*reserved.reservation)
             .status == ModernReplayReservationReleaseStatus::NotFound);
  const auto advanced = repository.ReserveModernReplayPath(
      completed.attemptId, completed.score.chartSha256,
      completed.playedAtUnixMillis);
  assert(advanced.reservation &&
         advanced.reservation->identity.historyIndex == 1);

  const auto staged = repository.StageModernChartResult(
      completed, std::nullopt, attachment(*advanced.reservation), {}, 12'345);
  assert(staged.status == ModernChartStageStatus::Staged && staged.receipt);
  const auto pending =
      repository.LoadPendingModernChartScore(completed.attemptId);
  assert(pending.status == result_persistence::PendingReadStatus::Found &&
         pending.value && pending.value->replayId == 0 &&
         pending.value->modernResultId == staged.receipt->resultId &&
         pending.value->hasExactlyOneOwner() &&
         pending.value->score == completed.score &&
         pending.value->averageJudgeMicros == 12'345);
  const auto batch = repository.ListPendingModernChartScores();
  assert(batch.storageAvailable && batch.entries.size() == 1 &&
         batch.entries.front().status ==
             result_persistence::PendingReadStatus::Found &&
         batch.entries.front().value &&
         batch.entries.front().value->attemptId == pending.value->attemptId &&
         batch.entries.front().value->modernResultId ==
             pending.value->modernResultId &&
         batch.entries.front().value->score == pending.value->score &&
         batch.entries.front().value->averageJudgeMicros == 12'345);
  assert(repository
             .StageModernChartResult(completed, std::nullopt,
                                     attachment(*advanced.reservation), {},
                                     12'345)
             .status == ModernChartStageStatus::AlreadyStaged);
  assert(repository
             .StageModernChartResult(completed, std::nullopt,
                                     attachment(*advanced.reservation), {},
                                     12'346)
             .status == ModernChartStageStatus::IntegrityConflict);
  assert(repository
             .RecordPendingModernChartScoreRecoveryAttempt(
                 completed.attemptId,
                 result_persistence::RecoveryAttemptKind::StorageFailure)
             .status == result_persistence::RecoveryMarkStatus::Recorded);
  assert(repository
             .AcknowledgePendingModernChartScore(completed.attemptId,
                                                 staged.receipt->resultId)
             .status == result_persistence::AcknowledgeStatus::Acknowledged);
  assert(repository.LoadPendingModernChartScore(completed.attemptId).status ==
         result_persistence::PendingReadStatus::NotFound);
  assert(repository
             .AcknowledgePendingModernChartScore(completed.attemptId,
                                                 staged.receipt->resultId)
             .status ==
         result_persistence::AcknowledgeStatus::AlreadyAcknowledged);
}

void testIrSourceHistoryOverGlobalBoundReturnsKeysetPages() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replay.db";
  ReplayRepository repository(databasePath);
  assert(repository.EnsureSchema());
  const auto seed = result(12, '1');
  const auto seedStage = repository.StageModernChartResult(
      seed, snapshot(seed), std::nullopt, {});
  assert(seedStage.status == ModernChartStageStatus::Staged &&
         seedStage.receipt);
  repository.Shutdown();

  auto database = openDatabase(databasePath);
  exec(database.get(),
       "WITH RECURSIVE seq(value) AS (SELECT 1 UNION ALL SELECT value+1 "
       "FROM seq WHERE value<" +
           std::to_string(ir::kMaximumIrUploadCandidateRows) +
           ") INSERT INTO modern_chart_results("
           "attempt_id,chart_path,chart_md5,chart_sha256,chart_title,"
           "chart_artist,long_note_mode,score,max_score,max_combo,combo_break,"
           "p_great,great,good,bad,poor,k_poor,fast,slow,final_gauge,"
           "clear_type,key_mode,adopted_gauge_type,gauge_history_json,"
           "judgement_timing_json,provenance_json,result_fingerprint,"
           "played_at_unix_ms) SELECT "
           "printf('20000000-0000-4000-8000-%012x',value),chart_path,"
           "chart_md5,chart_sha256,chart_title,chart_artist,long_note_mode,"
           "score,max_score,max_combo,combo_break,p_great,great,good,bad,"
           "poor,k_poor,fast,slow,final_gauge,clear_type,key_mode,"
           "adopted_gauge_type,gauge_history_json,judgement_timing_json,"
           "provenance_json,result_fingerprint,played_at_unix_ms+value "
           "FROM modern_chart_results,seq WHERE id=" +
           std::to_string(seedStage.receipt->resultId));
  exec(database.get(),
       "INSERT INTO ir_submission_snapshots(modern_chart_result_id,attempt_id,"
       "schema_version,payload_json,fingerprint) SELECT cloned.id,"
       "cloned.attempt_id,source.schema_version,source.payload_json,"
       "source.fingerprint FROM modern_chart_results cloned JOIN "
       "ir_submission_snapshots source ON source.modern_chart_result_id=" +
           std::to_string(seedStage.receipt->resultId) +
           " WHERE cloned.id!=" +
           std::to_string(seedStage.receipt->resultId));
  database.reset();

  const auto newest = result(13, '2');
  const auto newestStage = repository.StageModernChartResult(
      newest, snapshot(newest), std::nullopt, {});
  assert(newestStage.status == ModernChartStageStatus::Staged &&
         newestStage.receipt);

  const auto records = repository.ListIrUploadRecords(
      "tachi", "https://boku.tachi.ac");
  assert(records.status == ir::IrUploadRecordReadStatus::Loaded &&
         records.records.size() == 1 &&
         records.records.front().attemptId == newest.attemptId &&
         records.nextBeforeModernChartResultId.has_value());
  const auto candidates = repository.ListIrUploadCandidates(
      "tachi", "https://boku.tachi.ac", std::nullopt, 1);
  assert(candidates.status == ir::IrUploadCandidateReadStatus::Loaded &&
         candidates.nextBeforeModernChartResultId.has_value());
  const auto reconciliation = repository.LoadIrReconciliationCandidates(
      "tachi", "https://boku.tachi.ac", std::nullopt, 1);
  assert(reconciliation.status ==
             ir::IrReconciliationReadOutcome::Status::Loaded &&
         reconciliation.candidates.size() == 1 &&
         reconciliation.candidates.front().attemptId == newest.attemptId &&
         reconciliation.nextBeforeModernChartResultId.has_value());
}

void testChartScopedIrRecordsIgnoreUnrelatedHistory() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path / "replay.db");
  assert(repository.EnsureSchema());

  const auto selected = result(14, '3');
  const auto unrelated = result(15, '4');
  const auto selectedStage = repository.StageModernChartResult(
      selected, snapshot(selected), std::nullopt, {});
  const auto unrelatedStage = repository.StageModernChartResult(
      unrelated, snapshot(unrelated), std::nullopt, {});
  assert(selectedStage.status == ModernChartStageStatus::Staged &&
         selectedStage.receipt &&
         unrelatedStage.status == ModernChartStageStatus::Staged &&
         unrelatedStage.receipt &&
         unrelatedStage.receipt->resultId > selectedStage.receipt->resultId);

  const auto scoped = repository.ListIrUploadRecordsForChart(
      "tachi", "https://boku.tachi.ac", selected.score.chartSha256, 1);
  assert(scoped.status == ir::IrUploadRecordReadStatus::Loaded &&
         scoped.records.size() == 1 &&
         scoped.records.front().attemptId == selected.attemptId &&
         !scoped.nextBeforeModernChartResultId.has_value());
  assert(repository
             .ListIrUploadRecordsForChart("tachi",
                                          "https://boku.tachi.ac", "bad", 1)
             .status == ir::IrUploadRecordReadStatus::Invalid);
  assert(repository
             .ListIrUploadRecordsForChart("tachi",
                                          "https://boku.tachi.ac", {}, 1)
             .status == ir::IrUploadRecordReadStatus::Invalid);
  assert(repository
             .ListIrUploadRecordsForChart(
                 "tachi", "https://boku.tachi.ac",
                 selected.score.chartSha256,
                 kMaximumModernChartHistoryRows + 1)
             .status == ir::IrUploadRecordReadStatus::Invalid);
}

void testPendingOwnerCorruptionFailsClosed() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replay.db";
  const auto completed = result(8, 'f');
  {
    ReplayRepository repository(databasePath);
    assert(repository.EnsureSchema());
    assert(
        repository
            .StageModernChartResult(completed, std::nullopt, std::nullopt, {})
            .status == ModernChartStageStatus::Staged);
  }
  {
    auto database = openDatabase(databasePath);
    exec(database.get(), "PRAGMA foreign_keys=OFF");
    exec(database.get(), "DELETE FROM modern_chart_results");
    assert(queryInt(database.get(), "SELECT COUNT(*) FROM "
                                    "modern_pending_chart_score_writes") == 1);
  }
  ReplayRepository reopened(databasePath);
  assert(reopened.LoadPendingModernChartScore(completed.attemptId).status ==
         result_persistence::PendingReadStatus::IntegrityConflict);
  const auto batch = reopened.ListPendingModernChartScores();
  assert(batch.storageAvailable && batch.entries.size() == 1 &&
         batch.entries.front().status ==
             result_persistence::PendingReadStatus::IntegrityConflict);
}

void testPendingTimestampComesFromResultOwner() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replay.db";
  ReplayRepository repository(databasePath);
  assert(repository.EnsureSchema());
  {
    auto database = openDatabase(databasePath);
    exec(database.get(),
         "CREATE TRIGGER force_modern_result_timestamp AFTER INSERT ON "
         "modern_chart_results BEGIN UPDATE modern_chart_results SET "
         "created_at='2001-02-03 04:05:06' WHERE id=NEW.id; END");
  }
  const auto completed = result(9, '1');
  const auto staged = repository.StageModernChartResult(completed, std::nullopt,
                                                        std::nullopt, {});
  assert(staged.status == ModernChartStageStatus::Staged && staged.receipt &&
         staged.receipt->createdAt == "2001-02-03 04:05:06");
  const auto pending =
      repository.LoadPendingModernChartScore(completed.attemptId);
  assert(pending.status == result_persistence::PendingReadStatus::Found &&
         pending.value &&
         pending.value->createdAt == staged.receipt->createdAt);
}

void testFutureCodecReferencePreservesChartResultHistory() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replay.db";
  ReplayRepository repository(databasePath);
  assert(repository.EnsureSchema());
  const auto completed = result(16, '5');
  const auto reserved = repository.ReserveModernReplayPath(
      completed.attemptId, completed.score.chartSha256,
      completed.playedAtUnixMillis);
  assert(reserved.reservation);
  const auto staged = repository.StageModernChartResult(
      completed, std::nullopt, attachment(*reserved.reservation), {});
  assert(staged.status == ModernChartStageStatus::Staged && staged.receipt);
  repository.Shutdown();
  {
    auto database = openDatabase(databasePath);
    exec(database.get(),
         "UPDATE modern_replay_files SET codec_version=" +
             std::to_string(replay::BeatorajaReplayCodec::kCodecVersion + 1) +
             " WHERE modern_chart_result_id=" +
             std::to_string(staged.receipt->resultId));
  }

  const auto loaded =
      repository.LoadModernChartResultByAttempt(completed.attemptId);
  assert(loaded.status == ModernChartResultReadStatus::Loaded &&
         loaded.record && loaded.record->replayFile &&
         loaded.record->result.score == completed.score &&
         loaded.record->replayFile->metadata.codecVersion ==
             replay::BeatorajaReplayCodec::kCodecVersion + 1);
  const auto history = repository.ListModernChartResults(
      completed.score.chartSha256);
  const auto inventory = repository.ListModernReplayFileReferences();
  assert(history.status == ModernChartHistoryReadStatus::Loaded &&
         history.records.size() == 1 && history.records.front().replayFile &&
         inventory.status == ModernReplayFileInventoryStatus::Loaded &&
         inventory.entries.size() == 1 &&
         inventory.entries.front().reference.metadata.codecVersion ==
             replay::BeatorajaReplayCodec::kCodecVersion + 1);
}

void testExactRetryAttachesOnlyMissingReplayAtomically() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replay.db";
  ReplayRepository repository(databasePath);
  assert(repository.EnsureSchema());
  auto database = openDatabase(databasePath);

  const auto completed = result(20, '6');
  const auto summary = repository.StageModernChartResult(
      completed, std::nullopt, std::nullopt, {});
  assert(summary.status == ModernChartStageStatus::Staged && summary.receipt);
  const auto reserved = repository.ReserveModernReplayPath(
      completed.attemptId, completed.score.chartSha256,
      completed.playedAtUnixMillis);
  assert(reserved.reservation);
  const auto file = attachment(*reserved.reservation, '7');
  assert(repository
             .RecordModernReplayInstallIntent(
                 *reserved.reservation,
                 {.attemptToken = completed.attemptId,
                  .metadata = file.metadata})
             .status == ModernReplayOwnershipRecordStatus::Recorded);

  exec(database.get(),
       "CREATE TRIGGER fail_late_chart_replay BEFORE INSERT ON "
       "modern_replay_files BEGIN SELECT RAISE(ABORT,'injected'); END");
  const auto failed = repository.StageModernChartResult(
      completed, std::nullopt, file, {});
  assert(failed.status == ModernChartStageStatus::StorageFailure &&
         queryInt(database.get(), "SELECT COUNT(*) FROM modern_replay_files") ==
             0 &&
         queryInt(database.get(),
                  "SELECT COUNT(*) FROM modern_replay_file_reservations") ==
             1);
  const auto unchanged =
      repository.LoadModernChartResultByAttempt(completed.attemptId);
  assert(unchanged.status == ModernChartResultReadStatus::Loaded &&
         unchanged.record && !unchanged.record->replayFile &&
         unchanged.record->result.resultId == summary.receipt->resultId);

  exec(database.get(), "DROP TRIGGER fail_late_chart_replay");
  const auto attached = repository.StageModernChartResult(
      completed, std::nullopt, file, {});
  assert(attached.status == ModernChartStageStatus::AlreadyStaged &&
         attached.receipt == summary.receipt &&
         queryInt(database.get(), "SELECT COUNT(*) FROM modern_replay_files") ==
             1 &&
         queryInt(database.get(),
                  "SELECT COUNT(*) FROM modern_replay_file_reservations") ==
             0);
  const auto loaded =
      repository.LoadModernChartResultByAttempt(completed.attemptId);
  assert(loaded.status == ModernChartResultReadStatus::Loaded && loaded.record &&
         loaded.record->replayFile &&
         loaded.record->replayFile->identity == file.identity &&
         loaded.record->replayFile->metadata == file.metadata);
  assert(repository
             .StageModernChartResult(completed, std::nullopt, std::nullopt, {})
             .status == ModernChartStageStatus::IntegrityConflict);

  auto replacement = file;
  replacement.metadata.sha256 = repeated('8', 64);
  assert(repository
             .StageModernChartResult(completed, std::nullopt, replacement, {})
             .status == ModernChartStageStatus::IntegrityConflict);
}

} // namespace

int main() {
  testCurrentSchemaAddsSelectorMetricsTable();
  testSchemaReservationAtomicStageAndExactRetry();
  testRollbackAndReplayOptionality();
  testIrSourceHistoryOverGlobalBoundReturnsKeysetPages();
  testChartScopedIrRecordsIgnoreUnrelatedHistory();
  testReservationReleaseAndModernPendingLifecycle();
  testPendingOwnerCorruptionFailsClosed();
  testPendingTimestampComesFromResultOwner();
  testFutureCodecReferencePreservesChartResultHistory();
  testExactRetryAttachesOnlyMissingReplayAtomically();
  return 0;
}
