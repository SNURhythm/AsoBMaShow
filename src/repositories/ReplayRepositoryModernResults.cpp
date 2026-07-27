#include "ReplayRepository.h"
#include "ReplayRepositoryInternal.h"

#include "../ProfileDatabaseActivity.h"
#include "../Uuid.h"
#include "../ir/IrSubmissionSnapshot.h"
#include "../replay/ReplayFormat.h"
#include "SqliteRAII.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <set>
#include <string>
#include <utility>

namespace {

using Json = nlohmann::ordered_json;

constexpr const char *kModernChartColumns =
    "id,attempt_id,chart_path,chart_md5,chart_sha256,chart_title,"
    "chart_artist,long_note_mode,score,max_score,max_combo,combo_break,"
    "p_great,great,good,bad,poor,k_poor,fast,slow,final_gauge,clear_type,"
    "key_mode,adopted_gauge_type,gauge_history_json,judgement_timing_json,"
    "provenance_json,result_fingerprint,played_at_unix_ms,created_at";

bool bindText(sqlite3_stmt *statement, int index, std::string_view value) {
  return sqlite3_bind_text(statement, index, value.data(),
                           static_cast<int>(value.size()),
                           SQLITE_TRANSIENT) == SQLITE_OK;
}

std::optional<std::string>
serializeGaugeHistory(std::span<const float> history) {
  try {
    if (history.size() > durable_payload::kMaximumResultGaugeSamples ||
        std::ranges::any_of(
            history, [](float value) { return !std::isfinite(value); })) {
      return std::nullopt;
    }
    const std::string serialized = Json(history).dump();
    return durable_payload::withinLimit(
               serialized.size(), durable_payload::kMaximumResultPayloadBytes)
               ? std::optional<std::string>(serialized)
               : std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::vector<float>>
deserializeGaugeHistory(std::string_view serialized) {
  try {
    if (!durable_payload::withinLimit(
            serialized.size(), durable_payload::kMaximumResultPayloadBytes)) {
      return std::nullopt;
    }
    const Json value = Json::parse(serialized.begin(), serialized.end());
    auto result = value.get<std::vector<float>>();
    if (result.size() > durable_payload::kMaximumResultGaugeSamples ||
        std::ranges::any_of(
            result, [](float sample) { return !std::isfinite(sample); }) ||
        Json(result).dump() != serialized) {
      return std::nullopt;
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string> serializeJudgementTiming(
    const std::optional<result_persistence::ChartJudgementTiming> &timing) {
  if (!timing.has_value()) {
    return std::string{};
  }
  try {
    Json value = Json::array();
    for (const auto &count : timing->byJudgement) {
      value.push_back(Json::array({count.fast, count.slow}));
    }
    return value.dump();
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<result_persistence::ChartJudgementTiming>
deserializeJudgementTiming(std::string_view serialized) {
  if (serialized.empty()) {
    return std::nullopt;
  }
  try {
    const Json value = Json::parse(serialized.begin(), serialized.end());
    if (!value.is_array() || value.size() != JudgementCount ||
        value.dump() != serialized) {
      return std::nullopt;
    }
    result_persistence::ChartJudgementTiming result;
    for (std::size_t index = 0; index < value.size(); ++index) {
      const auto &pair = value[index];
      if (!pair.is_array() || pair.size() != 2 ||
          !pair[0].is_number_integer() || !pair[1].is_number_integer()) {
        return std::nullopt;
      }
      result.byJudgement[index] = {.fast = pair[0].get<int>(),
                                   .slow = pair[1].get<int>()};
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

bool validAttachment(const ModernReplayFileAttachment &attachment,
                     std::string &diagnostic) {
  diagnostic.clear();
  const auto rebuilt = replay::pathForStem(
      attachment.identity.stem, attachment.identity.historyIndex, diagnostic);
  if (!rebuilt || *rebuilt != attachment.identity ||
      attachment.metadata.relativePath != attachment.identity.relativePath ||
      !replay::isCanonicalLowerHex(attachment.metadata.sha256, 64) ||
      attachment.metadata.compressedSize == 0 ||
      attachment.metadata.compressedSize >
          replay::kReplayLimits.maxCompressedBytes ||
      attachment.metadata.codecVersion !=
          replay::BeatorajaReplayCodec::kCodecVersion) {
    if (diagnostic.empty()) {
      diagnostic = "modern replay attachment is malformed";
    }
    return false;
  }
  return true;
}

std::optional<ModernReplayPathReservation>
decodeReservation(sqlite3_stmt *statement, std::string &diagnostic) {
  if (sqlite3_column_type(statement, 0) != SQLITE_TEXT ||
      sqlite3_column_type(statement, 1) != SQLITE_TEXT ||
      sqlite3_column_type(statement, 2) != SQLITE_INTEGER ||
      sqlite3_column_type(statement, 3) != SQLITE_TEXT ||
      sqlite3_column_type(statement, 4) != SQLITE_INTEGER) {
    diagnostic = "modern replay reservation row has invalid types";
    return std::nullopt;
  }
  ModernReplayPathReservation reservation{
      .attemptId = sqliteColumnString(statement, 0),
      .identity = {.stem = sqliteColumnString(statement, 1),
                   .historyIndex = sqlite3_column_int64(statement, 2),
                   .relativePath =
                       std::filesystem::path(sqliteColumnString(statement, 3))},
      .createdAtUnixMillis = sqlite3_column_int64(statement, 4),
  };
  std::string pathDiagnostic;
  const auto rebuilt =
      replay::pathForStem(reservation.identity.stem,
                          reservation.identity.historyIndex, pathDiagnostic);
  if (!uuid::isCanonicalLowerV4(reservation.attemptId) ||
      reservation.createdAtUnixMillis <= 0 || !rebuilt ||
      *rebuilt != reservation.identity) {
    diagnostic = "modern replay reservation row is inconsistent";
    return std::nullopt;
  }
  diagnostic.clear();
  return reservation;
}

std::optional<result_persistence::ModernChartResult>
decodeModernChartResult(sqlite3_stmt *statement, std::string &diagnostic) {
  const int textColumns[] = {1, 2, 3, 4, 5, 6, 24, 26, 27, 29};
  const int integerColumns[] = {0,  7,  8,  9,  10, 11, 12, 13, 14,
                                15, 16, 17, 18, 19, 21, 22, 23, 28};
  if (std::ranges::any_of(textColumns,
                          [&](int column) {
                            return sqlite3_column_type(statement, column) !=
                                   SQLITE_TEXT;
                          }) ||
      std::ranges::any_of(integerColumns,
                          [&](int column) {
                            return sqlite3_column_type(statement, column) !=
                                   SQLITE_INTEGER;
                          }) ||
      (sqlite3_column_type(statement, 20) != SQLITE_FLOAT &&
       sqlite3_column_type(statement, 20) != SQLITE_INTEGER) ||
      (sqlite3_column_type(statement, 25) != SQLITE_NULL &&
       sqlite3_column_type(statement, 25) != SQLITE_TEXT)) {
    diagnostic = "modern chart result row has invalid types";
    return std::nullopt;
  }

  const std::string gaugeJson = sqliteColumnString(statement, 24);
  auto gaugeHistory = deserializeGaugeHistory(gaugeJson);
  if (!gaugeHistory) {
    diagnostic = "modern chart result gauge history is invalid";
    return std::nullopt;
  }
  std::optional<result_persistence::ChartJudgementTiming> timing;
  if (sqlite3_column_type(statement, 25) == SQLITE_TEXT) {
    const std::string timingJson = sqliteColumnString(statement, 25);
    timing = deserializeJudgementTiming(timingJson);
    if (!timing.has_value()) {
      diagnostic = "modern chart result judgement timing is invalid";
      return std::nullopt;
    }
  }
  const std::string provenanceJson = sqliteColumnString(statement, 26);
  std::string provenanceDiagnostic;
  auto provenance =
      deserializeScoreProvenance(provenanceJson, provenanceDiagnostic);
  if (!provenance || serializeScoreProvenance(*provenance) != provenanceJson) {
    diagnostic = "modern chart result provenance is not canonical";
    return std::nullopt;
  }

  result_persistence::ModernChartResult result{
      .resultId = sqlite3_column_int(statement, 0),
      .attemptId = sqliteColumnString(statement, 1),
      .score = {.chartPath = sqliteColumnString(statement, 2),
                .chartMd5 = sqliteColumnString(statement, 3),
                .chartSha256 = sqliteColumnString(statement, 4),
                .chartTitle = sqliteColumnString(statement, 5),
                .chartArtist = sqliteColumnString(statement, 6),
                .longNoteMode = sqlite3_column_int(statement, 7),
                .score = sqlite3_column_int(statement, 8),
                .maxScore = sqlite3_column_int(statement, 9),
                .maxCombo = sqlite3_column_int(statement, 10),
                .comboBreak = sqlite3_column_int(statement, 11),
                .pGreat = sqlite3_column_int(statement, 12),
                .great = sqlite3_column_int(statement, 13),
                .good = sqlite3_column_int(statement, 14),
                .bad = sqlite3_column_int(statement, 15),
                .poor = sqlite3_column_int(statement, 16),
                .kPoor = sqlite3_column_int(statement, 17),
                .fast = sqlite3_column_int(statement, 18),
                .slow = sqlite3_column_int(statement, 19),
                .finalGauge =
                    static_cast<float>(sqlite3_column_double(statement, 20)),
                .clearType = sqlite3_column_int(statement, 21),
                .provenance = std::move(*provenance)},
      .keyMode = sqlite3_column_int(statement, 22),
      .adoptedGaugeType =
          static_cast<GaugeType>(sqlite3_column_int(statement, 23)),
      .adoptedGaugeHistory = std::move(*gaugeHistory),
      .judgementTiming = std::move(timing),
      .playedAtUnixMillis = sqlite3_column_int64(statement, 28),
      .resultFingerprint = sqliteColumnString(statement, 27),
  };
  if (result.resultId <= 0 ||
      !result_persistence::validateModernChartResult(result, diagnostic)) {
    if (diagnostic.empty()) {
      diagnostic = "modern chart result row is invalid";
    }
    return std::nullopt;
  }
  return result;
}

enum class ReadRecordStatus { Loaded, NotFound, Invalid, StorageFailure };

struct ReadRecordOutcome {
  ReadRecordStatus status = ReadRecordStatus::StorageFailure;
  std::optional<ModernChartResultRecord> record;
  std::string createdAt;
  std::string diagnostic;
};

std::optional<ModernReplayFileReference>
readReplayReference(sqlite3 *database, int resultId, bool &found,
                    std::string &diagnostic, bool courseOwner = false) {
  found = false;
  SqliteStatementHandle statement;
  const std::string ownerColumn =
      courseOwner ? "modern_course_result_id" : "modern_chart_result_id";
  const std::string query =
      "SELECT id," + ownerColumn +
      ",stem,history_index,relative_path,content_sha256,compressed_size,"
      "codec_version FROM modern_replay_files WHERE " +
      ownerColumn + "=?";
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK ||
      sqlite3_bind_int(statement.get(), 1, resultId) != SQLITE_OK) {
    diagnostic = "could not prepare modern replay reference read";
    return std::nullopt;
  }
  const int rc = sqlite3_step(statement.get());
  if (rc == SQLITE_DONE) {
    diagnostic.clear();
    return std::nullopt;
  }
  if (rc != SQLITE_ROW) {
    diagnostic = "could not read modern replay reference";
    return std::nullopt;
  }
  found = true;
  if (sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER ||
      sqlite3_column_type(statement.get(), 1) != SQLITE_INTEGER ||
      sqlite3_column_type(statement.get(), 2) != SQLITE_TEXT ||
      sqlite3_column_type(statement.get(), 3) != SQLITE_INTEGER ||
      sqlite3_column_type(statement.get(), 4) != SQLITE_TEXT ||
      sqlite3_column_type(statement.get(), 5) != SQLITE_TEXT ||
      sqlite3_column_type(statement.get(), 6) != SQLITE_INTEGER ||
      sqlite3_column_type(statement.get(), 7) != SQLITE_INTEGER) {
    diagnostic = "modern replay reference row has invalid types";
    return std::nullopt;
  }
  const auto compressedSize = sqlite3_column_int64(statement.get(), 6);
  if (compressedSize <= 0) {
    diagnostic = "modern replay reference size is invalid";
    return std::nullopt;
  }
  ModernReplayFileReference reference{
      .id = sqlite3_column_int64(statement.get(), 0),
      .resultId = sqlite3_column_int(statement.get(), 1),
      .identity = {.stem = sqliteColumnString(statement.get(), 2),
                   .historyIndex = sqlite3_column_int64(statement.get(), 3),
                   .relativePath = std::filesystem::path(
                       sqliteColumnString(statement.get(), 4))},
      .metadata = {.relativePath = std::filesystem::path(
                       sqliteColumnString(statement.get(), 4)),
                   .sha256 = sqliteColumnString(statement.get(), 5),
                   .compressedSize = static_cast<std::uint64_t>(compressedSize),
                   .codecVersion = sqlite3_column_int(statement.get(), 7)},
  };
  ModernReplayFileAttachment attachment{.identity = reference.identity,
                                        .metadata = reference.metadata};
  if (reference.id <= 0 || reference.resultId != resultId ||
      !validAttachment(attachment, diagnostic) ||
      sqlite3_step(statement.get()) != SQLITE_DONE) {
    if (diagnostic.empty()) {
      diagnostic = "modern replay reference row is inconsistent";
    }
    return std::nullopt;
  }
  diagnostic.clear();
  return reference;
}

ReadRecordOutcome readRecord(sqlite3 *database, const char *predicate,
                             std::string_view textValue, int intValue) {
  SqliteStatementHandle statement;
  const std::string query = std::string("SELECT ") + kModernChartColumns +
                            " FROM modern_chart_results WHERE " + predicate;
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK) {
    return {.diagnostic = "could not prepare modern chart result read"};
  }
  const bool bound =
      textValue.empty()
          ? sqlite3_bind_int(statement.get(), 1, intValue) == SQLITE_OK
          : bindText(statement.get(), 1, textValue);
  if (!bound) {
    return {.diagnostic = "could not bind modern chart result identity"};
  }
  const int rc = sqlite3_step(statement.get());
  if (rc == SQLITE_DONE) {
    return {.status = ReadRecordStatus::NotFound};
  }
  if (rc != SQLITE_ROW) {
    return {.diagnostic = "could not read modern chart result"};
  }
  std::string diagnostic;
  auto result = decodeModernChartResult(statement.get(), diagnostic);
  if (!result) {
    return {.status = ReadRecordStatus::Invalid,
            .diagnostic = std::move(diagnostic)};
  }
  const std::string createdAt = sqliteColumnString(statement.get(), 29);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    return {.status = ReadRecordStatus::Invalid,
            .diagnostic = "modern chart result identity is not unique"};
  }
  bool referenceFound = false;
  auto reference = readReplayReference(database, result->resultId,
                                       referenceFound, diagnostic);
  if (!reference && !diagnostic.empty()) {
    return {.status = referenceFound ? ReadRecordStatus::Invalid
                                     : ReadRecordStatus::StorageFailure,
            .diagnostic = std::move(diagnostic)};
  }
  return {.status = ReadRecordStatus::Loaded,
          .record = ModernChartResultRecord{.result = std::move(*result),
                                            .replayFile = std::move(reference)},
          .createdAt = createdAt};
}

std::optional<ir::IrSubmissionSnapshot> readSnapshot(sqlite3 *database,
                                                     std::string_view attemptId,
                                                     bool &found,
                                                     std::string &diagnostic) {
  found = false;
  SqliteStatementHandle statement;
  constexpr const char *query =
      "SELECT schema_version,payload_json,fingerprint FROM "
      "ir_submission_snapshots WHERE attempt_id=?";
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK ||
      !bindText(statement.get(), 1, attemptId)) {
    diagnostic = "could not prepare modern IR snapshot read";
    return std::nullopt;
  }
  const int rc = sqlite3_step(statement.get());
  if (rc == SQLITE_DONE) {
    diagnostic.clear();
    return std::nullopt;
  }
  if (rc != SQLITE_ROW) {
    diagnostic = "could not read modern IR snapshot";
    return std::nullopt;
  }
  found = true;
  if (sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER ||
      sqlite3_column_type(statement.get(), 1) != SQLITE_TEXT ||
      sqlite3_column_type(statement.get(), 2) != SQLITE_TEXT ||
      sqlite3_column_int(statement.get(), 0) !=
          ir::IrSubmissionSnapshot::kSchemaVersion) {
    diagnostic = "modern IR snapshot row has invalid types";
    return std::nullopt;
  }
  const std::string payload = sqliteColumnString(statement.get(), 1);
  const std::string fingerprint = sqliteColumnString(statement.get(), 2);
  auto result =
      ir::deserializeIrSubmissionSnapshot(payload, fingerprint, diagnostic);
  if (!result || result->submission.attemptId != attemptId ||
      sqlite3_step(statement.get()) != SQLITE_DONE) {
    if (diagnostic.empty()) {
      diagnostic = "modern IR snapshot row is inconsistent";
    }
    return std::nullopt;
  }
  return result;
}

bool validateDrafts(const result_persistence::ModernChartResult &result,
                    const std::optional<ir::IrSubmissionSnapshot> &snapshot,
                    std::span<const ir::IrOutboxDraft> drafts,
                    std::string &diagnostic) {
  if (!drafts.empty() && !snapshot.has_value()) {
    diagnostic = "IR drafts require a durable modern snapshot";
    return false;
  }
  std::set<std::string> providers;
  for (const auto &draft : drafts) {
    if (!ir::validateIrOutboxDraft(draft, diagnostic) ||
        draft.attemptId != result.attemptId ||
        draft.chartMd5 != result.score.chartMd5 ||
        draft.chartSha256 != result.score.chartSha256 ||
        !providers.insert(draft.providerId).second) {
      if (diagnostic.empty()) {
        diagnostic = "IR draft disagrees with the modern result";
      }
      return false;
    }
  }
  return true;
}

bool insertReadyDrafts(sqlite3 *database,
                       std::span<const ir::IrOutboxDraft> drafts) {
  SqliteStatementHandle statement;
  constexpr const char *query =
      "INSERT INTO ir_outbox(provider_id,attempt_id,chart_md5,chart_sha256,"
      "payload_json,ruleset_id,ruleset_revision,validation_fingerprint,state,"
      "local_result_ready,next_request_user_intent,created_at_ms,updated_at_ms)"
      " VALUES(?,?,?,?,?,?,?,?,0,1,0,?,?)";
  if (!drafts.empty() &&
      prepareSqliteStatement(database, query, statement) != SQLITE_OK) {
    return false;
  }
  for (const auto &draft : drafts) {
    if (sqlite3_reset(statement.get()) != SQLITE_OK ||
        sqlite3_clear_bindings(statement.get()) != SQLITE_OK ||
        !bindText(statement.get(), 1, draft.providerId) ||
        !bindText(statement.get(), 2, draft.attemptId) ||
        (draft.chartMd5.empty()
             ? sqlite3_bind_null(statement.get(), 3) != SQLITE_OK
             : !bindText(statement.get(), 3, draft.chartMd5)) ||
        !bindText(statement.get(), 4, draft.chartSha256) ||
        !bindText(statement.get(), 5, draft.payloadJson) ||
        !bindText(statement.get(), 6, draft.rulesetProof.rulesetId) ||
        sqlite3_bind_int(statement.get(), 7,
                         draft.rulesetProof.rulesetRevision) != SQLITE_OK ||
        !bindText(statement.get(), 8,
                  draft.rulesetProof.validationFingerprint) ||
        sqlite3_bind_int64(statement.get(), 9, draft.createdAtUnixMillis) !=
            SQLITE_OK ||
        sqlite3_bind_int64(statement.get(), 10, draft.createdAtUnixMillis) !=
            SQLITE_OK ||
        sqlite3_step(statement.get()) != SQLITE_DONE ||
        sqlite3_changes(database) != 1) {
      return false;
    }
  }
  return true;
}

bool verifyDrafts(sqlite3 *database, std::string_view attemptId,
                  std::span<const ir::IrOutboxDraft> drafts,
                  std::string &diagnostic) {
  const auto verified = replay_repository_detail::VerifyIrDraftsOnConnection(
      database, attemptId, drafts);
  if (verified.status !=
      replay_repository_detail::IrDraftStageStatus::Succeeded) {
    diagnostic = verified.diagnostic;
    return false;
  }
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(
          database,
          "SELECT COUNT(*) FROM ir_outbox WHERE attempt_id=? AND "
          "local_result_ready!=1",
          statement) != SQLITE_OK ||
      !bindText(statement.get(), 1, attemptId) ||
      sqlite3_step(statement.get()) != SQLITE_ROW ||
      sqlite3_column_int(statement.get(), 0) != 0) {
    diagnostic = "modern IR outbox readiness differs";
    return false;
  }
  return true;
}

bool insertResult(sqlite3 *database,
                  const result_persistence::ModernChartResult &result,
                  std::string_view gaugeJson, std::string_view timingJson,
                  std::string_view provenanceJson, int &resultId) {
  SqliteStatementHandle statement;
  constexpr const char *query =
      "INSERT INTO modern_chart_results(attempt_id,chart_path,chart_md5,"
      "chart_sha256,chart_title,chart_artist,long_note_mode,score,max_score,"
      "max_combo,combo_break,p_great,great,good,bad,poor,k_poor,fast,slow,"
      "final_gauge,clear_type,key_mode,adopted_gauge_type,gauge_history_json,"
      "judgement_timing_json,provenance_json,result_fingerprint,"
      "played_at_unix_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,"
      "?,?,?,?,?,?)";
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK) {
    return false;
  }
  const auto &score = result.score;
  int index = 1;
  bool bound = bindText(statement.get(), index++, result.attemptId) &&
               bindText(statement.get(), index++, score.chartPath) &&
               bindText(statement.get(), index++, score.chartMd5) &&
               bindText(statement.get(), index++, score.chartSha256) &&
               bindText(statement.get(), index++, score.chartTitle) &&
               bindText(statement.get(), index++, score.chartArtist);
  const int integers[] = {score.longNoteMode, score.score,      score.maxScore,
                          score.maxCombo,     score.comboBreak, score.pGreat,
                          score.great,        score.good,       score.bad,
                          score.poor,         score.kPoor,      score.fast,
                          score.slow};
  for (const int value : integers) {
    bound =
        bound && sqlite3_bind_int(statement.get(), index++, value) == SQLITE_OK;
  }
  bound =
      bound &&
      sqlite3_bind_double(statement.get(), index++, score.finalGauge) ==
          SQLITE_OK &&
      sqlite3_bind_int(statement.get(), index++, score.clearType) ==
          SQLITE_OK &&
      sqlite3_bind_int(statement.get(), index++, result.keyMode) == SQLITE_OK &&
      sqlite3_bind_int(statement.get(), index++,
                       static_cast<int>(result.adoptedGaugeType)) ==
          SQLITE_OK &&
      bindText(statement.get(), index++, gaugeJson);
  if (timingJson.empty()) {
    bound = bound && sqlite3_bind_null(statement.get(), index++) == SQLITE_OK;
  } else {
    bound = bound && bindText(statement.get(), index++, timingJson);
  }
  bound = bound && bindText(statement.get(), index++, provenanceJson) &&
          bindText(statement.get(), index++, result.resultFingerprint) &&
          sqlite3_bind_int64(statement.get(), index++,
                             result.playedAtUnixMillis) == SQLITE_OK;
  if (!bound || index != 29 || sqlite3_step(statement.get()) != SQLITE_DONE ||
      sqlite3_changes(database) != 1) {
    return false;
  }
  const sqlite3_int64 inserted = sqlite3_last_insert_rowid(database);
  if (inserted <= 0 || inserted > std::numeric_limits<int>::max()) {
    return false;
  }
  resultId = static_cast<int>(inserted);
  return true;
}

result_persistence::PendingReadOutcome
readPendingModernScore(sqlite3 *database, std::string_view attemptId) {
  using result_persistence::PendingReadStatus;
  SqliteStatementHandle statement;
  constexpr const char *query =
      "SELECT pending.modern_chart_result_id,pending.created_at,"
      "result.created_at,result.attempt_id FROM "
      "modern_pending_chart_score_writes pending LEFT JOIN "
      "modern_chart_results result ON result.id="
      "pending.modern_chart_result_id WHERE pending.attempt_id=?";
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK ||
      !bindText(statement.get(), 1, attemptId)) {
    return {.status = PendingReadStatus::StorageFailure,
            .diagnostic = "could not prepare modern pending score read"};
  }
  const int rc = sqlite3_step(statement.get());
  if (rc == SQLITE_DONE) {
    return {.status = PendingReadStatus::NotFound};
  }
  if (rc != SQLITE_ROW ||
      sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER ||
      sqlite3_column_type(statement.get(), 1) != SQLITE_TEXT ||
      sqlite3_column_type(statement.get(), 2) != SQLITE_TEXT ||
      sqlite3_column_type(statement.get(), 3) != SQLITE_TEXT) {
    return {.status = rc == SQLITE_ROW ? PendingReadStatus::IntegrityConflict
                                       : PendingReadStatus::StorageFailure,
            .diagnostic = "modern pending score row has invalid types"};
  }
  const sqlite3_int64 rawId = sqlite3_column_int64(statement.get(), 0);
  const std::string pendingCreatedAt = sqliteColumnString(statement.get(), 1);
  const std::string resultCreatedAt = sqliteColumnString(statement.get(), 2);
  const std::string resultAttemptId = sqliteColumnString(statement.get(), 3);
  if (rawId <= 0 || rawId > std::numeric_limits<int>::max() ||
      pendingCreatedAt.empty() || pendingCreatedAt != resultCreatedAt ||
      resultAttemptId != attemptId ||
      sqlite3_step(statement.get()) != SQLITE_DONE) {
    return {.status = PendingReadStatus::IntegrityConflict,
            .diagnostic = "modern pending score row is inconsistent"};
  }
  const int resultId = static_cast<int>(rawId);
  auto loaded = readRecord(database, "id=?", {}, resultId);
  if (loaded.status == ReadRecordStatus::StorageFailure) {
    return {.status = PendingReadStatus::StorageFailure,
            .diagnostic = std::move(loaded.diagnostic)};
  }
  if (loaded.status != ReadRecordStatus::Loaded || !loaded.record ||
      loaded.record->result.attemptId != attemptId) {
    return {.status = PendingReadStatus::IntegrityConflict,
            .diagnostic = loaded.diagnostic.empty()
                              ? "modern pending score owner is inconsistent"
                              : std::move(loaded.diagnostic)};
  }
  return {
      .status = PendingReadStatus::Found,
      .value =
          result_persistence::PendingChartScoreWrite{
              .attemptId = std::string(attemptId),
              .modernResultId = resultId,
              .createdAt = std::move(pendingCreatedAt),
              .score = std::move(loaded.record->result.score),
          },
  };
}

constexpr const char *kModernCourseColumns =
    "id,attempt_id,course_key,legacy_course_id,course_name,"
    "course_group_name,constraint_json,completed_charts,total_charts,"
    "requested_play_option,assist_option,initial_gauge_type,gauge_profile,"
    "gauge_auto_shift,gauge_auto_shift_lower_bound,long_note_mode,"
    "final_score,max_score,max_combo,final_gauge,clear_type,provenance_json,"
    "result_fingerprint,played_at_unix_ms,created_at";

std::optional<result_persistence::ModernCourseStageResult>
decodeModernCourseStage(sqlite3_stmt *statement, std::string &diagnostic) {
  const int textColumns[] = {1, 2, 3, 4, 5, 23, 25};
  const int integerColumns[] = {0,  6,  7,  8,  9,  10, 11, 12, 13,
                                14, 15, 16, 17, 18, 20, 21, 22};
  if (std::ranges::any_of(textColumns,
                          [&](int column) {
                            return sqlite3_column_type(statement, column) !=
                                   SQLITE_TEXT;
                          }) ||
      std::ranges::any_of(integerColumns,
                          [&](int column) {
                            return sqlite3_column_type(statement, column) !=
                                   SQLITE_INTEGER;
                          }) ||
      (sqlite3_column_type(statement, 19) != SQLITE_FLOAT &&
       sqlite3_column_type(statement, 19) != SQLITE_INTEGER) ||
      (sqlite3_column_type(statement, 24) != SQLITE_NULL &&
       sqlite3_column_type(statement, 24) != SQLITE_TEXT)) {
    diagnostic = "modern course stage row has invalid types";
    return std::nullopt;
  }
  auto gaugeHistory =
      deserializeGaugeHistory(sqliteColumnString(statement, 23));
  if (!gaugeHistory) {
    diagnostic = "modern course stage gauge history is invalid";
    return std::nullopt;
  }
  std::optional<result_persistence::ChartJudgementTiming> timing;
  if (sqlite3_column_type(statement, 24) == SQLITE_TEXT) {
    timing = deserializeJudgementTiming(sqliteColumnString(statement, 24));
    if (!timing) {
      diagnostic = "modern course stage judgement timing is invalid";
      return std::nullopt;
    }
  }
  const std::string provenanceJson = sqliteColumnString(statement, 25);
  std::string provenanceDiagnostic;
  auto provenance =
      deserializeScoreProvenance(provenanceJson, provenanceDiagnostic);
  if (!provenance || serializeScoreProvenance(*provenance) != provenanceJson) {
    diagnostic = "modern course stage provenance is not canonical";
    return std::nullopt;
  }
  return result_persistence::ModernCourseStageResult{
      .stageIndex = sqlite3_column_int(statement, 0),
      .score = {.chartPath = sqliteColumnString(statement, 1),
                .chartMd5 = sqliteColumnString(statement, 2),
                .chartSha256 = sqliteColumnString(statement, 3),
                .chartTitle = sqliteColumnString(statement, 4),
                .chartArtist = sqliteColumnString(statement, 5),
                .longNoteMode = sqlite3_column_int(statement, 6),
                .score = sqlite3_column_int(statement, 7),
                .maxScore = sqlite3_column_int(statement, 8),
                .maxCombo = sqlite3_column_int(statement, 9),
                .comboBreak = sqlite3_column_int(statement, 10),
                .pGreat = sqlite3_column_int(statement, 11),
                .great = sqlite3_column_int(statement, 12),
                .good = sqlite3_column_int(statement, 13),
                .bad = sqlite3_column_int(statement, 14),
                .poor = sqlite3_column_int(statement, 15),
                .kPoor = sqlite3_column_int(statement, 16),
                .fast = sqlite3_column_int(statement, 17),
                .slow = sqlite3_column_int(statement, 18),
                .finalGauge =
                    static_cast<float>(sqlite3_column_double(statement, 19)),
                .clearType = sqlite3_column_int(statement, 20),
                .provenance = std::move(*provenance)},
      .keyMode = sqlite3_column_int(statement, 21),
      .adoptedGaugeType =
          static_cast<GaugeType>(sqlite3_column_int(statement, 22)),
      .adoptedGaugeHistory = std::move(*gaugeHistory),
      .judgementTiming = std::move(timing),
  };
}

struct ReadCourseRecordOutcome {
  ReadRecordStatus status = ReadRecordStatus::StorageFailure;
  std::optional<ModernCourseResultRecord> record;
  std::string createdAt;
  std::string diagnostic;
};

ReadCourseRecordOutcome readCourseRecord(sqlite3 *database,
                                         const char *predicate,
                                         std::string_view textValue,
                                         int intValue) {
  SqliteStatementHandle statement;
  const std::string query = std::string("SELECT ") + kModernCourseColumns +
                            " FROM modern_course_results WHERE " + predicate;
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK) {
    return {.diagnostic = "could not prepare modern course result read"};
  }
  const bool bound =
      textValue.empty()
          ? sqlite3_bind_int(statement.get(), 1, intValue) == SQLITE_OK
          : bindText(statement.get(), 1, textValue);
  if (!bound) {
    return {.diagnostic = "could not bind modern course result identity"};
  }
  const int rc = sqlite3_step(statement.get());
  if (rc == SQLITE_DONE) {
    return {.status = ReadRecordStatus::NotFound};
  }
  if (rc != SQLITE_ROW) {
    return {.diagnostic = "could not read modern course result"};
  }
  const int textColumns[] = {1, 2, 4, 5, 6, 9, 10, 21, 22, 24};
  const int integerColumns[] = {0,  3,  7,  8,  11, 12, 13,
                                14, 15, 16, 17, 18, 20, 23};
  if (std::ranges::any_of(textColumns,
                          [&](int column) {
                            return sqlite3_column_type(statement.get(),
                                                       column) != SQLITE_TEXT;
                          }) ||
      std::ranges::any_of(integerColumns,
                          [&](int column) {
                            return sqlite3_column_type(statement.get(),
                                                       column) !=
                                   SQLITE_INTEGER;
                          }) ||
      (sqlite3_column_type(statement.get(), 19) != SQLITE_FLOAT &&
       sqlite3_column_type(statement.get(), 19) != SQLITE_INTEGER)) {
    return {.status = ReadRecordStatus::Invalid,
            .diagnostic = "modern course result row has invalid types"};
  }
  const std::string provenanceJson = sqliteColumnString(statement.get(), 21);
  std::string provenanceDiagnostic;
  auto provenance =
      deserializeScoreProvenance(provenanceJson, provenanceDiagnostic);
  if (!provenance || serializeScoreProvenance(*provenance) != provenanceJson) {
    return {.status = ReadRecordStatus::Invalid,
            .diagnostic = "modern course provenance is not canonical"};
  }
  result_persistence::ModernCourseResult result{
      .resultId = sqlite3_column_int(statement.get(), 0),
      .attemptId = sqliteColumnString(statement.get(), 1),
      .courseKey = sqliteColumnString(statement.get(), 2),
      .legacyCourseId = sqlite3_column_int(statement.get(), 3),
      .courseName = sqliteColumnString(statement.get(), 4),
      .courseGroupName = sqliteColumnString(statement.get(), 5),
      .constraintJson = sqliteColumnString(statement.get(), 6),
      .completedCharts = sqlite3_column_int(statement.get(), 7),
      .totalCharts = sqlite3_column_int(statement.get(), 8),
      .requestedPlayOption = sqliteColumnString(statement.get(), 9),
      .assistOption = sqliteColumnString(statement.get(), 10),
      .initialGaugeType =
          static_cast<GaugeType>(sqlite3_column_int(statement.get(), 11)),
      .gaugeProfile =
          static_cast<GaugeProfile>(sqlite3_column_int(statement.get(), 12)),
      .gaugeAutoShift = static_cast<GaugeAutoShiftMode>(
          sqlite3_column_int(statement.get(), 13)),
      .gaugeAutoShiftLowerBound =
          static_cast<GaugeType>(sqlite3_column_int(statement.get(), 14)),
      .longNoteMode = sqlite3_column_int(statement.get(), 15),
      .finalScore = sqlite3_column_int(statement.get(), 16),
      .maxScore = sqlite3_column_int(statement.get(), 17),
      .maxCombo = sqlite3_column_int(statement.get(), 18),
      .finalGauge =
          static_cast<float>(sqlite3_column_double(statement.get(), 19)),
      .clearType = sqlite3_column_int(statement.get(), 20),
      .provenance = std::move(*provenance),
      .playedAtUnixMillis = sqlite3_column_int64(statement.get(), 23),
      .resultFingerprint = sqliteColumnString(statement.get(), 22),
  };
  const std::string createdAt = sqliteColumnString(statement.get(), 24);
  if (result.resultId <= 0 || sqlite3_step(statement.get()) != SQLITE_DONE) {
    return {.status = ReadRecordStatus::Invalid,
            .diagnostic = "modern course result identity is not unique"};
  }
  statement.reset();

  if (prepareSqliteStatement(
          database,
          "SELECT entry_index,total_notes,play_length_micros FROM "
          "modern_course_entries WHERE modern_course_result_id=? ORDER BY "
          "entry_index",
          statement) != SQLITE_OK ||
      sqlite3_bind_int(statement.get(), 1, result.resultId) != SQLITE_OK) {
    return {.diagnostic = "could not prepare modern course entry read"};
  }
  int childRc = SQLITE_OK;
  while ((childRc = sqlite3_step(statement.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 1) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 2) != SQLITE_INTEGER ||
        sqlite3_column_int(statement.get(), 0) !=
            static_cast<int>(result.entryFacts.size())) {
      return {.status = ReadRecordStatus::Invalid,
              .diagnostic = "modern course entries are not contiguous"};
    }
    result.entryFacts.push_back(
        {.totalNotes = sqlite3_column_int(statement.get(), 1),
         .playLengthMicros = sqlite3_column_int64(statement.get(), 2)});
  }
  if (childRc != SQLITE_DONE) {
    return {.diagnostic = "could not finish modern course entry read"};
  }
  statement.reset();

  if (prepareSqliteStatement(
          database,
          "SELECT stage_index,chart_path,chart_md5,chart_sha256,chart_title,"
          "chart_artist,long_note_mode,score,max_score,max_combo,combo_break,"
          "p_great,great,good,bad,poor,k_poor,fast,slow,final_gauge,clear_type,"
          "key_mode,adopted_gauge_type,gauge_history_json,"
          "judgement_timing_json,provenance_json FROM modern_course_stages "
          "WHERE modern_course_result_id=? ORDER BY stage_index",
          statement) != SQLITE_OK ||
      sqlite3_bind_int(statement.get(), 1, result.resultId) != SQLITE_OK) {
    return {.diagnostic = "could not prepare modern course stage read"};
  }
  while ((childRc = sqlite3_step(statement.get())) == SQLITE_ROW) {
    std::string diagnostic;
    auto stage = decodeModernCourseStage(statement.get(), diagnostic);
    if (!stage || stage->stageIndex != static_cast<int>(result.stages.size())) {
      return {.status = ReadRecordStatus::Invalid,
              .diagnostic = diagnostic.empty()
                                ? "modern course stages are not contiguous"
                                : std::move(diagnostic)};
    }
    result.stages.push_back(std::move(*stage));
  }
  if (childRc != SQLITE_DONE) {
    return {.diagnostic = "could not finish modern course stage read"};
  }
  std::string diagnostic;
  if (!result_persistence::validateModernCourseResult(result, diagnostic)) {
    return {.status = ReadRecordStatus::Invalid,
            .diagnostic = std::move(diagnostic)};
  }
  bool referenceFound = false;
  auto reference = readReplayReference(database, result.resultId,
                                       referenceFound, diagnostic, true);
  if (!reference && !diagnostic.empty()) {
    return {.status = referenceFound ? ReadRecordStatus::Invalid
                                     : ReadRecordStatus::StorageFailure,
            .diagnostic = std::move(diagnostic)};
  }
  return {.status = ReadRecordStatus::Loaded,
          .record =
              ModernCourseResultRecord{.result = std::move(result),
                                       .replayFile = std::move(reference)},
          .createdAt = createdAt};
}

bool insertModernCourseResult(
    sqlite3 *database, const result_persistence::ModernCourseResult &result,
    std::string_view provenanceJson, int &resultId) {
  SqliteStatementHandle statement;
  constexpr const char *query =
      "INSERT INTO modern_course_results(attempt_id,course_key,"
      "legacy_course_id,course_name,course_group_name,constraint_json,"
      "completed_charts,total_charts,requested_play_option,assist_option,"
      "initial_gauge_type,gauge_profile,gauge_auto_shift,"
      "gauge_auto_shift_lower_bound,long_note_mode,final_score,max_score,"
      "max_combo,final_gauge,clear_type,provenance_json,result_fingerprint,"
      "played_at_unix_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,"
      "?)";
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK) {
    return false;
  }
  int index = 1;
  bool bound = bindText(statement.get(), index++, result.attemptId) &&
               bindText(statement.get(), index++, result.courseKey) &&
               sqlite3_bind_int(statement.get(), index++,
                                result.legacyCourseId) == SQLITE_OK &&
               bindText(statement.get(), index++, result.courseName) &&
               bindText(statement.get(), index++, result.courseGroupName) &&
               bindText(statement.get(), index++, result.constraintJson);
  const int integers[] = {
      result.completedCharts,
      result.totalCharts,
  };
  for (const int value : integers) {
    bound =
        bound && sqlite3_bind_int(statement.get(), index++, value) == SQLITE_OK;
  }
  bound = bound &&
          bindText(statement.get(), index++, result.requestedPlayOption) &&
          bindText(statement.get(), index++, result.assistOption);
  const int setupAndAggregate[] = {
      static_cast<int>(result.initialGaugeType),
      static_cast<int>(result.gaugeProfile),
      static_cast<int>(result.gaugeAutoShift),
      static_cast<int>(result.gaugeAutoShiftLowerBound),
      result.longNoteMode,
      result.finalScore,
      result.maxScore,
      result.maxCombo,
  };
  for (const int value : setupAndAggregate) {
    bound =
        bound && sqlite3_bind_int(statement.get(), index++, value) == SQLITE_OK;
  }
  bound = bound &&
          sqlite3_bind_double(statement.get(), index++, result.finalGauge) ==
              SQLITE_OK &&
          sqlite3_bind_int(statement.get(), index++, result.clearType) ==
              SQLITE_OK &&
          bindText(statement.get(), index++, provenanceJson) &&
          bindText(statement.get(), index++, result.resultFingerprint) &&
          sqlite3_bind_int64(statement.get(), index++,
                             result.playedAtUnixMillis) == SQLITE_OK;
  if (!bound || index != 24 || sqlite3_step(statement.get()) != SQLITE_DONE ||
      sqlite3_changes(database) != 1) {
    return false;
  }
  const sqlite3_int64 inserted = sqlite3_last_insert_rowid(database);
  if (inserted <= 0 || inserted > std::numeric_limits<int>::max()) {
    return false;
  }
  resultId = static_cast<int>(inserted);
  return true;
}

bool insertModernCourseChildren(
    sqlite3 *database, int resultId,
    const result_persistence::ModernCourseResult &result) {
  SqliteStatementHandle entry;
  if (prepareSqliteStatement(
          database,
          "INSERT INTO modern_course_entries(modern_course_result_id,"
          "entry_index,total_notes,play_length_micros) VALUES(?,?,?,?)",
          entry) != SQLITE_OK) {
    return false;
  }
  for (std::size_t index = 0; index < result.entryFacts.size(); ++index) {
    const auto &value = result.entryFacts[index];
    if (sqlite3_reset(entry.get()) != SQLITE_OK ||
        sqlite3_clear_bindings(entry.get()) != SQLITE_OK ||
        sqlite3_bind_int(entry.get(), 1, resultId) != SQLITE_OK ||
        sqlite3_bind_int(entry.get(), 2, static_cast<int>(index)) !=
            SQLITE_OK ||
        sqlite3_bind_int(entry.get(), 3, value.totalNotes) != SQLITE_OK ||
        sqlite3_bind_int64(entry.get(), 4, value.playLengthMicros) !=
            SQLITE_OK ||
        sqlite3_step(entry.get()) != SQLITE_DONE ||
        sqlite3_changes(database) != 1) {
      return false;
    }
  }

  SqliteStatementHandle stage;
  constexpr const char *stageQuery =
      "INSERT INTO modern_course_stages(modern_course_result_id,stage_index,"
      "chart_path,chart_md5,chart_sha256,chart_title,chart_artist,"
      "long_note_mode,score,max_score,max_combo,combo_break,p_great,great,"
      "good,bad,poor,k_poor,fast,slow,final_gauge,clear_type,key_mode,"
      "adopted_gauge_type,gauge_history_json,judgement_timing_json,"
      "provenance_json) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,"
      "?,?,?,?)";
  if (prepareSqliteStatement(database, stageQuery, stage) != SQLITE_OK) {
    return false;
  }
  for (const auto &value : result.stages) {
    const auto gaugeJson = serializeGaugeHistory(value.adoptedGaugeHistory);
    const auto timingJson = serializeJudgementTiming(value.judgementTiming);
    std::string diagnostic;
    const auto provenanceJson =
        serializeValidatedScoreProvenance(value.score.provenance, diagnostic);
    if (!gaugeJson || !timingJson || !provenanceJson ||
        sqlite3_reset(stage.get()) != SQLITE_OK ||
        sqlite3_clear_bindings(stage.get()) != SQLITE_OK) {
      return false;
    }
    int index = 1;
    bool bound =
        sqlite3_bind_int(stage.get(), index++, resultId) == SQLITE_OK &&
        sqlite3_bind_int(stage.get(), index++, value.stageIndex) == SQLITE_OK &&
        bindText(stage.get(), index++, value.score.chartPath) &&
        bindText(stage.get(), index++, value.score.chartMd5) &&
        bindText(stage.get(), index++, value.score.chartSha256) &&
        bindText(stage.get(), index++, value.score.chartTitle) &&
        bindText(stage.get(), index++, value.score.chartArtist);
    const int scoreValues[] = {
        value.score.longNoteMode, value.score.score,      value.score.maxScore,
        value.score.maxCombo,     value.score.comboBreak, value.score.pGreat,
        value.score.great,        value.score.good,       value.score.bad,
        value.score.poor,         value.score.kPoor,      value.score.fast,
        value.score.slow,
    };
    for (const int scoreValue : scoreValues) {
      bound = bound &&
              sqlite3_bind_int(stage.get(), index++, scoreValue) == SQLITE_OK;
    }
    bound =
        bound &&
        sqlite3_bind_double(stage.get(), index++, value.score.finalGauge) ==
            SQLITE_OK &&
        sqlite3_bind_int(stage.get(), index++, value.score.clearType) ==
            SQLITE_OK &&
        sqlite3_bind_int(stage.get(), index++, value.keyMode) == SQLITE_OK &&
        sqlite3_bind_int(stage.get(), index++,
                         static_cast<int>(value.adoptedGaugeType)) ==
            SQLITE_OK &&
        bindText(stage.get(), index++, *gaugeJson);
    if (timingJson->empty()) {
      bound = bound && sqlite3_bind_null(stage.get(), index++) == SQLITE_OK;
    } else {
      bound = bound && bindText(stage.get(), index++, *timingJson);
    }
    bound = bound && bindText(stage.get(), index++, *provenanceJson);
    if (!bound || index != 28 || sqlite3_step(stage.get()) != SQLITE_DONE ||
        sqlite3_changes(database) != 1) {
      return false;
    }
  }
  return true;
}

} // namespace

ModernReplayReservationOutcome
ReplayRepository::ReserveModernReplayPath(std::string_view attemptId,
                                          std::string_view stem,
                                          std::int64_t createdAtUnixMillis) {
  profile_database_activity::WriteGuard writeGuard;
  std::string pathDiagnostic;
  if (!uuid::isCanonicalLowerV4(attemptId) || createdAtUnixMillis <= 0 ||
      !replay::pathForStem(stem, 0, pathDiagnostic)) {
    return {.status = ModernReplayReservationStatus::Invalid,
            .diagnostic = pathDiagnostic.empty()
                              ? "modern replay reservation input is invalid"
                              : std::move(pathDiagnostic)};
  }
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ModernReplayReservationStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(
      impl_->sessionDatabase, "BEGIN IMMEDIATE TRANSACTION", transactionError);
  if (!transaction.active()) {
    return {.status = ModernReplayReservationStatus::StorageFailure,
            .diagnostic = "could not start modern replay reservation"};
  }

  SqliteStatementHandle existing;
  constexpr const char *existingQuery =
      "SELECT attempt_id,stem,history_index,relative_path,created_at_unix_ms "
      "FROM modern_replay_file_reservations WHERE attempt_id=?";
  if (prepareSqliteStatement(impl_->sessionDatabase, existingQuery, existing) !=
          SQLITE_OK ||
      !bindText(existing.get(), 1, attemptId)) {
    return {.status = ModernReplayReservationStatus::StorageFailure,
            .diagnostic = "could not inspect modern replay reservation"};
  }
  int rc = sqlite3_step(existing.get());
  if (rc == SQLITE_ROW) {
    std::string diagnostic;
    auto reservation = decodeReservation(existing.get(), diagnostic);
    if (!reservation || sqlite3_step(existing.get()) != SQLITE_DONE) {
      return {.status = ModernReplayReservationStatus::IntegrityConflict,
              .diagnostic = diagnostic.empty()
                                ? "modern replay reservation is not unique"
                                : std::move(diagnostic)};
    }
    if (reservation->identity.stem != stem) {
      return {.status = ModernReplayReservationStatus::IntegrityConflict,
              .diagnostic = "attempt already reserves a different replay "
                            "stem"};
    }
    if (reservation->createdAtUnixMillis != createdAtUnixMillis) {
      return {.status = ModernReplayReservationStatus::IntegrityConflict,
              .diagnostic = "attempt replay reservation timestamp differs"};
    }
    if (!transaction.commit(transactionError)) {
      return {.status = ModernReplayReservationStatus::StorageFailure,
              .diagnostic = "could not finish replay reservation retry"};
    }
    return {.status = ModernReplayReservationStatus::AlreadyReserved,
            .reservation = std::move(reservation)};
  }
  if (rc != SQLITE_DONE) {
    return {.status = ModernReplayReservationStatus::StorageFailure,
            .diagnostic = "could not inspect modern replay reservation"};
  }

  std::int64_t historyIndex = 0;
  SqliteStatementHandle sequence;
  if (prepareSqliteStatement(
          impl_->sessionDatabase,
          "SELECT last_history_index FROM modern_replay_stem_sequences WHERE "
          "stem=?",
          sequence) != SQLITE_OK ||
      !bindText(sequence.get(), 1, stem)) {
    return {.status = ModernReplayReservationStatus::StorageFailure,
            .diagnostic = "could not read modern replay history sequence"};
  }
  rc = sqlite3_step(sequence.get());
  if (rc == SQLITE_ROW) {
    if (sqlite3_column_type(sequence.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_int64(sequence.get(), 0) ==
            std::numeric_limits<std::int64_t>::max()) {
      return {.status = ModernReplayReservationStatus::IntegrityConflict,
              .diagnostic = "modern replay history sequence is invalid"};
    }
    historyIndex = sqlite3_column_int64(sequence.get(), 0) + 1;
    if (sqlite3_step(sequence.get()) != SQLITE_DONE) {
      return {.status = ModernReplayReservationStatus::IntegrityConflict,
              .diagnostic = "modern replay history sequence is not unique"};
    }
    SqliteStatementHandle update;
    if (prepareSqliteStatement(
            impl_->sessionDatabase,
            "UPDATE modern_replay_stem_sequences SET last_history_index=? "
            "WHERE stem=?",
            update) != SQLITE_OK ||
        sqlite3_bind_int64(update.get(), 1, historyIndex) != SQLITE_OK ||
        !bindText(update.get(), 2, stem) ||
        sqlite3_step(update.get()) != SQLITE_DONE ||
        sqlite3_changes(impl_->sessionDatabase) != 1) {
      return {.status = ModernReplayReservationStatus::StorageFailure,
              .diagnostic = "could not advance modern replay history"};
    }
  } else if (rc == SQLITE_DONE) {
    SqliteStatementHandle insertSequence;
    if (prepareSqliteStatement(
            impl_->sessionDatabase,
            "INSERT INTO modern_replay_stem_sequences(stem,last_history_index)"
            " VALUES(?,0)",
            insertSequence) != SQLITE_OK ||
        !bindText(insertSequence.get(), 1, stem) ||
        sqlite3_step(insertSequence.get()) != SQLITE_DONE ||
        sqlite3_changes(impl_->sessionDatabase) != 1) {
      return {.status = ModernReplayReservationStatus::StorageFailure,
              .diagnostic = "could not start modern replay history"};
    }
  } else {
    return {.status = ModernReplayReservationStatus::StorageFailure,
            .diagnostic = "could not read modern replay history"};
  }

  auto identity = replay::pathForStem(stem, historyIndex, pathDiagnostic);
  if (!identity) {
    return {.status = ModernReplayReservationStatus::IntegrityConflict,
            .diagnostic = std::move(pathDiagnostic)};
  }
  SqliteStatementHandle insert;
  constexpr const char *insertQuery =
      "INSERT INTO modern_replay_file_reservations(attempt_id,stem,"
      "history_index,relative_path,created_at_unix_ms) VALUES(?,?,?,?,?)";
  if (prepareSqliteStatement(impl_->sessionDatabase, insertQuery, insert) !=
          SQLITE_OK ||
      !bindText(insert.get(), 1, attemptId) ||
      !bindText(insert.get(), 2, identity->stem) ||
      sqlite3_bind_int64(insert.get(), 3, identity->historyIndex) !=
          SQLITE_OK ||
      !bindText(insert.get(), 4, identity->relativePath.generic_string()) ||
      sqlite3_bind_int64(insert.get(), 5, createdAtUnixMillis) != SQLITE_OK ||
      sqlite3_step(insert.get()) != SQLITE_DONE ||
      sqlite3_changes(impl_->sessionDatabase) != 1 ||
      !transaction.commit(transactionError)) {
    return {.status = ModernReplayReservationStatus::StorageFailure,
            .diagnostic = "could not commit modern replay reservation"};
  }
  return {.status = ModernReplayReservationStatus::Reserved,
          .reservation = ModernReplayPathReservation{
              .attemptId = std::string(attemptId),
              .identity = std::move(*identity),
              .createdAtUnixMillis = createdAtUnixMillis}};
}

ModernReplayReservationReleaseOutcome
ReplayRepository::ReleaseModernReplayPathReservation(
    const ModernReplayPathReservation &reservation) {
  profile_database_activity::WriteGuard writeGuard;
  std::string pathDiagnostic;
  const auto rebuilt =
      replay::pathForStem(reservation.identity.stem,
                          reservation.identity.historyIndex, pathDiagnostic);
  if (!uuid::isCanonicalLowerV4(reservation.attemptId) ||
      reservation.createdAtUnixMillis <= 0 || !rebuilt ||
      *rebuilt != reservation.identity) {
    return {.status = ModernReplayReservationReleaseStatus::Invalid,
            .diagnostic = pathDiagnostic.empty()
                              ? "modern replay reservation is invalid"
                              : std::move(pathDiagnostic)};
  }
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ModernReplayReservationReleaseStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(
      impl_->sessionDatabase, "BEGIN IMMEDIATE TRANSACTION", transactionError);
  if (!transaction.active()) {
    return {.status = ModernReplayReservationReleaseStatus::StorageFailure,
            .diagnostic = "could not start replay reservation release"};
  }

  SqliteStatementHandle query;
  constexpr const char *querySql =
      "SELECT attempt_id,stem,history_index,relative_path,created_at_unix_ms "
      "FROM modern_replay_file_reservations WHERE attempt_id=?";
  if (prepareSqliteStatement(impl_->sessionDatabase, querySql, query) !=
          SQLITE_OK ||
      !bindText(query.get(), 1, reservation.attemptId)) {
    return {.status = ModernReplayReservationReleaseStatus::StorageFailure,
            .diagnostic = "could not inspect replay reservation release"};
  }
  const int rc = sqlite3_step(query.get());
  if (rc == SQLITE_DONE) {
    if (!transaction.commit(transactionError)) {
      return {.status = ModernReplayReservationReleaseStatus::StorageFailure,
              .diagnostic = "could not finish absent reservation release"};
    }
    return {.status = ModernReplayReservationReleaseStatus::NotFound};
  }
  std::string diagnostic;
  auto stored = rc == SQLITE_ROW ? decodeReservation(query.get(), diagnostic)
                                 : std::nullopt;
  if (!stored || sqlite3_step(query.get()) != SQLITE_DONE) {
    return {.status =
                rc == SQLITE_ROW
                    ? ModernReplayReservationReleaseStatus::IntegrityConflict
                    : ModernReplayReservationReleaseStatus::StorageFailure,
            .diagnostic = diagnostic.empty()
                              ? "modern replay reservation is malformed"
                              : std::move(diagnostic)};
  }
  if (*stored != reservation) {
    return {.status = ModernReplayReservationReleaseStatus::IntegrityConflict,
            .diagnostic = "modern replay reservation release identity differs"};
  }
  query.reset();

  SqliteStatementHandle remove;
  if (prepareSqliteStatement(
          impl_->sessionDatabase,
          "DELETE FROM modern_replay_file_reservations WHERE attempt_id=? "
          "AND stem=? AND history_index=? AND relative_path=? AND "
          "created_at_unix_ms=?",
          remove) != SQLITE_OK ||
      !bindText(remove.get(), 1, reservation.attemptId) ||
      !bindText(remove.get(), 2, reservation.identity.stem) ||
      sqlite3_bind_int64(remove.get(), 3, reservation.identity.historyIndex) !=
          SQLITE_OK ||
      !bindText(remove.get(), 4,
                reservation.identity.relativePath.generic_string()) ||
      sqlite3_bind_int64(remove.get(), 5, reservation.createdAtUnixMillis) !=
          SQLITE_OK ||
      sqlite3_step(remove.get()) != SQLITE_DONE ||
      sqlite3_changes(impl_->sessionDatabase) != 1 ||
      !transaction.commit(transactionError)) {
    return {.status = ModernReplayReservationReleaseStatus::StorageFailure,
            .diagnostic = "could not commit replay reservation release"};
  }
  return {.status = ModernReplayReservationReleaseStatus::Released};
}

ModernChartStageOutcome ReplayRepository::StageModernChartResult(
    const result_persistence::ModernChartResult &result,
    const std::optional<ir::IrSubmissionSnapshot> &snapshot,
    const std::optional<ModernReplayFileAttachment> &replayFile,
    std::span<const ir::IrOutboxDraft> irDrafts) {
  profile_database_activity::WriteGuard writeGuard;
  std::string diagnostic;
  if (result.resultId != 0 ||
      !result_persistence::validateModernChartResult(result, diagnostic)) {
    return {.status = ModernChartStageStatus::Invalid,
            .diagnostic = std::move(diagnostic)};
  }
  const auto gaugeJson = serializeGaugeHistory(result.adoptedGaugeHistory);
  const auto timingJson = serializeJudgementTiming(result.judgementTiming);
  const auto provenanceJson =
      serializeValidatedScoreProvenance(result.score.provenance, diagnostic);
  if (!gaugeJson || !timingJson || !provenanceJson) {
    return {.status = ModernChartStageStatus::Invalid,
            .diagnostic = diagnostic.empty()
                              ? "modern chart durable payload is invalid"
                              : std::move(diagnostic)};
  }
  std::optional<std::string> snapshotJson;
  if (snapshot.has_value()) {
    auto expected = ir::captureIrSubmissionSnapshot(result, diagnostic);
    snapshotJson = ir::serializeIrSubmissionSnapshot(*snapshot, diagnostic);
    if (!expected || !snapshotJson || *expected != *snapshot) {
      return {.status = ModernChartStageStatus::Invalid,
              .diagnostic = diagnostic.empty()
                                ? "IR snapshot disagrees with modern result"
                                : std::move(diagnostic)};
    }
  }
  bool replayIdentityAgrees = true;
  if (replayFile.has_value()) {
    replayIdentityAgrees = validAttachment(*replayFile, diagnostic) &&
                           replay::chartStemMatches(replayFile->identity.stem,
                                                    result.score.chartSha256,
                                                    result.score.longNoteMode,
                                                    std::nullopt, diagnostic);
    if (!replayIdentityAgrees && diagnostic.empty()) {
      diagnostic = "modern replay path disagrees with chart identity";
    }
  }
  if (!validateDrafts(result, snapshot, irDrafts, diagnostic) ||
      !replayIdentityAgrees) {
    return {.status = ModernChartStageStatus::Invalid,
            .diagnostic = std::move(diagnostic)};
  }

  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ModernChartStageStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(
      impl_->sessionDatabase, "BEGIN IMMEDIATE TRANSACTION", transactionError);
  if (!transaction.active()) {
    return {.status = ModernChartStageStatus::StorageFailure,
            .diagnostic = "could not start modern chart staging"};
  }

  auto existing =
      readRecord(impl_->sessionDatabase, "attempt_id=?", result.attemptId, 0);
  if (existing.status == ReadRecordStatus::Invalid) {
    return {.status = ModernChartStageStatus::IntegrityConflict,
            .diagnostic = std::move(existing.diagnostic)};
  }
  if (existing.status == ReadRecordStatus::StorageFailure) {
    return {.status = ModernChartStageStatus::StorageFailure,
            .diagnostic = std::move(existing.diagnostic)};
  }
  if (existing.status == ReadRecordStatus::Loaded) {
    auto expected = result;
    expected.resultId = existing.record->result.resultId;
    bool snapshotFound = false;
    auto storedSnapshot = readSnapshot(impl_->sessionDatabase, result.attemptId,
                                       snapshotFound, diagnostic);
    if (!snapshotFound && !diagnostic.empty()) {
      return {.status = ModernChartStageStatus::StorageFailure,
              .diagnostic = std::move(diagnostic)};
    }
    const bool snapshotAgrees =
        snapshotFound == snapshot.has_value() &&
        (!snapshotFound || (storedSnapshot && *storedSnapshot == *snapshot));
    const bool fileAgrees =
        existing.record->replayFile.has_value() == replayFile.has_value() &&
        (!replayFile.has_value() ||
         (existing.record->replayFile->identity == replayFile->identity &&
          existing.record->replayFile->metadata == replayFile->metadata));
    if (existing.record->result != expected || !snapshotAgrees || !fileAgrees ||
        !verifyDrafts(impl_->sessionDatabase, result.attemptId, irDrafts,
                      diagnostic)) {
      return {.status = ModernChartStageStatus::IntegrityConflict,
              .diagnostic = diagnostic.empty()
                                ? "attempt ID already names different modern "
                                  "payloads"
                                : std::move(diagnostic)};
    }
    if (!transaction.commit(transactionError)) {
      return {.status = ModernChartStageStatus::StorageFailure,
              .diagnostic = "could not finish modern chart staging retry"};
    }
    return {.status = ModernChartStageStatus::AlreadyStaged,
            .receipt = ModernChartStageReceipt{
                .attemptId = result.attemptId,
                .resultId = existing.record->result.resultId,
                .createdAt = existing.createdAt}};
  }

  std::optional<ModernReplayPathReservation> reservation;
  if (replayFile.has_value()) {
    SqliteStatementHandle statement;
    constexpr const char *query =
        "SELECT attempt_id,stem,history_index,relative_path,created_at_unix_ms "
        "FROM modern_replay_file_reservations WHERE attempt_id=?";
    if (prepareSqliteStatement(impl_->sessionDatabase, query, statement) !=
            SQLITE_OK ||
        !bindText(statement.get(), 1, result.attemptId) ||
        sqlite3_step(statement.get()) != SQLITE_ROW) {
      return {.status = ModernChartStageStatus::IntegrityConflict,
              .diagnostic = "modern replay file has no reservation"};
    }
    reservation = decodeReservation(statement.get(), diagnostic);
    if (!reservation || sqlite3_step(statement.get()) != SQLITE_DONE ||
        reservation->identity != replayFile->identity) {
      return {.status = ModernChartStageStatus::IntegrityConflict,
              .diagnostic = diagnostic.empty()
                                ? "modern replay reservation disagrees with "
                                  "the installed file"
                                : std::move(diagnostic)};
    }
  }

  int resultId = 0;
  if (!insertResult(impl_->sessionDatabase, result, *gaugeJson, *timingJson,
                    *provenanceJson, resultId)) {
    return {.status = ModernChartStageStatus::StorageFailure,
            .diagnostic = "could not insert modern chart result"};
  }
  if (snapshot.has_value()) {
    SqliteStatementHandle statement;
    constexpr const char *query =
        "INSERT INTO ir_submission_snapshots(modern_chart_result_id,"
        "attempt_id,schema_version,payload_json,fingerprint) VALUES(?,?,?,?,?)";
    if (prepareSqliteStatement(impl_->sessionDatabase, query, statement) !=
            SQLITE_OK ||
        sqlite3_bind_int(statement.get(), 1, resultId) != SQLITE_OK ||
        !bindText(statement.get(), 2, result.attemptId) ||
        sqlite3_bind_int(statement.get(), 3, snapshot->schemaVersion) !=
            SQLITE_OK ||
        !bindText(statement.get(), 4, *snapshotJson) ||
        !bindText(statement.get(), 5, snapshot->fingerprint) ||
        sqlite3_step(statement.get()) != SQLITE_DONE ||
        sqlite3_changes(impl_->sessionDatabase) != 1) {
      return {.status = ModernChartStageStatus::StorageFailure,
              .diagnostic = "could not insert modern IR snapshot"};
    }
  }
  if (replayFile.has_value()) {
    SqliteStatementHandle statement;
    constexpr const char *query =
        "INSERT INTO modern_replay_files(modern_chart_result_id,stem,"
        "history_index,relative_path,content_sha256,compressed_size,"
        "codec_version) VALUES(?,?,?,?,?,?,?)";
    if (prepareSqliteStatement(impl_->sessionDatabase, query, statement) !=
            SQLITE_OK ||
        sqlite3_bind_int(statement.get(), 1, resultId) != SQLITE_OK ||
        !bindText(statement.get(), 2, replayFile->identity.stem) ||
        sqlite3_bind_int64(statement.get(), 3,
                           replayFile->identity.historyIndex) != SQLITE_OK ||
        !bindText(statement.get(), 4,
                  replayFile->identity.relativePath.generic_string()) ||
        !bindText(statement.get(), 5, replayFile->metadata.sha256) ||
        sqlite3_bind_int64(
            statement.get(), 6,
            static_cast<sqlite3_int64>(replayFile->metadata.compressedSize)) !=
            SQLITE_OK ||
        sqlite3_bind_int(statement.get(), 7,
                         replayFile->metadata.codecVersion) != SQLITE_OK ||
        sqlite3_step(statement.get()) != SQLITE_DONE ||
        sqlite3_changes(impl_->sessionDatabase) != 1) {
      return {.status = ModernChartStageStatus::StorageFailure,
              .diagnostic = "could not insert modern replay reference"};
    }
  }

  SqliteStatementHandle pending;
  if (prepareSqliteStatement(
          impl_->sessionDatabase,
          "INSERT INTO modern_pending_chart_score_writes(attempt_id,"
          "modern_chart_result_id,created_at) SELECT attempt_id,id,created_at "
          "FROM modern_chart_results WHERE id=? AND attempt_id=?",
          pending) != SQLITE_OK ||
      sqlite3_bind_int(pending.get(), 1, resultId) != SQLITE_OK ||
      !bindText(pending.get(), 2, result.attemptId) ||
      sqlite3_step(pending.get()) != SQLITE_DONE ||
      sqlite3_changes(impl_->sessionDatabase) != 1 ||
      !insertReadyDrafts(impl_->sessionDatabase, irDrafts)) {
    return {.status = ModernChartStageStatus::StorageFailure,
            .diagnostic = "could not stage modern score or IR work"};
  }
  if (replayFile.has_value()) {
    SqliteStatementHandle removeReservation;
    if (prepareSqliteStatement(
            impl_->sessionDatabase,
            "DELETE FROM modern_replay_file_reservations WHERE attempt_id=?",
            removeReservation) != SQLITE_OK ||
        !bindText(removeReservation.get(), 1, result.attemptId) ||
        sqlite3_step(removeReservation.get()) != SQLITE_DONE ||
        sqlite3_changes(impl_->sessionDatabase) != 1) {
      return {.status = ModernChartStageStatus::StorageFailure,
              .diagnostic = "could not finalize modern replay reservation"};
    }
  }

  auto stored = readRecord(impl_->sessionDatabase, "id=?", {}, resultId);
  if (stored.status != ReadRecordStatus::Loaded ||
      !transaction.commit(transactionError)) {
    return {.status = ModernChartStageStatus::StorageFailure,
            .diagnostic = stored.diagnostic.empty()
                              ? "could not commit modern chart result"
                              : std::move(stored.diagnostic)};
  }
  return {.status = ModernChartStageStatus::Staged,
          .receipt = ModernChartStageReceipt{.attemptId = result.attemptId,
                                             .resultId = resultId,
                                             .createdAt = stored.createdAt}};
}

ModernChartResultReadOutcome
ReplayRepository::LoadModernChartResultByAttempt(std::string_view attemptId) {
  profile_database_activity::ReadGuard readGuard;
  if (!uuid::isCanonicalLowerV4(attemptId)) {
    return {.status = ModernChartResultReadStatus::Invalid,
            .diagnostic = "modern result attempt ID is invalid"};
  }
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ModernChartResultReadStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  auto loaded =
      readRecord(impl_->sessionDatabase, "attempt_id=?", attemptId, 0);
  switch (loaded.status) {
  case ReadRecordStatus::Loaded:
    return {.status = ModernChartResultReadStatus::Loaded,
            .record = std::move(loaded.record)};
  case ReadRecordStatus::NotFound:
    return {.status = ModernChartResultReadStatus::NotFound};
  case ReadRecordStatus::Invalid:
    return {.status = ModernChartResultReadStatus::Invalid,
            .diagnostic = std::move(loaded.diagnostic)};
  case ReadRecordStatus::StorageFailure:
    return {.status = ModernChartResultReadStatus::StorageFailure,
            .diagnostic = std::move(loaded.diagnostic)};
  }
  return {.status = ModernChartResultReadStatus::StorageFailure};
}

ModernChartResultReadOutcome
ReplayRepository::LoadModernChartResult(int resultId) {
  profile_database_activity::ReadGuard readGuard;
  if (resultId <= 0) {
    return {.status = ModernChartResultReadStatus::Invalid,
            .diagnostic = "modern result ID is invalid"};
  }
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ModernChartResultReadStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  auto loaded = readRecord(impl_->sessionDatabase, "id=?", {}, resultId);
  switch (loaded.status) {
  case ReadRecordStatus::Loaded:
    return {.status = ModernChartResultReadStatus::Loaded,
            .record = std::move(loaded.record)};
  case ReadRecordStatus::NotFound:
    return {.status = ModernChartResultReadStatus::NotFound};
  case ReadRecordStatus::Invalid:
    return {.status = ModernChartResultReadStatus::Invalid,
            .diagnostic = std::move(loaded.diagnostic)};
  case ReadRecordStatus::StorageFailure:
    return {.status = ModernChartResultReadStatus::StorageFailure,
            .diagnostic = std::move(loaded.diagnostic)};
  }
  return {.status = ModernChartResultReadStatus::StorageFailure};
}

ModernChartHistoryReadOutcome
ReplayRepository::ListModernChartResults(std::string_view chartSha256,
                                         std::size_t limit) {
  profile_database_activity::ReadGuard readGuard;
  if (!replay::isCanonicalLowerHex(chartSha256, 64) || limit == 0 ||
      limit > kMaximumModernChartHistoryRows) {
    return {.status = ModernChartHistoryReadStatus::Invalid,
            .diagnostic = "modern chart history request is invalid"};
  }
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ModernChartHistoryReadStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(impl_->sessionDatabase,
                                      "BEGIN TRANSACTION", transactionError);
  if (!transaction.active()) {
    return {.status = ModernChartHistoryReadStatus::StorageFailure,
            .diagnostic = "could not start modern chart history read"};
  }
  SqliteStatementHandle statement;
  constexpr const char *query =
      "SELECT id FROM modern_chart_results WHERE chart_sha256=? "
      "ORDER BY played_at_unix_ms DESC,id DESC LIMIT ?";
  if (prepareSqliteStatement(impl_->sessionDatabase, query, statement) !=
          SQLITE_OK ||
      !bindText(statement.get(), 1, chartSha256) ||
      sqlite3_bind_int64(statement.get(), 2,
                         static_cast<sqlite3_int64>(limit)) != SQLITE_OK) {
    return {.status = ModernChartHistoryReadStatus::StorageFailure,
            .diagnostic = "could not prepare modern chart history read"};
  }
  std::vector<int> resultIds;
  resultIds.reserve(limit);
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER) {
      return {.status = ModernChartHistoryReadStatus::IntegrityConflict,
              .diagnostic = "modern chart history ID has an invalid type"};
    }
    const sqlite3_int64 id = sqlite3_column_int64(statement.get(), 0);
    if (id <= 0 || id > std::numeric_limits<int>::max()) {
      return {.status = ModernChartHistoryReadStatus::IntegrityConflict,
              .diagnostic = "modern chart history ID is out of range"};
    }
    resultIds.push_back(static_cast<int>(id));
  }
  if (rc != SQLITE_DONE) {
    return {.status = ModernChartHistoryReadStatus::StorageFailure,
            .diagnostic = "modern chart history scan did not complete"};
  }
  statement.reset();

  ModernChartHistoryReadOutcome outcome{
      .status = ModernChartHistoryReadStatus::Loaded};
  outcome.records.reserve(resultIds.size());
  for (const int resultId : resultIds) {
    auto loaded = readRecord(impl_->sessionDatabase, "id=?", {}, resultId);
    if (loaded.status == ReadRecordStatus::StorageFailure) {
      return {.status = ModernChartHistoryReadStatus::StorageFailure,
              .diagnostic = std::move(loaded.diagnostic)};
    }
    if (loaded.status != ReadRecordStatus::Loaded || !loaded.record ||
        loaded.record->result.score.chartSha256 != chartSha256) {
      return {.status = ModernChartHistoryReadStatus::IntegrityConflict,
              .diagnostic = loaded.diagnostic.empty()
                                ? "modern chart history row is inconsistent"
                                : std::move(loaded.diagnostic)};
    }
    outcome.records.push_back(std::move(*loaded.record));
  }
  if (!transaction.commit(transactionError)) {
    return {.status = ModernChartHistoryReadStatus::StorageFailure,
            .diagnostic = "could not finish modern chart history read"};
  }
  return outcome;
}

ModernCourseStageOutcome ReplayRepository::StageModernCourseResult(
    const result_persistence::ModernCourseResult &result,
    const std::optional<ModernReplayFileAttachment> &replayFile,
    const std::optional<replay::CoursePathInput> &replayPath) {
  profile_database_activity::WriteGuard writeGuard;
  std::string diagnostic;
  if (result.resultId != 0 ||
      !result_persistence::validateModernCourseResult(result, diagnostic)) {
    return {.status = ModernCourseStageStatus::Invalid,
            .diagnostic = std::move(diagnostic)};
  }
  const auto provenanceJson =
      serializeValidatedScoreProvenance(result.provenance, diagnostic);
  if (!provenanceJson) {
    return {.status = ModernCourseStageStatus::Invalid,
            .diagnostic = diagnostic.empty()
                              ? "modern course provenance is invalid"
                              : std::move(diagnostic)};
  }

  if (replayFile.has_value() != replayPath.has_value()) {
    return {.status = ModernCourseStageStatus::Invalid,
            .diagnostic =
                "modern course replay file and path facts must be paired"};
  }
  if (replayFile) {
    std::vector<std::string> stageSha256;
    stageSha256.reserve(result.stages.size());
    for (const auto &stage : result.stages) {
      stageSha256.push_back(stage.score.chartSha256);
    }
    if (!validAttachment(*replayFile, diagnostic) ||
        replayPath->stageSha256 != stageSha256 ||
        replayPath->longNoteMode != result.longNoteMode ||
        !replay::courseStemMatches(replayFile->identity.stem, *replayPath,
                                   replayPath->hasUndefinedLongNotes,
                                   diagnostic)) {
      return {.status = ModernCourseStageStatus::Invalid,
              .diagnostic = diagnostic.empty()
                                ? "modern replay path disagrees with course "
                                  "identity"
                                : std::move(diagnostic)};
    }
  }

  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ModernCourseStageStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(
      impl_->sessionDatabase, "BEGIN IMMEDIATE TRANSACTION", transactionError);
  if (!transaction.active()) {
    return {.status = ModernCourseStageStatus::StorageFailure,
            .diagnostic = "could not start modern course staging"};
  }

  auto existing = readCourseRecord(impl_->sessionDatabase, "attempt_id=?",
                                   result.attemptId, 0);
  if (existing.status == ReadRecordStatus::Invalid) {
    return {.status = ModernCourseStageStatus::IntegrityConflict,
            .diagnostic = std::move(existing.diagnostic)};
  }
  if (existing.status == ReadRecordStatus::StorageFailure) {
    return {.status = ModernCourseStageStatus::StorageFailure,
            .diagnostic = std::move(existing.diagnostic)};
  }
  if (existing.status == ReadRecordStatus::Loaded) {
    auto expected = result;
    expected.resultId = existing.record->result.resultId;
    const bool fileAgrees =
        existing.record->replayFile.has_value() == replayFile.has_value() &&
        (!replayFile ||
         (existing.record->replayFile->identity == replayFile->identity &&
          existing.record->replayFile->metadata == replayFile->metadata));
    if (existing.record->result != expected || !fileAgrees) {
      return {.status = ModernCourseStageStatus::IntegrityConflict,
              .diagnostic =
                  "attempt ID already names different modern course payloads"};
    }
    if (!transaction.commit(transactionError)) {
      return {.status = ModernCourseStageStatus::StorageFailure,
              .diagnostic = "could not finish modern course staging retry"};
    }
    return {.status = ModernCourseStageStatus::AlreadyStaged,
            .receipt = ModernCourseStageReceipt{
                .attemptId = result.attemptId,
                .resultId = existing.record->result.resultId,
                .createdAt = existing.createdAt}};
  }

  std::optional<ModernReplayPathReservation> reservation;
  if (replayFile) {
    SqliteStatementHandle statement;
    constexpr const char *query =
        "SELECT attempt_id,stem,history_index,relative_path,created_at_unix_ms "
        "FROM modern_replay_file_reservations WHERE attempt_id=?";
    if (prepareSqliteStatement(impl_->sessionDatabase, query, statement) !=
            SQLITE_OK ||
        !bindText(statement.get(), 1, result.attemptId) ||
        sqlite3_step(statement.get()) != SQLITE_ROW) {
      return {.status = ModernCourseStageStatus::IntegrityConflict,
              .diagnostic = "modern course replay file has no reservation"};
    }
    reservation = decodeReservation(statement.get(), diagnostic);
    if (!reservation || sqlite3_step(statement.get()) != SQLITE_DONE ||
        reservation->identity != replayFile->identity) {
      return {.status = ModernCourseStageStatus::IntegrityConflict,
              .diagnostic = diagnostic.empty()
                                ? "modern course replay reservation disagrees "
                                  "with the installed file"
                                : std::move(diagnostic)};
    }
  }

  int resultId = 0;
  if (!insertModernCourseResult(impl_->sessionDatabase, result, *provenanceJson,
                                resultId) ||
      !insertModernCourseChildren(impl_->sessionDatabase, resultId, result)) {
    return {.status = ModernCourseStageStatus::StorageFailure,
            .diagnostic = "could not insert modern course result"};
  }
  if (replayFile) {
    SqliteStatementHandle statement;
    constexpr const char *query =
        "INSERT INTO modern_replay_files(modern_chart_result_id,"
        "modern_course_result_id,stem,history_index,relative_path,"
        "content_sha256,compressed_size,codec_version) "
        "VALUES(NULL,?,?,?,?,?,?,?)";
    if (prepareSqliteStatement(impl_->sessionDatabase, query, statement) !=
            SQLITE_OK ||
        sqlite3_bind_int(statement.get(), 1, resultId) != SQLITE_OK ||
        !bindText(statement.get(), 2, replayFile->identity.stem) ||
        sqlite3_bind_int64(statement.get(), 3,
                           replayFile->identity.historyIndex) != SQLITE_OK ||
        !bindText(statement.get(), 4,
                  replayFile->identity.relativePath.generic_string()) ||
        !bindText(statement.get(), 5, replayFile->metadata.sha256) ||
        sqlite3_bind_int64(
            statement.get(), 6,
            static_cast<sqlite3_int64>(replayFile->metadata.compressedSize)) !=
            SQLITE_OK ||
        sqlite3_bind_int(statement.get(), 7,
                         replayFile->metadata.codecVersion) != SQLITE_OK ||
        sqlite3_step(statement.get()) != SQLITE_DONE ||
        sqlite3_changes(impl_->sessionDatabase) != 1) {
      return {.status = ModernCourseStageStatus::StorageFailure,
              .diagnostic = "could not insert modern course replay reference"};
    }

    SqliteStatementHandle remove;
    if (prepareSqliteStatement(
            impl_->sessionDatabase,
            "DELETE FROM modern_replay_file_reservations WHERE attempt_id=? "
            "AND stem=? AND history_index=? AND relative_path=? AND "
            "created_at_unix_ms=?",
            remove) != SQLITE_OK ||
        !bindText(remove.get(), 1, reservation->attemptId) ||
        !bindText(remove.get(), 2, reservation->identity.stem) ||
        sqlite3_bind_int64(remove.get(), 3,
                           reservation->identity.historyIndex) != SQLITE_OK ||
        !bindText(remove.get(), 4,
                  reservation->identity.relativePath.generic_string()) ||
        sqlite3_bind_int64(remove.get(), 5, reservation->createdAtUnixMillis) !=
            SQLITE_OK ||
        sqlite3_step(remove.get()) != SQLITE_DONE ||
        sqlite3_changes(impl_->sessionDatabase) != 1) {
      return {.status = ModernCourseStageStatus::StorageFailure,
              .diagnostic =
                  "could not finalize modern course replay reservation"};
    }
  }

  auto stored = readCourseRecord(impl_->sessionDatabase, "id=?", {}, resultId);
  if (stored.status != ReadRecordStatus::Loaded ||
      !transaction.commit(transactionError)) {
    return {.status = ModernCourseStageStatus::StorageFailure,
            .diagnostic = stored.diagnostic.empty()
                              ? "could not commit modern course result"
                              : std::move(stored.diagnostic)};
  }
  return {.status = ModernCourseStageStatus::Staged,
          .receipt = ModernCourseStageReceipt{.attemptId = result.attemptId,
                                              .resultId = resultId,
                                              .createdAt = stored.createdAt}};
}

ModernCourseResultReadOutcome
ReplayRepository::LoadModernCourseResultByAttempt(std::string_view attemptId) {
  profile_database_activity::ReadGuard readGuard;
  if (!uuid::isCanonicalLowerV4(attemptId)) {
    return {.status = ModernCourseResultReadStatus::Invalid,
            .diagnostic = "modern course result attempt ID is invalid"};
  }
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ModernCourseResultReadStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  auto loaded =
      readCourseRecord(impl_->sessionDatabase, "attempt_id=?", attemptId, 0);
  switch (loaded.status) {
  case ReadRecordStatus::Loaded:
    return {.status = ModernCourseResultReadStatus::Loaded,
            .record = std::move(loaded.record)};
  case ReadRecordStatus::NotFound:
    return {.status = ModernCourseResultReadStatus::NotFound};
  case ReadRecordStatus::Invalid:
    return {.status = ModernCourseResultReadStatus::Invalid,
            .diagnostic = std::move(loaded.diagnostic)};
  case ReadRecordStatus::StorageFailure:
    return {.status = ModernCourseResultReadStatus::StorageFailure,
            .diagnostic = std::move(loaded.diagnostic)};
  }
  return {.status = ModernCourseResultReadStatus::StorageFailure};
}

ModernCourseResultReadOutcome
ReplayRepository::LoadModernCourseResult(int resultId) {
  profile_database_activity::ReadGuard readGuard;
  if (resultId <= 0) {
    return {.status = ModernCourseResultReadStatus::Invalid,
            .diagnostic = "modern course result ID is invalid"};
  }
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ModernCourseResultReadStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  auto loaded = readCourseRecord(impl_->sessionDatabase, "id=?", {}, resultId);
  switch (loaded.status) {
  case ReadRecordStatus::Loaded:
    return {.status = ModernCourseResultReadStatus::Loaded,
            .record = std::move(loaded.record)};
  case ReadRecordStatus::NotFound:
    return {.status = ModernCourseResultReadStatus::NotFound};
  case ReadRecordStatus::Invalid:
    return {.status = ModernCourseResultReadStatus::Invalid,
            .diagnostic = std::move(loaded.diagnostic)};
  case ReadRecordStatus::StorageFailure:
    return {.status = ModernCourseResultReadStatus::StorageFailure,
            .diagnostic = std::move(loaded.diagnostic)};
  }
  return {.status = ModernCourseResultReadStatus::StorageFailure};
}

ModernCourseHistoryReadOutcome
ReplayRepository::ListModernCourseResults(std::string_view courseKey,
                                          std::size_t limit) {
  profile_database_activity::ReadGuard readGuard;
  if (!course_identity::isCanonicalKey(courseKey) || limit == 0 ||
      limit > kMaximumModernCourseHistoryRows) {
    return {.status = ModernCourseHistoryReadStatus::Invalid,
            .diagnostic = "modern course history request is invalid"};
  }
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ModernCourseHistoryReadStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(impl_->sessionDatabase,
                                      "BEGIN TRANSACTION", transactionError);
  if (!transaction.active()) {
    return {.status = ModernCourseHistoryReadStatus::StorageFailure,
            .diagnostic = "could not start modern course history read"};
  }
  SqliteStatementHandle statement;
  constexpr const char *query =
      "SELECT id FROM modern_course_results WHERE course_key=? "
      "ORDER BY played_at_unix_ms DESC,id DESC LIMIT ?";
  if (prepareSqliteStatement(impl_->sessionDatabase, query, statement) !=
          SQLITE_OK ||
      !bindText(statement.get(), 1, courseKey) ||
      sqlite3_bind_int64(statement.get(), 2,
                         static_cast<sqlite3_int64>(limit)) != SQLITE_OK) {
    return {.status = ModernCourseHistoryReadStatus::StorageFailure,
            .diagnostic = "could not prepare modern course history read"};
  }
  std::vector<int> resultIds;
  resultIds.reserve(limit);
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER) {
      return {.status = ModernCourseHistoryReadStatus::IntegrityConflict,
              .diagnostic = "modern course history ID has an invalid type"};
    }
    const sqlite3_int64 id = sqlite3_column_int64(statement.get(), 0);
    if (id <= 0 || id > std::numeric_limits<int>::max()) {
      return {.status = ModernCourseHistoryReadStatus::IntegrityConflict,
              .diagnostic = "modern course history ID is out of range"};
    }
    resultIds.push_back(static_cast<int>(id));
  }
  if (rc != SQLITE_DONE) {
    return {.status = ModernCourseHistoryReadStatus::StorageFailure,
            .diagnostic = "modern course history scan did not complete"};
  }
  statement.reset();

  ModernCourseHistoryReadOutcome outcome{
      .status = ModernCourseHistoryReadStatus::Loaded};
  outcome.records.reserve(resultIds.size());
  for (const int resultId : resultIds) {
    auto loaded =
        readCourseRecord(impl_->sessionDatabase, "id=?", {}, resultId);
    if (loaded.status == ReadRecordStatus::StorageFailure) {
      return {.status = ModernCourseHistoryReadStatus::StorageFailure,
              .diagnostic = std::move(loaded.diagnostic)};
    }
    if (loaded.status != ReadRecordStatus::Loaded || !loaded.record ||
        loaded.record->result.courseKey != courseKey) {
      return {.status = ModernCourseHistoryReadStatus::IntegrityConflict,
              .diagnostic = loaded.diagnostic.empty()
                                ? "modern course history row is inconsistent"
                                : std::move(loaded.diagnostic)};
    }
    outcome.records.push_back(std::move(*loaded.record));
  }
  if (!transaction.commit(transactionError)) {
    return {.status = ModernCourseHistoryReadStatus::StorageFailure,
            .diagnostic = "could not finish modern course history read"};
  }
  return outcome;
}

ModernIrSnapshotReadOutcome
ReplayRepository::LoadModernIrSubmissionSnapshot(std::string_view attemptId) {
  profile_database_activity::ReadGuard readGuard;
  if (!uuid::isCanonicalLowerV4(attemptId)) {
    return {.status = ModernIrSnapshotReadStatus::Invalid,
            .diagnostic = "modern IR snapshot attempt ID is invalid"};
  }
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ModernIrSnapshotReadStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  bool found = false;
  std::string diagnostic;
  auto snapshot =
      readSnapshot(impl_->sessionDatabase, attemptId, found, diagnostic);
  if (!found) {
    if (!diagnostic.empty()) {
      return {.status = ModernIrSnapshotReadStatus::StorageFailure,
              .diagnostic = std::move(diagnostic)};
    }
    return {.status = ModernIrSnapshotReadStatus::NotFound};
  }
  if (!snapshot) {
    return {.status = ModernIrSnapshotReadStatus::Invalid,
            .diagnostic = std::move(diagnostic)};
  }
  return {.status = ModernIrSnapshotReadStatus::Loaded,
          .snapshot = std::move(snapshot)};
}

result_persistence::PendingReadOutcome
ReplayRepository::LoadPendingModernChartScore(std::string_view attemptId) {
  using result_persistence::PendingReadStatus;
  profile_database_activity::ReadGuard readGuard;
  if (!uuid::isCanonicalLowerV4(attemptId)) {
    return {.status = PendingReadStatus::IntegrityConflict,
            .diagnostic = "modern pending score attempt ID is invalid"};
  }
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = PendingReadStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  return readPendingModernScore(impl_->sessionDatabase, attemptId);
}

result_persistence::PendingBatchOutcome
ReplayRepository::ListPendingModernChartScores(std::size_t limit) {
  using result_persistence::PendingReadStatus;
  profile_database_activity::ReadGuard readGuard;
  if (limit == 0 || limit > kMaximumModernChartHistoryRows) {
    return {.diagnostic = "modern pending score limit is invalid"};
  }
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.diagnostic = "replay storage is unavailable"};
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(impl_->sessionDatabase,
                                      "BEGIN TRANSACTION", transactionError);
  if (!transaction.active()) {
    return {.diagnostic = "could not start modern pending score scan"};
  }
  SqliteStatementHandle statement;
  constexpr const char *query =
      "SELECT attempt_id FROM modern_pending_chart_score_writes ORDER BY "
      "recovery_attempts,last_recovery_at,created_at,attempt_id LIMIT ?";
  if (prepareSqliteStatement(impl_->sessionDatabase, query, statement) !=
          SQLITE_OK ||
      sqlite3_bind_int64(statement.get(), 1,
                         static_cast<sqlite3_int64>(limit)) != SQLITE_OK) {
    return {.diagnostic = "could not prepare modern pending score scan"};
  }
  std::vector<std::string> attemptIds;
  attemptIds.reserve(limit);
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT) {
      return {.diagnostic = "modern pending score attempt ID has invalid type"};
    }
    attemptIds.push_back(sqliteColumnString(statement.get(), 0));
  }
  if (rc != SQLITE_DONE) {
    return {.diagnostic = "modern pending score scan did not complete"};
  }
  statement.reset();

  result_persistence::PendingBatchOutcome outcome{.storageAvailable = true};
  outcome.entries.reserve(attemptIds.size());
  for (const auto &attemptId : attemptIds) {
    auto pending = readPendingModernScore(impl_->sessionDatabase, attemptId);
    outcome.entries.push_back({.status = pending.status,
                               .attemptId = attemptId,
                               .value = std::move(pending.value),
                               .diagnostic = std::move(pending.diagnostic)});
  }
  SqliteStatementHandle count;
  if (prepareSqliteStatement(
          impl_->sessionDatabase,
          "SELECT COUNT(*) FROM modern_pending_chart_score_writes",
          count) != SQLITE_OK ||
      sqlite3_step(count.get()) != SQLITE_ROW ||
      sqlite3_column_type(count.get(), 0) != SQLITE_INTEGER) {
    return {.diagnostic = "could not count modern pending scores"};
  }
  const sqlite3_int64 total = sqlite3_column_int64(count.get(), 0);
  if (total < 0 || static_cast<std::uint64_t>(total) < outcome.entries.size() ||
      sqlite3_step(count.get()) != SQLITE_DONE ||
      !transaction.commit(transactionError)) {
    return {.diagnostic = "modern pending score count is inconsistent"};
  }
  outcome.remaining = static_cast<std::size_t>(total) - outcome.entries.size();
  return outcome;
}

result_persistence::AcknowledgeOutcome
ReplayRepository::AcknowledgePendingModernChartScore(std::string_view attemptId,
                                                     int modernResultId) {
  using result_persistence::AcknowledgeStatus;
  profile_database_activity::WriteGuard writeGuard;
  if (!uuid::isCanonicalLowerV4(attemptId) || modernResultId <= 0) {
    return {.status = AcknowledgeStatus::IntegrityConflict,
            .diagnostic = "modern score acknowledgement identity is invalid"};
  }
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(
      impl_->sessionDatabase, "BEGIN IMMEDIATE TRANSACTION", transactionError);
  if (!transaction.active()) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = "could not start modern score acknowledgement"};
  }
  auto pending = readPendingModernScore(impl_->sessionDatabase, attemptId);
  if (pending.status == result_persistence::PendingReadStatus::StorageFailure) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = std::move(pending.diagnostic)};
  }
  if (pending.status ==
      result_persistence::PendingReadStatus::IntegrityConflict) {
    return {.status = AcknowledgeStatus::IntegrityConflict,
            .diagnostic = std::move(pending.diagnostic)};
  }
  if (pending.status == result_persistence::PendingReadStatus::Found) {
    if (!pending.value || pending.value->modernResultId != modernResultId ||
        pending.value->replayId != 0 || !pending.value->hasExactlyOneOwner()) {
      return {.status = AcknowledgeStatus::IntegrityConflict,
              .diagnostic = "modern pending score names a different result"};
    }
    SqliteStatementHandle remove;
    if (prepareSqliteStatement(
            impl_->sessionDatabase,
            "DELETE FROM modern_pending_chart_score_writes WHERE attempt_id=? "
            "AND modern_chart_result_id=?",
            remove) != SQLITE_OK ||
        !bindText(remove.get(), 1, attemptId) ||
        sqlite3_bind_int(remove.get(), 2, modernResultId) != SQLITE_OK ||
        sqlite3_step(remove.get()) != SQLITE_DONE ||
        sqlite3_changes(impl_->sessionDatabase) != 1 ||
        !transaction.commit(transactionError)) {
      return {.status = AcknowledgeStatus::StorageFailure,
              .diagnostic = "could not commit modern score acknowledgement"};
    }
    return {.status = AcknowledgeStatus::Acknowledged};
  }

  auto existing =
      readRecord(impl_->sessionDatabase, "attempt_id=?", attemptId, 0);
  if (existing.status == ReadRecordStatus::StorageFailure) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = std::move(existing.diagnostic)};
  }
  if (existing.status != ReadRecordStatus::Loaded || !existing.record ||
      existing.record->result.resultId != modernResultId) {
    return {.status = AcknowledgeStatus::IntegrityConflict,
            .diagnostic = "acknowledged modern score has no matching result"};
  }
  SqliteStatementHandle inactive;
  if (prepareSqliteStatement(
          impl_->sessionDatabase,
          "SELECT COUNT(*) FROM ir_outbox WHERE attempt_id=? AND "
          "local_result_ready=0",
          inactive) != SQLITE_OK ||
      !bindText(inactive.get(), 1, attemptId) ||
      sqlite3_step(inactive.get()) != SQLITE_ROW ||
      sqlite3_column_type(inactive.get(), 0) != SQLITE_INTEGER ||
      sqlite3_column_int64(inactive.get(), 0) != 0 ||
      sqlite3_step(inactive.get()) != SQLITE_DONE ||
      !transaction.commit(transactionError)) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = "could not verify modern score acknowledgement"};
  }
  return {.status = AcknowledgeStatus::AlreadyAcknowledged};
}

result_persistence::RecoveryMarkOutcome
ReplayRepository::RecordPendingModernChartScoreRecoveryAttempt(
    std::string_view attemptId, result_persistence::RecoveryAttemptKind kind) {
  using result_persistence::RecoveryMarkStatus;
  profile_database_activity::WriteGuard writeGuard;
  (void)kind;
  if (!uuid::isCanonicalLowerV4(attemptId)) {
    return {.status = RecoveryMarkStatus::NotFound,
            .diagnostic = "modern pending score attempt ID is invalid"};
  }
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = RecoveryMarkStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(
          impl_->sessionDatabase,
          "UPDATE modern_pending_chart_score_writes SET recovery_attempts="
          "recovery_attempts+1,last_recovery_at=CURRENT_TIMESTAMP WHERE "
          "attempt_id=?",
          statement) != SQLITE_OK ||
      !bindText(statement.get(), 1, attemptId) ||
      sqlite3_step(statement.get()) != SQLITE_DONE) {
    return {.status = RecoveryMarkStatus::StorageFailure,
            .diagnostic = "could not record modern score recovery"};
  }
  const int changes = sqlite3_changes(impl_->sessionDatabase);
  if (changes == 0) {
    return {.status = RecoveryMarkStatus::NotFound};
  }
  if (changes != 1) {
    return {.status = RecoveryMarkStatus::StorageFailure,
            .diagnostic = "modern score recovery updated unexpected rows"};
  }
  return {.status = RecoveryMarkStatus::Recorded};
}
