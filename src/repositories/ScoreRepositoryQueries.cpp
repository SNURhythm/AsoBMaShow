#include "ScoreRepository.h"
#include "ScoreRepositoryInternal.h"

#include "../BmsMetadataText.h"
#include "ChartRepository.h"
#include "ChartSqlExpressions.h"
#include "../CoursePlaySession.h"
#include "../LongNoteModeUtils.h"
#include "../ProfileDatabaseActivity.h"
#include "ReplayRepository.h"
#include "../ResultPersistenceModel.h"
#include "ScoreCacheQueries.h"
#include "SqliteRAII.h"
#include "../Utils.h"
#include "../Uuid.h"
#include "../path.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
std::atomic<std::uint64_t> gScoreRevision{1};
bool isCanonicalCourseKey(std::string_view key) {
  constexpr std::string_view prefix = "course:v1:";
  if (!key.starts_with(prefix) || key.size() != prefix.size() + 64) {
    return false;
  }
  return std::ranges::all_of(key.substr(prefix.size()), [](unsigned char ch) {
    return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f');
  });
}

using asobmshow::bms_metadata::normalizedHash;
using asobmshow::chart_sql::boundNormalizedHashMatchCondition;
using asobmshow::chart_sql::boundStoredOrLegacyBmsPathMatchCondition;
using asobmshow::chart_sql::chartSourceArchiveSizeExpr;
using asobmshow::chart_sql::chartSourcePriorityExpr;
using asobmshow::chart_sql::normalizedSqlHash;
using asobmshow::chart_sql::storedOrLegacyBmsPathMatchCondition;

void logSqlErrorText(const char *context, const std::string &error) {
  SDL_Log("SQL error while %s: %s", context, error.c_str());
}

void logSqlError(const char *context, sqlite3 *db) {
  logSqlErrorText(context, sqliteDatabaseError(db));
}

int judgeCount(const RhythmState &state, Judgement judgement) {
  const auto it = state.judgeCount.find(judgement);
  return it == state.judgeCount.end() ? 0 : it->second;
}

bool serializeProvenanceForWrite(const ScoreProvenance &provenance,
                                 const char *scoreKind,
                                 std::string &provenanceJson) {
  std::string provenanceError;
  const auto serialized =
      serializeValidatedScoreProvenance(provenance, provenanceError);
  if (!serialized.has_value()) {
    SDL_Log("Refusing to save %s with invalid provenance: %s", scoreKind,
            provenanceError.c_str());
    return false;
  }
  provenanceJson = *serialized;
  return true;
}

bool bindSqliteTextView(sqlite3_stmt *stmt, int index, std::string_view value) {
  if (value.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  return sqlite3_bind_text(stmt, index, value.data(),
                           static_cast<int>(value.size()),
                           SQLITE_TRANSIENT) == SQLITE_OK;
}

bool isAttemptIdentityUniqueConstraint(int extendedError,
                                       std::string_view diagnostic) {
  return extendedError == SQLITE_CONSTRAINT_UNIQUE &&
         diagnostic == "UNIQUE constraint failed: scores.attempt_id";
}

score_repository_detail::ScoreWriteOutcome insertScoreWriteOnConnectionImpl(
    sqlite3 *db, const result_persistence::ChartScoreWrite &score,
    std::optional<std::string_view> attemptId,
    std::optional<std::string_view> createdAt,
    const std::string &provenanceJson,
    const score_repository_detail::ScoreStorageMetadata &storage) {
  using score_repository_detail::ScoreWriteStatus;
  if (!result_persistence::hasProjectableChartIdentity(score)) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                    "Refusing to save score without a projectable chart "
                    "identity: %s",
                    score.chartPath.c_str());
    std::abort();
  }

  std::string query =
      "INSERT INTO scores ("
      "chart_path, chart_md5, chart_sha256, ln_mode, chart_title, "
      "chart_artist, score, max_score, max_combo, combo_break, pgreat, great, "
      "good, bad, poor, kpoor, fast, slow, final_gauge, clear_type, "
      "ruleset_version, eligibility, provenance_json, attempt_id, "
      "score_source, source_provider_id, source_server_origin, "
      "source_remote_score_id, source_sync_generation";
  if (createdAt.has_value()) {
    query += ", created_at";
  }
  query += ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
           "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?";
  if (createdAt.has_value()) {
    query += ", ?";
  }
  query += ")";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "preparing score insert",
                                    logSqlErrorText)) {
    return {.diagnostic = "could not prepare the score insert"};
  }

  bool bound = true;
  int bindIndex = 1;
  const auto bindText = [&](std::string_view value) {
    const bool result = bindSqliteTextView(stmt.get(), bindIndex++, value);
    bound = bound && result;
  };
  const auto bindInt = [&](int value) {
    const bool result =
        sqlite3_bind_int(stmt.get(), bindIndex++, value) == SQLITE_OK;
    bound = bound && result;
  };

  bindText(score.chartPath);
  bindText(score.chartMd5);
  bindText(score.chartSha256);
  bindInt(score.longNoteMode);
  bindText(score.chartTitle);
  bindText(score.chartArtist);
  bindInt(score.score);
  bindInt(score.maxScore);
  bindInt(score.maxCombo);
  bindInt(score.comboBreak);
  bindInt(score.pGreat);
  bindInt(score.great);
  bindInt(score.good);
  bindInt(score.bad);
  bindInt(score.poor);
  bindInt(score.kPoor);
  bindInt(score.fast);
  bindInt(score.slow);
  bound = bound && sqlite3_bind_double(stmt.get(), bindIndex++,
                                       score.finalGauge) == SQLITE_OK;
  bindInt(score.clearType);
  bindInt(score.provenance.ruleset.version);
  bindInt(static_cast<int>(score.provenance.eligibility));
  bindText(provenanceJson);
  if (attemptId.has_value()) {
    bindText(*attemptId);
  } else {
    bound = bound && sqlite3_bind_null(stmt.get(), bindIndex++) == SQLITE_OK;
  }
  bindInt(static_cast<int>(storage.source));
  const auto bindOptionalText = [&](std::optional<std::string_view> value) {
    if (value.has_value()) {
      bindText(*value);
    } else {
      bound = bound && sqlite3_bind_null(stmt.get(), bindIndex++) == SQLITE_OK;
    }
  };
  bindOptionalText(storage.providerId);
  bindOptionalText(storage.serverOrigin);
  bindOptionalText(storage.remoteScoreId);
  if (storage.syncGeneration.has_value()) {
    bound = bound &&
            sqlite3_bind_int64(stmt.get(), bindIndex++,
                               *storage.syncGeneration) == SQLITE_OK;
  } else {
    bound = bound && sqlite3_bind_null(stmt.get(), bindIndex++) == SQLITE_OK;
  }
  if (createdAt.has_value()) {
    bindText(*createdAt);
  }
  const int expectedBindIndex = createdAt.has_value() ? 31 : 30;
  if (!bound || bindIndex != expectedBindIndex) {
    logSqlError("binding score insert", db);
    return {.diagnostic = "could not bind the score insert"};
  }

  const int rc = sqlite3_step(stmt.get());
  const int extendedError = sqlite3_extended_errcode(db);
  const std::string error = sqliteDatabaseError(db);
  stmt.reset();
  if (rc == SQLITE_DONE) {
    return {.status = ScoreWriteStatus::Inserted};
  }

  logSqlErrorText("saving score", error);
  if (!attemptId.has_value() &&
      storage.source == ScoreStorageSource::LocalGameplay &&
      extendedError == SQLITE_CONSTRAINT_NOTNULL) {
    std::abort();
  }
  if (attemptId.has_value() &&
      isAttemptIdentityUniqueConstraint(extendedError, error)) {
    return {.status = ScoreWriteStatus::AttemptIdentityCollision,
            .diagnostic = error};
  }
  return {.status = ScoreWriteStatus::StorageFailure,
          .diagnostic = "could not save the score: " + error};
}

bool readStrictScoreText(sqlite3_stmt *stmt, int column, std::string &value,
                         bool allowEmpty = true) {
  if (sqlite3_column_type(stmt, column) != SQLITE_TEXT) {
    return false;
  }
  value = sqliteColumnString(stmt, column);
  return allowEmpty || !value.empty();
}

bool readStrictScoreInteger(sqlite3_stmt *stmt, int column, int &value) {
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

result_persistence::ProjectionOutcome classifyProjectedScoreCollision(
    sqlite3 *db, const result_persistence::PendingChartScoreWrite &pending,
    const std::string &expectedProvenanceJson) {
  using result_persistence::ProjectionOutcome;
  using result_persistence::ProjectionStatus;

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db,
          "SELECT attempt_id, chart_path, chart_md5, chart_sha256, ln_mode, "
          "chart_title, chart_artist, score, max_score, max_combo, "
          "combo_break, pgreat, great, good, bad, poor, kpoor, fast, slow, "
          "final_gauge, clear_type, ruleset_version, eligibility, "
          "provenance_json, created_at FROM scores WHERE attempt_id = ?",
          stmt, "preparing projected score collision lookup",
          logSqlErrorText) ||
      !bindSqliteText(stmt.get(), 1, pending.attemptId)) {
    return {.status = ProjectionStatus::StorageFailure,
            .diagnostic = "could not query the projected score collision"};
  }

  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_DONE) {
    return {.status = ProjectionStatus::StorageFailure,
            .diagnostic =
                "score attempt identity collision has no matching row"};
  }
  if (rc != SQLITE_ROW) {
    logSqlError("reading projected score collision", db);
    return {.status = ProjectionStatus::StorageFailure,
            .diagnostic = "could not read the projected score collision"};
  }

  std::string storedAttemptId;
  result_persistence::ChartScoreWrite stored;
  int indexedRulesetVersion = 0;
  int indexedEligibility = 0;
  std::string storedProvenanceJson;
  std::string storedCreatedAt;
  if (!readStrictScoreText(stmt.get(), 0, storedAttemptId, false) ||
      !readStrictScoreText(stmt.get(), 1, stored.chartPath) ||
      !readStrictScoreText(stmt.get(), 2, stored.chartMd5) ||
      !readStrictScoreText(stmt.get(), 3, stored.chartSha256, false) ||
      !readStrictScoreInteger(stmt.get(), 4, stored.longNoteMode) ||
      !readStrictScoreText(stmt.get(), 5, stored.chartTitle) ||
      !readStrictScoreText(stmt.get(), 6, stored.chartArtist) ||
      !readStrictScoreInteger(stmt.get(), 7, stored.score) ||
      !readStrictScoreInteger(stmt.get(), 8, stored.maxScore) ||
      !readStrictScoreInteger(stmt.get(), 9, stored.maxCombo) ||
      !readStrictScoreInteger(stmt.get(), 10, stored.comboBreak) ||
      !readStrictScoreInteger(stmt.get(), 11, stored.pGreat) ||
      !readStrictScoreInteger(stmt.get(), 12, stored.great) ||
      !readStrictScoreInteger(stmt.get(), 13, stored.good) ||
      !readStrictScoreInteger(stmt.get(), 14, stored.bad) ||
      !readStrictScoreInteger(stmt.get(), 15, stored.poor) ||
      !readStrictScoreInteger(stmt.get(), 16, stored.kPoor) ||
      !readStrictScoreInteger(stmt.get(), 17, stored.fast) ||
      !readStrictScoreInteger(stmt.get(), 18, stored.slow) ||
      sqlite3_column_type(stmt.get(), 19) != SQLITE_FLOAT ||
      !readStrictScoreInteger(stmt.get(), 20, stored.clearType) ||
      !readStrictScoreInteger(stmt.get(), 21, indexedRulesetVersion) ||
      !readStrictScoreInteger(stmt.get(), 22, indexedEligibility) ||
      !readStrictScoreText(stmt.get(), 23, storedProvenanceJson, false) ||
      !readStrictScoreText(stmt.get(), 24, storedCreatedAt, false)) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic =
                "stored projected score has invalid SQLite value types"};
  }

  const double storedGauge = sqlite3_column_double(stmt.get(), 19);
  if (!std::isfinite(storedGauge) ||
      storedGauge < -static_cast<double>(std::numeric_limits<float>::max()) ||
      storedGauge > static_cast<double>(std::numeric_limits<float>::max())) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic = "stored projected score gauge is invalid"};
  }
  if (storedGauge != static_cast<double>(pending.score.finalGauge)) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic =
                "stored projected score gauge does not match the attempted "
                "payload exactly"};
  }
  stored.finalGauge = static_cast<float>(storedGauge);

  const int nextRc = sqlite3_step(stmt.get());
  if (nextRc == SQLITE_ROW) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic = "score attempt identity matched multiple rows"};
  }
  if (nextRc != SQLITE_DONE) {
    logSqlError("finishing projected score collision lookup", db);
    return {.status = ProjectionStatus::StorageFailure,
            .diagnostic = "could not finish the projected score lookup"};
  }

  std::string provenanceError;
  auto storedProvenance =
      deserializeScoreProvenance(storedProvenanceJson, provenanceError);
  if (!storedProvenance.has_value()) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic = "stored projected score provenance is malformed"};
  }
  const auto canonicalStoredProvenance =
      serializeValidatedScoreProvenance(*storedProvenance, provenanceError);
  if (!canonicalStoredProvenance.has_value() ||
      *canonicalStoredProvenance != storedProvenanceJson ||
      storedProvenanceJson != expectedProvenanceJson ||
      indexedRulesetVersion != storedProvenance->ruleset.version ||
      indexedEligibility != static_cast<int>(storedProvenance->eligibility)) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic =
                "stored projected score provenance is not canonical or "
                "indexed consistently"};
  }
  stored.provenance = std::move(*storedProvenance);

  if (storedAttemptId != pending.attemptId) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic =
                "stored projected score attempt identity differs"};
  }
  if (storedCreatedAt != pending.createdAt) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic = "stored projected score timestamp differs"};
  }
  const std::string scoreDifference =
      describeChartScoreDifference(pending.score, stored);
  if (!scoreDifference.empty()) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic = scoreDifference};
  }
  return {.status = ProjectionStatus::AlreadyPresent};
}

struct ScoreChartMatch {
  std::string chartPath;
  std::string sha256;
  std::string md5;
};

ScoreChartMatch scoreChartMatchFor(const bms_parser::ChartMeta &chartMeta) {
  return {
      .chartPath = Utils::GetStoragePathUtf8RelativeToDocuments(
          chartMeta.BmsPath, "BMS/"),
      .sha256 = normalizedHash(chartMeta.SHA256),
      .md5 = normalizedHash(chartMeta.MD5),
  };
}

std::string scoreChartMatchPredicate() {
  return "((" + boundNormalizedHashMatchCondition("chart_sha256") + ") OR (" +
         boundNormalizedHashMatchCondition("chart_md5") + ") OR (" +
         boundStoredOrLegacyBmsPathMatchCondition("chart_path") + "))";
}

int bindScoreChartMatch(sqlite3_stmt *stmt, int bindIndex,
                        const ScoreChartMatch &match) {
  bindSqliteText(stmt, bindIndex++, match.sha256);
  bindSqliteText(stmt, bindIndex++, match.sha256);
  bindSqliteText(stmt, bindIndex++, match.md5);
  bindSqliteText(stmt, bindIndex++, match.md5);
  bindSqliteText(stmt, bindIndex++, match.chartPath);
  bindSqliteText(stmt, bindIndex++, match.chartPath);
  bindSqliteText(stmt, bindIndex++, match.chartPath);
  bindSqliteText(stmt, bindIndex++, match.chartPath);
  return bindIndex;
}

std::string courseKeyForSession(const CoursePlaySession &session) {
  std::string key = session.courseKey;
  if (key.empty()) {
    key = course_identity::makeCourseKey(session);
  }
  return key;
}

int scoreLongNoteModeForClearLampValues(int chartLongNoteMode,
                                        int totalLongNotes,
                                        int totalBackSpinNotes,
                                        int selectedLongNoteMode) {
  if (std::max(0, totalLongNotes) + std::max(0, totalBackSpinNotes) <= 0) {
    return 0;
  }
  const int forcedLongNoteMode =
      long_note_mode::normalizeValue(chartLongNoteMode);
  if (forcedLongNoteMode > 0) {
    return forcedLongNoteMode;
  }
  return long_note_mode::normalizeValue(selectedLongNoteMode);
}

void storeBestRank(ScoreRankMap &ranks, const std::string &key, int lnMode,
                   int rank) {
  if (key.empty()) {
    return;
  }
  auto it = ranks.find(key);
  if (it == ranks.end()) {
    it = ranks.emplace(key, ScoreRankByLongNoteMode{}).first;
  }
  const int mode = long_note_mode::normalizeValue(lnMode);
  if (rank > it->second.ranks[static_cast<size_t>(mode)]) {
    it->second.ranks[static_cast<size_t>(mode)] = rank;
  }
}

void storeBestCourseRank(CourseScoreRankByLongNoteMode &ranks, int lnMode,
                         int rank) {
  if (lnMode == -1) {
    ranks.wildcardRank = std::max(ranks.wildcardRank, rank);
    return;
  }
  if (lnMode < 0 || lnMode >= static_cast<int>(ranks.ranks.size())) {
    return;
  }
  auto &stored = ranks.ranks[static_cast<std::size_t>(lnMode)];
  stored = std::max(stored, rank);
}

bool isBetterScoreSnapshot(const ScoreBestSnapshot &candidate,
                           const std::optional<ScoreBestSnapshot> &current) {
  return scoreBestSnapshotIsBetter(candidate, current);
}

void storeBestScore(ScoreBestMap &scores, const std::string &key, int lnMode,
                    const ScoreBestSnapshot &snapshot) {
  if (key.empty()) {
    return;
  }
  auto it = scores.find(key);
  if (it == scores.end()) {
    it = scores.emplace(key, ScoreBestByLongNoteMode{}).first;
  }
  const int mode = long_note_mode::normalizeValue(lnMode);
  auto &current = it->second.snapshots[static_cast<size_t>(mode)];
  if (isBetterScoreSnapshot(snapshot, current)) {
    current = snapshot;
  }
}

std::string bestClearMarkRankExpr(std::string_view alias) {
  return "MAX(" + score_cache_queries::detail::fullComboClearRankExpr(alias) +
         ")";
}

std::string qualifiedScoreTable(std::string_view schema,
                                std::string_view table) {
  return schema.empty() ? std::string(table)
                        : std::string(schema) + "." + std::string(table);
}

void loadBestChartRanks(sqlite3 *db, ScoreClearRankCache &cache,
                        std::string_view schema = {}) {
  const std::string query =
      "SELECT chart_sha256, ln_mode, rank FROM " +
      qualifiedScoreTable(schema, "score_sha256_clear_rank_cache");
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, query, stmt, "loading score clear ranks", logSqlErrorText)) {
    return;
  }

  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const std::string sha256 = sqliteColumnString(stmt.get(), 0);
    const int lnMode = sqlite3_column_int(stmt.get(), 1);
    const int rank = sqlite3_column_int(stmt.get(), 2);
    storeBestRank(cache.rankBySha256, sha256, lnMode, rank);
  }
}

void loadLocalBestChartRanks(sqlite3 *db, ScoreClearRankCache &cache,
                             std::string_view schema = {}) {
  const std::string query =
      "SELECT c.chart_sha256, c.ln_mode, " + bestClearMarkRankExpr("c") +
      " FROM " + qualifiedScoreTable(schema, "scores") +
      " c WHERE " +
      score_cache_queries::detail::scoreParticipatesInBestExpr("c") +
      " AND c.score_source=" +
      std::to_string(static_cast<int>(ScoreStorageSource::LocalGameplay)) +
      " GROUP BY c.chart_sha256, c.ln_mode";
  SqliteStatementHandle statement;
  if (!prepareSqliteStatementLogged(db, query, statement,
                                    "loading local score clear ranks",
                                    logSqlErrorText)) {
    return;
  }
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    storeBestRank(cache.rankBySha256,
                  sqliteColumnString(statement.get(), 0),
                  sqlite3_column_int(statement.get(), 1),
                  sqlite3_column_int(statement.get(), 2));
  }
}

void loadBestChartScores(sqlite3 *db, ScoreBestCache &cache,
                         std::string_view schema = {}) {
  const std::string query =
      "SELECT chart_sha256, ln_mode, score, max_score, max_combo, combo_break, "
      "final_gauge, clear_rank, created_at "
      "FROM " +
      qualifiedScoreTable(schema, "score_sha256_best_score_cache");
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, query, stmt, "loading score best scores", logSqlErrorText)) {
    return;
  }

  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const std::string sha256 = sqliteColumnString(stmt.get(), 0);
    const int lnMode = sqlite3_column_int(stmt.get(), 1);
    ScoreBestSnapshot snapshot;
    snapshot.score = sqlite3_column_int(stmt.get(), 2);
    snapshot.maxScore = sqlite3_column_int(stmt.get(), 3);
    snapshot.maxCombo = sqlite3_column_int(stmt.get(), 4);
    snapshot.comboBreak = sqlite3_column_int(stmt.get(), 5);
    snapshot.finalGauge =
        static_cast<float>(sqlite3_column_double(stmt.get(), 6));
    snapshot.clearType = sqlite3_column_int(stmt.get(), 7);
    snapshot.createdAt = sqliteColumnString(stmt.get(), 8);
    storeBestScore(cache.scoreBySha256, sha256, lnMode, snapshot);
  }
}

void loadBestCourseRanks(sqlite3 *db, ScoreClearRankCache &cache,
                         std::string_view schema = {}) {
  const std::string eligible =
      score_cache_queries::detail::scoreParticipatesInBestExpr("c");
  const std::string query =
      "SELECT COALESCE(course_key, ''), COALESCE(course_id, 0), ln_mode, " +
      bestClearMarkRankExpr("c") + " FROM " +
      qualifiedScoreTable(schema, "course_scores") + " c WHERE " + eligible +
      " GROUP BY COALESCE(course_key, ''), COALESCE(course_id, 0), ln_mode";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "loading course score clear ranks",
                                    logSqlErrorText)) {
    return;
  }

  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const std::string courseKey = sqliteColumnString(stmt.get(), 0);
    const int courseId = sqlite3_column_int(stmt.get(), 1);
    const int lnMode = sqlite3_column_int(stmt.get(), 2);
    const int rank = sqlite3_column_int(stmt.get(), 3);
    if (!courseKey.empty()) {
      storeBestCourseRank(cache.rankByCourseKey[courseKey], lnMode, rank);
    } else if (courseId > 0) {
      storeBestCourseRank(cache.rankByLegacyCourseId[courseId], lnMode, rank);
    }
  }
}

} // namespace

score_repository_detail::ScoreWriteOutcome
score_repository_detail::InsertScoreWriteOnConnection(
    sqlite3 *database, const result_persistence::ChartScoreWrite &score,
    std::optional<std::string_view> attemptId,
    std::optional<std::string_view> createdAt,
    const std::string &provenanceJson, const ScoreStorageMetadata &storage) {
  return insertScoreWriteOnConnectionImpl(database, score, attemptId,
                                          createdAt, provenanceJson, storage);
}

std::size_t
TransparentStringHash::operator()(std::string_view value) const noexcept {
  return std::hash<std::string_view>{}(value);
}

int ScoreRankByLongNoteMode::bestRankForMode(int lnMode) const {
  const int mode = long_note_mode::normalizeValue(lnMode);
  const int rank = ranks[static_cast<size_t>(mode)];
  if (rank != kNoClearTypeRank || mode == long_note_mode::kUnknownValue) {
    return rank;
  }
  const int classicLongNoteRank = ranks[long_note_mode::kLnValue];
  if (classicLongNoteRank != kNoClearTypeRank) {
    return classicLongNoteRank;
  }
  return ranks[0];
}

int CourseScoreRankByLongNoteMode::bestRankForMode(int lnMode) const {
  const int mode = long_note_mode::normalizeValue(lnMode);
  return std::max(ranks[static_cast<std::size_t>(mode)], wildcardRank);
}

std::optional<ScoreBestSnapshot>
ScoreBestByLongNoteMode::bestForMode(int lnMode) const {
  const int mode = long_note_mode::normalizeValue(lnMode);
  const auto &snapshot = snapshots[static_cast<size_t>(mode)];
  if (snapshot.has_value() || mode != 1) {
    return snapshot;
  }
  return snapshots[0];
}

int scoreLongNoteModeForClearLamp(const bms_parser::ChartMeta &chartMeta,
                                  int selectedLongNoteMode) {
  return scoreLongNoteModeForClearLampValues(
      chartMeta.LnMode, chartMeta.TotalLongNotes, chartMeta.TotalBackSpinNotes,
      selectedLongNoteMode);
}

int scoreLongNoteModeForClearLamp(int chartLongNoteMode, int totalLongNotes,
                                  int totalBackSpinNotes,
                                  int selectedLongNoteMode) {
  return scoreLongNoteModeForClearLampValues(chartLongNoteMode, totalLongNotes,
                                             totalBackSpinNotes,
                                             selectedLongNoteMode);
}

int ScoreClearRankCache::bestRankFor(const bms_parser::ChartMeta &chartMeta,
                                     int selectedLongNoteMode) const {
  const int longNoteMode =
      scoreLongNoteModeForClearLamp(chartMeta, selectedLongNoteMode);
  return bestRankForStoredKey(normalizedHash(chartMeta.SHA256), longNoteMode);
}

int ScoreClearRankCache::bestRankForHash(const std::string &sha256,
                                         int longNoteMode) const {
  const std::string normalizedSha = normalizedHash(sha256);
  return bestRankForStoredKey(normalizedSha, longNoteMode);
}

int ScoreClearRankCache::bestRankForStoredKey(std::string_view sha256,
                                              int longNoteMode) const {
  const auto shaIt = rankBySha256.find(sha256);
  return shaIt == rankBySha256.end()
             ? kNoClearTypeRank
             : shaIt->second.bestRankForMode(longNoteMode);
}

int ScoreClearRankCache::bestCourseRankFor(std::string_view courseKey,
                                           int legacyCourseId,
                                           int lnMode) const {
  int rank = kNoClearTypeRank;
  if (!courseKey.empty()) {
    const auto keyIt = rankByCourseKey.find(courseKey);
    if (keyIt != rankByCourseKey.end()) {
      rank = keyIt->second.bestRankForMode(lnMode);
    }
  }
  if (legacyCourseId > 0) {
    const auto idIt = rankByLegacyCourseId.find(legacyCourseId);
    if (idIt != rankByLegacyCourseId.end()) {
      rank = std::max(rank, idIt->second.bestRankForMode(lnMode));
    }
  }
  return rank;
}

std::optional<ScoreBestSnapshot>
ScoreBestCache::bestFor(const bms_parser::ChartMeta &chartMeta,
                        int selectedLongNoteMode) const {
  const int longNoteMode =
      scoreLongNoteModeForClearLamp(chartMeta, selectedLongNoteMode);
  return bestForStoredKey(normalizedHash(chartMeta.SHA256), longNoteMode);
}

std::optional<ScoreBestSnapshot>
ScoreBestCache::bestForHash(const std::string &sha256, int longNoteMode) const {
  const std::string normalizedSha = normalizedHash(sha256);
  return bestForStoredKey(normalizedSha, longNoteMode);
}

std::optional<ScoreBestSnapshot>
ScoreBestCache::bestForStoredKey(std::string_view sha256,
                                 int longNoteMode) const {
  const auto shaIt = scoreBySha256.find(sha256);
  return shaIt == scoreBySha256.end()
             ? std::nullopt
             : shaIt->second.bestForMode(longNoteMode);
}
bool score_repository_detail::InsertCourseScoreOnConnection(
    sqlite3 *db, const CoursePlaySession &session, const RhythmState &state,
    int completedCharts, int totalCharts, const ScoreProvenance &provenance,
    const std::string &provenanceJson) {
  const std::string courseKey = courseKeyForSession(session);
  if (courseKey.empty()) {
    SDL_Log("Refusing to save course score without a durable course key");
    return false;
  }
  const char *query =
      "INSERT INTO course_scores ("
      "course_id, course_key, ln_mode, course_name, course_group_name, "
      "constraint_json,"
      "gauge_type, gauge_profile, gauge_auto_shift, play_option, assist_option,"
      "completed_charts, total_charts,"
      "score, max_score, max_combo, combo_break,"
      "pgreat, great, good, bad, poor, kpoor, fast, slow, final_gauge,"
      "clear_type, ruleset_version, eligibility, provenance_json"
      ") VALUES ("
      "@course_id, @course_key, @ln_mode, @course_name, @course_group_name,"
      "@constraint_json, @gauge_type, @gauge_profile, @gauge_auto_shift,"
      "@play_option, @assist_option, @completed_charts, @total_charts,"
      "@score, @max_score, @max_combo, @combo_break,"
      "@pgreat, @great, @good, @bad, @poor, @kpoor, @fast, @slow,"
      "@final_gauge, @clear_type, @ruleset_version, @eligibility,"
      "@provenance_json"
      ")";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, query, stmt, "preparing course score insert", logSqlErrorText)) {
    return false;
  }

  int courseTotalNotes = 0;
  for (const auto &entry : session.entries) {
    courseTotalNotes += std::max(0, entry.meta.TotalNotes);
  }

  int bindIndex = 1;
  sqlite3_bind_int(stmt.get(), bindIndex++, session.courseId);
  bindSqliteText(stmt.get(), bindIndex++, courseKey);
  sqlite3_bind_int(stmt.get(), bindIndex++,
                   long_note_mode::normalizeValue(session.longNoteMode));
  bindSqliteText(stmt.get(), bindIndex++, session.courseName);
  bindSqliteText(stmt.get(), bindIndex++, session.courseGroupName);
  bindSqliteText(stmt.get(), bindIndex++, session.constraintJson);
  sqlite3_bind_int(stmt.get(), bindIndex++, gaugeTypeIndex(session.gaugeType));
  sqlite3_bind_int(stmt.get(), bindIndex++,
                   static_cast<int>(session.gaugeProfile));
  sqlite3_bind_int(stmt.get(), bindIndex++,
                   gaugeAutoShiftModeValue(session.gaugeAutoShift));
  bindSqliteText(stmt.get(), bindIndex++,
                 session.playOption.value_or(session.requestedPlayOption));
  bindSqliteText(stmt.get(), bindIndex++, session.assistOption);
  sqlite3_bind_int(stmt.get(), bindIndex++, completedCharts);
  sqlite3_bind_int(stmt.get(), bindIndex++, totalCharts);
  sqlite3_bind_int(stmt.get(), bindIndex++, state.getScore());
  sqlite3_bind_int(stmt.get(), bindIndex++, courseTotalNotes * 2);
  sqlite3_bind_int(stmt.get(), bindIndex++, state.maxCombo);
  sqlite3_bind_int(stmt.get(), bindIndex++, state.comboBreak);
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, PGreat));
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, Great));
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, Good));
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, Bad));
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, Poor));
  sqlite3_bind_int(stmt.get(), bindIndex++, judgeCount(state, Kpoor));
  sqlite3_bind_int(stmt.get(), bindIndex++, state.fastCount);
  sqlite3_bind_int(stmt.get(), bindIndex++, state.slowCount);
  sqlite3_bind_double(stmt.get(), bindIndex++, state.currentGauge);
  int clearRank = state.getClearTypeRank();
  const bool fullCombo = completedCharts == totalCharts && totalCharts > 0 &&
                         state.currentGauge > 0.0f && state.comboBreak == 0 &&
                         state.maxCombo >= courseTotalNotes;
  clearRank = clear_policy::fullComboRankForPlayback(clearRank, fullCombo,
                                                     provenance.playback);
  sqlite3_bind_int(stmt.get(), bindIndex++, clearRank);
  sqlite3_bind_int(stmt.get(), bindIndex++, provenance.ruleset.version);
  sqlite3_bind_int(stmt.get(), bindIndex++,
                   static_cast<int>(provenance.eligibility));
  bindSqliteText(stmt.get(), bindIndex++, provenanceJson);

  int rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    logSqlError("saving course score", db);
    return false;
  }
  return true;
}

bool ScoreRepository::SaveScore(const bms_parser::ChartMeta &chartMeta,
                              const RhythmState &state,
                              const ScoreProvenance &provenance) {
  profile_database_activity::WriteGuard writeGuard;
  const result_persistence::ChartScoreWrite score =
      result_persistence::captureChartScoreWrite(
          chartMeta, state, provenance,
          scoreLongNoteModeForClearLamp(chartMeta));
  std::string provenanceJson;
  if (!serializeProvenanceForWrite(score.provenance, "score", provenanceJson)) {
    return false;
  }

  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return false;
  }
  sqlite3 *db = impl_->sessionDatabase;

  const bool result =
      score_repository_detail::InsertScoreWriteOnConnection(
          db, score, std::nullopt, std::nullopt, provenanceJson)
          .status == score_repository_detail::ScoreWriteStatus::Inserted;
  if (result) {
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

result_persistence::ProjectionOutcome ScoreRepository::SaveProjectedScore(
    const result_persistence::PendingChartScoreWrite &pending) {
  using result_persistence::ProjectionOutcome;
  using result_persistence::ProjectionStatus;

  profile_database_activity::WriteGuard writeGuard;
  if (!uuid::isCanonicalLowerV4(pending.attemptId) ||
      !pending.hasExactlyOneOwner()) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic =
                "score projection attempt or owner identity is invalid"};
  }
  if (pending.createdAt.empty()) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic = "score projection timestamp is empty"};
  }
  if (!result_persistence::hasProjectableChartIdentity(pending.score)) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic =
                "score projection chart identity is not projectable"};
  }

  std::string provenanceError;
  const auto provenanceJson = serializeValidatedScoreProvenance(
      pending.score.provenance, provenanceError);
  if (!provenanceJson.has_value()) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic =
                "score projection provenance is invalid: " + provenanceError};
  }

  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = ProjectionStatus::StorageFailure,
            .diagnostic = "score storage is unavailable"};
  }
  sqlite3 *db = impl_->sessionDatabase;

  const score_repository_detail::ScoreWriteOutcome inserted =
      score_repository_detail::InsertScoreWriteOnConnection(
          db, pending.score, pending.attemptId, pending.createdAt,
          *provenanceJson);
  if (inserted.status == score_repository_detail::ScoreWriteStatus::Inserted) {
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
    return {.status = ProjectionStatus::Inserted};
  }
  if (inserted.status ==
      score_repository_detail::ScoreWriteStatus::AttemptIdentityCollision) {
    return classifyProjectedScoreCollision(db, pending, *provenanceJson);
  }
  return {.status = ProjectionStatus::StorageFailure,
          .diagnostic = inserted.diagnostic};
}

bool ScoreRepository::SaveCourseScore(const CoursePlaySession &session,
                                    const RhythmState &state,
                                    int completedCharts, int totalCharts,
                                    const ScoreProvenance &provenance) {
  profile_database_activity::WriteGuard writeGuard;
  if (courseKeyForSession(session).empty()) {
    SDL_Log("Refusing to save course score without a durable course key");
    return false;
  }
  std::lock_guard lock(impl_->sessionMutex);
  std::string provenanceJson;
  if (!serializeProvenanceForWrite(provenance, "course score",
                                   provenanceJson)) {
    return false;
  }

  if (!EnsureSessionDatabaseLocked()) {
    return false;
  }
  sqlite3 *db = impl_->sessionDatabase;

  const bool result = score_repository_detail::InsertCourseScoreOnConnection(
      db, session, state, completedCharts, totalCharts, provenance,
      provenanceJson);
  if (result) {
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

std::optional<ScoreBestSnapshot> ScoreRepository::LoadBestScore(
    const bms_parser::ChartMeta &chartMeta,
    const std::optional<std::string> &beforeCreatedAt,
    const std::optional<std::string> &excludeAttemptId,
    int selectedLongNoteMode) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return std::nullopt;
  }
  return score_repository_detail::LoadBestScoreOnConnection(
      impl_->sessionDatabase, chartMeta, beforeCreatedAt, excludeAttemptId,
      selectedLongNoteMode);
}

std::optional<ScoreBestSnapshot> ScoreRepository::LoadBestScoreForRuleset(
    const bms_parser::ChartMeta &chartMeta,
    const RulesetDescriptor &requiredRuleset, int selectedLongNoteMode) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return std::nullopt;
  }
  return score_repository_detail::LoadBestScoreOnConnection(
      impl_->sessionDatabase, chartMeta, std::nullopt, std::nullopt,
      selectedLongNoteMode, &requiredRuleset);
}

std::optional<ScoreBestSnapshot>
score_repository_detail::LoadBestScoreOnConnection(
    sqlite3 *db, const bms_parser::ChartMeta &chartMeta,
    const std::optional<std::string> &beforeCreatedAt,
    const std::optional<std::string> &excludeAttemptId,
    int selectedLongNoteMode, const RulesetDescriptor *requiredRuleset) {
  const auto match = scoreChartMatchFor(chartMeta);
  const std::string cutoff = beforeCreatedAt.value_or("");
  const int longNoteMode =
      scoreLongNoteModeForClearLamp(chartMeta, selectedLongNoteMode);
  const bool legacyLongNoteModeFallback = longNoteMode == 1;
  const std::string effectiveClearRank =
      score_cache_queries::detail::fullComboClearRankExpr("s", {}, true);
  const auto bestOrder = score_cache_queries::detail::bestScoreOrderKey(
      "s", effectiveClearRank, "id");

  std::string query =
      "SELECT score, max_score, max_combo, combo_break, "
      "CAST(bad AS INTEGER) + CAST(poor AS INTEGER) + "
      "CAST(kpoor AS INTEGER), final_gauge, ";
  query += effectiveClearRank +
           ", created_at, provenance_json, score_source FROM scores s WHERE ";
  query += scoreChartMatchPredicate();
  query += " AND " +
           score_cache_queries::detail::scoreParticipatesInBestExpr("s") +
           " AND (ln_mode = ? OR ln_mode = -1 OR (? != 0 AND ln_mode = 0)) "
           "AND (? = '' OR created_at < ?) ";
  if (excludeAttemptId.has_value()) {
    query += "AND (attempt_id IS NULL OR attempt_id <> ?) ";
  }
  if (requiredRuleset != nullptr) {
    query += "AND ruleset_version = ? ";
  }
  query += "ORDER BY CASE WHEN ln_mode = ? OR ln_mode = -1 THEN 0 ELSE 1 END, " +
           score_cache_queries::detail::bestScoreOrderBySql(bestOrder);

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "loading best score",
                                    logSqlErrorText)) {
    return std::nullopt;
  }

  int bindIndex = 1;
  bindIndex = bindScoreChartMatch(stmt.get(), bindIndex, match);
  sqlite3_bind_int(stmt.get(), bindIndex++, longNoteMode);
  sqlite3_bind_int(stmt.get(), bindIndex++, legacyLongNoteModeFallback ? 1 : 0);
  bindSqliteText(stmt.get(), bindIndex++, cutoff);
  bindSqliteText(stmt.get(), bindIndex++, cutoff);
  if (excludeAttemptId.has_value()) {
    bindSqliteText(stmt.get(), bindIndex++, *excludeAttemptId);
  }
  if (requiredRuleset != nullptr) {
    sqlite3_bind_int(stmt.get(), bindIndex++, requiredRuleset->version);
  }
  sqlite3_bind_int(stmt.get(), bindIndex++, longNoteMode);

  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    if (requiredRuleset != nullptr) {
      std::string provenanceError;
      const auto provenance = deserializeScoreProvenance(
          sqliteColumnString(stmt.get(), 8), provenanceError);
      if (!provenance.has_value() ||
          provenance->ruleset != *requiredRuleset) {
        continue;
      }
    }

    ScoreBestSnapshot snapshot;
    snapshot.score = sqlite3_column_int(stmt.get(), 0);
    snapshot.maxScore = sqlite3_column_int(stmt.get(), 1);
    const bool imported = sqlite3_column_int(stmt.get(), 9) ==
                          static_cast<int>(ScoreStorageSource::ImportedIr);
    if (!imported) {
      snapshot.maxCombo = sqlite3_column_int(stmt.get(), 2);
      snapshot.comboBreak = sqlite3_column_int(stmt.get(), 3);
      if (sqlite3_column_type(stmt.get(), 4) == SQLITE_INTEGER) {
        const sqlite3_int64 badPoints = sqlite3_column_int64(stmt.get(), 4);
        if (badPoints >= 0 && badPoints <= std::numeric_limits<int>::max()) {
          snapshot.badPoints = static_cast<int>(badPoints);
        }
      }
      snapshot.finalGauge =
          static_cast<float>(sqlite3_column_double(stmt.get(), 5));
    }
    snapshot.clearType = sqlite3_column_int(stmt.get(), 6);
    snapshot.createdAt = sqliteColumnString(stmt.get(), 7);
    snapshot.source = imported ? ScoreBestSource::ImportedIr
                               : ScoreBestSource::Local;
    return snapshot;
  }
  return std::nullopt;
}

std::optional<ScoreBestSnapshot>
ScoreRepository::LoadBestCourseScore(const CoursePlaySession &session) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return std::nullopt;
  }
  return score_repository_detail::LoadBestCourseScoreOnConnection(
      impl_->sessionDatabase, session);
}

std::optional<ScoreBestSnapshot>
score_repository_detail::LoadBestCourseScoreOnConnection(
    sqlite3 *db, const CoursePlaySession &session) {
  const std::string courseKey = courseKeyForSession(session);
  const int lnMode = long_note_mode::normalizeValue(session.longNoteMode);
  const std::string query =
      "SELECT score, max_score, max_combo, combo_break, final_gauge,"
      "clear_type, created_at "
      "FROM course_scores c "
      "WHERE ((? != '' AND course_key = ?) OR "
      "(COALESCE(course_key, '') = '' AND course_id = ?)) "
      "AND " +
      score_cache_queries::detail::scoreParticipatesInBestExpr("c") +
      " "
      "AND (ln_mode = ? OR ln_mode = -1) "
      "ORDER BY score DESC, clear_type DESC, created_at DESC, id DESC "
      "LIMIT 1";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, query, stmt, "loading best course score", logSqlErrorText)) {
    return std::nullopt;
  }

  int bindIndex = 1;
  bindSqliteText(stmt.get(), bindIndex++, courseKey);
  bindSqliteText(stmt.get(), bindIndex++, courseKey);
  sqlite3_bind_int(stmt.get(), bindIndex++, session.courseId);
  sqlite3_bind_int(stmt.get(), bindIndex++, lnMode);

  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    return std::nullopt;
  }

  ScoreBestSnapshot snapshot;
  snapshot.score = sqlite3_column_int(stmt.get(), 0);
  snapshot.maxScore = sqlite3_column_int(stmt.get(), 1);
  snapshot.maxCombo = sqlite3_column_int(stmt.get(), 2);
  snapshot.comboBreak = sqlite3_column_int(stmt.get(), 3);
  snapshot.finalGauge =
      static_cast<float>(sqlite3_column_double(stmt.get(), 4));
  snapshot.clearType = sqlite3_column_int(stmt.get(), 5);
  snapshot.createdAt = sqliteColumnString(stmt.get(), 6);
  return snapshot;
}

CourseScoreRecoveryResult ScoreRepository::RecoverCourseRecords(
    std::span<const course_identity::Definition> definitions) {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {.errorMessage = "score database validation failed"};
  }
  return score_repository_detail::RecoverCourseRecordsOnConnection(
      impl_->sessionDatabase, definitions);
}

CourseScoreRecoveryResult
score_repository_detail::RecoverCourseRecordsOnConnection(
    sqlite3 *db, std::span<const course_identity::Definition> definitions) {
  CourseScoreRecoveryResult result;
  if (db == nullptr) {
    result.errorMessage = "score database is unavailable";
    return result;
  }

  const auto definitionBucketKey = [](std::string_view canonicalConstraints,
                                      std::size_t chartCount, bool sha256,
                                      std::string_view firstHash) {
    std::string key = std::to_string(canonicalConstraints.size());
    key.push_back(':');
    key.append(canonicalConstraints);
    key.push_back(':');
    key += std::to_string(chartCount);
    key += sha256 ? ":sha256:" : ":md5:";
    key.append(firstHash);
    return key;
  };
  std::unordered_map<std::string,
                     std::vector<const course_identity::Definition *>>
      definitionBuckets;
  std::unordered_set<std::string> currentKeys;
  for (const auto &definition : definitions) {
    const std::string canonicalConstraints =
        course_identity::canonicalConstraintPayload(definition.constraintJson);
    if (!isCanonicalCourseKey(definition.courseKey) ||
        definition.charts.empty() || canonicalConstraints.empty() ||
        course_identity::makeCourseKey(definition.charts,
                                       definition.constraintJson)
            .empty()) {
      continue;
    }
    currentKeys.insert(definition.courseKey);
    const auto &firstChart = definition.charts.front();
    const std::string firstSha256 = normalizedHash(firstChart.sha256);
    const std::string firstMd5 = normalizedHash(firstChart.md5);
    if (!firstSha256.empty()) {
      definitionBuckets[definitionBucketKey(canonicalConstraints,
                                            definition.charts.size(), true,
                                            firstSha256)]
          .push_back(&definition);
    }
    if (!firstMd5.empty()) {
      definitionBuckets[definitionBucketKey(canonicalConstraints,
                                            definition.charts.size(), false,
                                            firstMd5)]
          .push_back(&definition);
    }
  }

  const bool callerOwnsTransaction = sqlite3_get_autocommit(db) == 0;
  const char *beginQuery = callerOwnsTransaction
                               ? "SAVEPOINT asobmashow_score_course_recovery"
                               : "BEGIN IMMEDIATE TRANSACTION";
  const char *commitQuery = callerOwnsTransaction
                                ? "RELEASE asobmashow_score_course_recovery"
                                : "COMMIT";
  const char *rollbackQuery =
      callerOwnsTransaction
          ? "ROLLBACK TO asobmashow_score_course_recovery; RELEASE "
            "asobmashow_score_course_recovery"
          : "ROLLBACK";
  std::string transactionError;
  SqliteTransactionHandle transaction(db, beginQuery, transactionError,
                                      commitQuery, rollbackQuery);
  if (!transaction.active()) {
    result.errorMessage =
        "could not start score recovery transaction: " + transactionError;
    return result;
  }

  struct StoredCourseScore {
    sqlite3_int64 id = 0;
    int legacyCourseId = 0;
    int totalCharts = 0;
    std::string courseKey;
    std::string legacyCourseKey;
    std::string courseName;
    std::string courseGroupName;
    std::string constraintJson;
  };
  std::vector<StoredCourseScore> rows;
  SqliteStatementHandle selectStmt;
  if (!prepareSqliteStatementLogged(
          db,
          "SELECT id, COALESCE(course_id, 0), COALESCE(course_key, ''), "
          "legacy_course_key, COALESCE(course_name, ''), "
          "COALESCE(course_group_name, ''), COALESCE(constraint_json, ''), "
          "total_charts FROM course_scores ORDER BY id",
          selectStmt, "reading course scores for identity recovery",
          logSqlErrorText)) {
    result.errorMessage = sqliteDatabaseError(db);
    return result;
  }
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(selectStmt.get())) == SQLITE_ROW) {
    rows.push_back({
        .id = sqlite3_column_int64(selectStmt.get(), 0),
        .legacyCourseId = sqlite3_column_int(selectStmt.get(), 1),
        .totalCharts = sqlite3_column_int(selectStmt.get(), 7),
        .courseKey = sqliteColumnString(selectStmt.get(), 2),
        .legacyCourseKey = sqliteColumnString(selectStmt.get(), 3),
        .courseName = sqliteColumnString(selectStmt.get(), 4),
        .courseGroupName = sqliteColumnString(selectStmt.get(), 5),
        .constraintJson = sqliteColumnString(selectStmt.get(), 6),
    });
  }
  if (rc != SQLITE_DONE) {
    result.errorMessage = sqliteDatabaseError(db);
    return result;
  }
  selectStmt.reset();

  struct CourseKeyRewrite {
    sqlite3_int64 id = 0;
    std::string courseKey;
  };
  std::vector<CourseKeyRewrite> rewrites;
  std::unordered_map<std::string, std::optional<std::string>>
      rawEvidenceResolutions;
  const auto resolveRawEvidence = [&](const std::string &rawEvidence) {
    const auto cached = rawEvidenceResolutions.find(rawEvidence);
    if (cached != rawEvidenceResolutions.end()) {
      return cached->second;
    }

    std::optional<std::string> resolvedKey;
    const auto parsed = course_identity::parseLegacyScoreKey(rawEvidence);
    if (parsed.has_value() && !parsed->charts.empty()) {
      const std::string canonicalConstraints =
          course_identity::canonicalConstraintPayload(parsed->constraintJson);
      const auto &firstChart = parsed->charts.front();
      const std::string firstSha256 = normalizedHash(firstChart.sha256);
      const std::string firstMd5 = normalizedHash(firstChart.md5);
      std::unordered_set<const course_identity::Definition *> candidates;
      const auto collectCandidates = [&](bool sha256,
                                         const std::string &firstHash) {
        if (firstHash.empty()) {
          return;
        }
        const auto bucket = definitionBuckets.find(definitionBucketKey(
            canonicalConstraints, parsed->charts.size(), sha256, firstHash));
        if (bucket == definitionBuckets.end()) {
          return;
        }
        candidates.insert(bucket->second.begin(), bucket->second.end());
      };
      collectCandidates(true, firstSha256);
      collectCandidates(false, firstMd5);

      course_identity::Definition legacyDefinition{
          .constraintJson = parsed->constraintJson,
          .charts = parsed->charts,
      };
      std::unordered_set<std::string> matchingKeys;
      for (const auto *definition : candidates) {
        if (course_identity::sameDefinition(legacyDefinition, *definition)) {
          matchingKeys.insert(definition->courseKey);
        }
      }
      if (matchingKeys.size() == 1) {
        resolvedKey = *matchingKeys.begin();
      }
    }
    rawEvidenceResolutions.emplace(rawEvidence, resolvedKey);
    return resolvedKey;
  };
  for (const auto &row : rows) {
    std::string resultingKey = row.courseKey;
    std::string rawEvidence = row.legacyCourseKey;
    if (rawEvidence.empty() && !row.courseKey.empty() &&
        !isCanonicalCourseKey(row.courseKey)) {
      rawEvidence = row.courseKey;
    }

    if (!rawEvidence.empty()) {
      const auto resolvedKey = resolveRawEvidence(rawEvidence);
      if (resolvedKey.has_value() && !resolvedKey->empty()) {
        resultingKey = *resolvedKey;
        if (resultingKey != row.courseKey) {
          rewrites.push_back({.id = row.id, .courseKey = resultingKey});
        }
      }
    }

    if (!currentKeys.contains(resultingKey) || row.totalCharts <= 0) {
      continue;
    }
    const std::string canonicalConstraint =
        course_identity::canonicalConstraintPayload(row.constraintJson);
    if (canonicalConstraint.empty()) {
      continue;
    }
    result.evidence.push_back({
        .legacyCourseId = row.legacyCourseId,
        .totalCharts = row.totalCharts,
        .courseName = row.courseName,
        .courseGroupName = row.courseGroupName,
        .canonicalConstraintPayload = canonicalConstraint,
        .courseKey = resultingKey,
    });
  }

  SqliteStatementHandle updateStmt;
  if (!rewrites.empty() &&
      !prepareSqliteStatementLogged(
          db, "UPDATE course_scores SET course_key = ?1 WHERE id = ?2",
          updateStmt, "preparing course score recovery update",
          logSqlErrorText)) {
    result.errorMessage = sqliteDatabaseError(db);
    result.evidence.clear();
    return result;
  }
  int changedRows = 0;
  for (const auto &rewrite : rewrites) {
    bindSqliteText(updateStmt.get(), 1, rewrite.courseKey);
    sqlite3_bind_int64(updateStmt.get(), 2, rewrite.id);
    if (sqlite3_step(updateStmt.get()) != SQLITE_DONE) {
      result.errorMessage = sqliteDatabaseError(db);
      result.evidence.clear();
      return result;
    }
    changedRows += sqlite3_changes(db);
    sqlite3_reset(updateStmt.get());
    sqlite3_clear_bindings(updateStmt.get());
  }

  std::ranges::sort(result.evidence, [](const CourseScoreEvidence &left,
                                        const CourseScoreEvidence &right) {
    return std::tie(left.legacyCourseId, left.courseName, left.courseGroupName,
                    left.canonicalConstraintPayload, left.totalCharts,
                    left.courseKey) <
           std::tie(right.legacyCourseId, right.courseName,
                    right.courseGroupName, right.canonicalConstraintPayload,
                    right.totalCharts, right.courseKey);
  });
  result.evidence.erase(
      std::unique(result.evidence.begin(), result.evidence.end()),
      result.evidence.end());

  if (!transaction.commit(transactionError)) {
    result.errorMessage =
        "could not commit score recovery transaction: " + transactionError;
    result.evidence.clear();
    return result;
  }
  if (changedRows > 0) {
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

ScoreClearRankCache ScoreRepository::LoadBestClearRanks() {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  ScoreClearRankCache cache;
  if (!EnsureSessionDatabaseLocked()) {
    return cache;
  }
  return score_repository_detail::LoadBestClearRanksOnConnection(
      impl_->sessionDatabase);
}

ScoreClearRankCache ScoreRepository::LoadLocalBestClearRanks() {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  ScoreClearRankCache cache;
  if (!EnsureSessionDatabaseLocked()) {
    return cache;
  }
  return score_repository_detail::LoadLocalBestClearRanksOnConnection(
      impl_->sessionDatabase);
}

ScoreClearRankCache
score_repository_detail::LoadLocalBestClearRanksOnConnection(
    sqlite3 *database, std::string_view schema) {
  profile_database_activity::ReadGuard operation;
  ScoreClearRankCache cache;
  if (database != nullptr) {
    loadLocalBestChartRanks(database, cache, schema);
    loadBestCourseRanks(database, cache, schema);
  }
  return cache;
}

ScoreClearRankCache score_repository_detail::LoadBestClearRanksOnConnection(
    sqlite3 *db, std::string_view schema) {
  profile_database_activity::ReadGuard operation;
  ScoreClearRankCache cache;
  if (db != nullptr) {
    loadBestChartRanks(db, cache, schema);
    loadBestCourseRanks(db, cache, schema);
  }
  return cache;
}

ScoreBestCache ScoreRepository::LoadBestScores() {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  ScoreBestCache cache;
  if (!EnsureSessionDatabaseLocked()) {
    return cache;
  }
  return score_repository_detail::LoadBestScoresOnConnection(
      impl_->sessionDatabase);
}

ScoreBestCache score_repository_detail::LoadBestScoresOnConnection(
    sqlite3 *db, std::string_view schema) {
  profile_database_activity::ReadGuard operation;
  ScoreBestCache cache;
  if (db != nullptr) {
    loadBestChartScores(db, cache, schema);
  }
  return cache;
}

std::uint64_t ScoreRepository::GetRevision() const {
  return gScoreRevision.load(std::memory_order_relaxed);
}

void score_repository_detail::IncrementRevision() {
  gScoreRevision.fetch_add(1, std::memory_order_relaxed);
}
