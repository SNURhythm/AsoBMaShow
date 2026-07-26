#include "ReplayRepositoryInternal.h"

#include "../BmsMetadataText.h"
#include "../ProfileDatabaseActivity.h"
#include "../Uuid.h"
#include "../ir/IrProfileSettings.h"
#include "../ir/IrSubmissionSnapshot.h"
#include "../replay/BeatorajaReplayCodec.h"
#include "../replay/ReplayFileStore.h"
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

std::optional<std::string> readCourseCreatedAt(sqlite3 *database,
                                               int resultId) {
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(database,
                             "SELECT created_at FROM course_results WHERE id=?",
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
                             ReplayFileReference::RecordKind expectedKind,
                             std::string &diagnostic) {
  diagnostic.clear();
  if (reference.id != 0 || reference.recordId != 0 ||
      reference.recordKind != expectedKind ||
      !lowerHex(reference.contentSha256, 64) || reference.compressedSize == 0 ||
      reference.compressedSize >
          static_cast<std::uint64_t>(
              std::numeric_limits<sqlite3_int64>::max()) ||
      reference.codecVersion != replay::BeatorajaReplayCodec::kCodecVersion) {
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

CourseResultReadOutcome invalidCourseResult(std::string diagnostic) {
  return {.status = CourseResultReadOutcome::Status::Invalid,
          .diagnostic = std::move(diagnostic)};
}

} // namespace

namespace replay_repository_detail {

using asobmshow::bms_metadata::normalizedHash;

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
          "JOIN replay_files f ON f.chart_result_id=r.id WHERE r.attempt_id=? "
          "UNION ALL SELECT f.stem,f.history_index,f.relative_path FROM "
          "course_results r JOIN replay_files f ON f.course_result_id=r.id "
          "WHERE r.attempt_id=?",
          existing) != SQLITE_OK ||
      !bindText(existing.get(), 1, attemptId) ||
      !bindText(existing.get(), 2, attemptId) ||
      !bindText(existing.get(), 3, attemptId)) {
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
    if (!validateReplayReference(check,
                                 ReplayFileReference::RecordKind::ChartResult,
                                 referenceDiagnostic)) {
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

CourseResultReadOutcome LoadCourseResultOnConnection(sqlite3 *database,
                                                     int resultId) {
  if (database == nullptr || resultId <= 0) {
    return invalidCourseResult("course result ID is invalid");
  }
  constexpr const char *query =
      "SELECT r.id,r.attempt_id,r.course_key,r.legacy_course_id,"
      "r.course_name,r.course_group_name,r.constraint_json,"
      "r.completed_charts,r.total_charts,r.requested_play_option,"
      "r.assist_option,r.initial_gauge_type,r.gauge_profile,"
      "r.gauge_auto_shift,r.gauge_auto_shift_lower_bound,r.long_note_mode,"
      "r.final_score,r.max_score,r.max_combo,r.final_gauge,r.clear_type,"
      "r.provenance_json,r.result_fingerprint,r.played_at_unix_ms,"
      "f.id,f.stem,f.history_index,f.relative_path,f.content_sha256,"
      "f.compressed_size,f.codec_version FROM course_results r LEFT JOIN "
      "replay_files f ON f.course_result_id=r.id WHERE r.id=?";
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK ||
      sqlite3_bind_int(statement.get(), 1, resultId) != SQLITE_OK) {
    return {.status = CourseResultReadOutcome::Status::StorageFailure,
            .diagnostic = "could not query course result"};
  }
  const int step = sqlite3_step(statement.get());
  if (step == SQLITE_DONE) {
    return {.status = CourseResultReadOutcome::Status::NotFound};
  }
  if (step != SQLITE_ROW) {
    return {.status = CourseResultReadOutcome::Status::StorageFailure,
            .diagnostic = "course result query failed"};
  }

  result_persistence::PersistedCourseResult result;
  result.resultId = sqlite3_column_int(statement.get(), 0);
  if (sqlite3_column_type(statement.get(), 1) != SQLITE_NULL) {
    result.attemptId = columnText(statement.get(), 1);
  }
  result.courseKey = columnText(statement.get(), 2);
  result.legacyCourseId = sqlite3_column_int(statement.get(), 3);
  result.courseName = columnText(statement.get(), 4);
  result.courseGroupName = columnText(statement.get(), 5);
  result.constraintJson = columnText(statement.get(), 6);
  result.completedCharts = sqlite3_column_int(statement.get(), 7);
  result.totalCharts = sqlite3_column_int(statement.get(), 8);
  result.requestedPlayOption = columnText(statement.get(), 9);
  result.assistOption = columnText(statement.get(), 10);
  const int gaugeType = sqlite3_column_int(statement.get(), 11);
  const int gaugeProfile = sqlite3_column_int(statement.get(), 12);
  const int gaugeAutoShift = sqlite3_column_int(statement.get(), 13);
  const int lowerBound = sqlite3_column_int(statement.get(), 14);
  if (gaugeType < 0 || gaugeType >= static_cast<int>(kGaugeTypeCount) ||
      gaugeProfile < static_cast<int>(GaugeProfile::Standard) ||
      gaugeProfile > static_cast<int>(GaugeProfile::Standard24Keys) ||
      gaugeAutoShift < static_cast<int>(GaugeAutoShiftMode::None) ||
      gaugeAutoShift > static_cast<int>(GaugeAutoShiftMode::BestClear) ||
      lowerBound < 0 || lowerBound >= static_cast<int>(kGaugeTypeCount)) {
    return invalidCourseResult("course result gauge configuration is invalid");
  }
  result.initialGaugeType = gaugeTypeAtIndex(gaugeType);
  result.gaugeProfile = static_cast<GaugeProfile>(gaugeProfile);
  result.gaugeAutoShift = gaugeAutoShiftModeFromValue(gaugeAutoShift);
  result.gaugeAutoShiftLowerBound = gaugeTypeAtIndex(lowerBound);
  result.longNoteMode = sqlite3_column_int(statement.get(), 15);
  result.finalScore = sqlite3_column_int(statement.get(), 16);
  result.maxScore = sqlite3_column_int(statement.get(), 17);
  result.maxCombo = sqlite3_column_int(statement.get(), 18);
  result.finalGauge =
      static_cast<float>(sqlite3_column_double(statement.get(), 19));
  result.clearType = sqlite3_column_int(statement.get(), 20);
  const std::string provenanceJson = columnText(statement.get(), 21);
  if (provenanceJson.size() > kMaximumResultJsonBytes) {
    return invalidCourseResult("course result provenance is oversized");
  }
  std::string provenanceDiagnostic;
  auto provenance =
      deserializeScoreProvenance(provenanceJson, provenanceDiagnostic);
  if (!provenance.has_value()) {
    return invalidCourseResult("course result provenance is malformed");
  }
  result.provenance = std::move(*provenance);
  result.resultFingerprint = columnText(statement.get(), 22);
  result.playedAtUnixMillis = sqlite3_column_int64(statement.get(), 23);

  CourseResultRecord record{.result = std::move(result)};
  if (sqlite3_column_type(statement.get(), 24) != SQLITE_NULL) {
    const auto size = sqlite3_column_int64(statement.get(), 29);
    if (size <= 0) {
      return invalidCourseResult("course replay file size is invalid");
    }
    record.replayFile = ReplayFileReference{
        .id = sqlite3_column_int64(statement.get(), 24),
        .recordKind = ReplayFileReference::RecordKind::CourseResult,
        .recordId = resultId,
        .stem = columnText(statement.get(), 25),
        .historyIndex = sqlite3_column_int64(statement.get(), 26),
        .relativePath = std::filesystem::path(columnText(statement.get(), 27)),
        .contentSha256 = columnText(statement.get(), 28),
        .compressedSize = static_cast<std::uint64_t>(size),
        .codecVersion = sqlite3_column_int(statement.get(), 30),
    };
    ReplayFileReference check = *record.replayFile;
    check.id = 0;
    check.recordId = 0;
    std::string referenceDiagnostic;
    if (!validateReplayReference(check,
                                 ReplayFileReference::RecordKind::CourseResult,
                                 referenceDiagnostic)) {
      return invalidCourseResult(std::move(referenceDiagnostic));
    }
  }
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    return {.status = CourseResultReadOutcome::Status::IntegrityConflict,
            .diagnostic = "course result ID is not unique"};
  }

  SqliteStatementHandle stages;
  constexpr const char *stageQuery =
      "SELECT stage_index,chart_path,chart_md5,chart_sha256,chart_title,"
      "chart_artist,key_mode,long_note_mode,score,max_score,max_combo,"
      "combo_break,p_great,great,good,bad,poor,k_poor,fast,slow,final_gauge,"
      "clear_type,gauge_history_json,judgement_timing_json,provenance_json "
      "FROM course_result_stages WHERE course_result_id=? ORDER BY "
      "stage_index";
  if (prepareSqliteStatement(database, stageQuery, stages) != SQLITE_OK ||
      sqlite3_bind_int(stages.get(), 1, resultId) != SQLITE_OK) {
    return {.status = CourseResultReadOutcome::Status::StorageFailure,
            .diagnostic = "could not query course result stages"};
  }
  while (true) {
    const int stageStep = sqlite3_step(stages.get());
    if (stageStep == SQLITE_DONE) {
      break;
    }
    if (stageStep != SQLITE_ROW) {
      return {.status = CourseResultReadOutcome::Status::StorageFailure,
              .diagnostic = "course result stage query failed"};
    }
    result_persistence::PersistedCourseStageResult stage;
    stage.stageIndex = sqlite3_column_int(stages.get(), 0);
    auto &score = stage.score;
    score.chartPath = columnText(stages.get(), 1);
    score.chartMd5 = columnText(stages.get(), 2);
    score.chartSha256 = columnText(stages.get(), 3);
    score.chartTitle = columnText(stages.get(), 4);
    score.chartArtist = columnText(stages.get(), 5);
    stage.keyMode = sqlite3_column_int(stages.get(), 6);
    score.longNoteMode = sqlite3_column_int(stages.get(), 7);
    score.score = sqlite3_column_int(stages.get(), 8);
    score.maxScore = sqlite3_column_int(stages.get(), 9);
    score.maxCombo = sqlite3_column_int(stages.get(), 10);
    score.comboBreak = sqlite3_column_int(stages.get(), 11);
    score.pGreat = sqlite3_column_int(stages.get(), 12);
    score.great = sqlite3_column_int(stages.get(), 13);
    score.good = sqlite3_column_int(stages.get(), 14);
    score.bad = sqlite3_column_int(stages.get(), 15);
    score.poor = sqlite3_column_int(stages.get(), 16);
    score.kPoor = sqlite3_column_int(stages.get(), 17);
    score.fast = sqlite3_column_int(stages.get(), 18);
    score.slow = sqlite3_column_int(stages.get(), 19);
    score.finalGauge =
        static_cast<float>(sqlite3_column_double(stages.get(), 20));
    score.clearType = sqlite3_column_int(stages.get(), 21);
    auto history = parseGaugeHistory(columnText(stages.get(), 22));
    if (!history.has_value()) {
      return invalidCourseResult(
          "course result stage gauge history is malformed");
    }
    stage.adoptedGaugeHistory = std::move(*history);
    if (sqlite3_column_type(stages.get(), 23) != SQLITE_NULL) {
      stage.judgementTiming =
          parseJudgementTiming(columnText(stages.get(), 23));
      if (!stage.judgementTiming.has_value()) {
        return invalidCourseResult("course result stage timing is malformed");
      }
    }
    const std::string stageProvenanceJson = columnText(stages.get(), 24);
    if (stageProvenanceJson.size() > kMaximumResultJsonBytes) {
      return invalidCourseResult("course result stage provenance is oversized");
    }
    auto stageProvenance =
        deserializeScoreProvenance(stageProvenanceJson, provenanceDiagnostic);
    if (!stageProvenance.has_value()) {
      return invalidCourseResult("course result stage provenance is malformed");
    }
    score.provenance = std::move(*stageProvenance);
    record.result.stages.push_back(std::move(stage));
  }

  std::string resultDiagnostic;
  if (!result_persistence::validatePersistedCourseResult(record.result,
                                                         resultDiagnostic) ||
      record.result.resultFingerprint.empty()) {
    return invalidCourseResult(resultDiagnostic.empty()
                                   ? "course result fingerprint is missing"
                                   : std::move(resultDiagnostic));
  }
  return {.status = CourseResultReadOutcome::Status::Loaded,
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
      !validateReplayReference(replayFile,
                               ReplayFileReference::RecordKind::ChartResult,
                               diagnostic) ||
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

result_persistence::StageOutcome StageCompletedCourseAttemptOnConnection(
    sqlite3 *database,
    const result_persistence::PersistedCourseResult &sourceResult,
    const ReplayFileReference &replayFile) {
  using result_persistence::StageOutcome;
  using result_persistence::StageReceipt;
  using result_persistence::StageStatus;

  std::string diagnostic;
  if (database == nullptr || sourceResult.resultId != 0 ||
      !sourceResult.attemptId.has_value() ||
      !result_persistence::validatePersistedCourseResult(sourceResult,
                                                         diagnostic) ||
      sourceResult.resultFingerprint.empty() ||
      !validateReplayReference(replayFile,
                               ReplayFileReference::RecordKind::CourseResult,
                               diagnostic)) {
    return {.status = StageStatus::IntegrityConflict,
            .diagnostic = diagnostic.empty()
                              ? "completed course attempt is inconsistent"
                              : std::move(diagnostic)};
  }

  const auto provenance =
      serializeValidatedScoreProvenance(sourceResult.provenance, diagnostic);
  std::vector<std::string> histories;
  std::vector<std::string> timings;
  std::vector<std::string> provenances;
  histories.reserve(sourceResult.stages.size());
  timings.reserve(sourceResult.stages.size());
  provenances.reserve(sourceResult.stages.size());
  for (const auto &stage : sourceResult.stages) {
    const auto history = gaugeHistoryJson(stage.adoptedGaugeHistory);
    const auto timing = judgementTimingJson(stage.judgementTiming);
    const auto stageProvenance =
        serializeValidatedScoreProvenance(stage.score.provenance, diagnostic);
    if (!history.has_value() || !timing.has_value() ||
        !stageProvenance.has_value()) {
      return {.status = StageStatus::IntegrityConflict,
              .diagnostic = diagnostic.empty()
                                ? "course result stage cannot be serialized"
                                : std::move(diagnostic)};
    }
    histories.push_back(*history);
    timings.push_back(*timing);
    provenances.push_back(*stageProvenance);
  }
  if (!provenance.has_value()) {
    return {.status = StageStatus::IntegrityConflict,
            .diagnostic = diagnostic.empty()
                              ? "course result cannot be serialized"
                              : std::move(diagnostic)};
  }

  std::string transactionError;
  SqliteTransactionHandle transaction(database, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not start compact course staging"};
  }

  SqliteStatementHandle existingId;
  if (prepareSqliteStatement(database,
                             "SELECT id FROM course_results WHERE attempt_id=?",
                             existingId) != SQLITE_OK ||
      !bindText(existingId.get(), 1, *sourceResult.attemptId)) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not inspect compact course staging"};
  }
  const int existingStep = sqlite3_step(existingId.get());
  if (existingStep == SQLITE_ROW) {
    const int id = sqlite3_column_int(existingId.get(), 0);
    if (sqlite3_step(existingId.get()) != SQLITE_DONE) {
      return {.status = StageStatus::IntegrityConflict,
              .diagnostic = "course attempt identity is not unique"};
    }
    const auto loaded = LoadCourseResultOnConnection(database, id);
    auto expected = sourceResult;
    expected.resultId = id;
    ReplayFileReference expectedFile = replayFile;
    if (loaded.status != CourseResultReadOutcome::Status::Loaded ||
        !loaded.record.has_value() || loaded.record->result != expected ||
        !loaded.record->replayFile.has_value()) {
      return {.status = StageStatus::IntegrityConflict,
              .diagnostic = "course attempt ID already names another result"};
    }
    expectedFile.id = loaded.record->replayFile->id;
    expectedFile.recordId = id;
    if (*loaded.record->replayFile != expectedFile) {
      return {.status = StageStatus::IntegrityConflict,
              .diagnostic = "staged course replay file differs"};
    }
    const auto createdAt = readCourseCreatedAt(database, id);
    if (!createdAt.has_value() || !transaction.commit(transactionError)) {
      return {.status = StageStatus::StorageFailure,
              .diagnostic = "could not finish idempotent course staging"};
    }
    return {.status = StageStatus::AlreadyStaged,
            .receipt = StageReceipt{.attemptId = *sourceResult.attemptId,
                                    .resultId = id,
                                    .createdAt = *createdAt,
                                    .scorePending = false}};
  }
  if (existingStep != SQLITE_DONE) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not read compact course staging"};
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
      columnText(reservation.get(), 2) != pathText(replayFile.relativePath) ||
      sqlite3_step(reservation.get()) != SQLITE_DONE) {
    return {.status = StageStatus::IntegrityConflict,
            .diagnostic =
                "completed course replay has no matching reservation"};
  }

  constexpr const char *resultSql =
      "INSERT INTO course_results(attempt_id,course_key,legacy_course_id,"
      "course_name,course_group_name,constraint_json,completed_charts,"
      "total_charts,requested_play_option,assist_option,initial_gauge_type,"
      "gauge_profile,gauge_auto_shift,gauge_auto_shift_lower_bound,"
      "long_note_mode,final_score,max_score,max_combo,final_gauge,clear_type,"
      "provenance_json,result_fingerprint,played_at_unix_ms)"
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
  SqliteStatementHandle resultInsert;
  if (prepareSqliteStatement(database, resultSql, resultInsert) != SQLITE_OK) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not prepare compact course result"};
  }
  int column = 1;
  bool bound =
      bindText(resultInsert.get(), column++, *sourceResult.attemptId) &&
      bindText(resultInsert.get(), column++, sourceResult.courseKey) &&
      sqlite3_bind_int(resultInsert.get(), column++,
                       sourceResult.legacyCourseId) == SQLITE_OK &&
      bindText(resultInsert.get(), column++, sourceResult.courseName) &&
      bindText(resultInsert.get(), column++, sourceResult.courseGroupName) &&
      bindText(resultInsert.get(), column++, sourceResult.constraintJson) &&
      sqlite3_bind_int(resultInsert.get(), column++,
                       sourceResult.completedCharts) == SQLITE_OK &&
      sqlite3_bind_int(resultInsert.get(), column++,
                       sourceResult.totalCharts) == SQLITE_OK &&
      bindText(resultInsert.get(), column++,
               sourceResult.requestedPlayOption) &&
      bindText(resultInsert.get(), column++, sourceResult.assistOption) &&
      sqlite3_bind_int(resultInsert.get(), column++,
                       static_cast<int>(sourceResult.initialGaugeType)) ==
          SQLITE_OK &&
      sqlite3_bind_int(resultInsert.get(), column++,
                       static_cast<int>(sourceResult.gaugeProfile)) ==
          SQLITE_OK &&
      sqlite3_bind_int(resultInsert.get(), column++,
                       gaugeAutoShiftModeValue(sourceResult.gaugeAutoShift)) ==
          SQLITE_OK &&
      sqlite3_bind_int(
          resultInsert.get(), column++,
          static_cast<int>(sourceResult.gaugeAutoShiftLowerBound)) ==
          SQLITE_OK &&
      sqlite3_bind_int(resultInsert.get(), column++,
                       sourceResult.longNoteMode) == SQLITE_OK &&
      sqlite3_bind_int(resultInsert.get(), column++, sourceResult.finalScore) ==
          SQLITE_OK &&
      sqlite3_bind_int(resultInsert.get(), column++, sourceResult.maxScore) ==
          SQLITE_OK &&
      sqlite3_bind_int(resultInsert.get(), column++, sourceResult.maxCombo) ==
          SQLITE_OK &&
      sqlite3_bind_double(resultInsert.get(), column++,
                          sourceResult.finalGauge) == SQLITE_OK &&
      sqlite3_bind_int(resultInsert.get(), column++, sourceResult.clearType) ==
          SQLITE_OK &&
      bindText(resultInsert.get(), column++, *provenance) &&
      bindText(resultInsert.get(), column++, sourceResult.resultFingerprint) &&
      sqlite3_bind_int64(resultInsert.get(), column++,
                         sourceResult.playedAtUnixMillis) == SQLITE_OK;
  if (!bound || column != 24 ||
      sqlite3_step(resultInsert.get()) != SQLITE_DONE) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not insert compact course result"};
  }
  const sqlite3_int64 rawId = sqlite3_last_insert_rowid(database);
  if (rawId <= 0 || rawId > std::numeric_limits<int>::max()) {
    return {.status = StageStatus::IntegrityConflict,
            .diagnostic = "course result ID is out of range"};
  }
  const int resultId = static_cast<int>(rawId);

  constexpr const char *stageSql =
      "INSERT INTO course_result_stages(course_result_id,stage_index,"
      "chart_path,chart_md5,chart_sha256,chart_title,chart_artist,key_mode,"
      "long_note_mode,score,max_score,max_combo,combo_break,p_great,great,"
      "good,bad,poor,k_poor,fast,slow,final_gauge,clear_type,"
      "gauge_history_json,judgement_timing_json,provenance_json)"
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
  for (std::size_t index = 0; index < sourceResult.stages.size(); ++index) {
    const auto &stage = sourceResult.stages[index];
    const auto &score = stage.score;
    SqliteStatementHandle stageInsert;
    if (prepareSqliteStatement(database, stageSql, stageInsert) != SQLITE_OK) {
      return {.status = StageStatus::StorageFailure,
              .diagnostic = "could not prepare compact course stage"};
    }
    column = 1;
    bound =
        sqlite3_bind_int(stageInsert.get(), column++, resultId) == SQLITE_OK &&
        sqlite3_bind_int(stageInsert.get(), column++, stage.stageIndex) ==
            SQLITE_OK &&
        bindText(stageInsert.get(), column++, score.chartPath) &&
        bindText(stageInsert.get(), column++, score.chartMd5) &&
        bindText(stageInsert.get(), column++, score.chartSha256) &&
        bindText(stageInsert.get(), column++, score.chartTitle) &&
        bindText(stageInsert.get(), column++, score.chartArtist) &&
        sqlite3_bind_int(stageInsert.get(), column++, stage.keyMode) ==
            SQLITE_OK;
    const int values[] = {score.longNoteMode, score.score,      score.maxScore,
                          score.maxCombo,     score.comboBreak, score.pGreat,
                          score.great,        score.good,       score.bad,
                          score.poor,         score.kPoor,      score.fast,
                          score.slow};
    for (int value : values) {
      bound = bound &&
              sqlite3_bind_int(stageInsert.get(), column++, value) == SQLITE_OK;
    }
    bound = bound &&
            sqlite3_bind_double(stageInsert.get(), column++,
                                score.finalGauge) == SQLITE_OK &&
            sqlite3_bind_int(stageInsert.get(), column++, score.clearType) ==
                SQLITE_OK &&
            bindText(stageInsert.get(), column++, histories[index]);
    if (stage.judgementTiming.has_value()) {
      bound = bound && bindText(stageInsert.get(), column++, timings[index]);
    } else {
      bound =
          bound && sqlite3_bind_null(stageInsert.get(), column++) == SQLITE_OK;
    }
    bound = bound && bindText(stageInsert.get(), column++, provenances[index]);
    if (!bound || column != 27 ||
        sqlite3_step(stageInsert.get()) != SQLITE_DONE) {
      return {.status = StageStatus::StorageFailure,
              .diagnostic = "could not insert compact course stage"};
    }
  }

  SqliteStatementHandle fileInsert;
  if (prepareSqliteStatement(
          database,
          "INSERT INTO replay_files(chart_result_id,course_result_id,stem,"
          "history_index,relative_path,content_sha256,compressed_size,"
          "codec_version) VALUES(NULL,?,?,?,?,?,?,?)",
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
            .diagnostic = "could not associate compact course replay file"};
  }

  SqliteStatementHandle deleteReservation;
  if (prepareSqliteStatement(
          database, "DELETE FROM replay_file_reservations WHERE attempt_id=?",
          deleteReservation) != SQLITE_OK ||
      !bindText(deleteReservation.get(), 1, *sourceResult.attemptId) ||
      sqlite3_step(deleteReservation.get()) != SQLITE_DONE ||
      sqlite3_changes(database) != 1) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not consume course replay reservation"};
  }
  const auto createdAt = readCourseCreatedAt(database, resultId);
  if (!createdAt.has_value() || !transaction.commit(transactionError)) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not commit compact course staging"};
  }
  return {.status = StageStatus::Staged,
          .receipt = StageReceipt{.attemptId = *sourceResult.attemptId,
                                  .resultId = resultId,
                                  .createdAt = *createdAt,
                                  .scorePending = false}};
}

std::vector<ReplaySummary> ListCompactChartResultsOnConnection(
    sqlite3 *database, const bms_parser::ChartMeta &chartMeta, int limit,
    std::string_view irProviderId, std::string_view irServerOrigin) {
  std::vector<ReplaySummary> summaries;
  if (database == nullptr) {
    return summaries;
  }
  const std::string sha256 = normalizedHash(chartMeta.SHA256);
  const std::string md5 = normalizedHash(chartMeta.MD5);
  if (sha256.empty() && md5.empty()) {
    return summaries;
  }

  SqliteStatementHandle ids;
  if (prepareSqliteStatement(
          database,
          "SELECT id FROM chart_results WHERE "
          "((?<>'' AND chart_sha256=?) OR (?<>'' AND chart_md5=?)) "
          "ORDER BY played_at_unix_ms DESC,id DESC LIMIT ?",
          ids) != SQLITE_OK ||
      !bindText(ids.get(), 1, sha256) || !bindText(ids.get(), 2, sha256) ||
      !bindText(ids.get(), 3, md5) || !bindText(ids.get(), 4, md5) ||
      sqlite3_bind_int(ids.get(), 5, limit > 0 ? limit : -1) != SQLITE_OK) {
    return summaries;
  }

  const auto normalizedOrigin = irServerOrigin.empty()
                                    ? std::optional<std::string>{}
                                    : ir::normalizeServerOrigin(irServerOrigin);
  int step = SQLITE_OK;
  while ((step = sqlite3_step(ids.get())) == SQLITE_ROW) {
    const int resultId = sqlite3_column_int(ids.get(), 0);
    auto loaded = LoadChartResultOnConnection(database, resultId);
    if (loaded.status != ResultReadOutcome::Status::Loaded ||
        !loaded.record.has_value()) {
      continue;
    }
    const auto &record = *loaded.record;
    const auto &result = record.result;
    const auto &score = result.score;
    ReplaySummary summary;
    summary.id = result.resultId;
    summary.initialGaugeType = score.provenance.gaugeType;
    summary.gaugeAutoShift = score.provenance.gaugeAutoShift;
    summary.finalScore = score.score;
    summary.maxScore = score.maxScore;
    summary.maxCombo = score.maxCombo;
    summary.finalGauge = score.finalGauge;
    summary.clearType = score.clearType;
    summary.createdAt =
        readCreatedAt(database, resultId).value_or(std::string{});
    summary.replayFileState = record.replayFile.has_value()
                                  ? ReplaySummary::ReplayFileState::Available
                                  : ReplaySummary::ReplayFileState::Missing;
    if (record.replayFile.has_value()) {
      summary.replayFileSize = record.replayFile->compressedSize;
    }
    summary.chartMeta = bms_parser::ChartMeta{
        .BmsPath = score.chartPath,
        .MD5 = score.chartMd5,
        .SHA256 = score.chartSha256,
        .Title = score.chartTitle,
        .Artist = score.chartArtist,
        .KeyMode = result.keyMode,
        .TotalNotes = score.maxScore / 2,
        .LnMode = score.longNoteMode,
    };
    summary.playOption = score.provenance.player1.option;
    summary.playOptionSeed = score.provenance.player1.seed;
    summary.playOption2 = score.provenance.player2.option;
    summary.playOption2Seed = score.provenance.player2.seed;
    summary.assistOption = score.provenance.assistOption;
    summary.rulesetVersion = score.provenance.ruleset.version;
    summary.eligibility = score.provenance.eligibility;
    summary.playback = score.provenance.playback;
    summary.provenance =
        std::make_shared<const ScoreProvenance>(score.provenance);
    summary.attemptId = result.attemptId;
    summary.hasCanonicalAttemptFingerprint =
        result.attemptId.has_value() && !result.resultFingerprint.empty();

    if (!irProviderId.empty() && result.attemptId.has_value()) {
      SqliteStatementHandle outbox;
      if (prepareSqliteStatement(
              database,
              "SELECT state,last_error_message FROM ir_outbox "
              "WHERE provider_id=? AND attempt_id=? LIMIT 1",
              outbox) == SQLITE_OK &&
          bindText(outbox.get(), 1, irProviderId) &&
          bindText(outbox.get(), 2, *result.attemptId) &&
          sqlite3_step(outbox.get()) == SQLITE_ROW) {
        const int state = sqlite3_column_int(outbox.get(), 0);
        if (ir::isKnownIrOutboxState(state)) {
          summary.requestedIrOutboxState =
              static_cast<ir::IrOutboxState>(state);
        }
        if (sqlite3_column_type(outbox.get(), 1) == SQLITE_TEXT) {
          summary.requestedIrOutboxDiagnostic =
              ir::sanitizeDiagnostic(columnText(outbox.get(), 1));
        }
      }
    }
    if (!irProviderId.empty() && normalizedOrigin.has_value()) {
      SqliteStatementHandle receipt;
      if (prepareSqliteStatement(
              database,
              "SELECT remote_score_id FROM ir_submission_receipts "
              "WHERE provider_id=? AND server_origin=? AND result_id=? "
              "LIMIT 1",
              receipt) == SQLITE_OK &&
          bindText(receipt.get(), 1, irProviderId) &&
          bindText(receipt.get(), 2, *normalizedOrigin) &&
          sqlite3_bind_int(receipt.get(), 3, resultId) == SQLITE_OK &&
          sqlite3_step(receipt.get()) == SQLITE_ROW) {
        summary.hasIrReceipt = true;
        summary.receiptProviderId = std::string(irProviderId);
        summary.receiptServerOrigin = *normalizedOrigin;
        if (sqlite3_column_type(receipt.get(), 0) == SQLITE_TEXT) {
          summary.receiptRemoteScoreId = columnText(receipt.get(), 0);
        }
      }
    }
    summaries.push_back(std::move(summary));
  }
  return step == SQLITE_DONE ? summaries : std::vector<ReplaySummary>{};
}

std::vector<ReplaySummary> ListCompactCourseResultsOnConnection(
    sqlite3 *database, const CourseReplayLookup &lookup, int limit) {
  std::vector<ReplaySummary> summaries;
  if (database == nullptr ||
      (lookup.courseKey.empty() && lookup.legacyCourseId <= 0)) {
    return summaries;
  }
  SqliteStatementHandle ids;
  if (prepareSqliteStatement(
          database,
          "SELECT id FROM course_results WHERE "
          "((?<>'' AND course_key=?) OR (?='' AND ? > 0 AND "
          "legacy_course_id=?)) ORDER BY played_at_unix_ms DESC,id DESC "
          "LIMIT ?",
          ids) != SQLITE_OK ||
      !bindText(ids.get(), 1, lookup.courseKey) ||
      !bindText(ids.get(), 2, lookup.courseKey) ||
      !bindText(ids.get(), 3, lookup.courseKey) ||
      sqlite3_bind_int(ids.get(), 4, lookup.legacyCourseId) != SQLITE_OK ||
      sqlite3_bind_int(ids.get(), 5, lookup.legacyCourseId) != SQLITE_OK ||
      sqlite3_bind_int(ids.get(), 6, limit > 0 ? limit : -1) != SQLITE_OK) {
    return summaries;
  }

  int step = SQLITE_OK;
  while ((step = sqlite3_step(ids.get())) == SQLITE_ROW) {
    const int resultId = sqlite3_column_int(ids.get(), 0);
    auto loaded = LoadCourseResultOnConnection(database, resultId);
    if (loaded.status != CourseResultReadOutcome::Status::Loaded ||
        !loaded.record.has_value()) {
      continue;
    }
    const auto &record = *loaded.record;
    const auto &result = record.result;
    ReplaySummary summary;
    summary.id = result.resultId;
    summary.courseReplay = true;
    summary.initialGaugeType = result.initialGaugeType;
    summary.gaugeAutoShift = result.gaugeAutoShift;
    summary.finalScore = result.finalScore;
    summary.maxScore = result.maxScore;
    summary.maxCombo = result.maxCombo;
    summary.finalGauge = result.finalGauge;
    summary.clearType = result.clearType;
    summary.createdAt =
        readCourseCreatedAt(database, resultId).value_or(std::string{});
    summary.replayFileState = record.replayFile.has_value()
                                  ? ReplaySummary::ReplayFileState::Available
                                  : ReplaySummary::ReplayFileState::Missing;
    if (record.replayFile.has_value()) {
      summary.replayFileSize = record.replayFile->compressedSize;
    }
    summary.playOption = result.requestedPlayOption;
    summary.assistOption = result.assistOption;
    summary.completedCharts = result.completedCharts;
    summary.totalCharts = result.totalCharts;
    summary.stageCount = static_cast<int>(result.stages.size());
    summary.rulesetVersion = result.provenance.ruleset.version;
    summary.eligibility = result.provenance.eligibility;
    summary.playback = result.provenance.playback;
    summary.provenance =
        std::make_shared<const ScoreProvenance>(result.provenance);
    summary.attemptId = result.attemptId;
    summary.hasCanonicalAttemptFingerprint =
        result.attemptId.has_value() && !result.resultFingerprint.empty();
    summaries.push_back(std::move(summary));
  }
  return step == SQLITE_DONE ? summaries : std::vector<ReplaySummary>{};
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

result_persistence::StageOutcome ReplayRepository::stageCompletedCourseAttempt(
    const result_persistence::PersistedCourseResult &result,
    const ReplayFileReference &replayFile) {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = result_persistence::StageStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  return replay_repository_detail::StageCompletedCourseAttemptOnConnection(
      impl_->sessionDatabase, result, replayFile);
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

CourseResultReadOutcome ReplayRepository::loadCourseResult(int resultId) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = CourseResultReadOutcome::Status::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  return replay_repository_detail::LoadCourseResultOnConnection(
      impl_->sessionDatabase, resultId);
}

ChartReplayPlaybackReadOutcome
ReplayRepository::loadChartReplayPlayback(int resultId) {
  profile_database_activity::ReadGuard operation;
  ResultReadOutcome loaded;
  std::filesystem::path profileRoot;
  {
    std::lock_guard lock(impl_->sessionMutex);
    if (!EnsureSessionDatabaseLocked()) {
      return {.status = ChartReplayPlaybackReadOutcome::Status::StorageFailure,
              .diagnostic = "replay storage is unavailable"};
    }
    loaded = replay_repository_detail::LoadChartResultOnConnection(
        impl_->sessionDatabase, resultId);
    profileRoot =
        replay_repository_detail::ResolvedDatabasePath(impl_->databasePath)
            .parent_path();
  }

  using ReadStatus = ChartReplayPlaybackReadOutcome::Status;
  if (loaded.status != ResultReadOutcome::Status::Loaded ||
      !loaded.record.has_value()) {
    ReadStatus status = ReadStatus::StorageFailure;
    switch (loaded.status) {
    case ResultReadOutcome::Status::NotFound:
      status = ReadStatus::ResultNotFound;
      break;
    case ResultReadOutcome::Status::Invalid:
      status = ReadStatus::Invalid;
      break;
    case ResultReadOutcome::Status::IntegrityConflict:
      status = ReadStatus::IntegrityConflict;
      break;
    case ResultReadOutcome::Status::Loaded:
    case ResultReadOutcome::Status::StorageFailure:
      break;
    }
    return {.status = status, .diagnostic = std::move(loaded.diagnostic)};
  }

  auto result = loaded.record->result;
  if (!loaded.record->replayFile.has_value()) {
    return {.status = ReadStatus::ReplayUnavailable,
            .result = std::move(result),
            .diagnostic = "This result has no replay file reference."};
  }
  const auto &reference = *loaded.record->replayFile;
  const replay::ReplayFileMetadata metadata{
      .relativePath = reference.relativePath,
      .sha256 = reference.contentSha256,
      .compressedSize = reference.compressedSize,
      .codecVersion = reference.codecVersion,
  };
  replay::ReplayFileStore store(profileRoot);
  const auto inspection = store.inspect(metadata);
  if (inspection.state != replay::ReplayFileState::Available) {
    const bool unavailable =
        inspection.state == replay::ReplayFileState::Missing;
    return {.status = unavailable ? ReadStatus::ReplayUnavailable
                                  : ReadStatus::Invalid,
            .result = std::move(result),
            .diagnostic =
                inspection.diagnostic.empty()
                    ? (unavailable
                           ? "The replay file was deleted or moved."
                           : "The replay file is unreadable or corrupt.")
                    : inspection.diagnostic};
  }

  replay::BeatorajaReplayCodec codec;
  auto decoded = store.load(metadata, codec);
  if (!decoded.chart.has_value() || decoded.course.has_value()) {
    return {.status = ReadStatus::Invalid,
            .result = std::move(result),
            .diagnostic = decoded.diagnostic.empty()
                              ? "The replay file is not a chart replay."
                              : std::move(decoded.diagnostic)};
  }
  if (decoded.chart->setup.chartSha256 != result.score.chartSha256 ||
      decoded.chart->setup.keyMode != result.keyMode) {
    return {.status = ReadStatus::IntegrityConflict,
            .result = std::move(result),
            .diagnostic =
                "The replay file identity differs from its saved result."};
  }
  return {.status = ReadStatus::Loaded,
          .result = std::move(result),
          .playback = std::move(decoded.chart)};
}

CourseReplayPlaybackReadOutcome
ReplayRepository::loadCourseReplayPlayback(int resultId) {
  profile_database_activity::ReadGuard operation;
  CourseResultReadOutcome loaded;
  std::filesystem::path profileRoot;
  {
    std::lock_guard lock(impl_->sessionMutex);
    if (!EnsureSessionDatabaseLocked()) {
      return {.status = CourseReplayPlaybackReadOutcome::Status::StorageFailure,
              .diagnostic = "replay storage is unavailable"};
    }
    loaded = replay_repository_detail::LoadCourseResultOnConnection(
        impl_->sessionDatabase, resultId);
    profileRoot =
        replay_repository_detail::ResolvedDatabasePath(impl_->databasePath)
            .parent_path();
  }

  using ReadStatus = CourseReplayPlaybackReadOutcome::Status;
  if (loaded.status != CourseResultReadOutcome::Status::Loaded ||
      !loaded.record.has_value()) {
    ReadStatus status = ReadStatus::StorageFailure;
    switch (loaded.status) {
    case CourseResultReadOutcome::Status::NotFound:
      status = ReadStatus::ResultNotFound;
      break;
    case CourseResultReadOutcome::Status::Invalid:
      status = ReadStatus::Invalid;
      break;
    case CourseResultReadOutcome::Status::IntegrityConflict:
      status = ReadStatus::IntegrityConflict;
      break;
    case CourseResultReadOutcome::Status::Loaded:
    case CourseResultReadOutcome::Status::StorageFailure:
      break;
    }
    return {.status = status, .diagnostic = std::move(loaded.diagnostic)};
  }

  auto result = loaded.record->result;
  if (!loaded.record->replayFile.has_value()) {
    return {.status = ReadStatus::ReplayUnavailable,
            .result = std::move(result),
            .diagnostic = "This course result has no replay file reference."};
  }
  const auto &reference = *loaded.record->replayFile;
  const replay::ReplayFileMetadata metadata{
      .relativePath = reference.relativePath,
      .sha256 = reference.contentSha256,
      .compressedSize = reference.compressedSize,
      .codecVersion = reference.codecVersion,
  };
  replay::ReplayFileStore store(profileRoot);
  const auto inspection = store.inspect(metadata);
  if (inspection.state != replay::ReplayFileState::Available) {
    const bool unavailable =
        inspection.state == replay::ReplayFileState::Missing;
    return {.status = unavailable ? ReadStatus::ReplayUnavailable
                                  : ReadStatus::Invalid,
            .result = std::move(result),
            .diagnostic =
                inspection.diagnostic.empty()
                    ? (unavailable
                           ? "The course replay file was deleted or moved."
                           : "The course replay file is unreadable or corrupt.")
                    : inspection.diagnostic};
  }
  replay::BeatorajaReplayCodec codec;
  auto decoded = store.load(metadata, codec);
  if (!decoded.course.has_value() || decoded.chart.has_value()) {
    return {.status = ReadStatus::Invalid,
            .result = std::move(result),
            .diagnostic = decoded.diagnostic.empty()
                              ? "The replay file is not a course replay."
                              : std::move(decoded.diagnostic)};
  }
  if (decoded.course->stages.size() != result.stages.size() ||
      decoded.course->restMicrosAfterStage.size() != result.stages.size()) {
    return {.status = ReadStatus::IntegrityConflict,
            .result = std::move(result),
            .diagnostic =
                "The course replay stage count differs from its result."};
  }
  for (std::size_t index = 0; index < result.stages.size(); ++index) {
    const auto &setup = decoded.course->stages[index].setup;
    const auto &stage = result.stages[index];
    if (setup.chartSha256 != stage.score.chartSha256 ||
        setup.keyMode != stage.keyMode ||
        setup.longNoteMode != stage.score.longNoteMode) {
      return {.status = ReadStatus::IntegrityConflict,
              .result = std::move(result),
              .diagnostic =
                  "A course replay stage differs from its saved result."};
    }
  }
  return {.status = ReadStatus::Loaded,
          .result = std::move(result),
          .playback = std::move(decoded.course)};
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

bool ReplayRepository::ListReplayFileReferencesSnapshot(
    const std::filesystem::path &snapshotDatabasePath,
    std::vector<ReplayFileReference> &references, std::string &errorMessage) {
  references.clear();
  if (snapshotDatabasePath.empty()) {
    errorMessage = "replay reference snapshot path is empty";
    return false;
  }
  std::error_code filesystemError;
  const auto status =
      std::filesystem::symlink_status(snapshotDatabasePath, filesystemError);
  if (filesystemError || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    errorMessage = "replay reference snapshot is not a regular database file";
    return false;
  }

  ReplayRepository snapshot(snapshotDatabasePath);
  if (!snapshot.EnsureSchema()) {
    errorMessage = "replay reference snapshot schema is unavailable";
    return false;
  }

  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(snapshot.impl_->sessionMutex);
  if (!snapshot.EnsureSessionDatabaseLocked()) {
    errorMessage = "replay reference snapshot storage is unavailable";
    return false;
  }
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(
          snapshot.impl_->sessionDatabase,
          "SELECT id,chart_result_id,course_result_id,stem,history_index,"
          "relative_path,content_sha256,compressed_size,codec_version FROM "
          "replay_files ORDER BY relative_path",
          statement) != SQLITE_OK) {
    errorMessage = "could not read replay reference snapshot";
    return false;
  }

  int result = SQLITE_OK;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    const bool chartReference =
        sqlite3_column_type(statement.get(), 1) == SQLITE_INTEGER;
    const bool courseReference =
        sqlite3_column_type(statement.get(), 2) == SQLITE_INTEGER;
    const std::int64_t recordId =
        sqlite3_column_int64(statement.get(), chartReference ? 1 : 2);
    const std::int64_t compressedSize =
        sqlite3_column_int64(statement.get(), 7);
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER ||
        chartReference == courseReference ||
        sqlite3_column_type(statement.get(), chartReference ? 2 : 1) !=
            SQLITE_NULL ||
        sqlite3_column_type(statement.get(), 3) != SQLITE_TEXT ||
        sqlite3_column_type(statement.get(), 4) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 5) != SQLITE_TEXT ||
        sqlite3_column_type(statement.get(), 6) != SQLITE_TEXT ||
        sqlite3_column_type(statement.get(), 7) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 8) != SQLITE_INTEGER ||
        sqlite3_column_int64(statement.get(), 0) <= 0 || recordId <= 0 ||
        recordId > std::numeric_limits<int>::max() || compressedSize <= 0 ||
        sqlite3_column_int64(statement.get(), 8) !=
            replay::BeatorajaReplayCodec::kCodecVersion) {
      references.clear();
      errorMessage = "replay reference snapshot contains an invalid row";
      return false;
    }
    ReplayFileReference reference{
        .id = sqlite3_column_int64(statement.get(), 0),
        .recordKind = chartReference
                          ? ReplayFileReference::RecordKind::ChartResult
                          : ReplayFileReference::RecordKind::CourseResult,
        .recordId = static_cast<int>(recordId),
        .stem = columnText(statement.get(), 3),
        .historyIndex = sqlite3_column_int64(statement.get(), 4),
        .relativePath = std::filesystem::path(columnText(statement.get(), 5)),
        .contentSha256 = columnText(statement.get(), 6),
        .compressedSize = static_cast<std::uint64_t>(compressedSize),
        .codecVersion = sqlite3_column_int(statement.get(), 8),
    };
    ReplayFileReference validationReference = reference;
    validationReference.id = 0;
    validationReference.recordId = 0;
    std::string diagnostic;
    if (!validateReplayReference(validationReference, reference.recordKind,
                                 diagnostic)) {
      references.clear();
      errorMessage = diagnostic.empty()
                         ? "replay reference snapshot contains an invalid row"
                         : std::move(diagnostic);
      return false;
    }
    references.push_back(std::move(reference));
  }
  if (result != SQLITE_DONE) {
    references.clear();
    errorMessage = "could not finish reading replay reference snapshot";
    return false;
  }
  errorMessage.clear();
  return true;
}
