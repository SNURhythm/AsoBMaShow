#include "ReplayRepositoryInternal.h"

#include "../ProfileDatabaseActivity.h"
#include "../Uuid.h"
#include "../ir/IrSubmissionSnapshot.h"
#include "SqliteRAII.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace {

using Json = nlohmann::ordered_json;

constexpr std::size_t kMaximumResultJsonBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumGaugeSamples = 1'000'000U;

bool lowerHex(std::string_view value, std::size_t size) {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

std::string pathText(const std::filesystem::path &path) {
  return path.generic_string();
}

std::optional<std::string> gaugeHistoryJson(const std::vector<float> &history) {
  if (history.size() > kMaximumGaugeSamples ||
      std::ranges::any_of(history,
                          [](float value) { return !std::isfinite(value); })) {
    return std::nullopt;
  }
  const std::string serialized = Json(history).dump();
  return serialized.size() <= kMaximumResultJsonBytes
             ? std::optional<std::string>(serialized)
             : std::nullopt;
}

std::optional<std::string> judgementTimingJson(
    const std::optional<result_persistence::ChartJudgementTiming> &timing) {
  if (!timing.has_value()) {
    return std::string{};
  }
  Json value = Json::array();
  for (const auto &count : timing->byJudgement) {
    value.push_back(Json::array({count.fast, count.slow}));
  }
  return value.dump();
}

std::optional<std::vector<float>> parseGaugeHistory(std::string_view text) {
  try {
    if (text.size() > kMaximumResultJsonBytes) {
      return std::nullopt;
    }
    auto result = Json::parse(text).get<std::vector<float>>();
    if (result.size() > kMaximumGaugeSamples ||
        std::ranges::any_of(
            result, [](float value) { return !std::isfinite(value); }) ||
        Json(result).dump() != text) {
      return std::nullopt;
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<result_persistence::ChartJudgementTiming>
parseJudgementTiming(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  try {
    const Json value = Json::parse(text);
    if (!value.is_array() || value.size() != JudgementCount ||
        value.dump() != text) {
      return std::nullopt;
    }
    result_persistence::ChartJudgementTiming result;
    for (std::size_t index = 0; index < value.size(); ++index) {
      const auto &entry = value[index];
      if (!entry.is_array() || entry.size() != 2) {
        return std::nullopt;
      }
      result.byJudgement[index] = {.fast = entry[0].get<int>(),
                                   .slow = entry[1].get<int>()};
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

bool bindText(sqlite3_stmt *statement, int index, std::string_view value) {
  return sqlite3_bind_text(statement, index, value.data(),
                           static_cast<int>(value.size()),
                           SQLITE_TRANSIENT) == SQLITE_OK;
}

std::string columnText(sqlite3_stmt *statement, int index) {
  return sqliteColumnString(statement, index);
}

std::optional<std::string> readCreatedAt(sqlite3 *database, int resultId) {
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(database,
                             "SELECT created_at FROM chart_results WHERE id=?",
                             statement) != SQLITE_OK ||
      sqlite3_bind_int(statement.get(), 1, resultId) != SQLITE_OK ||
      sqlite3_step(statement.get()) != SQLITE_ROW ||
      sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT) {
    return std::nullopt;
  }
  const std::string result = columnText(statement.get(), 0);
  return sqlite3_step(statement.get()) == SQLITE_DONE
             ? std::optional<std::string>(result)
             : std::nullopt;
}

bool validateReplayReference(const ReplayFileReference &reference,
                             std::string &diagnostic) {
  diagnostic.clear();
  if (reference.id != 0 || reference.recordId != 0 ||
      reference.recordKind != ReplayFileReference::RecordKind::ChartResult ||
      !lowerHex(reference.contentSha256, 64) || reference.compressedSize == 0 ||
      reference.compressedSize >
          static_cast<std::uint64_t>(
              std::numeric_limits<sqlite3_int64>::max()) ||
      reference.codecVersion != 1) {
    diagnostic = "replay file reference is malformed";
    return false;
  }
  std::string pathDiagnostic;
  const auto expected = replay::pathForStem(
      reference.stem, reference.historyIndex, pathDiagnostic);
  if (!expected.has_value() ||
      expected->relativePath != reference.relativePath) {
    diagnostic = "replay file reference path is inconsistent";
    return false;
  }
  return true;
}

bool validateDrafts(const ir::IrSubmissionSnapshot &snapshot,
                    std::span<const ir::IrOutboxDraft> drafts,
                    std::string &diagnostic) {
  std::set<std::string> providers;
  for (const auto &draft : drafts) {
    if (!ir::validateIrOutboxDraft(draft, diagnostic) ||
        !providers.insert(draft.providerId).second ||
        draft.attemptId != snapshot.submission.attemptId ||
        draft.chartMd5 != snapshot.submission.chartMd5 ||
        draft.chartSha256 != snapshot.submission.chartSha256) {
      if (diagnostic.empty()) {
        diagnostic = "IR draft does not match the persisted snapshot";
      }
      return false;
    }
  }
  return true;
}

ResultReadOutcome invalidResult(std::string diagnostic) {
  return {.status = ResultReadOutcome::Status::Invalid,
          .diagnostic = std::move(diagnostic)};
}

} // namespace

namespace replay_repository_detail {

ReservationOutcome ReserveReplayFileOnConnection(sqlite3 *database,
                                                 std::string_view attemptId,
                                                 std::string_view stem) {
  if (database == nullptr || !uuid::isCanonicalLowerV4(attemptId)) {
    return {.status = ReservationOutcome::Status::Invalid,
            .diagnostic = "replay reservation identity is invalid"};
  }
  std::string pathDiagnostic;
  if (!replay::pathForStem(stem, 0, pathDiagnostic).has_value()) {
    return {.status = ReservationOutcome::Status::Invalid,
            .diagnostic = std::move(pathDiagnostic)};
  }

  std::string transactionError;
  SqliteTransactionHandle transaction(database, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    return {.status = ReservationOutcome::Status::StorageFailure,
            .diagnostic = "could not start replay reservation"};
  }

  SqliteStatementHandle existing;
  if (prepareSqliteStatement(
          database,
          "SELECT stem,history_index,relative_path FROM "
          "replay_file_reservations WHERE attempt_id=? UNION ALL "
          "SELECT f.stem,f.history_index,f.relative_path FROM chart_results r "
          "JOIN replay_files f ON f.chart_result_id=r.id WHERE r.attempt_id=?",
          existing) != SQLITE_OK ||
      !bindText(existing.get(), 1, attemptId) ||
      !bindText(existing.get(), 2, attemptId)) {
    return {.status = ReservationOutcome::Status::StorageFailure,
            .diagnostic = "could not inspect replay reservation"};
  }
  const int existingStep = sqlite3_step(existing.get());
  if (existingStep == SQLITE_ROW) {
    ReplayFileReservation reservation{
        .attemptId = std::string(attemptId),
        .stem = columnText(existing.get(), 0),
        .historyIndex = sqlite3_column_int64(existing.get(), 1),
        .relativePath = std::filesystem::path(columnText(existing.get(), 2)),
    };
    if (reservation.stem != stem ||
        sqlite3_step(existing.get()) != SQLITE_DONE) {
      return {.status = ReservationOutcome::Status::IntegrityConflict,
              .diagnostic =
                  "attempt ID already has another replay reservation"};
    }
    if (!transaction.commit(transactionError)) {
      return {.status = ReservationOutcome::Status::StorageFailure,
              .diagnostic = "could not finish replay reservation read"};
    }
    return {.status = ReservationOutcome::Status::AlreadyReserved,
            .reservation = std::move(reservation)};
  }
  if (existingStep != SQLITE_DONE) {
    return {.status = ReservationOutcome::Status::StorageFailure,
            .diagnostic = "could not read replay reservation"};
  }

  SqliteStatementHandle maximum;
  if (prepareSqliteStatement(
          database,
          "SELECT MAX(history_index) FROM ("
          "SELECT history_index FROM replay_files WHERE stem=? UNION ALL "
          "SELECT history_index FROM replay_file_reservations WHERE stem=? "
          "UNION ALL SELECT last_history_index AS history_index FROM "
          "replay_stem_sequences WHERE stem=?)",
          maximum) != SQLITE_OK ||
      !bindText(maximum.get(), 1, stem) || !bindText(maximum.get(), 2, stem) ||
      !bindText(maximum.get(), 3, stem) ||
      sqlite3_step(maximum.get()) != SQLITE_ROW) {
    return {.status = ReservationOutcome::Status::StorageFailure,
            .diagnostic = "could not allocate replay history index"};
  }
  std::int64_t historyIndex = 0;
  if (sqlite3_column_type(maximum.get(), 0) != SQLITE_NULL) {
    const auto previous = sqlite3_column_int64(maximum.get(), 0);
    if (previous < 0 || previous == std::numeric_limits<sqlite3_int64>::max()) {
      return {.status = ReservationOutcome::Status::IntegrityConflict,
              .diagnostic = "replay history index is exhausted"};
    }
    historyIndex = previous + 1;
  }
  if (sqlite3_step(maximum.get()) != SQLITE_DONE) {
    return {.status = ReservationOutcome::Status::StorageFailure,
            .diagnostic = "could not finish replay index allocation"};
  }
  SqliteStatementHandle sequence;
  if (prepareSqliteStatement(
          database,
          "INSERT INTO replay_stem_sequences(stem,last_history_index) "
          "VALUES(?,?) ON CONFLICT(stem) DO UPDATE SET "
          "last_history_index=excluded.last_history_index",
          sequence) != SQLITE_OK ||
      !bindText(sequence.get(), 1, stem) ||
      sqlite3_bind_int64(sequence.get(), 2, historyIndex) != SQLITE_OK ||
      sqlite3_step(sequence.get()) != SQLITE_DONE) {
    return {.status = ReservationOutcome::Status::StorageFailure,
            .diagnostic = "could not advance replay history sequence"};
  }
  const auto identity = replay::pathForStem(stem, historyIndex, pathDiagnostic);
  if (!identity.has_value()) {
    return {.status = ReservationOutcome::Status::IntegrityConflict,
            .diagnostic = std::move(pathDiagnostic)};
  }
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  SqliteStatementHandle insert;
  if (prepareSqliteStatement(
          database,
          "INSERT INTO replay_file_reservations(attempt_id,stem,history_index,"
          "relative_path,created_at_unix_ms) VALUES(?,?,?,?,?)",
          insert) != SQLITE_OK ||
      !bindText(insert.get(), 1, attemptId) ||
      !bindText(insert.get(), 2, stem) ||
      sqlite3_bind_int64(insert.get(), 3, historyIndex) != SQLITE_OK ||
      !bindText(insert.get(), 4, pathText(identity->relativePath)) ||
      sqlite3_bind_int64(insert.get(), 5, now) != SQLITE_OK ||
      sqlite3_step(insert.get()) != SQLITE_DONE) {
    return {.status = ReservationOutcome::Status::StorageFailure,
            .diagnostic = "could not store replay reservation"};
  }
  if (!transaction.commit(transactionError)) {
    return {.status = ReservationOutcome::Status::StorageFailure,
            .diagnostic = "could not commit replay reservation"};
  }
  return {.status = ReservationOutcome::Status::Reserved,
          .reservation =
              ReplayFileReservation{.attemptId = std::string(attemptId),
                                    .stem = std::string(stem),
                                    .historyIndex = historyIndex,
                                    .relativePath = identity->relativePath}};
}

ResultReadOutcome LoadChartResultOnConnection(sqlite3 *database, int resultId) {
  if (database == nullptr || resultId <= 0) {
    return invalidResult("chart result ID is invalid");
  }
  constexpr const char *query =
      "SELECT r.id,r.attempt_id,r.chart_path,r.chart_md5,r.chart_sha256,"
      "r.chart_title,r.chart_artist,r.key_mode,r.long_note_mode,r.score,"
      "r.max_score,r.max_combo,r.combo_break,r.p_great,r.great,r.good,r.bad,"
      "r.poor,r.k_poor,r.fast,r.slow,r.final_gauge,r.clear_type,"
      "r.gauge_history_json,r.judgement_timing_json,r.provenance_json,"
      "r.result_fingerprint,r.played_at_unix_ms,f.id,f.stem,f.history_index,"
      "f.relative_path,f.content_sha256,f.compressed_size,f.codec_version "
      "FROM chart_results r LEFT JOIN replay_files f ON f.chart_result_id=r.id "
      "WHERE r.id=?";
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK ||
      sqlite3_bind_int(statement.get(), 1, resultId) != SQLITE_OK) {
    return {.status = ResultReadOutcome::Status::StorageFailure,
            .diagnostic = "could not query chart result"};
  }
  const int step = sqlite3_step(statement.get());
  if (step == SQLITE_DONE) {
    return {.status = ResultReadOutcome::Status::NotFound};
  }
  if (step != SQLITE_ROW) {
    return {.status = ResultReadOutcome::Status::StorageFailure,
            .diagnostic = "chart result query failed"};
  }

  result_persistence::PersistedChartResult result;
  result.resultId = sqlite3_column_int(statement.get(), 0);
  if (sqlite3_column_type(statement.get(), 1) != SQLITE_NULL) {
    result.attemptId = columnText(statement.get(), 1);
  }
  auto &score = result.score;
  score.chartPath = columnText(statement.get(), 2);
  score.chartMd5 = columnText(statement.get(), 3);
  score.chartSha256 = columnText(statement.get(), 4);
  score.chartTitle = columnText(statement.get(), 5);
  score.chartArtist = columnText(statement.get(), 6);
  result.keyMode = sqlite3_column_int(statement.get(), 7);
  score.longNoteMode = sqlite3_column_int(statement.get(), 8);
  score.score = sqlite3_column_int(statement.get(), 9);
  score.maxScore = sqlite3_column_int(statement.get(), 10);
  score.maxCombo = sqlite3_column_int(statement.get(), 11);
  score.comboBreak = sqlite3_column_int(statement.get(), 12);
  score.pGreat = sqlite3_column_int(statement.get(), 13);
  score.great = sqlite3_column_int(statement.get(), 14);
  score.good = sqlite3_column_int(statement.get(), 15);
  score.bad = sqlite3_column_int(statement.get(), 16);
  score.poor = sqlite3_column_int(statement.get(), 17);
  score.kPoor = sqlite3_column_int(statement.get(), 18);
  score.fast = sqlite3_column_int(statement.get(), 19);
  score.slow = sqlite3_column_int(statement.get(), 20);
  score.finalGauge =
      static_cast<float>(sqlite3_column_double(statement.get(), 21));
  score.clearType = sqlite3_column_int(statement.get(), 22);
  const auto history = parseGaugeHistory(columnText(statement.get(), 23));
  if (!history.has_value()) {
    return invalidResult("chart result gauge history is malformed");
  }
  result.adoptedGaugeHistory = *history;
  if (sqlite3_column_type(statement.get(), 24) != SQLITE_NULL) {
    result.judgementTiming =
        parseJudgementTiming(columnText(statement.get(), 24));
    if (!result.judgementTiming.has_value()) {
      return invalidResult("chart result timing breakdown is malformed");
    }
  }
  std::string provenanceDiagnostic;
  const std::string provenanceJson = columnText(statement.get(), 25);
  if (provenanceJson.size() > kMaximumResultJsonBytes) {
    return invalidResult("chart result provenance is oversized");
  }
  auto provenance =
      deserializeScoreProvenance(provenanceJson, provenanceDiagnostic);
  if (!provenance.has_value()) {
    return invalidResult("chart result provenance is malformed");
  }
  score.provenance = std::move(*provenance);
  result.resultFingerprint = columnText(statement.get(), 26);
  result.playedAtUnixMillis = sqlite3_column_int64(statement.get(), 27);
  std::string resultDiagnostic;
  if (!result_persistence::validatePersistedChartResult(result,
                                                        resultDiagnostic) ||
      result.resultFingerprint.empty()) {
    return invalidResult(resultDiagnostic.empty()
                             ? "chart result fingerprint is missing"
                             : std::move(resultDiagnostic));
  }

  ResultRecord record{.result = std::move(result)};
  if (sqlite3_column_type(statement.get(), 28) != SQLITE_NULL) {
    const auto size = sqlite3_column_int64(statement.get(), 33);
    if (size <= 0) {
      return invalidResult("replay file size is invalid");
    }
    record.replayFile = ReplayFileReference{
        .id = sqlite3_column_int64(statement.get(), 28),
        .recordKind = ReplayFileReference::RecordKind::ChartResult,
        .recordId = resultId,
        .stem = columnText(statement.get(), 29),
        .historyIndex = sqlite3_column_int64(statement.get(), 30),
        .relativePath = std::filesystem::path(columnText(statement.get(), 31)),
        .contentSha256 = columnText(statement.get(), 32),
        .compressedSize = static_cast<std::uint64_t>(size),
        .codecVersion = sqlite3_column_int(statement.get(), 34),
    };
    ReplayFileReference check = *record.replayFile;
    check.id = 0;
    check.recordId = 0;
    std::string referenceDiagnostic;
    if (!validateReplayReference(check, referenceDiagnostic)) {
      return invalidResult(std::move(referenceDiagnostic));
    }
  }
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    return {.status = ResultReadOutcome::Status::IntegrityConflict,
            .diagnostic = "chart result ID is not unique"};
  }
  return {.status = ResultReadOutcome::Status::Loaded,
          .record = std::move(record)};
}

ir::IrSubmissionSnapshotReadOutcome
LoadIrSubmissionSnapshotOnConnection(sqlite3 *database,
                                     std::string_view attemptId) {
  using Status = ir::IrSubmissionSnapshotReadOutcome::Status;
  if (database == nullptr || !uuid::isCanonicalLowerV4(attemptId)) {
    return {.status = Status::Invalid,
            .diagnostic = "IR snapshot attempt ID is invalid"};
  }
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(
          database,
          "SELECT schema_version,payload_json,fingerprint FROM "
          "ir_submission_snapshots WHERE attempt_id=?",
          statement) != SQLITE_OK ||
      !bindText(statement.get(), 1, attemptId)) {
    return {.status = Status::StorageFailure,
            .diagnostic = "could not query IR snapshot"};
  }
  const int step = sqlite3_step(statement.get());
  if (step == SQLITE_DONE) {
    return {.status = Status::NotFound};
  }
  if (step != SQLITE_ROW) {
    return {.status = Status::StorageFailure,
            .diagnostic = "IR snapshot query failed"};
  }
  const int schemaVersion = sqlite3_column_int(statement.get(), 0);
  const std::string payload = columnText(statement.get(), 1);
  const std::string fingerprint = columnText(statement.get(), 2);
  if (schemaVersion != ir::IrSubmissionSnapshot::kSchemaVersion) {
    return {.status = Status::Invalid,
            .diagnostic = "IR snapshot schema version is unsupported"};
  }
  std::string diagnostic;
  auto snapshot =
      ir::deserializeIrSubmissionSnapshot(payload, fingerprint, diagnostic);
  if (!snapshot.has_value() || snapshot->submission.attemptId != attemptId) {
    return {.status = Status::Invalid,
            .diagnostic = diagnostic.empty()
                              ? "IR snapshot attempt identity differs"
                              : std::move(diagnostic)};
  }
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    return {.status = Status::IntegrityConflict,
            .diagnostic = "IR snapshot attempt ID is not unique"};
  }
  return {.status = Status::Loaded, .snapshot = std::move(snapshot)};
}

result_persistence::StageOutcome StageCompletedChartAttemptOnConnection(
    sqlite3 *database,
    const result_persistence::PersistedChartResult &sourceResult,
    const ir::IrSubmissionSnapshot &snapshot,
    const ReplayFileReference &replayFile,
    std::span<const ir::IrOutboxDraft> irDrafts) {
  using result_persistence::StageOutcome;
  using result_persistence::StageReceipt;
  using result_persistence::StageStatus;

  std::string diagnostic;
  const auto expectedSnapshot =
      ir::captureIrSubmissionSnapshot(sourceResult, diagnostic);
  if (database == nullptr || sourceResult.resultId != 0 ||
      !sourceResult.attemptId.has_value() ||
      !result_persistence::validatePersistedChartResult(sourceResult,
                                                        diagnostic) ||
      sourceResult.resultFingerprint.empty() || !expectedSnapshot.has_value() ||
      *expectedSnapshot != snapshot ||
      !validateReplayReference(replayFile, diagnostic) ||
      !validateDrafts(snapshot, irDrafts, diagnostic)) {
    return {.status = StageStatus::IntegrityConflict,
            .diagnostic = diagnostic.empty()
                              ? "completed chart attempt is inconsistent"
                              : std::move(diagnostic)};
  }
  const auto serializedSnapshot =
      ir::serializeIrSubmissionSnapshot(snapshot, diagnostic);
  const auto history = gaugeHistoryJson(sourceResult.adoptedGaugeHistory);
  const auto timing = judgementTimingJson(sourceResult.judgementTiming);
  const auto provenance = serializeValidatedScoreProvenance(
      sourceResult.score.provenance, diagnostic);
  if (!serializedSnapshot.has_value() || !history.has_value() ||
      !timing.has_value() || !provenance.has_value()) {
    return {.status = StageStatus::IntegrityConflict,
            .diagnostic = diagnostic.empty()
                              ? "completed chart result cannot be serialized"
                              : std::move(diagnostic)};
  }

  std::string transactionError;
  SqliteTransactionHandle transaction(database, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not start compact result staging"};
  }

  SqliteStatementHandle existingId;
  if (prepareSqliteStatement(database,
                             "SELECT id FROM chart_results WHERE attempt_id=?",
                             existingId) != SQLITE_OK ||
      !bindText(existingId.get(), 1, *sourceResult.attemptId)) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not inspect compact result staging"};
  }
  const int existingStep = sqlite3_step(existingId.get());
  if (existingStep == SQLITE_ROW) {
    const int id = sqlite3_column_int(existingId.get(), 0);
    if (sqlite3_step(existingId.get()) != SQLITE_DONE) {
      return {.status = StageStatus::IntegrityConflict,
              .diagnostic = "chart attempt identity is not unique"};
    }
    const auto loaded = LoadChartResultOnConnection(database, id);
    const auto loadedSnapshot =
        LoadIrSubmissionSnapshotOnConnection(database, *sourceResult.attemptId);
    auto expected = sourceResult;
    expected.resultId = id;
    ReplayFileReference expectedFile = replayFile;
    if (loaded.status != ResultReadOutcome::Status::Loaded ||
        !loaded.record.has_value() || loaded.record->result != expected ||
        !loaded.record->replayFile.has_value()) {
      return {.status = StageStatus::IntegrityConflict,
              .diagnostic = "attempt ID already names another result"};
    }
    expectedFile.id = loaded.record->replayFile->id;
    expectedFile.recordId = id;
    if (*loaded.record->replayFile != expectedFile ||
        loadedSnapshot.status !=
            ir::IrSubmissionSnapshotReadOutcome::Status::Loaded ||
        loadedSnapshot.snapshot != snapshot) {
      return {.status = StageStatus::IntegrityConflict,
              .diagnostic = "staged replay file or IR snapshot differs"};
    }
    const auto verified =
        VerifyIrDraftsOnConnection(database, *sourceResult.attemptId, irDrafts);
    if (verified.status != IrDraftStageStatus::Succeeded) {
      return {.status = verified.status == IrDraftStageStatus::StorageFailure
                            ? StageStatus::StorageFailure
                            : StageStatus::IntegrityConflict,
              .diagnostic = verified.diagnostic};
    }
    const auto createdAt = readCreatedAt(database, id);
    SqliteStatementHandle pending;
    if (!createdAt.has_value() ||
        prepareSqliteStatement(
            database,
            "SELECT COUNT(*) FROM pending_chart_score_writes "
            "WHERE attempt_id=? AND result_id=?",
            pending) != SQLITE_OK ||
        !bindText(pending.get(), 1, *sourceResult.attemptId) ||
        sqlite3_bind_int(pending.get(), 2, id) != SQLITE_OK ||
        sqlite3_step(pending.get()) != SQLITE_ROW ||
        sqlite3_column_type(pending.get(), 0) != SQLITE_INTEGER) {
      return {.status = StageStatus::StorageFailure,
              .diagnostic = "could not inspect idempotent score state"};
    }
    const sqlite3_int64 pendingCount = sqlite3_column_int64(pending.get(), 0);
    if ((pendingCount != 0 && pendingCount != 1) ||
        sqlite3_step(pending.get()) != SQLITE_DONE) {
      return {.status = StageStatus::IntegrityConflict,
              .diagnostic = "idempotent result has invalid pending state"};
    }
    if (!transaction.commit(transactionError)) {
      return {.status = StageStatus::StorageFailure,
              .diagnostic = "could not finish idempotent result staging"};
    }
    return {.status = StageStatus::AlreadyStaged,
            .receipt = StageReceipt{.attemptId = *sourceResult.attemptId,
                                    .resultId = id,
                                    .createdAt = *createdAt,
                                    .scorePending = pendingCount == 1}};
  }
  if (existingStep != SQLITE_DONE) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not read compact result staging"};
  }

  SqliteStatementHandle reservation;
  if (prepareSqliteStatement(database,
                             "SELECT stem,history_index,relative_path FROM "
                             "replay_file_reservations WHERE attempt_id=?",
                             reservation) != SQLITE_OK ||
      !bindText(reservation.get(), 1, *sourceResult.attemptId) ||
      sqlite3_step(reservation.get()) != SQLITE_ROW ||
      columnText(reservation.get(), 0) != replayFile.stem ||
      sqlite3_column_int64(reservation.get(), 1) != replayFile.historyIndex ||
      columnText(reservation.get(), 2) != pathText(replayFile.relativePath)) {
    return {.status = StageStatus::IntegrityConflict,
            .diagnostic = "completed replay has no matching reservation"};
  }

  constexpr const char *insertSql =
      "INSERT INTO chart_results(attempt_id,chart_path,chart_md5,chart_sha256,"
      "chart_title,chart_artist,key_mode,long_note_mode,score,max_score,"
      "max_combo,combo_break,p_great,great,good,bad,poor,k_poor,fast,slow,"
      "final_gauge,clear_type,gauge_history_json,judgement_timing_json,"
      "provenance_json,result_fingerprint,played_at_unix_ms)"
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
  SqliteStatementHandle insert;
  if (prepareSqliteStatement(database, insertSql, insert) != SQLITE_OK) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not prepare compact chart result"};
  }
  const auto &score = sourceResult.score;
  int column = 1;
  bool bound = bindText(insert.get(), column++, *sourceResult.attemptId) &&
               bindText(insert.get(), column++, score.chartPath) &&
               bindText(insert.get(), column++, score.chartMd5) &&
               bindText(insert.get(), column++, score.chartSha256) &&
               bindText(insert.get(), column++, score.chartTitle) &&
               bindText(insert.get(), column++, score.chartArtist);
  const int integers[] = {
      sourceResult.keyMode, score.longNoteMode, score.score,  score.maxScore,
      score.maxCombo,       score.comboBreak,   score.pGreat, score.great,
      score.good,           score.bad,          score.poor,   score.kPoor,
      score.fast,           score.slow};
  for (int value : integers) {
    bound =
        bound && sqlite3_bind_int(insert.get(), column++, value) == SQLITE_OK;
  }
  bound =
      bound &&
      sqlite3_bind_double(insert.get(), column++, score.finalGauge) ==
          SQLITE_OK &&
      sqlite3_bind_int(insert.get(), column++, score.clearType) == SQLITE_OK &&
      bindText(insert.get(), column++, *history);
  if (sourceResult.judgementTiming.has_value()) {
    bound = bound && bindText(insert.get(), column++, *timing);
  } else {
    bound = bound && sqlite3_bind_null(insert.get(), column++) == SQLITE_OK;
  }
  bound = bound && bindText(insert.get(), column++, *provenance) &&
          bindText(insert.get(), column++, sourceResult.resultFingerprint) &&
          sqlite3_bind_int64(insert.get(), column++,
                             sourceResult.playedAtUnixMillis) == SQLITE_OK;
  if (!bound || column != 28 || sqlite3_step(insert.get()) != SQLITE_DONE) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not insert compact chart result"};
  }
  const sqlite3_int64 rawId = sqlite3_last_insert_rowid(database);
  if (rawId <= 0 || rawId > std::numeric_limits<int>::max()) {
    return {.status = StageStatus::IntegrityConflict,
            .diagnostic = "chart result ID is out of range"};
  }
  const int resultId = static_cast<int>(rawId);

  SqliteStatementHandle fileInsert;
  if (prepareSqliteStatement(
          database,
          "INSERT INTO replay_files(chart_result_id,course_result_id,stem,"
          "history_index,relative_path,content_sha256,compressed_size,"
          "codec_version) VALUES(?,NULL,?,?,?,?,?,?)",
          fileInsert) != SQLITE_OK ||
      sqlite3_bind_int(fileInsert.get(), 1, resultId) != SQLITE_OK ||
      !bindText(fileInsert.get(), 2, replayFile.stem) ||
      sqlite3_bind_int64(fileInsert.get(), 3, replayFile.historyIndex) !=
          SQLITE_OK ||
      !bindText(fileInsert.get(), 4, pathText(replayFile.relativePath)) ||
      !bindText(fileInsert.get(), 5, replayFile.contentSha256) ||
      sqlite3_bind_int64(
          fileInsert.get(), 6,
          static_cast<sqlite3_int64>(replayFile.compressedSize)) != SQLITE_OK ||
      sqlite3_bind_int(fileInsert.get(), 7, replayFile.codecVersion) !=
          SQLITE_OK ||
      sqlite3_step(fileInsert.get()) != SQLITE_DONE) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not associate compact replay file"};
  }

  SqliteStatementHandle snapshotInsert;
  if (prepareSqliteStatement(
          database,
          "INSERT INTO ir_submission_snapshots(attempt_id,schema_version,"
          "payload_json,fingerprint) VALUES(?,?,?,?)",
          snapshotInsert) != SQLITE_OK ||
      !bindText(snapshotInsert.get(), 1, *sourceResult.attemptId) ||
      sqlite3_bind_int(snapshotInsert.get(), 2, snapshot.schemaVersion) !=
          SQLITE_OK ||
      !bindText(snapshotInsert.get(), 3, *serializedSnapshot) ||
      !bindText(snapshotInsert.get(), 4, snapshot.fingerprint) ||
      sqlite3_step(snapshotInsert.get()) != SQLITE_DONE) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not store IR submission snapshot"};
  }

  const auto createdAt = readCreatedAt(database, resultId);
  SqliteStatementHandle pendingInsert;
  constexpr const char *pendingSql =
      "INSERT INTO pending_chart_score_writes(attempt_id,result_id,chart_path,"
      "chart_md5,chart_sha256,chart_title,chart_artist,ln_mode,score,max_score,"
      "max_combo,combo_break,pgreat,great,good,bad,poor,kpoor,fast,slow,"
      "final_gauge,clear_type,ruleset_version,eligibility,provenance_json,"
      "created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
  if (!createdAt.has_value() ||
      prepareSqliteStatement(database, pendingSql, pendingInsert) !=
          SQLITE_OK) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not prepare pending compact score"};
  }
  column = 1;
  bound =
      bindText(pendingInsert.get(), column++, *sourceResult.attemptId) &&
      sqlite3_bind_int(pendingInsert.get(), column++, resultId) == SQLITE_OK &&
      bindText(pendingInsert.get(), column++, score.chartPath) &&
      bindText(pendingInsert.get(), column++, score.chartMd5) &&
      bindText(pendingInsert.get(), column++, score.chartSha256) &&
      bindText(pendingInsert.get(), column++, score.chartTitle) &&
      bindText(pendingInsert.get(), column++, score.chartArtist);
  const int pendingIntegers[] = {
      score.longNoteMode, score.score,  score.maxScore, score.maxCombo,
      score.comboBreak,   score.pGreat, score.great,    score.good,
      score.bad,          score.poor,   score.kPoor,    score.fast,
      score.slow};
  for (int value : pendingIntegers) {
    bound = bound &&
            sqlite3_bind_int(pendingInsert.get(), column++, value) == SQLITE_OK;
  }
  bound = bound &&
          sqlite3_bind_double(pendingInsert.get(), column++,
                              score.finalGauge) == SQLITE_OK &&
          sqlite3_bind_int(pendingInsert.get(), column++, score.clearType) ==
              SQLITE_OK &&
          sqlite3_bind_int(pendingInsert.get(), column++,
                           score.provenance.ruleset.version) == SQLITE_OK &&
          sqlite3_bind_int(pendingInsert.get(), column++,
                           static_cast<int>(score.provenance.eligibility)) ==
              SQLITE_OK &&
          bindText(pendingInsert.get(), column++, *provenance) &&
          bindText(pendingInsert.get(), column++, *createdAt);
  if (!bound || column != 27 ||
      sqlite3_step(pendingInsert.get()) != SQLITE_DONE) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not stage pending compact score"};
  }

  const auto drafts = InsertInactiveIrDraftsOnConnection(database, irDrafts);
  if (drafts.status != IrDraftStageStatus::Succeeded) {
    return {.status = drafts.status == IrDraftStageStatus::StorageFailure
                          ? StageStatus::StorageFailure
                          : StageStatus::IntegrityConflict,
            .diagnostic = drafts.diagnostic};
  }
  SqliteStatementHandle deleteReservation;
  if (prepareSqliteStatement(
          database, "DELETE FROM replay_file_reservations WHERE attempt_id=?",
          deleteReservation) != SQLITE_OK ||
      !bindText(deleteReservation.get(), 1, *sourceResult.attemptId) ||
      sqlite3_step(deleteReservation.get()) != SQLITE_DONE ||
      sqlite3_changes(database) != 1) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not consume replay reservation"};
  }
  if (!transaction.commit(transactionError)) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not commit compact result staging"};
  }
  return {.status = StageStatus::Staged,
          .receipt = StageReceipt{.attemptId = *sourceResult.attemptId,
                                  .resultId = resultId,
                                  .createdAt = *createdAt,
                                  .scorePending = true}};
}

} // namespace replay_repository_detail

ReservationOutcome
ReplayRepository::reserveReplayFile(std::string_view attemptId,
                                    std::string_view stem) {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ReservationOutcome::Status::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  return replay_repository_detail::ReserveReplayFileOnConnection(
      impl_->sessionDatabase, attemptId, stem);
}

result_persistence::StageOutcome ReplayRepository::stageCompletedChartAttempt(
    const result_persistence::PersistedChartResult &result,
    const ir::IrSubmissionSnapshot &snapshot,
    const ReplayFileReference &replayFile,
    std::span<const ir::IrOutboxDraft> drafts) {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = result_persistence::StageStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  return replay_repository_detail::StageCompletedChartAttemptOnConnection(
      impl_->sessionDatabase, result, snapshot, replayFile, drafts);
}

ResultReadOutcome ReplayRepository::loadChartResult(int resultId) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ResultReadOutcome::Status::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  return replay_repository_detail::LoadChartResultOnConnection(
      impl_->sessionDatabase, resultId);
}

ir::IrSubmissionSnapshotReadOutcome
ReplayRepository::loadIrSubmissionSnapshot(std::string_view attemptId) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status =
                ir::IrSubmissionSnapshotReadOutcome::Status::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  return replay_repository_detail::LoadIrSubmissionSnapshotOnConnection(
      impl_->sessionDatabase, attemptId);
}
