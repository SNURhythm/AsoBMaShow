#include "ReplayDBHelper.h"

#include "BmsMetadataText.h"
#include "ChartSqlExpressions.h"
#include "LongNoteModeUtils.h"
#include "ProfileDatabaseActivity.h"
#include "SqliteRAII.h"
#include "Utils.h"
#include "path.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
constexpr int kLegacyReplayDatabaseSchemaVersion = 2;
constexpr int kReplayDatabaseSchemaVersion =
    ReplayDBHelper::kCurrentSchemaVersion;
constexpr const char *kLegacyProvenanceJson =
    "{\"schemaVersion\":1,\"ruleset\":{\"version\":0},\"stages\":[],"
    "\"eligibility\":\"legacy-unverified\"}";

using asobmshow::bms_metadata::normalizedHash;
using asobmshow::bms_metadata::trimCopy;
using asobmshow::chart_sql::boundNormalizedHashMatchCondition;
using asobmshow::chart_sql::boundStoredOrLegacyBmsPathMatchCondition;
using asobmshow::chart_sql::normalizedSqlHash;

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

bool hasDurableReplayChartIdentity(const ReplayData &replay) {
  return strictChartIdentity(replay.chartMeta.SHA256, replay.chartMeta.MD5)
      .has_value();
}

std::optional<ScoreProvenance> decodeStoredProvenance(
    sqlite3_stmt *stmt, int rulesetVersionColumn, int eligibilityColumn,
    int provenanceJsonColumn, std::string &error);

enum class StrictStageReadResult { Valid, InvalidRow, SqlError };

constexpr const char *kStrictCourseReplayStageIdentityQuery =
    "SELECT s.stage_index, s.replay_id, r.id, r.chart_md5, r.chart_sha256,"
    "r.ruleset_version, r.eligibility, r.provenance_json "
    "FROM course_replay_stages s "
    "LEFT JOIN replays r ON r.id = s.replay_id "
    "WHERE s.course_replay_id = ? ORDER BY s.stage_index LIMIT ?";

StrictStageReadResult readStrictCourseReplayStageIdentities(
    sqlite3_stmt *stmt, int courseReplayId, int expectedCount,
    std::vector<course_identity::ChartIdentity> &charts, std::string &error) {
  charts.clear();
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  if (sqlite3_bind_int(stmt, 1, courseReplayId) != SQLITE_OK ||
      sqlite3_bind_int(stmt, 2,
                       replay_summary_scan::kMaxCourseStagesPerCandidate + 1) !=
          SQLITE_OK) {
    error = sqliteDatabaseError(sqlite3_db_handle(stmt));
    return StrictStageReadResult::SqlError;
  }

  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const int expectedIndex = static_cast<int>(charts.size());
    if (sqlite3_column_int(stmt, 0) != expectedIndex ||
        charts.size() >= static_cast<std::size_t>(
                             replay_summary_scan::kMaxCourseStagesPerCandidate) ||
        sqlite3_column_type(stmt, 2) == SQLITE_NULL ||
        sqlite3_column_int(stmt, 1) <= 0 ||
        sqlite3_column_int(stmt, 1) != sqlite3_column_int(stmt, 2)) {
      error = "course replay has invalid stage links or indexes";
      return StrictStageReadResult::InvalidRow;
    }
    auto identity = strictChartIdentity(sqliteColumnString(stmt, 4),
                                        sqliteColumnString(stmt, 3));
    if (!identity.has_value()) {
      error = "course replay stage has no valid durable hash identity";
      return StrictStageReadResult::InvalidRow;
    }
    if (!decodeStoredProvenance(stmt, 5, 6, 7, error).has_value()) {
      return StrictStageReadResult::InvalidRow;
    }
    charts.push_back(std::move(*identity));
  }
  if (rc != SQLITE_DONE) {
    error = sqliteDatabaseError(sqlite3_db_handle(stmt));
    return StrictStageReadResult::SqlError;
  }
  if (expectedCount <= 0 ||
      charts.size() != static_cast<std::size_t>(expectedCount)) {
    error = "course replay stage count does not match completed charts";
    return StrictStageReadResult::InvalidRow;
  }
  return StrictStageReadResult::Valid;
}

void logSqlErrorText(const char *context, const std::string &error) {
  SDL_Log("SQL error while %s: %s", context, error.c_str());
}

void logSqlError(const char *context, sqlite3 *db) {
  logSqlErrorText(context, sqliteDatabaseError(db));
}

bool execSql(sqlite3 *db, const char *query, const char *context) {
  return executeSqliteLogged(db, query, context, logSqlErrorText);
}

bool setDatabaseUserVersion(sqlite3 *db, int version) {
  const std::string query =
      "PRAGMA user_version = " + std::to_string(std::max(0, version));
  return execSql(db, query.c_str(), "updating replay database version");
}

bool rejectFutureReplayDatabase(sqlite3 *db) {
  // Validated opens perform the guarded path-level preflight. Schema helpers
  // inspect the owned handle directly instead of snapshotting the same database
  // family again.
  std::string error;
  const auto version = readSqliteUserVersion(db, error);
  if (!version.has_value()) {
    SDL_Log("Refusing replay database with unreadable version: %s",
            error.c_str());
    return true;
  }
  if (*version <= kReplayDatabaseSchemaVersion) {
    return false;
  }
  SDL_Log("Refusing future replay database version %d (supported: %d)",
          *version, kReplayDatabaseSchemaVersion);
  return true;
}

bool ensureTableColumn(sqlite3 *db, const char *tableName,
                       const char *columnName, const char *alterQuery,
                       const char *context) {
  return ensureSqliteTableColumnLogged(db, tableName, columnName, alterQuery,
                                       "reading replay schema", context,
                                       logSqlErrorText);
}

bool normalizeReplayChartIdentityHashes(sqlite3 *db) {
  return updateSqliteColumnWithExpressionLogged(
             db, "replays", "chart_md5", normalizedSqlHash("chart_md5"),
             "normalizing stored replay md5 hashes", logSqlErrorText) &&
         updateSqliteColumnWithExpressionLogged(
             db, "replays", "chart_sha256", normalizedSqlHash("chart_sha256"),
             "normalizing stored replay sha256 hashes", logSqlErrorText);
}

bool backfillReplayMaxCombo(sqlite3 *db) {
  const std::string replayQuery =
      "UPDATE replays "
      "SET max_combo = COALESCE(("
      "SELECT MAX(e.combo) FROM replay_events e WHERE e.replay_id = replays.id"
      "), 0) "
      "WHERE max_combo = 0";
  const std::string courseReplayQuery =
      "UPDATE course_replays "
      "SET max_combo = COALESCE(("
      "SELECT MAX(e.combo) FROM replay_events e "
      "JOIN course_replay_stages s ON s.replay_id = e.replay_id "
      "WHERE s.course_replay_id = course_replays.id"
      "), 0) "
      "WHERE max_combo = 0";
  return execSql(db, replayQuery.c_str(), "backfilling replay max combo") &&
         execSql(db, courseReplayQuery.c_str(),
                 "backfilling course replay max combo");
}

bool ensureReplayProvenanceColumns(sqlite3 *db, const char *tableName) {
  const std::string table(tableName);
  const std::string addRuleset =
      "ALTER TABLE " + table +
      " ADD COLUMN ruleset_version INTEGER NOT NULL DEFAULT 0";
  const std::string addEligibility =
      "ALTER TABLE " + table +
      " ADD COLUMN eligibility INTEGER NOT NULL DEFAULT 2";
  const std::string addProvenance =
      "ALTER TABLE " + table +
      " ADD COLUMN provenance_json TEXT NOT NULL DEFAULT '" +
      kLegacyProvenanceJson + "'";
  return ensureTableColumn(db, tableName, "ruleset_version", addRuleset.c_str(),
                           "adding replay ruleset version column") &&
         ensureTableColumn(db, tableName, "eligibility", addEligibility.c_str(),
                           "adding replay eligibility column") &&
         ensureTableColumn(db, tableName, "provenance_json",
                           addProvenance.c_str(),
                           "adding replay provenance JSON column");
}

bool backfillCompleteCourseReplayKeys(sqlite3 *db) {
  const char *query =
      "SELECT id, COALESCE(constraint_json, ''), completed_charts,"
      "total_charts, ruleset_version, eligibility, provenance_json "
      "FROM course_replays ORDER BY id";
  SqliteStatementHandle rowStmt;
  if (!prepareSqliteStatementLogged(db, query, rowStmt,
                                    "reading course replays for key backfill",
                                    logSqlErrorText)) {
    return false;
  }
  SqliteStatementHandle stageStmt;
  if (!prepareSqliteStatementLogged(
          db, kStrictCourseReplayStageIdentityQuery, stageStmt,
          "preparing course replay key backfill stage scan",
          logSqlErrorText)) {
    return false;
  }

  struct Rewrite {
    sqlite3_int64 id = 0;
    std::string courseKey;
  };
  std::vector<Rewrite> rewrites;
  std::size_t invalidRows = 0;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(rowStmt.get())) == SQLITE_ROW) {
    const int completedCharts = sqlite3_column_int(rowStmt.get(), 2);
    const int totalCharts = sqlite3_column_int(rowStmt.get(), 3);
    if (totalCharts <= 0 ||
        totalCharts > replay_summary_scan::kMaxCourseStagesPerCandidate ||
        completedCharts != totalCharts) {
      ++invalidRows;
      continue;
    }

    std::string provenanceError;
    if (!decodeStoredProvenance(rowStmt.get(), 4, 5, 6, provenanceError)
             .has_value()) {
      ++invalidRows;
      continue;
    }
    const std::string constraintJson = sqliteColumnString(rowStmt.get(), 1);
    if (course_identity::canonicalConstraintPayload(constraintJson).empty()) {
      ++invalidRows;
      continue;
    }

    std::vector<course_identity::ChartIdentity> charts;
    std::string stageError;
    const StrictStageReadResult stageResult =
        readStrictCourseReplayStageIdentities(
            stageStmt.get(), sqlite3_column_int(rowStmt.get(), 0), totalCharts,
            charts, stageError);
    if (stageResult == StrictStageReadResult::SqlError) {
      logSqlErrorText("scanning course replay stages for key backfill",
                      stageError);
      return false;
    }
    if (stageResult == StrictStageReadResult::InvalidRow) {
      ++invalidRows;
      continue;
    }
    std::string courseKey =
        course_identity::makeCourseKey(charts, constraintJson);
    if (courseKey.empty()) {
      ++invalidRows;
      continue;
    }
    rewrites.push_back({.id = sqlite3_column_int64(rowStmt.get(), 0),
                        .courseKey = std::move(courseKey)});
  }
  if (rc != SQLITE_DONE) {
    logSqlError("reading course replays for key backfill", db);
    return false;
  }
  rowStmt.reset();
  stageStmt.reset();

  SqliteStatementHandle updateStmt;
  if (!rewrites.empty() &&
      !prepareSqliteStatementLogged(
          db, "UPDATE course_replays SET course_key = ? WHERE id = ?",
          updateStmt, "preparing course replay key backfill update",
          logSqlErrorText)) {
    return false;
  }
  for (const auto &rewrite : rewrites) {
    bindSqliteText(updateStmt.get(), 1, rewrite.courseKey);
    sqlite3_bind_int64(updateStmt.get(), 2, rewrite.id);
    if (sqlite3_step(updateStmt.get()) != SQLITE_DONE) {
      logSqlError("backfilling course replay key", db);
      return false;
    }
    sqlite3_reset(updateStmt.get());
    sqlite3_clear_bindings(updateStmt.get());
  }
  if (invalidRows > 0) {
    SDL_Log("Course replay key migration: backfilled=%zu unresolved=%zu",
            rewrites.size(), invalidRows);
  }
  return true;
}

bool migrateReplayDatabaseSchema(sqlite3 *db) {
  std::string versionError;
  const auto version = readSqliteUserVersion(db, versionError);
  if (!version.has_value()) {
    logSqlErrorText("reading replay migration version", versionError);
    return false;
  }
  if (*version > kReplayDatabaseSchemaVersion) {
    return false;
  }
  if (*version >= kReplayDatabaseSchemaVersion) {
    return true;
  }

  const bool callerOwnsTransaction = sqlite3_get_autocommit(db) == 0;
  const char *beginQuery = callerOwnsTransaction
                               ? "SAVEPOINT asobmashow_replay_migration"
                               : "BEGIN IMMEDIATE TRANSACTION";
  const char *commitQuery = callerOwnsTransaction
                                ? "RELEASE asobmashow_replay_migration"
                                : "COMMIT";
  const char *rollbackQuery =
      callerOwnsTransaction
          ? "ROLLBACK TO asobmashow_replay_migration; RELEASE "
            "asobmashow_replay_migration"
          : "ROLLBACK";
  std::string transactionError;
  SqliteTransactionHandle transaction(db, beginQuery, transactionError,
                                      commitQuery, rollbackQuery);
  if (!transaction.active()) {
    logSqlErrorText("starting replay database migration", transactionError);
    return false;
  }

  if (*version < 1 && !normalizeReplayChartIdentityHashes(db)) {
    return false;
  }
  if (*version < kLegacyReplayDatabaseSchemaVersion &&
      !backfillReplayMaxCombo(db)) {
    return false;
  }
  if (!ensureReplayProvenanceColumns(db, "replays") ||
      !ensureReplayProvenanceColumns(db, "course_replays")) {
    return false;
  }
  if (*version < 4 &&
      (!ensureTableColumn(
           db, "course_replays", "course_key",
           "ALTER TABLE course_replays ADD COLUMN course_key TEXT NOT NULL "
           "DEFAULT ''",
           "adding course replay content key column") ||
       !backfillCompleteCourseReplayKeys(db) ||
       !execSql(db,
                "CREATE INDEX IF NOT EXISTS idx_course_replays_key_id ON "
                "course_replays(course_key, id)",
                "creating course replay content key index"))) {
    return false;
  }
  if (!setDatabaseUserVersion(db, kReplayDatabaseSchemaVersion)) {
    return false;
  }
  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing replay database migration", transactionError);
    return false;
  }
  return true;
}

void bindOptionalText(sqlite3_stmt *stmt, int idx,
                      const std::optional<std::string> &value) {
  if (value.has_value() && !value->empty()) {
    bindSqliteText(stmt, idx, *value);
  } else {
    sqlite3_bind_null(stmt, idx);
  }
}

void bindOptionalInt64(sqlite3_stmt *stmt, int idx,
                       const std::optional<long long> &value) {
  if (value.has_value()) {
    sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(*value));
  } else {
    sqlite3_bind_null(stmt, idx);
  }
}

struct ReplayChartMatch {
  std::string chartPath;
  std::string sha256;
  std::string md5;
};

ReplayChartMatch replayChartMatchFor(const bms_parser::ChartMeta &chartMeta) {
  return {
      .chartPath = Utils::GetStoragePathUtf8RelativeToDocuments(
          chartMeta.BmsPath, "BMS/"),
      .sha256 = normalizedHash(chartMeta.SHA256),
      .md5 = normalizedHash(chartMeta.MD5),
  };
}

bool hasMatchableReplayChartIdentity(const ReplayData &replay) {
  const ReplayChartMatch match = replayChartMatchFor(replay.chartMeta);
  return !trimCopy(match.chartPath).empty() || !match.sha256.empty() ||
         !match.md5.empty();
}

int bindReplayChartMatch(sqlite3_stmt *stmt, int bindIndex,
                         const ReplayChartMatch &match) {
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

std::string replayChartMatchPredicate(const char *alias) {
  const std::string prefix =
      alias != nullptr && alias[0] != '\0' ? std::string(alias) + "." : "";
  return "((" + boundNormalizedHashMatchCondition(prefix + "chart_sha256") +
         ") OR (" + boundNormalizedHashMatchCondition(prefix + "chart_md5") +
         ") OR (" +
         boundStoredOrLegacyBmsPathMatchCondition(prefix + "chart_path") + "))";
}

std::string readText(sqlite3_stmt *stmt, int idx) {
  return sqliteColumnString(stmt, idx);
}

std::optional<std::string>
serializeRandomValues(const std::vector<int> &values) {
  if (values.empty()) {
    return std::nullopt;
  }
  std::ostringstream output;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      output << ",";
    }
    output << values[i];
  }
  return output.str();
}

std::vector<int> parseRandomValues(const std::string &value) {
  std::vector<int> values;
  std::istringstream input(value);
  std::string token;
  while (std::getline(input, token, ',')) {
    token = trimCopy(token);
    if (token.empty()) {
      continue;
    }
    char *end = nullptr;
    const long parsed = std::strtol(token.c_str(), &end, 10);
    if (end != token.c_str()) {
      values.push_back(static_cast<int>(parsed));
    }
  }
  return values;
}

ReplayEventAction actionFromInt(int value) {
  switch (value) {
  case 1:
    return ReplayEventAction::Release;
  case 2:
    return ReplayEventAction::Miss;
  case 3:
    return ReplayEventAction::Mine;
  case 0:
  default:
    return ReplayEventAction::Press;
  }
}

ReplayTouchAction touchActionFromInt(int value) {
  switch (value) {
  case 0:
    return ReplayTouchAction::Down;
  case 2:
    return ReplayTouchAction::Up;
  case 3:
    return ReplayTouchAction::Cancel;
  case 1:
  default:
    return ReplayTouchAction::Move;
  }
}

GaugeType gaugeTypeFromInt(int value) {
  if (value < 0 || value >= static_cast<int>(kGaugeTypeCount)) {
    return GaugeType::Normal;
  }
  return gaugeTypeAtIndex(value);
}

int gaugeProfileIndex(GaugeProfile profile) {
  return static_cast<int>(profile);
}

GaugeProfile gaugeProfileFromInt(int value) {
  switch (value) {
  case 1:
    return GaugeProfile::CourseDefault;
  case 2:
    return GaugeProfile::Course5Keys;
  case 3:
    return GaugeProfile::Course7Keys;
  case 4:
    return GaugeProfile::Course9Keys;
  case 5:
    return GaugeProfile::Course24Keys;
  case 6:
    return GaugeProfile::CourseLR2;
  case 0:
  default:
    return GaugeProfile::Standard;
  }
}

Judgement judgementFromInt(int value) {
  if (value < 0 || value >= JudgementCount) {
    return None;
  }
  return static_cast<Judgement>(value);
}

bool insertReplayEvent(sqlite3_stmt *stmt, int replayId, int eventIndex,
                       const ReplayEvent &event) {
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);

  int bindIndex = 1;
  sqlite3_bind_int(stmt, bindIndex++, replayId);
  sqlite3_bind_int(stmt, bindIndex++, eventIndex);
  sqlite3_bind_int(stmt, bindIndex++, static_cast<int>(event.action));
  sqlite3_bind_int(stmt, bindIndex++, event.lane);
  sqlite3_bind_int64(stmt, bindIndex++, event.noteTimeMicros);
  sqlite3_bind_int64(stmt, bindIndex++, event.songTimeMicros);
  sqlite3_bind_int64(stmt, bindIndex++, event.judgeTimeMicros);
  sqlite3_bind_int(stmt, bindIndex++, static_cast<int>(event.judgement));
  sqlite3_bind_int64(stmt, bindIndex++, event.diffMicros);
  sqlite3_bind_double(stmt, bindIndex++, event.gauge);
  sqlite3_bind_int(stmt, bindIndex++, gaugeTypeIndex(event.gaugeType));
  sqlite3_bind_int(stmt, bindIndex++, event.combo);
  sqlite3_bind_int(stmt, bindIndex++, event.score);

  return sqlite3_step(stmt) == SQLITE_DONE;
}

bool insertReplayTouchSample(sqlite3_stmt *stmt, int replayId, int sampleIndex,
                             const ReplayTouchSample &sample) {
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);

  int bindIndex = 1;
  sqlite3_bind_int(stmt, bindIndex++, replayId);
  sqlite3_bind_int(stmt, bindIndex++, sampleIndex);
  sqlite3_bind_int(stmt, bindIndex++, static_cast<int>(sample.action));
  sqlite3_bind_int64(stmt, bindIndex++,
                     static_cast<sqlite3_int64>(sample.fingerId));
  sqlite3_bind_int64(stmt, bindIndex++, sample.songTimeMicros);
  sqlite3_bind_double(stmt, bindIndex++, sample.x);
  sqlite3_bind_double(stmt, bindIndex++, sample.y);

  return sqlite3_step(stmt) == SQLITE_DONE;
}

bool insertReplayLaneCoverEvent(sqlite3_stmt *stmt, int replayId,
                                int eventIndex,
                                const ReplayLaneCoverEvent &event) {
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);

  int bindIndex = 1;
  sqlite3_bind_int(stmt, bindIndex++, replayId);
  sqlite3_bind_int(stmt, bindIndex++, eventIndex);
  sqlite3_bind_int64(stmt, bindIndex++, event.songTimeMicros);
  sqlite3_bind_int(stmt, bindIndex++, event.noteStartPositionPercent);
  sqlite3_bind_int(stmt, bindIndex++, event.resetVisibleTimeReference ? 1 : 0);

  return sqlite3_step(stmt) == SQLITE_DONE;
}

ReplaySummary readReplaySummary(sqlite3_stmt *stmt, int maxComboColumn,
                                int eventCountColumn,
                                int touchSampleCountColumn,
                                int rulesetVersionColumn,
                                int eligibilityColumn) {
  ReplaySummary summary;
  summary.id = sqlite3_column_int(stmt, 0);
  summary.initialGaugeType = gaugeTypeFromInt(sqlite3_column_int(stmt, 6));
  summary.gaugeAutoShift = sqlite3_column_int(stmt, 7) != 0;
  summary.finalScore = sqlite3_column_int(stmt, 8);
  summary.finalGauge = static_cast<float>(sqlite3_column_double(stmt, 9));
  summary.clearType = sqlite3_column_int(stmt, 10);
  summary.createdAt = readText(stmt, 11);
  if (sqlite3_column_type(stmt, 12) != SQLITE_NULL) {
    summary.playOption = readText(stmt, 12);
  }
  if (sqlite3_column_type(stmt, 13) != SQLITE_NULL) {
    summary.playOptionSeed = sqlite3_column_int64(stmt, 13);
  }
  if (sqlite3_column_type(stmt, 14) != SQLITE_NULL) {
    summary.playOption2 = readText(stmt, 14);
  }
  if (sqlite3_column_type(stmt, 15) != SQLITE_NULL) {
    summary.playOption2Seed = sqlite3_column_int64(stmt, 15);
  }
  if (sqlite3_column_type(stmt, 16) != SQLITE_NULL) {
    summary.assistOption = assist_options::normalize(readText(stmt, 16));
  }
  summary.maxCombo = sqlite3_column_int(stmt, maxComboColumn);
  summary.eventCount = sqlite3_column_int(stmt, eventCountColumn);
  summary.touchSampleCount = sqlite3_column_int(stmt, touchSampleCountColumn);
  summary.rulesetVersion = sqlite3_column_int(stmt, rulesetVersionColumn);
  const int eligibility = sqlite3_column_int(stmt, eligibilityColumn);
  if (eligibility >= static_cast<int>(ScoreEligibility::Verified) &&
      eligibility <= static_cast<int>(ScoreEligibility::LegacyUnverified)) {
    summary.eligibility = static_cast<ScoreEligibility>(eligibility);
  }
  return summary;
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

std::optional<ScoreProvenance> readStoredProvenance(sqlite3_stmt *stmt,
                                                    int rulesetVersionColumn,
                                                    int eligibilityColumn,
                                                    int provenanceJsonColumn,
                                                    const char *context) {
  std::string error;
  auto provenance =
      decodeStoredProvenance(stmt, rulesetVersionColumn, eligibilityColumn,
                             provenanceJsonColumn, error);
  if (!provenance.has_value()) {
    SDL_Log("Failed to load %s provenance: %s", context, error.c_str());
  }
  return provenance;
}

std::size_t replaySummaryCandidateBudget(int limit) {
  if (limit <= 0) {
    return std::numeric_limits<std::size_t>::max();
  }
  const std::size_t requested = static_cast<std::size_t>(limit);
  constexpr std::size_t allowance =
      replay_summary_scan::kCorruptCandidateAllowance;
  if (requested > std::numeric_limits<std::size_t>::max() - allowance) {
    return std::numeric_limits<std::size_t>::max();
  }
  return requested + allowance;
}

void logReplaySummaryScan(const char *label, std::size_t inspected,
                          std::size_t rejected, bool limited,
                          std::size_t budget, bool budgetReached) {
  if (rejected == 0 && !budgetReached) {
    return;
  }
  if (limited) {
    SDL_Log("%s summary provenance scan: inspected=%zu rejected=%zu "
            "budget=%zu%s",
            label, inspected, rejected, budget,
            budgetReached ? " exhausted" : "");
  } else {
    SDL_Log("%s summary provenance scan: inspected=%zu rejected=%zu "
            "budget=unlimited",
            label, inspected, rejected);
  }
}

std::optional<std::string>
validatedProvenanceJson(const ScoreProvenance &provenance,
                        const char *context) {
  std::string error;
  auto serialized = serializeValidatedScoreProvenance(provenance, error);
  if (!serialized.has_value()) {
    SDL_Log("Refusing to save %s with invalid provenance: %s", context,
            error.c_str());
  }
  return serialized;
}

struct CourseReplayStageDescriptor {
  int stageIndex = 0;
  int replayId = 0;
  long long restMicros = 0;
  bms_parser::ChartMeta chartMeta;
};

constexpr const char *kCourseReplayStageDescriptorQuery =
    "SELECT s.stage_index, s.replay_id, s.rest_micros_after_stage,"
    "r.id, r.chart_path, r.chart_md5, r.chart_sha256, r.chart_title,"
    "r.chart_artist, r.ruleset_version, r.eligibility, r.provenance_json "
    "FROM course_replay_stages s "
    "LEFT JOIN replays r ON r.id = s.replay_id "
    "WHERE s.course_replay_id = ? ORDER BY s.stage_index LIMIT ?";

enum class CourseReplayStageDescriptorReadResult {
  Valid,
  InvalidRow,
  SqlError,
};

CourseReplayStageDescriptorReadResult readCourseReplayStageDescriptors(
    sqlite3_stmt *stmt, int courseReplayId, bool validateProvenance,
    std::vector<CourseReplayStageDescriptor> &stages, std::string &error) {
  stages.clear();
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  if (sqlite3_bind_int(stmt, 1, courseReplayId) != SQLITE_OK ||
      sqlite3_bind_int(stmt, 2,
                       replay_summary_scan::kMaxCourseStagesPerCandidate + 1) !=
          SQLITE_OK) {
    error = sqliteDatabaseError(sqlite3_db_handle(stmt));
    return CourseReplayStageDescriptorReadResult::SqlError;
  }

  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const int expectedIndex = static_cast<int>(stages.size());
    if (sqlite3_column_int(stmt, 0) != expectedIndex) {
      error = "course replay stage indexes are not contiguous";
      return CourseReplayStageDescriptorReadResult::InvalidRow;
    }
    if (stages.size() >=
        static_cast<std::size_t>(
            replay_summary_scan::kMaxCourseStagesPerCandidate)) {
      error = "course replay has too many stages";
      return CourseReplayStageDescriptorReadResult::InvalidRow;
    }
    if (sqlite3_column_type(stmt, 3) == SQLITE_NULL) {
      error = "course replay references a missing stage replay";
      return CourseReplayStageDescriptorReadResult::InvalidRow;
    }
    const int replayId = sqlite3_column_int(stmt, 1);
    if (replayId <= 0 || replayId != sqlite3_column_int(stmt, 3)) {
      error = "course replay has an invalid stage replay id";
      return CourseReplayStageDescriptorReadResult::InvalidRow;
    }

    CourseReplayStageDescriptor stage;
    stage.stageIndex = expectedIndex;
    stage.replayId = replayId;
    stage.restMicros = sqlite3_column_int64(stmt, 2);
    stage.chartMeta.BmsPath = readText(stmt, 4);
    stage.chartMeta.MD5 = readText(stmt, 5);
    stage.chartMeta.SHA256 = readText(stmt, 6);
    stage.chartMeta.Title = readText(stmt, 7);
    stage.chartMeta.Artist = readText(stmt, 8);
    if (trimCopy(stage.chartMeta.BmsPath.string()).empty() &&
        normalizedHash(stage.chartMeta.MD5).empty() &&
        normalizedHash(stage.chartMeta.SHA256).empty()) {
      error = "course replay stage has no matchable chart identity";
      return CourseReplayStageDescriptorReadResult::InvalidRow;
    }
    if (validateProvenance &&
        !decodeStoredProvenance(stmt, 9, 10, 11, error).has_value()) {
      return CourseReplayStageDescriptorReadResult::InvalidRow;
    }
    stages.push_back(std::move(stage));
  }
  if (rc != SQLITE_DONE) {
    error = sqliteDatabaseError(sqlite3_db_handle(stmt));
    return CourseReplayStageDescriptorReadResult::SqlError;
  }
  if (stages.empty()) {
    error = "course replay has no linked stages";
    return CourseReplayStageDescriptorReadResult::InvalidRow;
  }
  return CourseReplayStageDescriptorReadResult::Valid;
}

CourseReplayStageDescriptorReadResult courseReplayStageProvenanceStatus(
    sqlite3_stmt *stmt, int courseReplayId, std::string &error) {
  std::vector<CourseReplayStageDescriptor> stages;
  return readCourseReplayStageDescriptors(stmt, courseReplayId, true, stages,
                                          error);
}

std::optional<int> insertReplayRows(sqlite3 *db, const ReplayData &replay,
                                    const std::string &provenanceJson) {
  const char *replayInsert =
      "INSERT INTO replays ("
      "chart_path, chart_md5, chart_sha256, chart_title, chart_artist,"
      "gauge_type, gauge_auto_shift, final_score, max_combo, final_gauge,"
      "clear_type,"
      "random_seed, random_prng, random_values, play_option, play_option_seed,"
      "play_option2, play_option2_seed, assist_option, ln_mode,"
      "ruleset_version, eligibility, provenance_json"
      ") VALUES ("
      "@chart_path, @chart_md5, @chart_sha256, @chart_title, @chart_artist,"
      "@gauge_type, @gauge_auto_shift, @final_score, @max_combo,"
      "@final_gauge, @clear_type, @random_seed, @random_prng, @random_values,"
      "@play_option, @play_option_seed, @play_option2, @play_option2_seed,"
      "@assist_option, @ln_mode, @ruleset_version, @eligibility,"
      "@provenance_json"
      ")";

  SqliteStatementHandle replayStmt;
  if (!prepareSqliteStatementLogged(db, replayInsert, replayStmt,
                                    "preparing replay insert",
                                    logSqlErrorText)) {
    return std::nullopt;
  }

  const auto chartPath = Utils::GetStoragePathUtf8RelativeToDocuments(
      replay.chartMeta.BmsPath, "BMS/");
  int bindIndex = 1;
  bindSqliteText(replayStmt.get(), bindIndex++, chartPath);
  bindSqliteText(replayStmt.get(), bindIndex++,
                 normalizedHash(replay.chartMeta.MD5));
  bindSqliteText(replayStmt.get(), bindIndex++,
                 normalizedHash(replay.chartMeta.SHA256));
  bindSqliteText(replayStmt.get(), bindIndex++, replay.chartMeta.Title);
  bindSqliteText(replayStmt.get(), bindIndex++, replay.chartMeta.Artist);
  sqlite3_bind_int(replayStmt.get(), bindIndex++,
                   gaugeTypeIndex(replay.initialGaugeType));
  sqlite3_bind_int(replayStmt.get(), bindIndex++,
                   replay.gaugeAutoShift ? 1 : 0);
  sqlite3_bind_int(replayStmt.get(), bindIndex++, replay.finalScore);
  sqlite3_bind_int(replayStmt.get(), bindIndex++, std::max(0, replay.maxCombo));
  sqlite3_bind_double(replayStmt.get(), bindIndex++, replay.finalGauge);
  sqlite3_bind_int(replayStmt.get(), bindIndex++, replay.clearType);
  if (replay.randomSeed.has_value()) {
    sqlite3_bind_int64(replayStmt.get(), bindIndex++,
                       static_cast<sqlite3_int64>(*replay.randomSeed));
  } else {
    sqlite3_bind_null(replayStmt.get(), bindIndex++);
  }
  if (replay.randomPrng.has_value()) {
    bindSqliteText(replayStmt.get(), bindIndex++, *replay.randomPrng);
  } else if (replay.randomSeed.has_value()) {
    bindSqliteText(replayStmt.get(), bindIndex++,
                   bms_parser::Parser::RandomPrngId);
  } else {
    sqlite3_bind_null(replayStmt.get(), bindIndex++);
  }
  bindOptionalText(replayStmt.get(), bindIndex++,
                   serializeRandomValues(replay.randomValues));
  bindOptionalText(replayStmt.get(), bindIndex++, replay.playOption);
  bindOptionalInt64(replayStmt.get(), bindIndex++, replay.playOptionSeed);
  bindOptionalText(replayStmt.get(), bindIndex++, replay.playOption2);
  bindOptionalInt64(replayStmt.get(), bindIndex++, replay.playOption2Seed);
  bindSqliteText(replayStmt.get(), bindIndex++,
                 assist_options::normalize(replay.assistOption));
  sqlite3_bind_int(replayStmt.get(), bindIndex++,
                   long_note_mode::normalizeValue(replay.chartMeta.LnMode));
  sqlite3_bind_int(replayStmt.get(), bindIndex++,
                   replay.provenance.ruleset.version);
  sqlite3_bind_int(replayStmt.get(), bindIndex++,
                   static_cast<int>(replay.provenance.eligibility));
  bindSqliteText(replayStmt.get(), bindIndex++, provenanceJson);

  int rc = sqlite3_step(replayStmt.get());
  replayStmt.reset();
  if (rc != SQLITE_DONE) {
    logSqlError("saving replay", db);
    return std::nullopt;
  }

  const int replayId = static_cast<int>(sqlite3_last_insert_rowid(db));
  const char *eventInsert =
      "INSERT INTO replay_events ("
      "replay_id, event_index, action, lane, note_time_micros,"
      "song_time_micros, judge_time_micros, judgement, diff_micros,"
      "gauge, gauge_type, combo, score"
      ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

  SqliteStatementHandle eventStmt;
  if (!prepareSqliteStatementLogged(db, eventInsert, eventStmt,
                                    "preparing replay event insert",
                                    logSqlErrorText)) {
    return std::nullopt;
  }

  for (size_t i = 0; i < replay.events.size(); ++i) {
    if (!insertReplayEvent(eventStmt.get(), replayId, static_cast<int>(i),
                           replay.events[i])) {
      logSqlError("saving replay event", db);
      return std::nullopt;
    }
  }
  eventStmt.reset();

  const char *touchSampleInsert =
      "INSERT INTO replay_touch_samples ("
      "replay_id, sample_index, action, finger_id, song_time_micros, x, y"
      ") VALUES (?, ?, ?, ?, ?, ?, ?)";

  SqliteStatementHandle touchSampleStmt;
  if (!prepareSqliteStatementLogged(db, touchSampleInsert, touchSampleStmt,
                                    "preparing replay touch sample insert",
                                    logSqlErrorText)) {
    return std::nullopt;
  }

  for (size_t i = 0; i < replay.touchSamples.size(); ++i) {
    if (!insertReplayTouchSample(touchSampleStmt.get(), replayId,
                                 static_cast<int>(i), replay.touchSamples[i])) {
      logSqlError("saving replay touch sample", db);
      return std::nullopt;
    }
  }
  touchSampleStmt.reset();

  const char *laneCoverEventInsert =
      "INSERT INTO replay_lane_cover_events ("
      "replay_id, event_index, song_time_micros,"
      "note_start_position_percent, reset_visible_time_reference"
      ") VALUES (?, ?, ?, ?, ?)";

  SqliteStatementHandle laneCoverEventStmt;
  if (!prepareSqliteStatementLogged(
          db, laneCoverEventInsert, laneCoverEventStmt,
          "preparing replay lane cover event insert", logSqlErrorText)) {
    return std::nullopt;
  }

  for (size_t i = 0; i < replay.laneCoverEvents.size(); ++i) {
    if (!insertReplayLaneCoverEvent(laneCoverEventStmt.get(), replayId,
                                    static_cast<int>(i),
                                    replay.laneCoverEvents[i])) {
      logSqlError("saving replay lane cover event", db);
      return std::nullopt;
    }
  }
  return replayId;
}

std::filesystem::path resolvedReplayDatabasePath(
    const std::filesystem::path &databasePath) {
  return databasePath.empty() ? Utils::GetDocumentsPath("db") / "replay.db"
                              : databasePath;
}

std::filesystem::path normalizedReplayDatabasePath(
    const std::filesystem::path &databasePath) {
  std::filesystem::path resolved = resolvedReplayDatabasePath(databasePath);
  std::error_code error;
  const std::filesystem::path absolute =
      std::filesystem::absolute(resolved, error);
  if (!error) {
    resolved = absolute;
  }
  error.clear();
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(resolved, error);
  return (error ? resolved : canonical).lexically_normal();
}

bool equivalentReplayDatabasePaths(const std::filesystem::path &first,
                                   const std::filesystem::path &second) {
  const std::filesystem::path firstResolved =
      resolvedReplayDatabasePath(first);
  const std::filesystem::path secondResolved =
      resolvedReplayDatabasePath(second);
  std::error_code firstExistsError;
  std::error_code secondExistsError;
  const bool firstExists =
      std::filesystem::exists(firstResolved, firstExistsError);
  const bool secondExists =
      std::filesystem::exists(secondResolved, secondExistsError);
  if (!firstExistsError && !secondExistsError && firstExists && secondExists) {
    std::error_code equivalentError;
    const bool equivalent = std::filesystem::equivalent(
        firstResolved, secondResolved, equivalentError);
    if (!equivalentError) {
      return equivalent;
    }
  }
  return normalizedReplayDatabasePath(first) ==
         normalizedReplayDatabasePath(second);
}

sqlite3 *openReplayDatabase(const std::filesystem::path &path,
                            std::string &errorMessage) {
  const std::filesystem::path directory = path.parent_path();
  std::error_code directoryError;
  if (!directory.empty() &&
      !Utils::EnsureDirectoryExists(directory, directoryError)) {
    errorMessage = "can't create replay database directory " +
                   fspath_to_utf8(directory) + ": " +
                   directoryError.message();
    return nullptr;
  }

  return openValidatedSqliteDatabase(path, kReplayDatabaseSchemaVersion, true,
                                     errorMessage);
}

sqlite3 *openTrustedReplayDatabase(const std::filesystem::path &path,
                                   std::string &errorMessage) {
  const std::string pathText = fspath_to_utf8(path);
  sqlite3 *rawDatabase = nullptr;
  const int openResult = sqlite3_open_v2(
      pathText.c_str(), &rawDatabase,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_PRIVATECACHE, nullptr);
  SqliteConnectionHandle database(rawDatabase);
  if (openResult != SQLITE_OK || !database) {
    errorMessage = rawDatabase != nullptr ? sqlite3_errmsg(rawDatabase)
                                          : "could not open database";
    return nullptr;
  }

  sqlite3_busy_timeout(database.get(), 1000);
  if (!configureSqliteNoCheckpoint(database.get(), {}, errorMessage)) {
    return nullptr;
  }
  std::string versionError;
  const auto version = readSqliteUserVersion(database.get(), versionError);
  if (!version.has_value() || *version != kReplayDatabaseSchemaVersion) {
    errorMessage = version.has_value()
                       ? "trusted replay database schema version changed"
                       : "could not read trusted replay database version: " +
                             versionError;
    return nullptr;
  }
  SqliteStatementHandle journalMode;
  if (prepareSqliteStatement(database.get(), "PRAGMA journal_mode",
                             journalMode) != SQLITE_OK ||
      sqlite3_step(journalMode.get()) != SQLITE_ROW ||
      sqliteColumnString(journalMode.get(), 0) != "wal") {
    errorMessage = "trusted replay database is not in WAL mode";
    return nullptr;
  }
  int foreignKeysEnabled = 0;
  if (sqlite3_db_config(database.get(), SQLITE_DBCONFIG_ENABLE_FKEY, 1,
                        &foreignKeysEnabled) != SQLITE_OK ||
      foreignKeysEnabled != 1) {
    errorMessage = "could not enable foreign keys";
    return nullptr;
  }
  return database.release();
}
} // namespace

ReplayDBHelper &ReplayDBHelper::GetInstance() {
  static ReplayDBHelper instance;
  return instance;
}

ReplayDBHelper::ReplayDBHelper(std::filesystem::path databasePath)
    : databasePath_(std::move(databasePath)) {}

ReplayDBHelper::~ReplayDBHelper() {
  std::lock_guard lock(sessionMutex_);
  ShutdownLocked();
}

void ReplayDBHelper::SetDatabasePath(std::filesystem::path databasePath) {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(sessionMutex_);
  if (!equivalentReplayDatabasePaths(databasePath_, databasePath)) {
    ShutdownLocked();
  }
  databasePath_ = std::move(databasePath);
}

std::filesystem::path ReplayDBHelper::GetDatabasePath() const {
  std::lock_guard lock(sessionMutex_);
  return databasePath_;
}

std::filesystem::path ReplayDBHelper::GetResolvedDatabasePath() const {
  std::lock_guard lock(sessionMutex_);
  return GetResolvedDatabasePathLocked();
}

std::filesystem::path ReplayDBHelper::GetResolvedDatabasePathLocked() const {
  return resolvedReplayDatabasePath(databasePath_);
}

bool ReplayDBHelper::BindDatabasePath(std::filesystem::path databasePath,
                                      std::string &errorMessage) {
  profile_database_activity::WriteGuard operation;
  if (databasePath.empty()) {
    errorMessage = "replay database path is empty";
    return false;
  }

  std::lock_guard lock(sessionMutex_);
  if (sessionDatabase_ != nullptr &&
      sqlite3_get_autocommit(sessionDatabase_) == 0) {
    SDL_Log("Discarding replay database with an unfinished transaction");
    ShutdownLocked();
  }
  if (sessionDatabase_ != nullptr &&
      equivalentReplayDatabasePaths(databasePath_, databasePath)) {
    databasePath_ = std::move(databasePath);
    errorMessage.clear();
    return true;
  }

  const std::filesystem::path resolvedPath =
      resolvedReplayDatabasePath(databasePath);
  std::string openError;
  SqliteConnectionHandle candidate(openReplayDatabase(resolvedPath, openError));
  if (!candidate) {
    SDL_Log("Refusing to bind replay database %s: %s",
            fspath_to_utf8(resolvedPath).c_str(), openError.c_str());
    errorMessage = "replay database validation failed";
    return false;
  }
  if (!CreateReplayTablesOnConnection(candidate.get())) {
    errorMessage = "replay database validation failed";
    return false;
  }

  sqlite3 *oldDatabase = sessionDatabase_;
  sessionDatabase_ = candidate.release();
  databasePath_ = std::move(databasePath);
  closeSqliteDatabase(oldDatabase);
  errorMessage.clear();
  return true;
}

bool ReplayDBHelper::HasActiveReads() {
  return profile_database_activity::readsActive();
}

bool ReplayDBHelper::HasActiveWrites() {
  return profile_database_activity::writesActive();
}

void ReplayDBHelper::Shutdown() {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(sessionMutex_);
  ShutdownLocked();
}

void ReplayDBHelper::ShutdownLocked() {
  sqlite3 *database = sessionDatabase_;
  sessionDatabase_ = nullptr;
  closeSqliteDatabase(database);
}

bool ReplayDBHelper::EnsureSessionDatabaseLocked() {
  if (sessionDatabase_ != nullptr) {
    if (sqlite3_get_autocommit(sessionDatabase_) != 0) {
      return true;
    }
    SDL_Log("Discarding replay database with an unfinished transaction");
    ShutdownLocked();
  }

  const std::filesystem::path path = GetResolvedDatabasePathLocked();
  std::string openError;
  SqliteConnectionHandle candidate(openReplayDatabase(path, openError));
  if (!candidate) {
    SDL_Log("Refusing to open replay database %s: %s",
            fspath_to_utf8(path).c_str(), openError.c_str());
    return false;
  }
  if (!CreateReplayTablesOnConnection(candidate.get())) {
    return false;
  }
  sessionDatabase_ = candidate.release();
  return true;
}

sqlite3 *ReplayDBHelper::Connect() {
  if (this == &GetInstance()) {
    SDL_Log("Raw replay connections are unavailable on the runtime singleton");
    return nullptr;
  }
  std::filesystem::path path;
  bool trustedSession = false;
  {
    std::lock_guard lock(sessionMutex_);
    path = GetResolvedDatabasePathLocked();
    trustedSession = sessionDatabase_ != nullptr;
  }
  std::string openError;
  sqlite3 *db = trustedSession ? openTrustedReplayDatabase(path, openError)
                               : openReplayDatabase(path, openError);
  if (db == nullptr) {
    SDL_Log("Refusing to open replay database %s: %s",
            fspath_to_utf8(path).c_str(), openError.c_str());
    return nullptr;
  }
  return db;
}

void ReplayDBHelper::Close(sqlite3 *db) { closeSqliteDatabase(db); }

bool ReplayDBHelper::CreateReplayTables(sqlite3 *db) {
  profile_database_activity::WriteGuard operation;
  return CreateReplayTablesOnConnection(db);
}

bool ReplayDBHelper::CreateReplayTablesOnConnection(sqlite3 *db) {
  if (db == nullptr || rejectFutureReplayDatabase(db)) {
    return false;
  }
  const char *replayQuery = "CREATE TABLE IF NOT EXISTS replays ("
                            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "chart_path TEXT,"
                            "chart_md5 TEXT,"
                            "chart_sha256 TEXT,"
                            "chart_title TEXT,"
                            "chart_artist TEXT,"
                            "ln_mode INTEGER NOT NULL DEFAULT 0,"
                            "gauge_type INTEGER NOT NULL,"
                            "gauge_auto_shift INTEGER NOT NULL,"
                            "final_score INTEGER NOT NULL,"
                            "max_combo INTEGER NOT NULL DEFAULT 0,"
                            "final_gauge REAL NOT NULL,"
                            "clear_type INTEGER NOT NULL,"
                            "random_seed INTEGER,"
                            "random_prng TEXT,"
                            "random_values TEXT,"
                            "play_option TEXT,"
                            "play_option_seed INTEGER,"
                            "play_option2 TEXT,"
                            "play_option2_seed INTEGER,"
                            "assist_option TEXT,"
                            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                            ")";
  if (!execSql(db, replayQuery, "creating replay table")) {
    return false;
  }
  struct ColumnMigration {
    const char *columnName;
    const char *alterQuery;
    const char *context;
  };
  const ColumnMigration replayColumnMigrations[] = {
      {"chart_path", "ALTER TABLE replays ADD COLUMN chart_path TEXT",
       "adding replay chart path column"},
      {"chart_md5", "ALTER TABLE replays ADD COLUMN chart_md5 TEXT",
       "adding replay chart md5 column"},
      {"chart_sha256", "ALTER TABLE replays ADD COLUMN chart_sha256 TEXT",
       "adding replay chart sha256 column"},
      {"random_seed", "ALTER TABLE replays ADD COLUMN random_seed INTEGER",
       "adding replay random seed column"},
      {"random_prng", "ALTER TABLE replays ADD COLUMN random_prng TEXT",
       "adding replay random PRNG column"},
      {"random_values", "ALTER TABLE replays ADD COLUMN random_values TEXT",
       "adding replay random values column"},
      {"play_option", "ALTER TABLE replays ADD COLUMN play_option TEXT",
       "adding replay play option column"},
      {"play_option_seed",
       "ALTER TABLE replays ADD COLUMN play_option_seed INTEGER",
       "adding replay play option seed column"},
      {"play_option2", "ALTER TABLE replays ADD COLUMN play_option2 TEXT",
       "adding replay 2P play option column"},
      {"play_option2_seed",
       "ALTER TABLE replays ADD COLUMN play_option2_seed INTEGER",
       "adding replay 2P play option seed column"},
      {"assist_option", "ALTER TABLE replays ADD COLUMN assist_option TEXT",
       "adding replay assist option column"},
      {"ln_mode",
       "ALTER TABLE replays ADD COLUMN ln_mode INTEGER NOT NULL DEFAULT 0",
       "adding replay long note mode column"},
      {"max_combo",
       "ALTER TABLE replays ADD COLUMN max_combo INTEGER NOT NULL DEFAULT 0",
       "adding replay max combo column"},
  };
  for (const ColumnMigration &migration : replayColumnMigrations) {
    if (!ensureTableColumn(db, "replays", migration.columnName,
                           migration.alterQuery, migration.context)) {
      return false;
    }
  }
  const char *eventQuery =
      "CREATE TABLE IF NOT EXISTS replay_events ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "replay_id INTEGER NOT NULL,"
      "event_index INTEGER NOT NULL,"
      "action INTEGER NOT NULL,"
      "lane INTEGER NOT NULL,"
      "note_time_micros INTEGER NOT NULL,"
      "song_time_micros INTEGER NOT NULL,"
      "judge_time_micros INTEGER NOT NULL,"
      "judgement INTEGER NOT NULL,"
      "diff_micros INTEGER NOT NULL,"
      "gauge REAL NOT NULL,"
      "gauge_type INTEGER NOT NULL,"
      "combo INTEGER NOT NULL,"
      "score INTEGER NOT NULL,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE"
      ")";
  if (!execSql(db, eventQuery, "creating replay event table")) {
    return false;
  }

  const char *touchSampleQuery =
      "CREATE TABLE IF NOT EXISTS replay_touch_samples ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "replay_id INTEGER NOT NULL,"
      "sample_index INTEGER NOT NULL,"
      "action INTEGER NOT NULL,"
      "finger_id INTEGER NOT NULL,"
      "song_time_micros INTEGER NOT NULL,"
      "x REAL NOT NULL,"
      "y REAL NOT NULL,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE"
      ")";
  if (!execSql(db, touchSampleQuery, "creating replay touch sample table")) {
    return false;
  }

  const char *laneCoverEventQuery =
      "CREATE TABLE IF NOT EXISTS replay_lane_cover_events ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "replay_id INTEGER NOT NULL,"
      "event_index INTEGER NOT NULL,"
      "song_time_micros INTEGER NOT NULL,"
      "note_start_position_percent INTEGER NOT NULL,"
      "reset_visible_time_reference INTEGER NOT NULL,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE"
      ")";
  if (!execSql(db, laneCoverEventQuery,
               "creating replay lane cover event table")) {
    return false;
  }

  const char *courseReplayQuery =
      "CREATE TABLE IF NOT EXISTS course_replays ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "course_id INTEGER NOT NULL,"
      "course_key TEXT NOT NULL DEFAULT '',"
      "course_name TEXT,"
      "course_group_name TEXT,"
      "constraint_json TEXT,"
      "gauge_type INTEGER NOT NULL,"
      "gauge_profile INTEGER NOT NULL DEFAULT 0,"
      "gauge_auto_shift INTEGER NOT NULL,"
      "ln_mode INTEGER NOT NULL DEFAULT 0,"
      "requested_play_option TEXT,"
      "assist_option TEXT,"
      "final_score INTEGER NOT NULL,"
      "max_combo INTEGER NOT NULL DEFAULT 0,"
      "final_gauge REAL NOT NULL,"
      "clear_type INTEGER NOT NULL,"
      "completed_charts INTEGER NOT NULL,"
      "total_charts INTEGER NOT NULL,"
      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
      ")";
  if (!execSql(db, courseReplayQuery, "creating course replay table")) {
    return false;
  }
  const ColumnMigration courseReplayColumnMigrations[] = {
      {"max_combo",
       "ALTER TABLE course_replays ADD COLUMN max_combo INTEGER NOT NULL "
       "DEFAULT 0",
       "adding course replay max combo column"},
  };
  for (const ColumnMigration &migration : courseReplayColumnMigrations) {
    if (!ensureTableColumn(db, "course_replays", migration.columnName,
                           migration.alterQuery, migration.context)) {
      return false;
    }
  }

  const char *courseReplayStageQuery =
      "CREATE TABLE IF NOT EXISTS course_replay_stages ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "course_replay_id INTEGER NOT NULL,"
      "stage_index INTEGER NOT NULL,"
      "replay_id INTEGER NOT NULL,"
      "rest_micros_after_stage INTEGER NOT NULL DEFAULT 0,"
      "FOREIGN KEY(course_replay_id) REFERENCES course_replays(id) "
      "ON DELETE CASCADE,"
      "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE"
      ")";
  if (!execSql(db, courseReplayStageQuery,
               "creating course replay stage table")) {
    return false;
  }

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_replays_chart_sha256 ON "
      "replays(chart_sha256, id)",
      "CREATE INDEX IF NOT EXISTS idx_replays_chart_md5 ON "
      "replays(chart_md5, id)",
      "CREATE INDEX IF NOT EXISTS idx_replays_chart_path ON "
      "replays(chart_path, id)",
      "CREATE INDEX IF NOT EXISTS idx_replay_events_replay_order ON "
      "replay_events(replay_id, event_index)",
      "CREATE INDEX IF NOT EXISTS idx_replay_touch_samples_replay_order ON "
      "replay_touch_samples(replay_id, sample_index)",
      "CREATE INDEX IF NOT EXISTS idx_replay_lane_cover_events_replay_order ON "
      "replay_lane_cover_events(replay_id, event_index)",
      "CREATE INDEX IF NOT EXISTS idx_course_replays_course ON "
      "course_replays(course_id, id)",
      "CREATE INDEX IF NOT EXISTS idx_course_replay_stages_course_order ON "
      "course_replay_stages(course_replay_id, stage_index)",
      "CREATE INDEX IF NOT EXISTS idx_course_replay_stages_replay ON "
      "course_replay_stages(replay_id)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating replay index")) {
      return false;
    }
  }
  if (!migrateReplayDatabaseSchema(db)) {
    return false;
  }
  return execSql(db,
                 "CREATE INDEX IF NOT EXISTS idx_course_replays_key_id ON "
                 "course_replays(course_key, id)",
                 "ensuring course replay content key index");
}

bool ReplayDBHelper::EnsureSchema() {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(sessionMutex_);
  return EnsureSessionDatabaseLocked();
}

std::optional<int> ReplayDBHelper::SaveReplay(const ReplayData &replay) {
  profile_database_activity::WriteGuard writeGuard;
  if (!hasMatchableReplayChartIdentity(replay)) {
    SDL_Log("Refusing to save replay without a matchable chart identity");
    return std::nullopt;
  }
  const auto provenanceJson =
      validatedProvenanceJson(replay.provenance, "replay");
  if (!provenanceJson.has_value()) {
    return std::nullopt;
  }
  std::lock_guard lock(sessionMutex_);
  if (!EnsureSessionDatabaseLocked()) {
    return std::nullopt;
  }
  return SaveReplayOnConnection(sessionDatabase_, replay, *provenanceJson);
}

std::optional<int> ReplayDBHelper::SaveReplayOnConnection(
    sqlite3 *db, const ReplayData &replay, const std::string &provenanceJson) {
  std::string transactionError;
  SqliteTransactionHandle transaction(db, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    logSqlErrorText("starting replay save", transactionError);
    return std::nullopt;
  }

  const auto replayId = insertReplayRows(db, replay, provenanceJson);
  if (!replayId.has_value()) {
    return std::nullopt;
  }

  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing replay save", transactionError);
    return std::nullopt;
  }

  return *replayId;
}

std::optional<int>
ReplayDBHelper::SaveCourseReplay(const CourseReplayData &replay) {
  profile_database_activity::WriteGuard writeGuard;
  if (!isCanonicalCourseKey(replay.courseKey) || replay.totalCharts <= 0 ||
      replay.totalCharts >
          replay_summary_scan::kMaxCourseStagesPerCandidate ||
      replay.completedCharts <= 0 ||
      replay.completedCharts > replay.totalCharts ||
      replay.stages.size() !=
          static_cast<std::size_t>(replay.completedCharts)) {
    SDL_Log("Refusing to save course replay with invalid key or stage counts");
    return std::nullopt;
  }
  if (std::any_of(replay.stages.begin(), replay.stages.end(),
                  [](const CourseReplayStageData &stage) {
                    return !hasDurableReplayChartIdentity(stage.replay);
                  })) {
    SDL_Log("Refusing to save course replay without durable stage hashes");
    return std::nullopt;
  }

  const auto courseProvenanceJson =
      validatedProvenanceJson(replay.provenance, "course replay");
  if (!courseProvenanceJson.has_value()) {
    return std::nullopt;
  }
  std::vector<std::string> stageProvenanceJson;
  stageProvenanceJson.reserve(replay.stages.size());
  for (const auto &stage : replay.stages) {
    auto serialized =
        validatedProvenanceJson(stage.replay.provenance, "course replay stage");
    if (!serialized.has_value()) {
      return std::nullopt;
    }
    stageProvenanceJson.push_back(std::move(*serialized));
  }

  std::lock_guard lock(sessionMutex_);
  if (!EnsureSessionDatabaseLocked()) {
    return std::nullopt;
  }
  return SaveCourseReplayOnConnection(sessionDatabase_, replay,
                                      *courseProvenanceJson,
                                      stageProvenanceJson);
}

std::optional<int> ReplayDBHelper::SaveCourseReplayOnConnection(
    sqlite3 *db, const CourseReplayData &replay,
    const std::string &courseProvenanceJson,
    const std::vector<std::string> &stageProvenanceJson) {
  std::string transactionError;
  SqliteTransactionHandle transaction(db, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    logSqlErrorText("starting course replay save", transactionError);
    return std::nullopt;
  }

  const char *courseInsert =
      "INSERT INTO course_replays ("
      "course_id, course_key, course_name, course_group_name, constraint_json,"
      "gauge_type, gauge_profile, gauge_auto_shift, ln_mode,"
      "requested_play_option, assist_option, final_score, max_combo, "
      "final_gauge,"
      "clear_type, completed_charts, total_charts, ruleset_version,"
      "eligibility, provenance_json"
      ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

  SqliteStatementHandle courseStmt;
  if (!prepareSqliteStatementLogged(db, courseInsert, courseStmt,
                                    "preparing course replay insert",
                                    logSqlErrorText)) {
    return std::nullopt;
  }

  int bindIndex = 1;
  sqlite3_bind_int(courseStmt.get(), bindIndex++, replay.courseId);
  bindSqliteText(courseStmt.get(), bindIndex++, replay.courseKey);
  bindSqliteText(courseStmt.get(), bindIndex++, replay.courseName);
  bindSqliteText(courseStmt.get(), bindIndex++, replay.courseGroupName);
  bindSqliteText(courseStmt.get(), bindIndex++, replay.constraintJson);
  sqlite3_bind_int(courseStmt.get(), bindIndex++,
                   gaugeTypeIndex(replay.initialGaugeType));
  sqlite3_bind_int(courseStmt.get(), bindIndex++,
                   gaugeProfileIndex(replay.gaugeProfile));
  sqlite3_bind_int(courseStmt.get(), bindIndex++,
                   replay.gaugeAutoShift ? 1 : 0);
  sqlite3_bind_int(courseStmt.get(), bindIndex++,
                   long_note_mode::normalizeValue(replay.longNoteMode));
  bindSqliteText(courseStmt.get(), bindIndex++, replay.requestedPlayOption);
  bindSqliteText(courseStmt.get(), bindIndex++,
                 assist_options::normalize(replay.assistOption));
  sqlite3_bind_int(courseStmt.get(), bindIndex++, replay.finalScore);
  sqlite3_bind_int(courseStmt.get(), bindIndex++, std::max(0, replay.maxCombo));
  sqlite3_bind_double(courseStmt.get(), bindIndex++, replay.finalGauge);
  sqlite3_bind_int(courseStmt.get(), bindIndex++, replay.clearType);
  sqlite3_bind_int(courseStmt.get(), bindIndex++, replay.completedCharts);
  sqlite3_bind_int(courseStmt.get(), bindIndex++, replay.totalCharts);
  sqlite3_bind_int(courseStmt.get(), bindIndex++,
                   replay.provenance.ruleset.version);
  sqlite3_bind_int(courseStmt.get(), bindIndex++,
                   static_cast<int>(replay.provenance.eligibility));
  bindSqliteText(courseStmt.get(), bindIndex++, courseProvenanceJson);

  int rc = sqlite3_step(courseStmt.get());
  courseStmt.reset();
  if (rc != SQLITE_DONE) {
    logSqlError("saving course replay", db);
    return std::nullopt;
  }

  const int courseReplayId = static_cast<int>(sqlite3_last_insert_rowid(db));
  const char *stageInsert =
      "INSERT INTO course_replay_stages ("
      "course_replay_id, stage_index, replay_id, rest_micros_after_stage"
      ") VALUES (?, ?, ?, ?)";

  SqliteStatementHandle stageStmt;
  if (!prepareSqliteStatementLogged(db, stageInsert, stageStmt,
                                    "preparing course replay stage insert",
                                    logSqlErrorText)) {
    return std::nullopt;
  }

  for (size_t i = 0; i < replay.stages.size(); ++i) {
    auto stageReplayId =
        insertReplayRows(db, replay.stages[i].replay, stageProvenanceJson[i]);
    if (!stageReplayId.has_value()) {
      return std::nullopt;
    }

    sqlite3_reset(stageStmt.get());
    sqlite3_clear_bindings(stageStmt.get());
    sqlite3_bind_int(stageStmt.get(), 1, courseReplayId);
    sqlite3_bind_int(stageStmt.get(), 2, static_cast<int>(i));
    sqlite3_bind_int(stageStmt.get(), 3, *stageReplayId);
    sqlite3_bind_int64(stageStmt.get(), 4,
                       std::max(0LL, replay.stages[i].restMicrosAfterStage));
    if (sqlite3_step(stageStmt.get()) != SQLITE_DONE) {
      logSqlError("saving course replay stage", db);
      return std::nullopt;
    }
  }

  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing course replay save", transactionError);
    return std::nullopt;
  }
  return courseReplayId;
}

std::vector<ReplaySummary>
ReplayDBHelper::ListReplays(const bms_parser::ChartMeta &chartMeta, int limit) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(sessionMutex_);
  if (!EnsureSessionDatabaseLocked()) {
    return {};
  }
  return ListReplaysOnConnection(sessionDatabase_, chartMeta, limit);
}

std::vector<ReplaySummary> ReplayDBHelper::ListReplaysOnConnection(
    sqlite3 *db, const bms_parser::ChartMeta &chartMeta, int limit) {
  std::vector<ReplaySummary> replays;

  std::string snapshotError;
  SqliteTransactionHandle readSnapshot(db, "BEGIN TRANSACTION", snapshotError);
  if (!readSnapshot.active()) {
    logSqlErrorText("starting replay summary snapshot", snapshotError);
    return replays;
  }

  const auto match = replayChartMatchFor(chartMeta);
  const bool hasLimit = limit > 0;
  const std::size_t requestedCount =
      hasLimit ? static_cast<std::size_t>(limit)
               : std::numeric_limits<std::size_t>::max();
  const std::size_t candidateBudget = replaySummaryCandidateBudget(limit);
  const int maxScore = std::max(0, chartMeta.TotalNotes) * 2;

  std::string candidateQuery =
      "SELECT r.id, r.ruleset_version, r.eligibility, r.provenance_json "
      "FROM replays r WHERE ";
  candidateQuery += replayChartMatchPredicate("r");
  candidateQuery +=
      " AND NOT EXISTS ("
      "SELECT 1 FROM course_replay_stages crs WHERE crs.replay_id = r.id"
      ") AND r.id < ? ORDER BY r.id DESC LIMIT ?";
  SqliteStatementHandle candidateStmt;
  if (!prepareSqliteStatementLogged(db, candidateQuery, candidateStmt,
                                    "preparing replay provenance scan",
                                    logSqlErrorText)) {
    return replays;
  }

  std::vector<int> validIds;
  sqlite3_int64 beforeId = std::numeric_limits<sqlite3_int64>::max();
  std::size_t inspected = 0;
  std::size_t rejected = 0;
  bool reachedEnd = false;
  bool scanFailed = false;
  while (validIds.size() < requestedCount && inspected < candidateBudget) {
    const std::size_t chunkSize = std::min<std::size_t>(
        replay_summary_scan::kChunkSize, candidateBudget - inspected);
    sqlite3_reset(candidateStmt.get());
    sqlite3_clear_bindings(candidateStmt.get());
    int bindIndex = bindReplayChartMatch(candidateStmt.get(), 1, match);
    sqlite3_bind_int64(candidateStmt.get(), bindIndex++, beforeId);
    sqlite3_bind_int(candidateStmt.get(), bindIndex++,
                     static_cast<int>(chunkSize));

    std::size_t rowsInChunk = 0;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(candidateStmt.get())) == SQLITE_ROW) {
      ++rowsInChunk;
      ++inspected;
      beforeId = sqlite3_column_int64(candidateStmt.get(), 0);
      std::string provenanceError;
      if (!decodeStoredProvenance(candidateStmt.get(), 1, 2, 3, provenanceError)
               .has_value()) {
        ++rejected;
        continue;
      }
      validIds.push_back(sqlite3_column_int(candidateStmt.get(), 0));
      if (validIds.size() >= requestedCount) {
        break;
      }
    }
    if (validIds.size() >= requestedCount) {
      break;
    }
    if (rc != SQLITE_DONE) {
      logSqlError("scanning replay provenance", db);
      scanFailed = true;
      break;
    }
    if (rowsInChunk < chunkSize) {
      reachedEnd = true;
      break;
    }
  }
  candidateStmt.reset();

  const bool budgetReached = hasLimit && validIds.size() < requestedCount &&
                             !reachedEnd && inspected >= candidateBudget;
  logReplaySummaryScan("Replay", inspected, rejected, hasLimit, candidateBudget,
                       budgetReached);
  if (scanFailed) {
    return replays;
  }

  const char *detailQuery =
      "SELECT r.id, r.chart_path, r.chart_md5, r.chart_sha256,"
      "r.chart_title, r.chart_artist, r.gauge_type, r.gauge_auto_shift,"
      "r.final_score, r.final_gauge, r.clear_type, r.created_at,"
      "r.play_option, r.play_option_seed, r.play_option2,"
      "r.play_option2_seed, r.assist_option, r.max_combo,"
      "(SELECT COUNT(*) FROM replay_events e WHERE e.replay_id = r.id),"
      "(SELECT COUNT(*) FROM replay_touch_samples t WHERE t.replay_id = r.id),"
      "r.ruleset_version, r.eligibility "
      "FROM replays r WHERE r.id = ?";
  SqliteStatementHandle detailStmt;
  if (!prepareSqliteStatementLogged(db, detailQuery, detailStmt,
                                    "preparing replay list", logSqlErrorText)) {
    return replays;
  }

  replays.reserve(validIds.size());
  for (const int replayId : validIds) {
    sqlite3_reset(detailStmt.get());
    sqlite3_clear_bindings(detailStmt.get());
    sqlite3_bind_int(detailStmt.get(), 1, replayId);
    const int rc = sqlite3_step(detailStmt.get());
    if (rc == SQLITE_DONE) {
      continue;
    }
    if (rc != SQLITE_ROW) {
      logSqlError("loading replay summary details", db);
      return {};
    }
    ReplaySummary summary =
        readReplaySummary(detailStmt.get(), 17, 18, 19, 20, 21);
    summary.maxScore = maxScore;
    summary.chartMeta = chartMeta;
    replays.push_back(std::move(summary));
  }
  detailStmt.reset();
  if (!readSnapshot.commit(snapshotError)) {
    logSqlErrorText("committing replay summary snapshot", snapshotError);
    return {};
  }
  return replays;
}

std::vector<ReplaySummary>
ReplayDBHelper::ListCourseReplays(const CourseReplayLookup &lookup,
                                  int limit) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(sessionMutex_);
  if (!EnsureSessionDatabaseLocked()) {
    return {};
  }
  return ListCourseReplaysOnConnection(sessionDatabase_, lookup, limit);
}

std::vector<ReplaySummary> ReplayDBHelper::ListCourseReplaysOnConnection(
    sqlite3 *db, const CourseReplayLookup &lookup, int limit) {
  std::vector<ReplaySummary> replays;
  if (lookup.courseKey.empty() && lookup.legacyCourseId <= 0) {
    return replays;
  }

  std::string snapshotError;
  SqliteTransactionHandle readSnapshot(db, "BEGIN TRANSACTION", snapshotError);
  if (!readSnapshot.active()) {
    logSqlErrorText("starting course replay summary snapshot", snapshotError);
    return replays;
  }

  const bool hasLimit = limit > 0;
  const std::size_t requestedCount =
      hasLimit ? static_cast<std::size_t>(limit)
               : std::numeric_limits<std::size_t>::max();
  const std::size_t candidateBudget = replaySummaryCandidateBudget(limit);
  const char *candidateQuery =
      "SELECT cr.id, cr.ruleset_version, cr.eligibility, cr.provenance_json "
      "FROM course_replays cr "
      "WHERE (((? <> '' AND cr.course_key = ?) OR "
      "(cr.course_key = '' AND ? > 0 AND cr.course_id = ?)) "
      "AND (?5 IS NULL OR cr.id < ?5)) "
      "ORDER BY cr.id DESC LIMIT ?6";
  SqliteStatementHandle candidateStmt;
  if (!prepareSqliteStatementLogged(db, candidateQuery, candidateStmt,
                                    "preparing course replay provenance scan",
                                    logSqlErrorText)) {
    return replays;
  }

  SqliteStatementHandle stageProvenanceStmt;
  if (!prepareSqliteStatementLogged(
          db, kCourseReplayStageDescriptorQuery, stageProvenanceStmt,
          "preparing course replay stage provenance scan", logSqlErrorText)) {
    return replays;
  }

  std::vector<int> validIds;
  sqlite3_int64 beforeId = std::numeric_limits<sqlite3_int64>::max();
  bool firstPage = true;
  std::size_t inspected = 0;
  std::size_t rejected = 0;
  bool reachedEnd = false;
  bool scanFailed = false;
  while (validIds.size() < requestedCount && inspected < candidateBudget) {
    const std::size_t chunkSize = std::min<std::size_t>(
        replay_summary_scan::kChunkSize, candidateBudget - inspected);
    sqlite3_reset(candidateStmt.get());
    sqlite3_clear_bindings(candidateStmt.get());
    bindSqliteText(candidateStmt.get(), 1, lookup.courseKey);
    bindSqliteText(candidateStmt.get(), 2, lookup.courseKey);
    sqlite3_bind_int(candidateStmt.get(), 3, lookup.legacyCourseId);
    sqlite3_bind_int(candidateStmt.get(), 4, lookup.legacyCourseId);
    if (firstPage) {
      sqlite3_bind_null(candidateStmt.get(), 5);
    } else {
      sqlite3_bind_int64(candidateStmt.get(), 5, beforeId);
    }
    sqlite3_bind_int(candidateStmt.get(), 6, static_cast<int>(chunkSize));
    firstPage = false;

    std::size_t rowsInChunk = 0;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(candidateStmt.get())) == SQLITE_ROW) {
      ++rowsInChunk;
      ++inspected;
      beforeId = sqlite3_column_int64(candidateStmt.get(), 0);
      std::string provenanceError;
      const bool aggregateValid =
          decodeStoredProvenance(candidateStmt.get(), 1, 2, 3, provenanceError)
              .has_value();
      const CourseReplayStageDescriptorReadResult stageResult =
          aggregateValid
              ? courseReplayStageProvenanceStatus(
                    stageProvenanceStmt.get(),
                    sqlite3_column_int(candidateStmt.get(), 0), provenanceError)
              : CourseReplayStageDescriptorReadResult::InvalidRow;
      if (stageResult == CourseReplayStageDescriptorReadResult::SqlError) {
        logSqlErrorText("scanning course replay stage provenance",
                        provenanceError);
        scanFailed = true;
        break;
      }
      if (!aggregateValid ||
          stageResult == CourseReplayStageDescriptorReadResult::InvalidRow) {
        ++rejected;
        continue;
      }
      validIds.push_back(sqlite3_column_int(candidateStmt.get(), 0));
      if (validIds.size() >= requestedCount) {
        break;
      }
    }
    if (scanFailed) {
      break;
    }
    if (validIds.size() >= requestedCount) {
      break;
    }
    if (rc != SQLITE_DONE) {
      logSqlError("scanning course replay provenance", db);
      scanFailed = true;
      break;
    }
    if (rowsInChunk < chunkSize) {
      reachedEnd = true;
      break;
    }
  }
  candidateStmt.reset();
  stageProvenanceStmt.reset();

  const bool budgetReached = hasLimit && validIds.size() < requestedCount &&
                             !reachedEnd && inspected >= candidateBudget;
  logReplaySummaryScan("Course replay", inspected, rejected, hasLimit,
                       candidateBudget, budgetReached);
  if (scanFailed) {
    return replays;
  }

  const char *detailQuery =
      "SELECT cr.id, cr.gauge_type, cr.gauge_auto_shift, cr.final_score,"
      "cr.final_gauge, cr.clear_type, cr.created_at,"
      "cr.requested_play_option, cr.assist_option, cr.completed_charts,"
      "cr.total_charts,"
      "(SELECT COUNT(*) FROM course_replay_stages s "
      "WHERE s.course_replay_id = cr.id),"
      "cr.max_combo,"
      "(SELECT COUNT(*) FROM replay_events e "
      "JOIN course_replay_stages s ON s.replay_id = e.replay_id "
      "WHERE s.course_replay_id = cr.id),"
      "(SELECT COUNT(*) FROM replay_touch_samples t "
      "JOIN course_replay_stages s ON s.replay_id = t.replay_id "
      "WHERE s.course_replay_id = cr.id),"
      "cr.ruleset_version, cr.eligibility "
      "FROM course_replays cr "
      "WHERE cr.id = ?";

  SqliteStatementHandle detailStmt;
  if (!prepareSqliteStatementLogged(db, detailQuery, detailStmt,
                                    "preparing course replay list",
                                    logSqlErrorText)) {
    return replays;
  }

  replays.reserve(validIds.size());
  for (const int replayId : validIds) {
    sqlite3_reset(detailStmt.get());
    sqlite3_clear_bindings(detailStmt.get());
    sqlite3_bind_int(detailStmt.get(), 1, replayId);
    const int rc = sqlite3_step(detailStmt.get());
    if (rc == SQLITE_DONE) {
      continue;
    }
    if (rc != SQLITE_ROW) {
      logSqlError("loading course replay summary details", db);
      return {};
    }
    ReplaySummary summary;
    summary.id = sqlite3_column_int(detailStmt.get(), 0);
    summary.courseReplay = true;
    summary.initialGaugeType =
        gaugeTypeFromInt(sqlite3_column_int(detailStmt.get(), 1));
    summary.gaugeAutoShift = sqlite3_column_int(detailStmt.get(), 2) != 0;
    summary.finalScore = sqlite3_column_int(detailStmt.get(), 3);
    summary.finalGauge =
        static_cast<float>(sqlite3_column_double(detailStmt.get(), 4));
    summary.clearType = sqlite3_column_int(detailStmt.get(), 5);
    summary.createdAt = readText(detailStmt.get(), 6);
    if (sqlite3_column_type(detailStmt.get(), 7) != SQLITE_NULL) {
      summary.playOption = readText(detailStmt.get(), 7);
    }
    if (sqlite3_column_type(detailStmt.get(), 8) != SQLITE_NULL) {
      summary.assistOption =
          assist_options::normalize(readText(detailStmt.get(), 8));
    }
    summary.completedCharts = sqlite3_column_int(detailStmt.get(), 9);
    summary.totalCharts = sqlite3_column_int(detailStmt.get(), 10);
    summary.stageCount = sqlite3_column_int(detailStmt.get(), 11);
    summary.maxCombo = sqlite3_column_int(detailStmt.get(), 12);
    summary.eventCount = sqlite3_column_int(detailStmt.get(), 13);
    summary.touchSampleCount = sqlite3_column_int(detailStmt.get(), 14);
    summary.rulesetVersion = sqlite3_column_int(detailStmt.get(), 15);
    const int eligibility = sqlite3_column_int(detailStmt.get(), 16);
    if (eligibility >= static_cast<int>(ScoreEligibility::Verified) &&
        eligibility <= static_cast<int>(ScoreEligibility::LegacyUnverified)) {
      summary.eligibility = static_cast<ScoreEligibility>(eligibility);
    }
    replays.push_back(std::move(summary));
  }
  detailStmt.reset();
  if (!readSnapshot.commit(snapshotError)) {
    logSqlErrorText("committing course replay summary snapshot", snapshotError);
    return {};
  }
  return replays;
}

bool ReplayDBHelper::RecoverCourseRecords(
    std::span<const course_identity::Definition> definitions,
    std::span<const CourseScoreEvidence> scoreEvidence,
    std::string &errorMessage) {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(sessionMutex_);
  if (!EnsureSessionDatabaseLocked()) {
    errorMessage = "replay database validation failed";
    return false;
  }
  return RecoverCourseRecordsOnConnection(sessionDatabase_, definitions,
                                          scoreEvidence, errorMessage);
}

bool ReplayDBHelper::RecoverCourseRecords(
    sqlite3 *db, std::span<const course_identity::Definition> definitions,
    std::span<const CourseScoreEvidence> scoreEvidence,
    std::string &errorMessage) {
  profile_database_activity::WriteGuard operation;
  if (this == &GetInstance() || db == nullptr) {
    errorMessage = "external replay recovery connection is unavailable";
    return false;
  }
  std::string versionError;
  const auto version = readSqliteUserVersion(db, versionError);
  if (!version.has_value() || *version != kReplayDatabaseSchemaVersion) {
    errorMessage = version.has_value()
                       ? "external replay recovery schema is not current"
                       : "could not read external replay recovery schema: " +
                             versionError;
    return false;
  }
  return RecoverCourseRecordsOnConnection(db, definitions, scoreEvidence,
                                          errorMessage);
}

bool ReplayDBHelper::RecoverCourseRecordsOnConnection(
    sqlite3 *db, std::span<const course_identity::Definition> definitions,
    std::span<const CourseScoreEvidence> scoreEvidence,
    std::string &errorMessage) {
  errorMessage.clear();
  if (db == nullptr) {
    errorMessage = "replay database is unavailable";
    return false;
  }

  const auto appendKeyField = [](std::string &key, std::string_view value) {
    key += std::to_string(value.size());
    key.push_back(':');
    key.append(value);
    key.push_back('|');
  };
  const auto definitionBucketKey = [&](std::string_view constraints,
                                       std::size_t chartCount, bool sha256,
                                       std::string_view firstHash) {
    std::string key;
    appendKeyField(key, constraints);
    key += std::to_string(chartCount);
    key += sha256 ? "|sha256|" : "|md5|";
    key.append(firstHash);
    return key;
  };
  const auto evidenceTupleKey = [&](int legacyCourseId, int totalCharts,
                                    std::string_view courseName,
                                    std::string_view courseGroupName,
                                    std::string_view constraints) {
    std::string key = std::to_string(legacyCourseId) + "|" +
                      std::to_string(totalCharts) + "|";
    appendKeyField(key, courseName);
    appendKeyField(key, courseGroupName);
    appendKeyField(key, constraints);
    return key;
  };

  std::unordered_map<
      std::string, std::vector<const course_identity::Definition *>>
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
    const std::string firstSha256 =
        normalizedHash(definition.charts.front().sha256);
    const std::string firstMd5 =
        normalizedHash(definition.charts.front().md5);
    if (!firstSha256.empty()) {
      definitionBuckets[definitionBucketKey(
                            canonicalConstraints, definition.charts.size(),
                            true, firstSha256)]
          .push_back(&definition);
    }
    if (!firstMd5.empty()) {
      definitionBuckets[definitionBucketKey(
                            canonicalConstraints, definition.charts.size(),
                            false, firstMd5)]
          .push_back(&definition);
    }
  }

  std::unordered_map<std::string, std::unordered_set<std::string>>
      evidenceKeysByTuple;
  for (const auto &evidence : scoreEvidence) {
    evidenceKeysByTuple[evidenceTupleKey(
                            evidence.legacyCourseId, evidence.totalCharts,
                            evidence.courseName, evidence.courseGroupName,
                            evidence.canonicalConstraintPayload)]
        .insert(evidence.courseKey);
  }

  const bool callerOwnsTransaction = sqlite3_get_autocommit(db) == 0;
  const char *beginQuery =
      callerOwnsTransaction ? "SAVEPOINT asobmashow_replay_course_recovery"
                            : "BEGIN IMMEDIATE TRANSACTION";
  const char *commitQuery =
      callerOwnsTransaction ? "RELEASE asobmashow_replay_course_recovery"
                            : "COMMIT";
  const char *rollbackQuery =
      callerOwnsTransaction
          ? "ROLLBACK TO asobmashow_replay_course_recovery; RELEASE "
            "asobmashow_replay_course_recovery"
          : "ROLLBACK";
  std::string transactionError;
  SqliteTransactionHandle transaction(db, beginQuery, transactionError,
                                      commitQuery, rollbackQuery);
  if (!transaction.active()) {
    errorMessage = "could not start replay recovery transaction: " +
                   transactionError;
    return false;
  }

  SqliteStatementHandle keyStmt;
  if (!prepareSqliteStatementLogged(
          db,
          "SELECT DISTINCT course_key FROM course_replays ORDER BY course_key",
          keyStmt, "reading course replay key groups for identity recovery",
          logSqlErrorText)) {
    errorMessage = sqliteDatabaseError(db);
    return false;
  }
  std::vector<std::string> unresolvedStoredKeys;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(keyStmt.get())) == SQLITE_ROW) {
    std::string storedKey = sqliteColumnString(keyStmt.get(), 0);
    if (storedKey.empty() || !currentKeys.contains(storedKey)) {
      unresolvedStoredKeys.push_back(std::move(storedKey));
    }
  }
  if (rc != SQLITE_DONE) {
    errorMessage = sqliteDatabaseError(db);
    return false;
  }
  keyStmt.reset();

  const char *selectQuery =
      "SELECT id, course_id, course_key, COALESCE(course_name, ''),"
      "COALESCE(course_group_name, ''), COALESCE(constraint_json, ''),"
      "completed_charts, total_charts, ruleset_version, eligibility,"
      "provenance_json FROM course_replays WHERE course_key = ? ORDER BY id";
  SqliteStatementHandle rowStmt;
  if (!prepareSqliteStatementLogged(
          db, selectQuery, rowStmt,
          "reading course replays for identity recovery", logSqlErrorText)) {
    errorMessage = sqliteDatabaseError(db);
    return false;
  }
  SqliteStatementHandle stageStmt;
  if (!prepareSqliteStatementLogged(
          db, kStrictCourseReplayStageIdentityQuery, stageStmt,
          "preparing course replay recovery stage scan", logSqlErrorText)) {
    errorMessage = sqliteDatabaseError(db);
    return false;
  }

  struct Rewrite {
    sqlite3_int64 id = 0;
    std::string courseKey;
  };
  std::vector<Rewrite> rewrites;
  std::size_t examined = 0;
  std::size_t malformed = 0;
  std::size_t ambiguous = 0;
  std::size_t evidenceResolved = 0;
  const auto processRow = [&](sqlite3_stmt *row) {
    const std::string storedKey = sqliteColumnString(row, 2);
    ++examined;

    const int completedCharts = sqlite3_column_int(row, 6);
    const int totalCharts = sqlite3_column_int(row, 7);
    if (completedCharts <= 0 || completedCharts > totalCharts ||
        totalCharts <= 0 ||
        totalCharts > replay_summary_scan::kMaxCourseStagesPerCandidate) {
      ++malformed;
      return true;
    }
    std::string provenanceError;
    if (!decodeStoredProvenance(row, 8, 9, 10, provenanceError)
             .has_value()) {
      ++malformed;
      return true;
    }
    const std::string constraintJson = sqliteColumnString(row, 5);
    const std::string canonicalConstraints =
        course_identity::canonicalConstraintPayload(constraintJson);
    if (canonicalConstraints.empty()) {
      ++malformed;
      return true;
    }

    std::vector<course_identity::ChartIdentity> storedCharts;
    std::string stageError;
    const StrictStageReadResult stageResult =
        readStrictCourseReplayStageIdentities(
            stageStmt.get(), sqlite3_column_int(row, 0), completedCharts,
            storedCharts, stageError);
    if (stageResult == StrictStageReadResult::SqlError) {
      errorMessage = stageError;
      return false;
    }
    if (stageResult == StrictStageReadResult::InvalidRow) {
      ++malformed;
      return true;
    }

    std::unordered_set<const course_identity::Definition *>
        candidateDefinitions;
    const auto collectDefinitionBucket = [&](bool sha256,
                                             const std::string &firstHash) {
      if (firstHash.empty()) {
        return;
      }
      const auto found = definitionBuckets.find(definitionBucketKey(
          canonicalConstraints, static_cast<std::size_t>(totalCharts), sha256,
          firstHash));
      if (found != definitionBuckets.end()) {
        candidateDefinitions.insert(found->second.begin(),
                                    found->second.end());
      }
    };
    collectDefinitionBucket(true, storedCharts.front().sha256);
    collectDefinitionBucket(false, storedCharts.front().md5);

    std::unordered_set<std::string> candidateKeys;
    for (const auto *candidate : candidateDefinitions) {
      if (course_identity::prefixMatches(storedCharts, candidate->charts)) {
        candidateKeys.insert(candidate->courseKey);
      }
    }

    std::optional<std::string> selectedKey;
    bool usedEvidence = false;
    if (candidateKeys.size() == 1) {
      selectedKey = *candidateKeys.begin();
    } else if (candidateKeys.size() > 1) {
      const int legacyCourseId = sqlite3_column_int(row, 1);
      const std::string courseName = sqliteColumnString(row, 3);
      const std::string courseGroupName = sqliteColumnString(row, 4);
      std::unordered_set<std::string> evidenceKeys;
      const auto matchingEvidence = evidenceKeysByTuple.find(evidenceTupleKey(
          legacyCourseId, totalCharts, courseName, courseGroupName,
          canonicalConstraints));
      if (matchingEvidence != evidenceKeysByTuple.end()) {
        for (const auto &evidenceKey : matchingEvidence->second) {
          if (candidateKeys.contains(evidenceKey)) {
            evidenceKeys.insert(evidenceKey);
          }
        }
      }
      if (evidenceKeys.size() == 1) {
        selectedKey = *evidenceKeys.begin();
        usedEvidence = true;
      }
    }

    if (!selectedKey.has_value()) {
      if (candidateKeys.size() > 1) {
        ++ambiguous;
      }
      return true;
    }
    if (*selectedKey != storedKey) {
      rewrites.push_back({.id = sqlite3_column_int64(row, 0),
                          .courseKey = std::move(*selectedKey)});
      if (usedEvidence) {
        ++evidenceResolved;
      }
    }
    return true;
  };

  for (const auto &storedKey : unresolvedStoredKeys) {
    sqlite3_reset(rowStmt.get());
    sqlite3_clear_bindings(rowStmt.get());
    bindSqliteText(rowStmt.get(), 1, storedKey);
    while ((rc = sqlite3_step(rowStmt.get())) == SQLITE_ROW) {
      if (!processRow(rowStmt.get())) {
        return false;
      }
    }
    if (rc != SQLITE_DONE) {
      errorMessage = sqliteDatabaseError(db);
      return false;
    }
  }
  rowStmt.reset();
  stageStmt.reset();

  SqliteStatementHandle updateStmt;
  if (!rewrites.empty() &&
      !prepareSqliteStatementLogged(
          db, "UPDATE course_replays SET course_key = ? WHERE id = ?",
          updateStmt, "preparing course replay recovery update",
          logSqlErrorText)) {
    errorMessage = sqliteDatabaseError(db);
    return false;
  }
  for (const auto &rewrite : rewrites) {
    bindSqliteText(updateStmt.get(), 1, rewrite.courseKey);
    sqlite3_bind_int64(updateStmt.get(), 2, rewrite.id);
    if (sqlite3_step(updateStmt.get()) != SQLITE_DONE) {
      errorMessage = sqliteDatabaseError(db);
      return false;
    }
    sqlite3_reset(updateStmt.get());
    sqlite3_clear_bindings(updateStmt.get());
  }
  if (!transaction.commit(transactionError)) {
    errorMessage = "could not commit replay recovery transaction: " +
                   transactionError;
    return false;
  }
  SDL_Log("Course replay recovery: examined=%zu rewritten=%zu malformed=%zu "
          "ambiguous=%zu evidence_resolved=%zu",
          examined, rewrites.size(), malformed, ambiguous, evidenceResolved);
  return true;
}

static std::optional<ReplayData>
loadReplayFromConnection(sqlite3 *db, int replayId,
                         const bms_parser::ChartMeta &chartMeta) {
  const auto match = replayChartMatchFor(chartMeta);
  std::string query =
      "SELECT id, chart_path, chart_md5, chart_sha256, chart_title,"
      "chart_artist, gauge_type, gauge_auto_shift, final_score, final_gauge,"
      "clear_type, created_at, random_seed, random_prng, random_values,"
      "play_option,"
      "play_option_seed, play_option2, play_option2_seed, assist_option, "
      "ln_mode, max_combo, ruleset_version, eligibility, provenance_json "
      "FROM replays WHERE id = ? AND ";
  query += replayChartMatchPredicate("");

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "preparing replay load",
                                    logSqlErrorText)) {
    return std::nullopt;
  }

  sqlite3_bind_int(stmt.get(), 1, replayId);
  bindReplayChartMatch(stmt.get(), 2, match);
  std::optional<ReplayData> replay;
  if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    ReplayData loaded;
    loaded.id = sqlite3_column_int(stmt.get(), 0);
    loaded.chartMeta = chartMeta;
    loaded.chartMeta.BmsPath = readText(stmt.get(), 1);
    loaded.chartMeta.MD5 = readText(stmt.get(), 2);
    loaded.chartMeta.SHA256 = readText(stmt.get(), 3);
    loaded.chartMeta.Title = readText(stmt.get(), 4);
    loaded.chartMeta.Artist = readText(stmt.get(), 5);
    loaded.initialGaugeType =
        gaugeTypeFromInt(sqlite3_column_int(stmt.get(), 6));
    loaded.gaugeAutoShift = sqlite3_column_int(stmt.get(), 7) != 0;
    loaded.finalScore = sqlite3_column_int(stmt.get(), 8);
    loaded.finalGauge =
        static_cast<float>(sqlite3_column_double(stmt.get(), 9));
    loaded.clearType = sqlite3_column_int(stmt.get(), 10);
    loaded.createdAt = readText(stmt.get(), 11);
    if (sqlite3_column_type(stmt.get(), 12) != SQLITE_NULL) {
      loaded.randomSeed =
          static_cast<unsigned int>(sqlite3_column_int64(stmt.get(), 12));
      loaded.chartMeta.RandomSeed = loaded.randomSeed;
    }
    if (sqlite3_column_type(stmt.get(), 13) != SQLITE_NULL) {
      loaded.randomPrng = readText(stmt.get(), 13);
    } else if (loaded.randomSeed.has_value()) {
      loaded.randomPrng = bms_parser::Parser::RandomPrngId;
    }
    loaded.chartMeta.RandomPrng = loaded.randomPrng;
    if (sqlite3_column_type(stmt.get(), 14) != SQLITE_NULL) {
      loaded.randomValues = parseRandomValues(readText(stmt.get(), 14));
    }
    loaded.chartMeta.RandomValues = loaded.randomValues;
    if (sqlite3_column_type(stmt.get(), 15) != SQLITE_NULL) {
      loaded.playOption = readText(stmt.get(), 15);
    }
    if (sqlite3_column_type(stmt.get(), 16) != SQLITE_NULL) {
      loaded.playOptionSeed = sqlite3_column_int64(stmt.get(), 16);
    }
    if (sqlite3_column_type(stmt.get(), 17) != SQLITE_NULL) {
      loaded.playOption2 = readText(stmt.get(), 17);
    }
    if (sqlite3_column_type(stmt.get(), 18) != SQLITE_NULL) {
      loaded.playOption2Seed = sqlite3_column_int64(stmt.get(), 18);
    }
    if (sqlite3_column_type(stmt.get(), 19) != SQLITE_NULL) {
      loaded.assistOption = assist_options::normalize(readText(stmt.get(), 19));
    }
    const int replayLongNoteMode =
        long_note_mode::normalizeValue(sqlite3_column_int(stmt.get(), 20));
    if (replayLongNoteMode > 0) {
      loaded.chartMeta.LnMode = replayLongNoteMode;
    }
    loaded.maxCombo = sqlite3_column_int(stmt.get(), 21);
    auto provenance = readStoredProvenance(stmt.get(), 22, 23, 24, "replay");
    if (!provenance.has_value()) {
      return std::nullopt;
    }
    loaded.provenance = std::move(*provenance);
    replay = std::move(loaded);
  }
  stmt.reset();

  if (!replay.has_value()) {
    return std::nullopt;
  }

  const char *eventQuery =
      "SELECT action, lane, note_time_micros, song_time_micros,"
      "judge_time_micros, judgement, diff_micros, gauge, gauge_type, combo,"
      "score FROM replay_events WHERE replay_id = ? ORDER BY event_index";
  SqliteStatementHandle eventStmt;
  if (!prepareSqliteStatementLogged(db, eventQuery, eventStmt,
                                    "preparing replay event load",
                                    logSqlErrorText)) {
    return std::nullopt;
  }
  sqlite3_bind_int(eventStmt.get(), 1, replay->id);

  int eventRc = SQLITE_OK;
  while ((eventRc = sqlite3_step(eventStmt.get())) == SQLITE_ROW) {
    ReplayEvent event;
    event.action = actionFromInt(sqlite3_column_int(eventStmt.get(), 0));
    event.lane = sqlite3_column_int(eventStmt.get(), 1);
    event.noteTimeMicros = sqlite3_column_int64(eventStmt.get(), 2);
    event.songTimeMicros = sqlite3_column_int64(eventStmt.get(), 3);
    event.judgeTimeMicros = sqlite3_column_int64(eventStmt.get(), 4);
    event.judgement = judgementFromInt(sqlite3_column_int(eventStmt.get(), 5));
    event.diffMicros = sqlite3_column_int64(eventStmt.get(), 6);
    event.gauge = static_cast<float>(sqlite3_column_double(eventStmt.get(), 7));
    event.gaugeType = gaugeTypeFromInt(sqlite3_column_int(eventStmt.get(), 8));
    event.combo = sqlite3_column_int(eventStmt.get(), 9);
    event.score = sqlite3_column_int(eventStmt.get(), 10);
    replay->events.push_back(event);
  }
  if (eventRc != SQLITE_DONE) {
    logSqlError("loading replay events", db);
    return std::nullopt;
  }
  eventStmt.reset();

  const char *touchSampleQuery =
      "SELECT action, finger_id, song_time_micros, x, y "
      "FROM replay_touch_samples WHERE replay_id = ? ORDER BY sample_index";
  SqliteStatementHandle touchSampleStmt;
  if (!prepareSqliteStatementLogged(db, touchSampleQuery, touchSampleStmt,
                                    "preparing replay touch sample load",
                                    logSqlErrorText)) {
    return std::nullopt;
  }
  sqlite3_bind_int(touchSampleStmt.get(), 1, replay->id);

  int touchSampleRc = SQLITE_OK;
  while ((touchSampleRc = sqlite3_step(touchSampleStmt.get())) == SQLITE_ROW) {
    ReplayTouchSample sample;
    sample.action =
        touchActionFromInt(sqlite3_column_int(touchSampleStmt.get(), 0));
    sample.fingerId = sqlite3_column_int64(touchSampleStmt.get(), 1);
    sample.songTimeMicros = sqlite3_column_int64(touchSampleStmt.get(), 2);
    sample.x =
        static_cast<float>(sqlite3_column_double(touchSampleStmt.get(), 3));
    sample.y =
        static_cast<float>(sqlite3_column_double(touchSampleStmt.get(), 4));
    replay->touchSamples.push_back(sample);
  }
  if (touchSampleRc != SQLITE_DONE) {
    logSqlError("loading replay touch samples", db);
    return std::nullopt;
  }
  touchSampleStmt.reset();

  const char *laneCoverEventQuery =
      "SELECT song_time_micros, note_start_position_percent,"
      "reset_visible_time_reference "
      "FROM replay_lane_cover_events WHERE replay_id = ? "
      "ORDER BY event_index";
  SqliteStatementHandle laneCoverEventStmt;
  if (!prepareSqliteStatementLogged(db, laneCoverEventQuery, laneCoverEventStmt,
                                    "preparing replay lane cover event load",
                                    logSqlErrorText)) {
    return std::nullopt;
  }
  sqlite3_bind_int(laneCoverEventStmt.get(), 1, replay->id);

  int laneCoverEventRc = SQLITE_OK;
  while ((laneCoverEventRc = sqlite3_step(laneCoverEventStmt.get())) ==
         SQLITE_ROW) {
    ReplayLaneCoverEvent event;
    event.songTimeMicros = sqlite3_column_int64(laneCoverEventStmt.get(), 0);
    event.noteStartPositionPercent =
        sqlite3_column_int(laneCoverEventStmt.get(), 1);
    event.resetVisibleTimeReference =
        sqlite3_column_int(laneCoverEventStmt.get(), 2) != 0;
    replay->laneCoverEvents.push_back(event);
  }
  if (laneCoverEventRc != SQLITE_DONE) {
    logSqlError("loading replay lane cover events", db);
    return std::nullopt;
  }

  return replay;
}

std::optional<ReplayData>
ReplayDBHelper::LoadReplay(int replayId,
                           const bms_parser::ChartMeta &chartMeta) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(sessionMutex_);
  if (!EnsureSessionDatabaseLocked()) {
    return std::nullopt;
  }
  return LoadReplayOnConnection(sessionDatabase_, replayId, chartMeta);
}

std::optional<ReplayData> ReplayDBHelper::LoadReplayOnConnection(
    sqlite3 *db, int replayId, const bms_parser::ChartMeta &chartMeta) {
  std::string snapshotError;
  SqliteTransactionHandle readSnapshot(db, "BEGIN TRANSACTION", snapshotError);
  if (!readSnapshot.active()) {
    logSqlErrorText("starting replay load snapshot", snapshotError);
    return std::nullopt;
  }
  auto replay = loadReplayFromConnection(db, replayId, chartMeta);
  if (!replay.has_value()) {
    return std::nullopt;
  }
  if (!readSnapshot.commit(snapshotError)) {
    logSqlErrorText("committing replay load snapshot", snapshotError);
    return std::nullopt;
  }
  return replay;
}

std::optional<CourseReplayData> ReplayDBHelper::LoadCourseReplay(int replayId) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(sessionMutex_);
  if (!EnsureSessionDatabaseLocked()) {
    return std::nullopt;
  }
  return LoadCourseReplayOnConnection(sessionDatabase_, replayId);
}

std::optional<CourseReplayData>
ReplayDBHelper::LoadCourseReplayOnConnection(sqlite3 *db, int replayId) {
  std::string snapshotError;
  SqliteTransactionHandle readSnapshot(db, "BEGIN TRANSACTION", snapshotError);
  if (!readSnapshot.active()) {
    logSqlErrorText("starting course replay load snapshot", snapshotError);
    return std::nullopt;
  }

  const char *query =
      "SELECT id, course_id, course_key, course_name, course_group_name, "
      "constraint_json, gauge_type, gauge_profile, gauge_auto_shift, ln_mode,"
      "requested_play_option, assist_option, final_score, final_gauge,"
      "clear_type, completed_charts, total_charts, created_at, max_combo,"
      "ruleset_version, eligibility, provenance_json "
      "FROM course_replays WHERE id = ?";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, query, stmt, "preparing course replay load", logSqlErrorText)) {
    return std::nullopt;
  }

  sqlite3_bind_int(stmt.get(), 1, replayId);
  CourseReplayData courseReplay;
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    return std::nullopt;
  }

  courseReplay.id = sqlite3_column_int(stmt.get(), 0);
  courseReplay.courseId = sqlite3_column_int(stmt.get(), 1);
  courseReplay.courseKey = readText(stmt.get(), 2);
  courseReplay.courseName = readText(stmt.get(), 3);
  courseReplay.courseGroupName = readText(stmt.get(), 4);
  courseReplay.constraintJson = readText(stmt.get(), 5);
  courseReplay.initialGaugeType =
      gaugeTypeFromInt(sqlite3_column_int(stmt.get(), 6));
  courseReplay.gaugeProfile =
      gaugeProfileFromInt(sqlite3_column_int(stmt.get(), 7));
  courseReplay.gaugeAutoShift = sqlite3_column_int(stmt.get(), 8) != 0;
  courseReplay.longNoteMode =
      long_note_mode::normalizeValue(sqlite3_column_int(stmt.get(), 9));
  courseReplay.requestedPlayOption = readText(stmt.get(), 10);
  courseReplay.assistOption =
      assist_options::normalize(readText(stmt.get(), 11));
  courseReplay.finalScore = sqlite3_column_int(stmt.get(), 12);
  courseReplay.finalGauge =
      static_cast<float>(sqlite3_column_double(stmt.get(), 13));
  courseReplay.clearType = sqlite3_column_int(stmt.get(), 14);
  courseReplay.completedCharts = sqlite3_column_int(stmt.get(), 15);
  courseReplay.totalCharts = sqlite3_column_int(stmt.get(), 16);
  courseReplay.createdAt = readText(stmt.get(), 17);
  courseReplay.maxCombo = sqlite3_column_int(stmt.get(), 18);
  auto provenance =
      readStoredProvenance(stmt.get(), 19, 20, 21, "course replay");
  if (!provenance.has_value()) {
    return std::nullopt;
  }
  courseReplay.provenance = std::move(*provenance);
  stmt.reset();

  SqliteStatementHandle stageStmt;
  if (!prepareSqliteStatementLogged(
          db, kCourseReplayStageDescriptorQuery, stageStmt,
          "preparing course replay stage load", logSqlErrorText)) {
    return std::nullopt;
  }

  std::vector<CourseReplayStageDescriptor> pendingStages;
  std::string stageError;
  if (readCourseReplayStageDescriptors(stageStmt.get(), replayId, true,
                                       pendingStages, stageError) !=
      CourseReplayStageDescriptorReadResult::Valid) {
    logSqlErrorText("validating course replay stages", stageError);
    return std::nullopt;
  }
  stageStmt.reset();

  for (auto &pendingStage : pendingStages) {
    auto stageReplay = loadReplayFromConnection(db, pendingStage.replayId,
                                                pendingStage.chartMeta);
    if (!stageReplay.has_value()) {
      SDL_Log("Failed to load course replay stage %d replay %d",
              pendingStage.stageIndex, pendingStage.replayId);
      return std::nullopt;
    }
    courseReplay.stages.push_back(CourseReplayStageData{
        .replay = std::move(*stageReplay),
        .restMicrosAfterStage = std::max(0LL, pendingStage.restMicros)});
  }

  if (!readSnapshot.commit(snapshotError)) {
    logSqlErrorText("committing course replay load snapshot", snapshotError);
    return std::nullopt;
  }
  return courseReplay;
}

std::optional<ReplayData>
ReplayDBHelper::LoadLatestReplay(const bms_parser::ChartMeta &chartMeta) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(sessionMutex_);
  if (!EnsureSessionDatabaseLocked()) {
    return std::nullopt;
  }
  return LoadLatestReplayOnConnection(sessionDatabase_, chartMeta);
}

std::optional<ReplayData> ReplayDBHelper::LoadLatestReplayOnConnection(
    sqlite3 *db, const bms_parser::ChartMeta &chartMeta) {
  const auto replays = ListReplaysOnConnection(db, chartMeta, 1);
  if (replays.empty()) {
    return std::nullopt;
  }
  return LoadReplayOnConnection(db, replays.front().id, chartMeta);
}
