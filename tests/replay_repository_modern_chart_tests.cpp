#include "repositories/ReplayRepository.h"
#include "repositories/ReplayRepositoryInternal.h"
#include "support/TestFlight166Fixture.h"

#include "ir/IrSubmissionSnapshot.h"
#include "FileChecksum.h"
#include "nlohmann/json.hpp"
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
  value.score.badPoints = 2;
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

void testLegacyLightAssistMigrationPreservesAuthenticatedResults() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  auto legacy = result(91);
  legacy.score.clearType = 150;
  legacy.resultFingerprint = result_persistence::modernResultFingerprint(legacy);
  // Construct the pre-migration wire payload, not a newly captured 50 snapshot.
  std::string snapshotDiagnostic;
  auto wire = nlohmann::ordered_json::parse(*ir::serializeIrSubmissionSnapshot(snapshot(legacy), snapshotDiagnostic));
  wire["submission"]["clearType"] = 150;
  nlohmann::ordered_json signedPayload;
  signedPayload["schemaVersion"] = wire["schemaVersion"];
  signedPayload["submission"] = wire["submission"];
  wire["fingerprint"] = file_checksum::sha256(signedPayload.dump());
  const auto legacySnapshot = ir::deserializeIrSubmissionSnapshot(
      wire.dump(), wire["fingerprint"].get<std::string>(), snapshotDiagnostic);
  assert(legacySnapshot && legacySnapshot->submission.clearType == 150);
  {
    ReplayRepository repository(path);
    assert(repository.EnsureSchema());
    const std::vector drafts{draft(*legacySnapshot)};
    const auto reserved=repository.ReserveModernReplayPath(legacy.attemptId,legacy.score.chartSha256,legacy.playedAtUnixMillis);
    assert(reserved.reservation);
    const auto file=attachment(*reserved.reservation);
    assert(repository.RecordModernReplayInstallIntent(*reserved.reservation,
        {.attemptToken=legacy.attemptId,.metadata=file.metadata}).status==ModernReplayOwnershipRecordStatus::Recorded);
    assert(repository.StageModernChartResult(legacy, legacySnapshot, file, drafts, 123).status ==
           ModernChartStageStatus::Staged);
    assert(repository.ReserveModernReplayPath(attemptId(92),repeated('e',64),legacy.playedAtUnixMillis).reservation);
  }
  testflight166::install(path,"replays.sql");
  testflight166::Rows original,expected;
  {
    auto db = openDatabase(path);
    exec(db.get(), "INSERT INTO ir_remote_scores(provider_id,server_origin,remote_score_id,"
        "remote_user_id,game,remote_chart_id,chart_md5,chart_sha256,title,artist,note_count,"
        "score,lamp_rank,service,time_added_ms,sync_generation) VALUES("
        "'tachi','https://boku.tachi.ac','legacy-light',42,'bms-7k','chart','" +
        repeated('b', 32) + "','" + repeated('a', 64) +
        "','Title','Artist',5,7,150,'Bokutachi',1700000000000,1)");
    exec(db.get(),"INSERT INTO legacy_chart_result_summaries(legacy_replay_id,chart_title,clear_type,partial) "
        "VALUES(166,'Legacy chart',150,1);"
        "INSERT INTO ir_submission_receipts(provider_id,server_origin,modern_chart_result_id,attempt_id,"
        "chart_sha256,confirmation_source,confirmed_at_ms) SELECT 'fake','https://example.invalid',id,"
        "attempt_id,chart_sha256,0,1700000000000 FROM modern_chart_results;");
    assert(queryInt(db.get(), "PRAGMA user_version")==19);
    original=testflight166::snapshot(db.get()); expected=testflight166::snapshot(db.get(),true);
    const auto schema=testflight166::schemaSnapshot(db.get());
    // Fail after the remote-lamp table rebuild, then verify transactional rollback.
    const auto deny=[](void *,int action,const char *name,const char *,const char *,const char *) {
      return action==SQLITE_CREATE_TABLE && name && std::string_view(name)=="modern_pending_chart_gauge_evidence"
          ? SQLITE_DENY : SQLITE_OK;
    };
    assert(sqlite3_set_authorizer(db.get(),deny,nullptr)==SQLITE_OK);
    assert(!replay_repository_detail::MigrateSchema(db.get()));
    assert(sqlite3_set_authorizer(db.get(),nullptr,nullptr)==SQLITE_OK);
    assert(queryInt(db.get(), "PRAGMA user_version")==19);
    assert(testflight166::snapshot(db.get())==original && testflight166::schemaSnapshot(db.get())==schema);
  }
  ReplayRepository upgraded(path);
  assert(upgraded.EnsureSchema());
  auto db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") == ReplayRepository::kCurrentSchemaVersion);
  assert(queryInt(db.get(), "SELECT lamp_rank FROM ir_remote_scores") == 50);
  assert(queryInt(db.get(), "SELECT clear_type FROM modern_chart_results") == 150);
  testflight166::preserved(expected,testflight166::snapshot(db.get()));
  const auto pending=upgraded.LoadPendingModernChartScore(legacy.attemptId);
  assert(pending.status==result_persistence::PendingReadStatus::Found && !pending.value->gaugeEvidence.recorded);

  const auto loaded = upgraded.LoadModernChartResultByAttempt(legacy.attemptId);
  assert(loaded.status == ModernChartResultReadStatus::Loaded && loaded.record);
  assert(loaded.record->result.resultFingerprint == legacy.resultFingerprint);
  std::string diagnostic;
  assert(result_persistence::validateModernChartResult(loaded.record->result, diagnostic));
  auto canonical = loaded.record->result;
  canonical.score.clearType = 50;
  canonical.resultFingerprint = result_persistence::modernResultFingerprint(canonical);
  assert(result_persistence::compareModernChartResultFacts(loaded.record->result, canonical).agrees());
  assert(result_persistence::describeChartScoreDifference(loaded.record->result.score, canonical.score).empty());
  const auto rows=testflight166::snapshot(db.get());
  assert(upgraded.EnsureSchema());
  assert(testflight166::snapshot(db.get())==rows);
}

void testCurrentVersionRemoteLampUpgrade() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  {
    ReplayRepository repository(path);
    assert(repository.EnsureSchema());
    assert(repository.StageModernChartResult(result(93), {}, {}).status ==
           ModernChartStageStatus::Staged);
  }
  testflight166::Rows rows;
  {
    auto db = openDatabase(path);
    SqliteStatementHandle objects;
    assert(prepareSqliteStatement(db.get(),
        "SELECT sql FROM sqlite_master WHERE tbl_name='ir_remote_scores' "
        "AND sql IS NOT NULL ORDER BY type DESC", objects) == SQLITE_OK);
    std::vector<std::string> statements;
    while (sqlite3_step(objects.get()) == SQLITE_ROW) {
      auto sql = sqliteColumnString(objects.get(), 0);
      const std::string ranks = "(0,50,100,200,300,400,500,600,700,800)";
      if (const auto pos = sql.find(ranks); pos != std::string::npos) {
        sql.replace(pos, ranks.size(), "(0,50,100,200,300,400,500,600)");
      }
      statements.push_back(std::move(sql));
    }
    objects.reset();
    exec(db.get(), "DROP TABLE ir_remote_scores");
    for (const auto &sql : statements) exec(db.get(), sql);
    exec(db.get(), "INSERT INTO ir_remote_scores(provider_id,server_origin,remote_score_id,"
        "remote_user_id,game,remote_chart_id,chart_md5,chart_sha256,title,artist,note_count,"
        "score,lamp_rank,service,time_added_ms,sync_generation) VALUES("
        "'tachi','https://boku.tachi.ac','current-lamp',42,'bms-7k','chart','" +
        repeated('b', 32) + "','" + repeated('a', 64) +
        "','Title','Artist',5,7,600,'Bokutachi',1700000000000,1)");
    assert(queryInt(db.get(), "PRAGMA user_version") == 21);
    rows = testflight166::snapshot(db.get());
    const auto schema = testflight166::schemaSnapshot(db.get());
    const auto deny = [](void *, int action, const char *name, const char *, const char *, const char *) {
      return action == SQLITE_CREATE_TABLE && name && std::string_view(name) == "ir_remote_scores"
          ? SQLITE_DENY : SQLITE_OK;
    };
    assert(sqlite3_set_authorizer(db.get(), deny, nullptr) == SQLITE_OK);
    assert(!replay_repository_detail::MigrateSchema(db.get()));
    assert(sqlite3_set_authorizer(db.get(), nullptr, nullptr) == SQLITE_OK);
    assert(testflight166::schemaSnapshot(db.get()) == schema);
  }
  ReplayRepository upgraded(path);
  assert(upgraded.EnsureSchema());
  assert(upgraded.EnsureSchema());
  auto db = openDatabase(path);
  assert(testflight166::snapshot(db.get()) == rows);
}

void testPendingGaugeEvidenceRecoveryAndCleanup() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  const auto completed = result(90);
  GaugeEvidence evidence;
  evidence.recorded = true;
  evidence.passedNotes = 5;
  evidence.survivalMask = 0x1f8;
  evidence.deathPassedNotes = {0,0,0,4,-1,3,-1,-1,-1};
  evidence.values = {59,77,78.25F,0,40,0,100,100,100};
  int resultId = 0;
  {
    ReplayRepository repository(path);
    assert(repository.EnsureSchema());
    const auto staged = repository.StageModernChartResult(completed, {}, {}, {}, {}, evidence);
    assert(staged.status == ModernChartStageStatus::Staged);
    resultId = staged.receipt->resultId;
    assert(repository.StageModernChartResult(completed, {}, {}, {}, {}, evidence).status == ModernChartStageStatus::AlreadyStaged);
    auto changed = evidence;
    changed.values[2] = 79;
    assert(repository.StageModernChartResult(completed, {}, {}, {}, {}, changed).status == ModernChartStageStatus::IntegrityConflict);
  }
  ReplayRepository recovered(path);
  auto pending = recovered.LoadPendingModernChartScore(completed.attemptId);
  assert(pending.status == result_persistence::PendingReadStatus::Found && pending.value->gaugeEvidence == evidence);
  auto database = openDatabase(path);
  assert(!columnExists(database.get(), "modern_chart_results", "gauge_evidence"));
  exec(database.get(), "UPDATE modern_pending_chart_gauge_evidence SET value2=79");
  assert(recovered.LoadPendingModernChartScore(completed.attemptId).status == result_persistence::PendingReadStatus::IntegrityConflict);
  exec(database.get(), "UPDATE modern_pending_chart_gauge_evidence SET value2=78.25");
  assert(recovered.AcknowledgePendingModernChartScore(completed.attemptId, resultId).status == result_persistence::AcknowledgeStatus::Acknowledged);
  assert(queryInt(database.get(), "SELECT COUNT(*) FROM modern_pending_chart_gauge_evidence") == 0);
  assert(queryInt(database.get(), "SELECT COUNT(*) FROM modern_chart_results") == 1);
  // Additive migration keeps old pending saves recoverable with unknown evidence.
  const auto older = result(91);
  assert(recovered.StageModernChartResult(older, {}, {}).status == ModernChartStageStatus::Staged);
  recovered.Shutdown();
  exec(database.get(), "DROP TABLE modern_pending_chart_gauge_evidence; PRAGMA user_version=19");
  database.reset();
  assert(recovered.EnsureSchema());
  pending = recovered.LoadPendingModernChartScore(older.attemptId);
  assert(pending.status == result_persistence::PendingReadStatus::Found && !pending.value->gaugeEvidence.recorded);
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

int irSummaryVmBlocks = 0;
bool countIrSummaryVm = false;
int countIrSummaryVmProgress(void *) {
  if (countIrSummaryVm) ++irSummaryVmBlocks;
  return 0;
}
int installIrSummaryVmCounter(sqlite3 *db, char **, const sqlite3_api_routines *) {
  sqlite3_progress_handler(db, 100, countIrSummaryVmProgress, nullptr);
  return SQLITE_OK;
}

void testIrSourceHistoryOverGlobalBoundReturnsKeysetPages() {
  assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(installIrSummaryVmCounter)) == SQLITE_OK);
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
  database = openDatabase(databasePath);
  // Reuse the large malformed-source fixture above. Seed negative cache rows
  // so this check isolates warm query cost rather than validation/backfill.
  exec(database.get(), "INSERT OR IGNORE INTO ir_candidate_summary_v1 "
      "SELECT id,'tachi',0,0,'','' FROM modern_chart_results");
  irSummaryVmBlocks = 0;
  countIrSummaryVm = true;
  const auto firstPage = repository.ListIrUploadCandidateSummaries("tachi", "https://boku.tachi.ac", {}, 32);
  countIrSummaryVm = false;
  assert(firstPage.status == ir::IrUploadCandidateReadStatus::Loaded && firstPage.nextBeforeModernChartResultId);
  std::cout << "Warm first IR page VM blocks: " << irSummaryVmBlocks << std::endl;
  assert(irSummaryVmBlocks < 200 && "first 32-row page must not scan all cached history");
  sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(installIrSummaryVmCounter));
}

// Count SELECT compilation, not execution, across the repository's own connections.
int irSelectPreparations = 0;
bool countIrSelectPreparations = false;
int countIrSelect(void *, int action, const char *, const char *, const char *, const char *) {
  if (countIrSelectPreparations && action == SQLITE_SELECT) ++irSelectPreparations;
  return SQLITE_OK;
}
int installIrSelectCounter(sqlite3 *db, char **, const sqlite3_api_routines *) {
  return sqlite3_set_authorizer(db, countIrSelect, nullptr);
}

int irPayloadReads = 0;
bool trackIrPayloadReads = false;
int countIrPayload(void *, int action, const char *, const char *column, const char *, const char *) {
  if (trackIrPayloadReads && action == SQLITE_READ && column &&
      (std::string_view(column) == "payload_json" ||
       std::string_view(column) == "provenance_json" ||
       std::string_view(column) == "gauge_history_json")) ++irPayloadReads;
  return SQLITE_OK;
}
int installIrPayloadCounter(sqlite3 *db, char **, const sqlite3_api_routines *) {
  return sqlite3_set_authorizer(db, countIrPayload, nullptr);
}

result_persistence::ModernChartResult verifiedResult(int suffix) {
  auto value = result(suffix);
  bms_parser::ChartMeta meta;
  meta.KeyMode = 7;
  meta.MD5 = value.score.chartMd5;
  meta.SHA256 = value.score.chartSha256;
  meta.Rank = 2;
  meta.TotalNotes = value.score.maxScore / 2;
  meta.HasTotal = true;
  meta.Total = 200.5;
  const auto judge =
      gameplay::compileGameplayJudgeRules(GameplayRuleset::LR2, meta.Rank);
  value.score.provenance = makeScoreProvenance({
      .chartMeta = meta,
      .longNoteMode = value.score.longNoteMode,
      .judgeRankSource = JudgeRankSource::Chart,
      .sourceJudgeRank = meta.Rank,
      .effectiveJudgeContexts = judge.contexts,
      .totalNotes = meta.TotalNotes,
      .authoredGaugeTotal = meta.Total,
      .effectiveGaugeTotal =
          resolveEffectiveGaugeTotal(GameplayRuleset::LR2, meta),
      .candidateSelection = gameplay::CandidateSelectionMode::LR2,
      .gaugeType = GaugeType::Hard,
      .inputDevices = {InputDeviceCategory::Keyboard},
      .ruleset = RulesetDescriptor::For(GameplayRuleset::LR2),
  });
  value.adoptedGaugeType = GaugeType::Hard;
  value.resultFingerprint = result_persistence::modernResultFingerprint(value);
  return value;
}

void testIrSummaryLiveStateAndSelectedValidation() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path / "replay.db");
  assert(repository.EnsureSchema());
  const auto value = verifiedResult(1);
  const auto staged = repository.StageModernChartResult(value, snapshot(value), std::nullopt, {});
  assert(staged.status == ModernChartStageStatus::Staged);
  const int id = staged.receipt->resultId;
  auto db = openDatabase(temporary.path / "replay.db");
  // Simulate an existing profile requiring bounded backfill.
  exec(db.get(), "DELETE FROM ir_candidate_summary_v1");
  const auto page = repository.ListIrUploadCandidateSummaries("tachi", "https://boku.tachi.ac", {}, 1);
  assert(page.status == ir::IrUploadCandidateReadStatus::Loaded && page.candidates.size() == 1);
  assert(page.candidates[0].result.score.chartTitle == "Title" &&
         page.candidates[0].result.score.score == 7);
  assert(repository.LoadIrUploadCandidate("tachi", "https://boku.tachi.ac", id, value.attemptId).candidates.size() == 1);
  assert(repository.LoadIrUploadCandidate("tachi", "https://boku.tachi.ac", id, attemptId(2)).candidates.empty());
  auto queued = draft(snapshot(value));
  queued.providerId = "tachi";
  assert(repository.EnqueueReadyIrOutboxDraft(queued, true).status == ir::IrOutboxInsertStatus::Inserted);
  assert(repository.ListIrUploadCandidateSummaries("tachi", "https://boku.tachi.ac").candidates.empty());
  assert(repository.LoadIrUploadCandidate("tachi", "https://boku.tachi.ac", id, value.attemptId).candidates.empty());
  exec(db.get(), "UPDATE ir_outbox SET state=4,last_error_message='rejected'");
  const auto failed = repository.ListIrUploadCandidateSummaries("tachi", "https://boku.tachi.ac");
  assert(failed.candidates.size() == 1 && failed.candidates[0].state == ir::IrRecordState::Failed &&
         failed.candidates[0].failureReason == "rejected");
  exec(db.get(), "INSERT INTO ir_submission_receipts(provider_id,server_origin,modern_chart_result_id,"
      "attempt_id,chart_md5,chart_sha256,confirmation_source,confirmed_at_ms) VALUES('tachi','https://other.example'," +
      std::to_string(id) + ",'" + value.attemptId + "','" + value.score.chartMd5 + "','" + value.score.chartSha256 + "',0,1700000000000)");
  assert(repository.ListIrUploadCandidateSummaries("tachi", "https://boku.tachi.ac").candidates.size() == 1);
  assert(repository.ListIrUploadCandidateSummaries("tachi", "https://other.example").candidates.empty());
  assert(repository.ListIrUploadCandidateSummaries("other", "https://boku.tachi.ac").candidates.size() == 1);
  exec(db.get(), "UPDATE modern_chart_results SET chart_title='tampered'");
  const auto corrupt = repository.ListIrUploadCandidateSummaries("tachi", "https://boku.tachi.ac");
  assert(corrupt.candidates.empty() && corrupt.omittedRows == 1);
  assert(repository.LoadIrUploadCandidate("tachi", "https://boku.tachi.ac", id, value.attemptId).candidates.empty());
}

void testIrSummaryPersistsAndInvalidates() {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "replay.db";
  assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(installIrPayloadCounter)) == SQLITE_OK);
  {
    ReplayRepository repository(path);
    assert(repository.EnsureSchema());
    const auto value = result(1);
    assert(repository.StageModernChartResult(value, snapshot(value), std::nullopt, {}).status ==
           ModernChartStageStatus::Staged);
    const auto first = repository.ListIrUploadCandidateSummaries("tachi", "https://boku.tachi.ac");
    assert(first.status == ir::IrUploadCandidateReadStatus::Loaded && first.omittedRows == 0);
    repository.Shutdown();
    assert(repository.EnsureSchema());
    irPayloadReads = 0;
    trackIrPayloadReads = true;
    const auto warm = repository.ListIrUploadCandidateSummaries("tachi", "https://boku.tachi.ac");
    trackIrPayloadReads = false;
    assert(warm.status == ir::IrUploadCandidateReadStatus::Loaded && warm.omittedRows == 0);
    assert(irPayloadReads == 0 && "warm summary must not read historical JSON after reopen");
    auto db = openDatabase(path);
    exec(db.get(), "UPDATE ir_submission_snapshots SET payload_json='invalid'");
    const auto invalid = repository.ListIrUploadCandidateSummaries("tachi", "https://boku.tachi.ac");
    assert(invalid.status == ir::IrUploadCandidateReadStatus::Loaded && invalid.omittedRows == 1);
    irPayloadReads = 0;
    trackIrPayloadReads = true;
    const auto invalidAgain = repository.ListIrUploadCandidateSummaries("tachi", "https://boku.tachi.ac");
    trackIrPayloadReads = false;
    assert(invalidAgain.omittedRows == 1 && irPayloadReads == 0);
    exec(db.get(), "DELETE FROM ir_submission_snapshots");
    const auto removed = repository.ListIrUploadCandidateSummaries("tachi", "https://boku.tachi.ac");
    assert(removed.omittedRows == 0 && removed.candidates.empty());
  }
  sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(installIrPayloadCounter));
}

void testIrPageReusesLookupStatements() {
  TemporaryDirectory temporary;
  assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(installIrSelectCounter)) == SQLITE_OK);
  {
    ReplayRepository repository(temporary.path / "replay.db");
    assert(repository.EnsureSchema());
    for (int i = 1; i <= 8; ++i) {
      const auto value = result(i);
      assert(repository.StageModernChartResult(value, snapshot(value), std::nullopt, {}).status ==
             ModernChartStageStatus::Staged);
    }
    irSelectPreparations = 0;
    countIrSelectPreparations = true;
    const auto page = repository.ListIrUploadCandidates("tachi", "https://boku.tachi.ac");
    countIrSelectPreparations = false;
    assert(page.status == ir::IrUploadCandidateReadStatus::Loaded && page.candidates.empty());
    std::cout << "IR page SELECT preparations: " << irSelectPreparations << std::endl;
    assert(irSelectPreparations <= 6); // Page/schema reads + four reusable lookups.
    const auto records = repository.ListIrUploadRecords("tachi", "https://boku.tachi.ac");
    assert(records.records.size() == 8);
    for (std::size_t i = 0; i < records.records.size(); ++i)
      assert(records.records[i].attemptId == result(8 - static_cast<int>(i)).attemptId);
    auto database = openDatabase(temporary.path / "replay.db");
    // Individually authenticated facts must still agree with their owner at
    // every projection boundary, including reconciliation (which has no UI projection).
    auto differentOwner = result(5);
    ++differentOwner.playedAtUnixMillis;
    differentOwner.resultFingerprint = result_persistence::modernResultFingerprint(differentOwner);
    const auto mismatchedSnapshot = snapshot(differentOwner);
    std::string diagnostic;
    const auto payload = ir::serializeIrSubmissionSnapshot(mismatchedSnapshot, diagnostic);
    assert(payload);
    SqliteStatementHandle replaceSnapshot;
    assert(prepareSqliteStatement(database.get(),
        "UPDATE ir_submission_snapshots SET payload_json=?,fingerprint=? WHERE attempt_id=?",
        replaceSnapshot) == SQLITE_OK);
    bindSqliteText(replaceSnapshot, 1, *payload);
    bindSqliteText(replaceSnapshot, 2, mismatchedSnapshot.fingerprint);
    bindSqliteText(replaceSnapshot, 3, differentOwner.attemptId);
    assert(sqlite3_step(replaceSnapshot) == SQLITE_DONE);
    replaceSnapshot.reset();
    const auto mismatched = repository.ListIrUploadCandidates("tachi", "https://boku.tachi.ac");
    assert(mismatched.status == ir::IrUploadCandidateReadStatus::Loaded && mismatched.omittedRows == 1);
    const auto reconciled = repository.LoadIrReconciliationCandidates("tachi", "https://boku.tachi.ac");
    assert(reconciled.status == ir::IrReconciliationReadOutcome::Status::Loaded &&
           reconciled.candidates.size() == 7 && !reconciled.diagnostic.empty());
    for (const auto &candidate : reconciled.candidates)
      assert(candidate.attemptId != differentOwner.attemptId);
    exec(database.get(), "UPDATE ir_submission_snapshots SET payload_json='invalid' WHERE attempt_id='" +
         result(4).attemptId + "'");
    const auto afterCorruption = repository.ListIrUploadRecords("tachi", "https://boku.tachi.ac");
    assert(afterCorruption.records.size() == 6 && afterCorruption.omittedRows == 2);
    assert(afterCorruption.records.back().attemptId == result(1).attemptId);

  }
  sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(installIrSelectCounter));
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
  testIrSummaryPersistsAndInvalidates();
  testIrSummaryLiveStateAndSelectedValidation();
  testIrPageReusesLookupStatements();
  testCurrentVersionRemoteLampUpgrade();
  testLegacyLightAssistMigrationPreservesAuthenticatedResults();
  testPendingGaugeEvidenceRecoveryAndCleanup();
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
