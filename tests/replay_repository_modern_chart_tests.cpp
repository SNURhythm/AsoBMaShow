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

void testSchemaReservationAtomicStageAndExactRetry() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replay.db";
  ReplayRepository repository(databasePath);
  assert(repository.EnsureSchema());

  auto database = openDatabase(databasePath);
  assert(queryInt(database.get(), "PRAGMA user_version") == 11);
  for (const std::string_view table :
       {"modern_chart_results", "modern_replay_files",
        "modern_replay_file_reservations", "modern_replay_stem_sequences",
        "ir_submission_snapshots", "modern_pending_chart_score_writes",
        "ir_submission_receipts"}) {
    assert(tableExists(database.get(), table));
  }
  assert(columnExists(database.get(), "ir_submission_receipts",
                      "modern_chart_result_id"));

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
  for (const std::string_view table :
       {"replay_events", "replay_touch_samples", "replay_lane_cover_events"}) {
    assert(queryInt(database.get(),
                    "SELECT COUNT(*) FROM " + std::string(table)) == 0);
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
  assert(queryInt(database.get(), "SELECT COUNT(*) FROM replay_events") == 0);
}

} // namespace

int main() {
  testSchemaReservationAtomicStageAndExactRetry();
  testRollbackAndReplayOptionality();
  return 0;
}
