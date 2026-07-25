#include "ReplayRepository.h"
#include "ReplayRepositoryInternal.h"

#include "../BmsMetadataText.h"
#include "../LongNoteModeUtils.h"
#include "../ProfileDatabaseActivity.h"
#include "../ir/IrProfileSettings.h"
#include "SqliteRAII.h"
#include "../Uuid.h"

#include <nlohmann/json.hpp>

#include <SDL2/SDL.h>
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using asobmshow::bms_metadata::normalizedHash;

bool isCanonicalCourseKey(std::string_view key) {
  constexpr std::string_view prefix = "course:v1:";
  if (!key.starts_with(prefix) || key.size() != prefix.size() + 64) {
    return false;
  }
  return std::ranges::all_of(key.substr(prefix.size()), [](unsigned char ch) {
    return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f');
  });
}

bool isHexDigest(std::string_view value, std::size_t expectedLength) {
  return value.size() == expectedLength &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isxdigit(character) != 0;
         });
}

bool isValidIrProviderId(std::string_view value) {
  return !value.empty() && value.size() <= ir::kMaximumIrProviderIdBytes &&
         value.front() >= 'a' && value.front() <= 'z' &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') ||
                  character == '_' || character == '-';
         });
}

std::optional<course_identity::ChartIdentity>
strictChartIdentity(std::string sha256, std::string md5) {
  const bool hadSha256 = !sha256.empty();
  const bool hadMd5 = !md5.empty();
  sha256 = normalizedHash(sha256);
  md5 = normalizedHash(md5);
  if ((hadSha256 && !isHexDigest(sha256, 64)) ||
      (hadMd5 && !isHexDigest(md5, 32)) ||
      (sha256.empty() && md5.empty())) {
    return std::nullopt;
  }
  return course_identity::ChartIdentity{.sha256 = std::move(sha256),
                                        .md5 = std::move(md5)};
}

std::optional<ScoreProvenance> decodeStoredProvenance(
    sqlite3_stmt *stmt, int rulesetVersionColumn, int eligibilityColumn,
    int provenanceJsonColumn, std::string &error);

void logSqlErrorText(const char *context, const std::string &error) {
  SDL_Log("SQL error while %s: %s", context, error.c_str());
}

bool bindTextView(sqlite3_stmt *statement, int index,
                  std::string_view value) {
  return sqlite3_bind_text(statement, index, value.data(),
                           static_cast<int>(value.size()), SQLITE_TRANSIENT) ==
         SQLITE_OK;
}

std::string readText(sqlite3_stmt *stmt, int idx) {
  return sqliteColumnString(stmt, idx);
}

std::optional<ScoreProvenance> decodeStoredProvenance(sqlite3_stmt *stmt,
                                                      int rulesetVersionColumn,
                                                      int eligibilityColumn,
                                                      int provenanceJsonColumn,
                                                      std::string &error) {
  const int rulesetVersion = sqlite3_column_int(stmt, rulesetVersionColumn);
  const int eligibilityValue = sqlite3_column_int(stmt, eligibilityColumn);
  const std::string serialized = readText(stmt, provenanceJsonColumn);
  auto provenance = deserializeScoreProvenance(serialized, error);
  if (!provenance.has_value()) {
    return std::nullopt;
  }
  if (provenance->ruleset.version != rulesetVersion ||
      static_cast<int>(provenance->eligibility) != eligibilityValue) {
    error = "indexed values disagree with JSON";
    return std::nullopt;
  }
  return provenance;
}

bool isCanonicalLowerHex(std::string_view value, std::size_t size) {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f');
         });
}

bool sameFloatBits(float left, float right) {
  return std::bit_cast<std::uint32_t>(left) ==
         std::bit_cast<std::uint32_t>(right);
}

bool isKnownClearRank(int clearType) {
  switch (clearType) {
  case kClearTypeFailedRank:
  case kClearTypeAssistedEasyClearRank:
  case kClearTypeLightAssistedEasyClearRank:
  case kClearTypeEasyClearRank:
  case kClearTypeNormalClearRank:
  case kClearTypeHardClearRank:
  case kClearTypeExHardClearRank:
  case kClearTypeFullComboRank:
    return true;
  default:
    return false;
  }
}

bool validateChartScoreReplaySemantics(
    const result_persistence::ChartScoreWrite &score,
    const bms_parser::ChartMeta &chartMeta, int replayFinalScore,
    int replayMaxCombo, float replayFinalGauge, int replayClearType,
    std::optional<int> expectedTotalNotes,
    std::optional<int> storedReplayChartLongNoteMode,
    std::string &diagnostic) {
  const auto reject = [&](std::string message) {
    diagnostic = std::move(message);
    return false;
  };

  if (long_note_mode::normalizeValue(score.longNoteMode) !=
      score.longNoteMode) {
    return reject("score long-note mode is outside the canonical range");
  }
  const ScoreStageProvenance *stage =
      score_provenance::uniqueStageForChart(score.provenance, chartMeta);
  if (stage == nullptr) {
    return reject(
        "score provenance does not identify a unique stage for the chart");
  }
  if (long_note_mode::normalizeValue(stage->longNoteMode) !=
      stage->longNoteMode) {
    return reject(
        "score provenance stage long-note mode is outside the canonical "
        "range");
  }
  if (stage->longNoteMode <= long_note_mode::kUnknownValue) {
    return reject("score provenance stage long-note mode is unspecified");
  }
  if (storedReplayChartLongNoteMode.has_value()) {
    const int replayChartLongNoteMode = *storedReplayChartLongNoteMode;
    if (long_note_mode::normalizeValue(replayChartLongNoteMode) !=
            replayChartLongNoteMode ||
        (score.longNoteMode > long_note_mode::kUnknownValue &&
         (stage->longNoteMode != score.longNoteMode ||
          (replayChartLongNoteMode > long_note_mode::kUnknownValue &&
           replayChartLongNoteMode != score.longNoteMode)))) {
      return reject(
          "score long-note mode does not match staged replay metadata");
    }
  } else {
    const int chartLongNoteMode =
        long_note_mode::normalizeValue(chartMeta.LnMode);
    const int expectedLongNoteMode =
        std::max(0, chartMeta.TotalLongNotes) +
                    std::max(0, chartMeta.TotalBackSpinNotes) <=
                0
            ? long_note_mode::kUnknownValue
            : (chartLongNoteMode > long_note_mode::kUnknownValue
                   ? chartLongNoteMode
                   : stage->longNoteMode);
    if (score.longNoteMode != expectedLongNoteMode ||
        (expectedLongNoteMode > long_note_mode::kUnknownValue &&
         stage->longNoteMode != expectedLongNoteMode)) {
      return reject(
          "score long-note mode does not match a unique provenance stage");
    }
  }

  if (score.score < 0 || score.maxScore < 0 || score.maxCombo < 0 ||
      score.comboBreak < 0 || score.pGreat < 0 || score.great < 0 ||
      score.good < 0 || score.bad < 0 || score.poor < 0 || score.kPoor < 0 ||
      score.fast < 0 || score.slow < 0) {
    return reject("score payload contains negative result counters");
  }
  if ((score.maxScore % 2) != 0) {
    return reject("score maximum is not a whole-note score");
  }
  const int totalNotes = score.maxScore / 2;
  if (expectedTotalNotes.has_value()) {
    const std::int64_t expectedMaxScore =
        static_cast<std::int64_t>(*expectedTotalNotes) * 2;
    if (*expectedTotalNotes < 0 ||
        expectedMaxScore > std::numeric_limits<int>::max() ||
        score.maxScore != static_cast<int>(expectedMaxScore)) {
      return reject("score maximum does not match replay chart notes");
    }
  }
  const std::int64_t judgementScore =
      static_cast<std::int64_t>(score.pGreat) * 2 + score.great;
  if (judgementScore != score.score) {
    return reject("score payload is inconsistent with its result counters");
  }
  const float maximumGauge = gaugeStartingMaximumValue(
      score.provenance.gaugeType, score.provenance.gaugeAutoShift,
      score.provenance.gaugeAutoShiftLowerBound, score.provenance.gaugeProfile);
  if (!std::isfinite(score.finalGauge) || score.finalGauge < 0.0f ||
      score.finalGauge > maximumGauge) {
    return reject("score gauge is outside the playable range");
  }
  if (!isKnownClearRank(score.clearType)) {
    return reject("score clear rank is not recognized");
  }
  if (score.score != replayFinalScore || score.maxCombo != replayMaxCombo ||
      !sameFloatBits(score.finalGauge, replayFinalGauge)) {
    return reject("score and replay result facts disagree");
  }

  const bool fullCombo =
      totalNotes > 0 && score.comboBreak == 0 && score.maxCombo >= totalNotes;
  const int expectedReplayClearType = clear_policy::fullComboRankForPlayback(
      score.clearType, fullCombo, score.provenance.playback);
  if (replayClearType != expectedReplayClearType) {
    return reject("score and replay clear ranks disagree");
  }
  return true;
}

constexpr const char *kPendingChartScoreSelect =
    "SELECT p.attempt_id, p.result_id, p.chart_path, p.chart_md5,"
    "p.chart_sha256, p.chart_title, p.chart_artist, p.ln_mode, p.score,"
    "p.max_score, p.max_combo, p.combo_break, p.pgreat, p.great, p.good,"
    "p.bad, p.poor, p.kpoor, p.fast, p.slow, p.final_gauge, p.clear_type,"
    "p.ruleset_version, p.eligibility, p.provenance_json, p.created_at,"
    "p.recovery_attempts, p.last_recovery_at, r.id, r.attempt_id,"
    "r.result_fingerprint, r.chart_path, r.chart_md5, r.chart_sha256,"
    "r.chart_title, r.chart_artist, r.created_at, r.score,"
    "r.max_combo, r.final_gauge, r.clear_type, p.ruleset_version,"
    "p.eligibility, r.provenance_json, r.long_note_mode "
    "FROM pending_chart_score_writes p "
    "LEFT JOIN chart_results r ON r.id = p.result_id ";

bool readStrictText(sqlite3_stmt *stmt, int column, std::string &value,
                    bool allowEmpty = true) {
  if (sqlite3_column_type(stmt, column) != SQLITE_TEXT) {
    return false;
  }
  value = sqliteColumnString(stmt, column);
  return allowEmpty || !value.empty();
}

bool readStrictInteger(sqlite3_stmt *stmt, int column, int &value) {
  if (sqlite3_column_type(stmt, column) != SQLITE_INTEGER) {
    return false;
  }
  const sqlite3_int64 stored = sqlite3_column_int64(stmt, column);
  if (stored < static_cast<sqlite3_int64>(std::numeric_limits<int>::min()) ||
      stored > static_cast<sqlite3_int64>(std::numeric_limits<int>::max())) {
    return false;
  }
  value = static_cast<int>(stored);
  return true;
}

result_persistence::PendingBatchEntry decodePendingChartScoreRow(
    sqlite3_stmt *stmt, std::optional<std::string_view> expectedAttemptId) {
  using result_persistence::PendingBatchEntry;
  using result_persistence::PendingChartScoreWrite;
  using result_persistence::PendingReadStatus;

  PendingBatchEntry result;
  result.attemptId = sqliteColumnString(stmt, 0);
  const auto conflict = [&](std::string diagnostic) {
    result.status = PendingReadStatus::IntegrityConflict;
    result.value.reset();
    result.diagnostic = std::move(diagnostic);
    return result;
  };

  if (sqlite3_column_type(stmt, 0) != SQLITE_TEXT ||
      !uuid::isCanonicalLowerV4(result.attemptId) ||
      (expectedAttemptId.has_value() &&
       result.attemptId != *expectedAttemptId)) {
    return conflict("pending score attempt identity is malformed");
  }

  PendingChartScoreWrite pending;
  pending.attemptId = result.attemptId;
  if (!readStrictInteger(stmt, 1, pending.resultId) ||
      pending.resultId <= 0) {
    return conflict("pending score replay identity is malformed");
  }

  auto &score = pending.score;
  if (!readStrictText(stmt, 2, score.chartPath) ||
      !readStrictText(stmt, 3, score.chartMd5) ||
      !readStrictText(stmt, 4, score.chartSha256) ||
      !readStrictText(stmt, 5, score.chartTitle) ||
      !readStrictText(stmt, 6, score.chartArtist) ||
      !readStrictInteger(stmt, 7, score.longNoteMode) ||
      !readStrictInteger(stmt, 8, score.score) ||
      !readStrictInteger(stmt, 9, score.maxScore) ||
      !readStrictInteger(stmt, 10, score.maxCombo) ||
      !readStrictInteger(stmt, 11, score.comboBreak) ||
      !readStrictInteger(stmt, 12, score.pGreat) ||
      !readStrictInteger(stmt, 13, score.great) ||
      !readStrictInteger(stmt, 14, score.good) ||
      !readStrictInteger(stmt, 15, score.bad) ||
      !readStrictInteger(stmt, 16, score.poor) ||
      !readStrictInteger(stmt, 17, score.kPoor) ||
      !readStrictInteger(stmt, 18, score.fast) ||
      !readStrictInteger(stmt, 19, score.slow) ||
      !readStrictInteger(stmt, 21, score.clearType)) {
    return conflict("pending score has an invalid SQLite storage class or "
                    "integer range");
  }

  if (sqlite3_column_type(stmt, 20) != SQLITE_FLOAT) {
    return conflict("pending score gauge has an invalid SQLite storage class");
  }
  const double storedGauge = sqlite3_column_double(stmt, 20);
  if (!std::isfinite(storedGauge) ||
      storedGauge < -static_cast<double>(std::numeric_limits<float>::max()) ||
      storedGauge > static_cast<double>(std::numeric_limits<float>::max())) {
    return conflict("pending score gauge is outside the finite float range");
  }
  score.finalGauge = static_cast<float>(storedGauge);

  int rulesetVersion = 0;
  int eligibility = 0;
  int recoveryAttempts = 0;
  std::string provenanceJson;
  if (!readStrictInteger(stmt, 22, rulesetVersion) ||
      !readStrictInteger(stmt, 23, eligibility) ||
      eligibility < static_cast<int>(ScoreEligibility::Verified) ||
      eligibility > static_cast<int>(ScoreEligibility::LegacyUnverified) ||
      !readStrictText(stmt, 24, provenanceJson, false) ||
      !readStrictText(stmt, 25, pending.createdAt, false) ||
      !readStrictInteger(stmt, 26, recoveryAttempts) ||
      recoveryAttempts < 0 ||
      (sqlite3_column_type(stmt, 27) != SQLITE_NULL &&
       sqlite3_column_type(stmt, 27) != SQLITE_TEXT) ||
      (sqlite3_column_type(stmt, 27) == SQLITE_TEXT &&
       sqliteColumnTextView(stmt, 27).empty())) {
    return conflict("pending score metadata is malformed");
  }

  std::string provenanceError;
  auto provenance =
      deserializeScoreProvenance(provenanceJson, provenanceError);
  if (!provenance.has_value()) {
    return conflict("pending score provenance is malformed");
  }
  auto canonicalProvenance =
      serializeValidatedScoreProvenance(*provenance, provenanceError);
  if (!canonicalProvenance.has_value() ||
      *canonicalProvenance != provenanceJson ||
      provenance->ruleset.version != rulesetVersion ||
      static_cast<int>(provenance->eligibility) != eligibility) {
    return conflict("pending score provenance is not canonical or linked");
  }
  score.provenance = std::move(*provenance);

  if (!result_persistence::hasProjectableChartIdentity(score)) {
    return conflict("pending score chart identity is not projectable");
  }

  int linkedReplayId = 0;
  std::string linkedAttemptId;
  std::string linkedFingerprint;
  std::string linkedPath;
  std::string linkedMd5;
  std::string linkedSha256;
  std::string linkedTitle;
  std::string linkedArtist;
  std::string linkedCreatedAt;
  if (!readStrictInteger(stmt, 28, linkedReplayId) ||
      linkedReplayId != pending.resultId ||
      !readStrictText(stmt, 29, linkedAttemptId, false) ||
      linkedAttemptId != pending.attemptId ||
      !uuid::isCanonicalLowerV4(linkedAttemptId) ||
      !readStrictText(stmt, 30, linkedFingerprint, false) ||
      !isCanonicalLowerHex(linkedFingerprint, 64) ||
      !readStrictText(stmt, 31, linkedPath) ||
      !readStrictText(stmt, 32, linkedMd5) ||
      !readStrictText(stmt, 33, linkedSha256) ||
      !readStrictText(stmt, 34, linkedTitle) ||
      !readStrictText(stmt, 35, linkedArtist) ||
      !readStrictText(stmt, 36, linkedCreatedAt, false) ||
      linkedCreatedAt != pending.createdAt || linkedPath != score.chartPath ||
      linkedMd5 != score.chartMd5 || linkedSha256 != score.chartSha256 ||
      linkedTitle != score.chartTitle || linkedArtist != score.chartArtist) {
    return conflict("pending score and staged replay linkage is malformed");
  }

  int linkedFinalScore = 0;
  int linkedMaxCombo = 0;
  int linkedClearType = 0;
  int linkedRulesetVersion = 0;
  int linkedEligibility = 0;
  int linkedLongNoteMode = 0;
  std::string linkedProvenanceJson;
  if (!readStrictInteger(stmt, 37, linkedFinalScore) ||
      !readStrictInteger(stmt, 38, linkedMaxCombo) ||
      sqlite3_column_type(stmt, 39) != SQLITE_FLOAT ||
      !readStrictInteger(stmt, 40, linkedClearType) ||
      !readStrictInteger(stmt, 41, linkedRulesetVersion) ||
      !readStrictInteger(stmt, 42, linkedEligibility) ||
      linkedEligibility < static_cast<int>(ScoreEligibility::Verified) ||
      linkedEligibility >
          static_cast<int>(ScoreEligibility::LegacyUnverified) ||
      !readStrictText(stmt, 43, linkedProvenanceJson, false) ||
      !readStrictInteger(stmt, 44, linkedLongNoteMode)) {
    return conflict("staged replay result metadata is malformed");
  }
  const double storedReplayGauge = sqlite3_column_double(stmt, 39);
  if (!std::isfinite(storedReplayGauge) ||
      storedReplayGauge <
          -static_cast<double>(std::numeric_limits<float>::max()) ||
      storedReplayGauge >
          static_cast<double>(std::numeric_limits<float>::max())) {
    return conflict("staged replay gauge is outside the finite float range");
  }
  const float linkedFinalGauge = static_cast<float>(storedReplayGauge);

  std::string linkedProvenanceError;
  auto linkedProvenance =
      deserializeScoreProvenance(linkedProvenanceJson, linkedProvenanceError);
  if (!linkedProvenance.has_value()) {
    return conflict("staged replay provenance is malformed");
  }
  auto canonicalLinkedProvenance = serializeValidatedScoreProvenance(
      *linkedProvenance, linkedProvenanceError);
  if (!canonicalLinkedProvenance.has_value() ||
      *canonicalLinkedProvenance != linkedProvenanceJson ||
      linkedProvenance->ruleset.version != linkedRulesetVersion ||
      static_cast<int>(linkedProvenance->eligibility) != linkedEligibility ||
      *linkedProvenance != score.provenance) {
    return conflict("staged replay provenance is not canonical or linked");
  }

  bms_parser::ChartMeta linkedChartMeta;
  linkedChartMeta.MD5 = score.chartMd5;
  linkedChartMeta.SHA256 = score.chartSha256;
  std::string semanticDiagnostic;
  if (!validateChartScoreReplaySemantics(score, linkedChartMeta,
                                         linkedFinalScore, linkedMaxCombo,
                                         linkedFinalGauge, linkedClearType,
                                         std::nullopt, linkedLongNoteMode,
                                         semanticDiagnostic)) {
    return conflict("pending score semantics are invalid: " +
                    semanticDiagnostic);
  }

  result.status = PendingReadStatus::Found;
  result.value = std::move(pending);
  result.diagnostic.clear();
  return result;
}

result_persistence::PendingReadOutcome loadPendingChartScoreOnConnection(
    sqlite3 *db, std::string_view attemptId) {
  using result_persistence::PendingReadOutcome;
  using result_persistence::PendingReadStatus;

  std::string query = kPendingChartScoreSelect;
  query += "WHERE p.attempt_id = ? OR r.attempt_id = ?";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing pending chart score lookup",
                                    logSqlErrorText) ||
      !bindTextView(stmt.get(), 1, attemptId) ||
      !bindTextView(stmt.get(), 2, attemptId)) {
    return {.status = PendingReadStatus::StorageFailure,
            .diagnostic = "could not query the pending score"};
  }

  int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_DONE) {
    return {.status = PendingReadStatus::NotFound};
  }
  if (rc != SQLITE_ROW) {
    return {.status = PendingReadStatus::StorageFailure,
            .diagnostic = "could not query the pending score"};
  }
  auto entry = decodePendingChartScoreRow(stmt.get(), attemptId);
  rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_ROW) {
    return {.status = PendingReadStatus::IntegrityConflict,
            .diagnostic =
                "multiple pending scores claim the same attempt identity"};
  }
  if (rc != SQLITE_DONE) {
    return {.status = PendingReadStatus::StorageFailure,
            .diagnostic = "pending score lookup did not complete"};
  }
  return {.status = entry.status,
          .value = std::move(entry.value),
          .diagnostic = std::move(entry.diagnostic)};
}

} // namespace

result_persistence::PendingReadOutcome
ReplayRepository::LoadPendingChartScore(std::string_view attemptId) {
  using result_persistence::PendingReadStatus;
  profile_database_activity::ReadGuard readGuard;
  if (!uuid::isCanonicalLowerV4(attemptId)) {
    return {.status = PendingReadStatus::IntegrityConflict,
            .diagnostic = "attempt ID is not a canonical version-4 UUID"};
  }
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = PendingReadStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  return loadPendingChartScoreOnConnection(impl_->sessionDatabase, attemptId);
}

result_persistence::PendingBatchOutcome
ReplayRepository::ListPendingChartScores(std::size_t limit) {
  using result_persistence::PendingBatchOutcome;
  profile_database_activity::ReadGuard readGuard;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.storageAvailable = false,
            .diagnostic = "replay storage is unavailable"};
  }

  SqliteStatementHandle countStmt;
  if (!prepareSqliteStatementLogged(
          impl_->sessionDatabase,
          "SELECT COUNT(*) FROM pending_chart_score_writes", countStmt,
          "counting pending chart score recovery", logSqlErrorText) ||
      sqlite3_step(countStmt.get()) != SQLITE_ROW ||
      sqlite3_column_type(countStmt.get(), 0) != SQLITE_INTEGER) {
    return {.storageAvailable = false,
            .diagnostic = "could not count pending score recovery"};
  }
  const sqlite3_int64 storedPendingCount =
      sqlite3_column_int64(countStmt.get(), 0);
  if (storedPendingCount < 0 ||
      static_cast<std::uint64_t>(storedPendingCount) >
          std::numeric_limits<std::size_t>::max() ||
      sqlite3_step(countStmt.get()) != SQLITE_DONE) {
    return {.storageAvailable = false,
            .diagnostic = "pending score recovery count is invalid"};
  }
  const std::size_t pendingCount =
      static_cast<std::size_t>(storedPendingCount);

  std::string query = kPendingChartScoreSelect;
  query += "ORDER BY p.recovery_attempts, p.last_recovery_at, p.created_at, "
           "p.attempt_id LIMIT ?";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(impl_->sessionDatabase, query, stmt,
                                    "preparing pending chart score recovery",
                                    logSqlErrorText)) {
    return {.storageAvailable = false,
            .diagnostic = "could not query pending score recovery"};
  }
  constexpr std::size_t maxRecoveryBatchSize = 256;
  const sqlite3_int64 queryLimit =
      static_cast<sqlite3_int64>(std::min(limit, maxRecoveryBatchSize));
  if (sqlite3_bind_int64(stmt.get(), 1, queryLimit) != SQLITE_OK) {
    return {.storageAvailable = false,
            .diagnostic = "could not bind pending score recovery limit"};
  }

  PendingBatchOutcome outcome{.storageAvailable = true};
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
    outcome.entries.push_back(
        decodePendingChartScoreRow(stmt.get(), std::nullopt));
  }
  if (rc != SQLITE_DONE) {
    outcome.storageAvailable = false;
    outcome.entries.clear();
    outcome.diagnostic = "pending score recovery query did not complete";
  } else if (outcome.entries.size() > pendingCount) {
    outcome.storageAvailable = false;
    outcome.entries.clear();
    outcome.diagnostic = "pending score recovery count changed unexpectedly";
  } else {
    outcome.remaining = pendingCount - outcome.entries.size();
  }
  return outcome;
}

result_persistence::AcknowledgeOutcome
ReplayRepository::AcknowledgePendingChartScoreAndActivateIr(
    std::string_view attemptId, int resultId) {
  using result_persistence::AcknowledgeStatus;
  using result_persistence::PendingReadStatus;
  profile_database_activity::WriteGuard writeGuard;
  if (!uuid::isCanonicalLowerV4(attemptId) || resultId <= 0) {
    return {.status = AcknowledgeStatus::IntegrityConflict,
            .diagnostic = "pending score identity is malformed"};
  }

  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(impl_->sessionDatabase,
                                      "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = "could not start score acknowledgement"};
  }

  auto pending =
      loadPendingChartScoreOnConnection(impl_->sessionDatabase, attemptId);
  if (pending.status == PendingReadStatus::StorageFailure) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = std::move(pending.diagnostic)};
  }
  if (pending.status == PendingReadStatus::IntegrityConflict) {
    return {.status = AcknowledgeStatus::IntegrityConflict,
            .diagnostic = std::move(pending.diagnostic)};
  }
  if (pending.status == PendingReadStatus::Found) {
    if (!pending.value.has_value() || pending.value->resultId != resultId) {
      return {.status = AcknowledgeStatus::IntegrityConflict,
              .diagnostic = "pending score names a different result"};
    }
    SqliteStatementHandle deleteStmt;
    if (!prepareSqliteStatementLogged(
            impl_->sessionDatabase,
            "DELETE FROM pending_chart_score_writes "
            "WHERE attempt_id = ? AND result_id = ?",
            deleteStmt, "preparing pending chart score acknowledgement",
            logSqlErrorText) ||
        !bindTextView(deleteStmt.get(), 1, attemptId) ||
        sqlite3_bind_int(deleteStmt.get(), 2, resultId) != SQLITE_OK ||
        sqlite3_step(deleteStmt.get()) != SQLITE_DONE ||
        sqlite3_changes(impl_->sessionDatabase) != 1) {
      return {.status = AcknowledgeStatus::StorageFailure,
              .diagnostic = "could not acknowledge the pending score"};
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    SqliteStatementHandle activateStmt;
    if (!prepareSqliteStatementLogged(
            impl_->sessionDatabase,
            "UPDATE ir_outbox SET local_result_ready=1,"
            "next_attempt_at_ms=COALESCE(next_attempt_at_ms,?),"
            "updated_at_ms=? WHERE attempt_id=? AND local_result_ready=0",
            activateStmt, "preparing automatic IR activation",
            logSqlErrorText) ||
        sqlite3_bind_int64(activateStmt.get(), 1, now) != SQLITE_OK ||
        sqlite3_bind_int64(activateStmt.get(), 2, now) != SQLITE_OK ||
        !bindTextView(activateStmt.get(), 3, attemptId) ||
        sqlite3_step(activateStmt.get()) != SQLITE_DONE) {
      return {.status = AcknowledgeStatus::StorageFailure,
              .diagnostic = "could not activate automatic IR work"};
    }
    if (!transaction.commit(transactionError)) {
      return {.status = AcknowledgeStatus::StorageFailure,
              .diagnostic = "could not commit score acknowledgement"};
    }
    return {.status = AcknowledgeStatus::Acknowledged};
  }

  SqliteStatementHandle resultStmt;
  if (!prepareSqliteStatementLogged(
          impl_->sessionDatabase,
          "SELECT COUNT(*) FROM chart_results r WHERE r.id=? AND "
          "r.attempt_id=? AND NOT EXISTS(SELECT 1 FROM "
          "pending_chart_score_writes p WHERE p.result_id=r.id)",
          resultStmt, "checking acknowledged chart result", logSqlErrorText) ||
      sqlite3_bind_int(resultStmt.get(), 1, resultId) != SQLITE_OK ||
      !bindTextView(resultStmt.get(), 2, attemptId) ||
      sqlite3_step(resultStmt.get()) != SQLITE_ROW ||
      sqlite3_column_type(resultStmt.get(), 0) != SQLITE_INTEGER) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = "could not verify acknowledged result"};
  }
  const sqlite3_int64 resultCount =
      sqlite3_column_int64(resultStmt.get(), 0);
  if (resultCount != 1 || sqlite3_step(resultStmt.get()) != SQLITE_DONE) {
    return {.status = AcknowledgeStatus::IntegrityConflict,
            .diagnostic =
                "acknowledged score has no matching durable result identity"};
  }
  SqliteStatementHandle inactiveStmt;
  if (!prepareSqliteStatementLogged(
          impl_->sessionDatabase,
          "SELECT COUNT(*) FROM ir_outbox WHERE attempt_id=? AND "
          "local_result_ready=0",
          inactiveStmt, "checking automatic IR activation", logSqlErrorText) ||
      !bindTextView(inactiveStmt.get(), 1, attemptId) ||
      sqlite3_step(inactiveStmt.get()) != SQLITE_ROW ||
      sqlite3_column_type(inactiveStmt.get(), 0) != SQLITE_INTEGER) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = "could not verify automatic IR activation"};
  }
  const sqlite3_int64 inactiveCount =
      sqlite3_column_int64(inactiveStmt.get(), 0);
  if (inactiveCount < 0 || sqlite3_step(inactiveStmt.get()) != SQLITE_DONE) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = "automatic IR activation count is invalid"};
  }
  if (inactiveCount != 0) {
    return {.status = AcknowledgeStatus::IntegrityConflict,
            .diagnostic =
                "acknowledged result still has inactive automatic IR work"};
  }
  if (!transaction.commit(transactionError)) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = "could not finish score acknowledgement"};
  }
  return {.status = AcknowledgeStatus::AlreadyAcknowledged};
}

result_persistence::RecoveryMarkOutcome
ReplayRepository::RecordPendingChartScoreRecoveryAttempt(
    std::string_view attemptId,
    result_persistence::RecoveryAttemptKind kind) {
  using result_persistence::RecoveryMarkStatus;
  profile_database_activity::WriteGuard writeGuard;
  (void)kind;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = RecoveryMarkStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          impl_->sessionDatabase,
          "UPDATE pending_chart_score_writes "
          "SET recovery_attempts = recovery_attempts + 1, "
          "last_recovery_at = CURRENT_TIMESTAMP WHERE attempt_id = ?",
          stmt, "preparing pending score recovery marker", logSqlErrorText) ||
      !bindTextView(stmt.get(), 1, attemptId) ||
      sqlite3_step(stmt.get()) != SQLITE_DONE) {
    return {.status = RecoveryMarkStatus::StorageFailure,
            .diagnostic = "could not record the pending score recovery"};
  }
  const int changes = sqlite3_changes(impl_->sessionDatabase);
  if (changes == 0) {
    return {.status = RecoveryMarkStatus::NotFound};
  }
  if (changes != 1) {
    return {.status = RecoveryMarkStatus::StorageFailure,
            .diagnostic = "pending score recovery updated unexpected rows"};
  }
  return {.status = RecoveryMarkStatus::Recorded};
}

ir::IrReconciliationReadOutcome
ReplayRepository::LoadIrReconciliationCandidates(
    std::string_view providerId, std::string_view serverOrigin) {
  const auto validProviderId = [](std::string_view value) {
    return !value.empty() && value.size() <= ir::kMaximumIrProviderIdBytes &&
           value.front() >= 'a' && value.front() <= 'z' &&
           std::ranges::all_of(value, [](unsigned char character) {
             return (character >= 'a' && character <= 'z') ||
                    (character >= '0' && character <= '9') ||
                    character == '_' || character == '-';
           });
  };
  const auto normalizedOrigin = ir::normalizeServerOrigin(serverOrigin);
  if (!validProviderId(providerId) || !normalizedOrigin ||
      *normalizedOrigin != serverOrigin) {
    return {.status = ir::IrReconciliationReadOutcome::Status::Invalid,
            .diagnostic = "IR reconciliation identity is invalid"};
  }

  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status =
                ir::IrReconciliationReadOutcome::Status::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }

  sqlite3 *database = impl_->sessionDatabase;
  std::string snapshotError;
  SqliteTransactionHandle snapshot(database, "BEGIN TRANSACTION",
                                   snapshotError);
  if (!snapshot.active()) {
    return {.status =
                ir::IrReconciliationReadOutcome::Status::StorageFailure,
            .diagnostic =
                "could not start the IR reconciliation read snapshot"};
  }

  constexpr const char *query =
      "SELECT r.id,r.attempt_id,r.chart_md5,r.chart_sha256,r.score,"
      "r.clear_type,CAST(json_extract(r.provenance_json,'$.ruleset.version') "
      "AS INTEGER),CASE json_extract(r.provenance_json,'$.eligibility') "
      "WHEN 'verified' THEN 0 WHEN 'modified' THEN 1 ELSE 2 END,"
      "r.provenance_json,receipt.id,receipt.provider_id,"
      "receipt.server_origin,receipt.result_id,"
      "receipt.attempt_id,receipt.chart_md5,receipt.chart_sha256,"
      "receipt.remote_user_id,receipt.remote_chart_id,"
      "receipt.remote_score_id,receipt.confirmation_source,"
      "receipt.observed_in_snapshot,receipt.confirmed_at_ms,"
      "outbox.id,outbox.state,outbox.payload_json,outbox.chart_md5,"
      "outbox.chart_sha256,outbox.local_result_ready "
      "FROM chart_results r "
      "LEFT JOIN ir_submission_receipts receipt ON receipt.provider_id=?1 "
      "AND receipt.server_origin=?2 AND receipt.result_id=r.id "
      "LEFT JOIN ir_outbox outbox ON outbox.provider_id=?1 "
      "AND outbox.attempt_id=r.attempt_id AND (outbox.state!=5 OR "
      "(receipt.id IS NOT NULL AND receipt.attempt_id=outbox.attempt_id "
      "AND receipt.confirmation_source=0)) "
      "WHERE r.attempt_id IS NOT NULL AND NOT EXISTS(SELECT 1 FROM "
      "ir_outbox inactive_outbox WHERE "
      "inactive_outbox.provider_id=?1 AND "
      "inactive_outbox.attempt_id=r.attempt_id AND "
      "inactive_outbox.local_result_ready=0) "
      "ORDER BY r.id LIMIT ?3";
  constexpr std::size_t scanLimit =
      ir::kMaximumIrRemoteScoreSnapshotEntries +
      replay_summary_scan::kCorruptCandidateAllowance + 1;
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK ||
      sqlite3_bind_text(statement.get(), 1, providerId.data(),
                        static_cast<int>(providerId.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(statement.get(), 2, serverOrigin.data(),
                        static_cast<int>(serverOrigin.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_int64(statement.get(), 3,
                         static_cast<sqlite3_int64>(scanLimit)) != SQLITE_OK) {
    return {.status =
                ir::IrReconciliationReadOutcome::Status::StorageFailure,
            .diagnostic =
                "could not prepare the IR reconciliation candidate read"};
  }

  const auto nullableText = [&](int column) {
    const int type = sqlite3_column_type(statement.get(), column);
    return type == SQLITE_NULL || type == SQLITE_TEXT;
  };
  const auto nullableInteger = [&](int column) {
    const int type = sqlite3_column_type(statement.get(), column);
    return type == SQLITE_NULL || type == SQLITE_INTEGER;
  };
  std::vector<ir::IrLocalReceiptCandidate> candidates;
  std::unordered_set<int> replayIds;
  std::size_t inspected = 0;
  std::size_t rejected = 0;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
    ++inspected;
    const auto rejectRow = [&] { ++rejected; };
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 1) != SQLITE_TEXT ||
        !nullableText(2) || !nullableText(3) ||
        sqlite3_column_type(statement.get(), 4) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 5) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 6) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 7) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 8) != SQLITE_TEXT) {
      rejectRow();
      continue;
    }
    const sqlite3_int64 storedReplayId = sqlite3_column_int64(statement.get(), 0);
    const sqlite3_int64 storedScore = sqlite3_column_int64(statement.get(), 4);
    const sqlite3_int64 storedLamp = sqlite3_column_int64(statement.get(), 5);
    if (storedReplayId <= 0 ||
        storedReplayId > std::numeric_limits<int>::max() || storedScore < 0 ||
        storedScore > std::numeric_limits<int>::max() ||
        storedLamp < std::numeric_limits<int>::min() ||
        storedLamp > std::numeric_limits<int>::max()) {
      rejectRow();
      continue;
    }
    const int replayId = static_cast<int>(storedReplayId);
    if (!replayIds.emplace(replayId).second) {
      return {.status = ir::IrReconciliationReadOutcome::Status::Invalid,
              .diagnostic =
                  "IR reconciliation candidate joins are not unique"};
    }
    const std::string attemptId = readText(statement.get(), 1);
    auto identity = strictChartIdentity(readText(statement.get(), 3),
                                        readText(statement.get(), 2));
    std::string provenanceError;
    auto provenance = decodeStoredProvenance(statement.get(), 6, 7, 8,
                                             provenanceError);
    if (!uuid::isCanonicalLowerV4(attemptId) || !identity || !provenance ||
        provenance->stages.size() != 1 ||
        !isKnownClearRank(static_cast<int>(storedLamp))) {
      rejectRow();
      continue;
    }
    bms_parser::ChartMeta chartMeta;
    chartMeta.MD5 = identity->md5;
    chartMeta.SHA256 = identity->sha256;
    if (!score_provenance::stageMatchesChart(provenance->stages.front(),
                                             chartMeta)) {
      rejectRow();
      continue;
    }

    ir::IrLocalReceiptCandidate candidate{
        .replayId = replayId,
        .attemptId = attemptId,
        .chartMd5 = identity->md5,
        .chartSha256 = identity->sha256,
        .score = static_cast<int>(storedScore),
        .lampRank = static_cast<int>(storedLamp),
        .eligible = provenance->eligibility == ScoreEligibility::Verified &&
                    scoreEligibilityForProvenance(*provenance) ==
                        ScoreEligibility::Verified &&
                    !gaugeProfileIsCourse(provenance->gaugeProfile) &&
                    (providerId != ir::kTachiProviderId ||
                     provenance->ruleset ==
                         RulesetDescriptor::For(GameplayRuleset::LR2)),
    };

    if (sqlite3_column_type(statement.get(), 9) != SQLITE_NULL) {
      if (sqlite3_column_type(statement.get(), 9) != SQLITE_INTEGER ||
          sqlite3_column_type(statement.get(), 10) != SQLITE_TEXT ||
          sqlite3_column_type(statement.get(), 11) != SQLITE_TEXT ||
          sqlite3_column_type(statement.get(), 12) != SQLITE_INTEGER ||
          sqlite3_column_type(statement.get(), 13) != SQLITE_TEXT ||
          !nullableText(14) ||
          sqlite3_column_type(statement.get(), 15) != SQLITE_TEXT ||
          !nullableInteger(16) || !nullableText(17) || !nullableText(18) ||
          sqlite3_column_type(statement.get(), 19) != SQLITE_INTEGER ||
          sqlite3_column_type(statement.get(), 20) != SQLITE_INTEGER ||
          sqlite3_column_type(statement.get(), 21) != SQLITE_INTEGER) {
        rejectRow();
        continue;
      }
      ir::IrSubmissionReceipt receipt{
          .id = sqlite3_column_int64(statement.get(), 9),
          .providerId = readText(statement.get(), 10),
          .serverOrigin = readText(statement.get(), 11),
          .replayId = sqlite3_column_int(statement.get(), 12),
          .attemptId = readText(statement.get(), 13),
          .chartMd5 = readText(statement.get(), 14),
          .chartSha256 = readText(statement.get(), 15),
          .remoteUserId =
              sqlite3_column_type(statement.get(), 16) == SQLITE_NULL
                  ? std::optional<std::int64_t>{}
                  : sqlite3_column_int64(statement.get(), 16),
          .remoteChartId = readText(statement.get(), 17),
          .remoteScoreId = readText(statement.get(), 18),
          .source = static_cast<ir::IrReceiptConfirmationSource>(
              sqlite3_column_int(statement.get(), 19)),
          .observedInSnapshot = sqlite3_column_int(statement.get(), 20) != 0,
          .confirmedAtUnixMillis = sqlite3_column_int64(statement.get(), 21),
      };
      std::string diagnostic;
      if (!ir::validateIrSubmissionReceipt(receipt, diagnostic) ||
          receipt.providerId != providerId ||
          receipt.serverOrigin != serverOrigin || receipt.replayId != replayId ||
          receipt.attemptId != attemptId ||
          (!identity->md5.empty() && !receipt.chartMd5.empty() &&
           receipt.chartMd5 != identity->md5) ||
          (!identity->sha256.empty() &&
           receipt.chartSha256 != identity->sha256)) {
        rejectRow();
        continue;
      }
      candidate.currentReceipt = std::move(receipt);
    }

    if (sqlite3_column_type(statement.get(), 22) != SQLITE_NULL) {
      if (sqlite3_column_type(statement.get(), 22) != SQLITE_INTEGER ||
          sqlite3_column_type(statement.get(), 23) != SQLITE_INTEGER ||
          sqlite3_column_type(statement.get(), 24) != SQLITE_TEXT ||
          !nullableText(25) ||
          sqlite3_column_type(statement.get(), 26) != SQLITE_TEXT ||
          sqlite3_column_type(statement.get(), 27) != SQLITE_INTEGER) {
        rejectRow();
        continue;
      }
      const sqlite3_int64 outboxId = sqlite3_column_int64(statement.get(), 22);
      const int state = sqlite3_column_int(statement.get(), 23);
      const int localResultReady = sqlite3_column_int(statement.get(), 27);
      const std::string outboxMd5 = readText(statement.get(), 25);
      const std::string outboxSha256 = readText(statement.get(), 26);
      if (outboxId <= 0 || !ir::isKnownIrOutboxState(state) ||
          (localResultReady != 0 && localResultReady != 1) ||
          (!identity->md5.empty() && !outboxMd5.empty() &&
           identity->md5 != outboxMd5) ||
          (!identity->sha256.empty() && identity->sha256 != outboxSha256)) {
        rejectRow();
        continue;
      }
      candidate.outboxRowId = outboxId;
      candidate.outboxState = static_cast<ir::IrOutboxState>(state);
      const auto document = nlohmann::json::parse(
          readText(statement.get(), 24), nullptr, false, false);
      if (document.is_object()) {
        const auto meta = document.find("meta");
        if (meta != document.end() && meta->is_object()) {
          const auto game = meta->find("game");
          const auto playtype = meta->find("playtype");
          if (game != meta->end() && game->is_string() &&
              game->get_ref<const std::string &>() == "bms" &&
              playtype != meta->end() && playtype->is_string()) {
            const auto &value = playtype->get_ref<const std::string &>();
            candidate.keyMode = value == "7K" ? 7 : value == "14K" ? 14 : 0;
          }
        }
      }
    }

    if (candidates.size() >= ir::kMaximumIrRemoteScoreSnapshotEntries) {
      return {.status = ir::IrReconciliationReadOutcome::Status::Invalid,
              .diagnostic =
                  "IR reconciliation candidate collection is oversized"};
    }
    candidates.push_back(std::move(candidate));
  }
  if (rc != SQLITE_DONE) {
    return {.status =
                ir::IrReconciliationReadOutcome::Status::StorageFailure,
            .diagnostic = "IR reconciliation candidate read failed"};
  }
  if (inspected >= scanLimit) {
    return {.status = ir::IrReconciliationReadOutcome::Status::Invalid,
            .diagnostic =
                "IR reconciliation candidate scan budget was exhausted"};
  }
  if (!snapshot.commit(snapshotError)) {
    return {.status =
                ir::IrReconciliationReadOutcome::Status::StorageFailure,
            .diagnostic =
                "could not complete the IR reconciliation read snapshot"};
  }

  std::string diagnostic;
  if (rejected != 0) {
    diagnostic = ir::sanitizeDiagnostic(
        "IR reconciliation candidates: inspected=" +
        std::to_string(inspected) + " rejected=" + std::to_string(rejected));
  }
  return {.status = ir::IrReconciliationReadOutcome::Status::Loaded,
          .candidates = std::move(candidates),
          .diagnostic = std::move(diagnostic)};
}

IrUploadReplayReadOutcome ReplayRepository::ListIrUploadCandidateReplays(
    std::string_view providerId, std::string_view serverOrigin) {
  const auto normalizedOrigin = ir::normalizeServerOrigin(serverOrigin);
  if (!isValidIrProviderId(providerId) || !normalizedOrigin ||
      *normalizedOrigin != serverOrigin) {
    return {.status = IrUploadReplayReadStatus::Invalid,
            .diagnostic = "IR upload replay identity is invalid"};
  }

  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = IrUploadReplayReadStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  return replay_repository_detail::ListIrUploadCandidateReplaysOnConnection(
      impl_->sessionDatabase, providerId, serverOrigin);
}

IrUploadReplayReadOutcome
replay_repository_detail::ListIrUploadCandidateReplaysOnConnection(
    sqlite3 *database, std::string_view providerId,
    std::string_view serverOrigin) {
  std::string snapshotError;
  SqliteTransactionHandle snapshot(database, "BEGIN TRANSACTION",
                                   snapshotError);
  if (!snapshot.active()) {
    return {.status = IrUploadReplayReadStatus::StorageFailure,
            .diagnostic =
                "could not start the IR upload replay read snapshot"};
  }

  constexpr const char *query =
      "SELECT replay.id,replay.chart_path,replay.chart_md5,"
      "replay.chart_sha256,replay.chart_title,replay.chart_artist,"
      "replay.score,replay.max_score,replay.max_combo,replay.final_gauge,"
      "replay.clear_type,replay.created_at,replay.provenance_json,"
      "replay.attempt_id,replay.result_fingerprint,"
      "CAST(json_extract(replay.provenance_json,'$.ruleset.version') AS "
      "INTEGER),CASE json_extract(replay.provenance_json,'$.eligibility') "
      "WHEN 'verified' THEN 0 WHEN 'modified' THEN 1 ELSE 2 END,"
      "outbox.state,outbox.last_error_message FROM chart_results replay "
      "LEFT JOIN ir_outbox outbox ON outbox.provider_id = ? "
      "AND outbox.attempt_id = replay.attempt_id "
      "WHERE replay.attempt_id IS NOT NULL "
      "AND replay.result_fingerprint <> '' "
      "AND NOT EXISTS ("
      "  SELECT 1 FROM ir_submission_receipts receipt "
      "  WHERE receipt.provider_id = ? AND receipt.server_origin = ? "
      "    AND receipt.result_id = replay.id"
      ") "
      "AND (outbox.id IS NULL OR outbox.state = 4) "
      "ORDER BY replay.id DESC "
      "LIMIT 16385";
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK ||
      !bindTextView(statement.get(), 1, providerId) ||
      !bindTextView(statement.get(), 2, providerId) ||
      !bindTextView(statement.get(), 3, serverOrigin)) {
    return {.status = IrUploadReplayReadStatus::StorageFailure,
            .diagnostic = "could not prepare the IR upload replay read"};
  }

  const auto nullableText = [&](int column) {
    const int type = sqlite3_column_type(statement.get(), column);
    return type == SQLITE_NULL || type == SQLITE_TEXT;
  };
  const auto nullableInteger = [&](int column) {
    const int type = sqlite3_column_type(statement.get(), column);
    return type == SQLITE_NULL || type == SQLITE_INTEGER;
  };

  std::vector<ReplaySummary> replays;
  replays.reserve(kMaximumIrUploadCandidateRows);
  std::unordered_set<int> replayIds;
  std::size_t omittedRows = 0;
  std::size_t inspectedRows = 0;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
    ++inspectedRows;
    if (inspectedRows > kMaximumIrUploadCandidateRows) {
      return {.status = IrUploadReplayReadStatus::Invalid,
              .diagnostic = "IR upload replay candidate collection is oversized"};
    }
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER ||
        !nullableText(1) || !nullableText(2) || !nullableText(3) ||
        !nullableText(4) || !nullableText(5) ||
        sqlite3_column_type(statement.get(), 6) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 7) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 8) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 9) != SQLITE_FLOAT ||
        sqlite3_column_type(statement.get(), 10) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 11) != SQLITE_TEXT ||
        sqlite3_column_type(statement.get(), 12) != SQLITE_TEXT ||
        sqlite3_column_type(statement.get(), 13) != SQLITE_TEXT ||
        sqlite3_column_type(statement.get(), 14) != SQLITE_TEXT ||
        sqlite3_column_type(statement.get(), 15) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 16) != SQLITE_INTEGER ||
        !nullableInteger(17) || !nullableText(18)) {
      ++omittedRows;
      continue;
    }

    const sqlite3_int64 storedId = sqlite3_column_int64(statement.get(), 0);
    if (storedId <= 0 || storedId > std::numeric_limits<int>::max()) {
      ++omittedRows;
      continue;
    }
    const int replayId = static_cast<int>(storedId);
    if (!replayIds.emplace(replayId).second) {
      return {.status = IrUploadReplayReadStatus::IntegrityConflict,
              .diagnostic = "IR upload replay candidate joins are not unique"};
    }

    const std::string attemptId = readText(statement.get(), 13);
    const std::string attemptFingerprint = readText(statement.get(), 14);
    std::string provenanceError;
    auto provenance = decodeStoredProvenance(statement.get(), 15, 16, 12,
                                             provenanceError);
    if (!uuid::isCanonicalLowerV4(attemptId) ||
        !isCanonicalLowerHex(attemptFingerprint, 64) || !provenance) {
      ++omittedRows;
      continue;
    }

    ReplaySummary summary;
    summary.id = replayId;
    summary.initialGaugeType = provenance->gaugeType;
    summary.gaugeAutoShift = provenance->gaugeAutoShift;
    summary.finalScore = sqlite3_column_int(statement.get(), 6);
    summary.maxScore = sqlite3_column_int(statement.get(), 7);
    summary.maxCombo = sqlite3_column_int(statement.get(), 8);
    summary.finalGauge =
        static_cast<float>(sqlite3_column_double(statement.get(), 9));
    summary.clearType = sqlite3_column_int(statement.get(), 10);
    summary.createdAt = readText(statement.get(), 11);
    summary.playOption = provenance->player1.option;
    summary.playOptionSeed = provenance->player1.seed;
    summary.playOption2 = provenance->player2.option;
    summary.playOption2Seed = provenance->player2.seed;
    summary.assistOption = provenance->assistOption;
    summary.rulesetVersion = provenance->ruleset.version;
    summary.eligibility = provenance->eligibility;
    summary.chartMeta = bms_parser::ChartMeta{
        .BmsPath = readText(statement.get(), 1),
        .MD5 = readText(statement.get(), 2),
        .SHA256 = readText(statement.get(), 3),
        .Title = readText(statement.get(), 4),
        .Artist = readText(statement.get(), 5),
    };
    summary.playback = provenance->playback;
    summary.provenance =
        std::make_shared<const ScoreProvenance>(std::move(*provenance));
    summary.attemptId = attemptId;
    summary.hasCanonicalAttemptFingerprint = true;
    if (sqlite3_column_type(statement.get(), 17) == SQLITE_INTEGER) {
      const int outboxState = sqlite3_column_int(statement.get(), 17);
      if (!ir::isKnownIrOutboxState(outboxState)) {
        ++omittedRows;
        continue;
      }
      summary.requestedIrOutboxState =
          static_cast<ir::IrOutboxState>(outboxState);
    }
    if (sqlite3_column_type(statement.get(), 18) == SQLITE_TEXT) {
      summary.requestedIrOutboxDiagnostic =
          ir::sanitizeDiagnostic(readText(statement.get(), 18));
    }
    replays.push_back(std::move(summary));
  }
  if (rc != SQLITE_DONE) {
    return {.status = IrUploadReplayReadStatus::StorageFailure,
            .diagnostic = "IR upload replay candidate read failed"};
  }
  if (!snapshot.commit(snapshotError)) {
    return {.status = IrUploadReplayReadStatus::StorageFailure,
            .diagnostic =
                "could not complete the IR upload replay read snapshot"};
  }

  std::string diagnostic;
  if (omittedRows != 0) {
    diagnostic = ir::sanitizeDiagnostic(
        "IR upload replay candidates: omitted=" +
        std::to_string(omittedRows));
  }
  return {.status = IrUploadReplayReadStatus::Loaded,
          .replays = std::move(replays),
          .omittedRows = omittedRows,
          .diagnostic = std::move(diagnostic)};
}

std::vector<ReplaySummary>
ReplayRepository::ListReplays(const bms_parser::ChartMeta &chartMeta, int limit,
                              std::string_view irProviderId,
                              std::string_view irServerOrigin) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {};
  }
  return replay_repository_detail::ListReplaysOnConnection(
      impl_->sessionDatabase, chartMeta, limit, irProviderId, irServerOrigin);
}

std::vector<ReplaySummary> replay_repository_detail::ListReplaysOnConnection(
    sqlite3 *db, const bms_parser::ChartMeta &chartMeta, int limit,
    std::string_view irProviderId, std::string_view irServerOrigin) {
  return ListCompactChartResultsOnConnection(
      db, chartMeta, limit, irProviderId, irServerOrigin);

}

std::vector<ReplaySummary>
ReplayRepository::ListCourseReplays(const CourseReplayLookup &lookup,
                                  int limit) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {};
  }
  return replay_repository_detail::ListCourseReplaysOnConnection(
      impl_->sessionDatabase, lookup, limit);
}

std::vector<ReplaySummary>
replay_repository_detail::ListCourseReplaysOnConnection(
    sqlite3 *db, const CourseReplayLookup &lookup, int limit) {
  return ListCompactCourseResultsOnConnection(db, lookup, limit);

}
