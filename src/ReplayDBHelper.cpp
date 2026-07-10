#include "ReplayDBHelper.h"

#include "BmsMetadataText.h"
#include "ChartSqlExpressions.h"
#include "LongNoteModeUtils.h"
#include "SqliteRAII.h"
#include "Utils.h"
#include "path.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {
constexpr int kLegacyReplayDatabaseSchemaVersion = 2;
constexpr int kReplayDatabaseSchemaVersion = 3;
constexpr const char *kLegacyProvenanceJson =
    "{\"schemaVersion\":1,\"ruleset\":{\"version\":0},\"stages\":[],"
    "\"eligibility\":\"legacy-unverified\"}";

using asobmshow::bms_metadata::normalizedHash;
using asobmshow::bms_metadata::trimCopy;
using asobmshow::chart_sql::boundNormalizedHashMatchCondition;
using asobmshow::chart_sql::boundStoredOrLegacyBmsPathMatchCondition;
using asobmshow::chart_sql::normalizedSqlHash;

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
  const char *filename =
      db != nullptr ? sqlite3_db_filename(db, "main") : nullptr;
  if (db != nullptr && sqlite3_get_autocommit(db) != 0 && filename != nullptr &&
      filename[0] != '\0') {
    std::string error;
    if (!preflightSqliteUserVersion(filename, kReplayDatabaseSchemaVersion,
                                    error)
             .has_value()) {
      SDL_Log("Refusing replay database schema preflight: %s", error.c_str());
      return true;
    }
    return false;
  }
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
  if (*version < 1 && !normalizeReplayChartIdentityHashes(db)) {
    return false;
  }
  if (*version < kLegacyReplayDatabaseSchemaVersion &&
      !backfillReplayMaxCombo(db)) {
    return false;
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(db, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    logSqlErrorText("starting replay provenance migration", transactionError);
    return false;
  }
  if (!ensureReplayProvenanceColumns(db, "replays") ||
      !ensureReplayProvenanceColumns(db, "course_replays") ||
      !setDatabaseUserVersion(db, kReplayDatabaseSchemaVersion)) {
    return false;
  }
  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing replay provenance migration", transactionError);
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

bool readCourseReplayStageDescriptors(
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
    return false;
  }

  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const int expectedIndex = static_cast<int>(stages.size());
    if (sqlite3_column_int(stmt, 0) != expectedIndex) {
      error = "course replay stage indexes are not contiguous";
      return false;
    }
    if (stages.size() >=
        static_cast<std::size_t>(
            replay_summary_scan::kMaxCourseStagesPerCandidate)) {
      error = "course replay has too many stages";
      return false;
    }
    if (sqlite3_column_type(stmt, 3) == SQLITE_NULL) {
      error = "course replay references a missing stage replay";
      return false;
    }
    const int replayId = sqlite3_column_int(stmt, 1);
    if (replayId <= 0 || replayId != sqlite3_column_int(stmt, 3)) {
      error = "course replay has an invalid stage replay id";
      return false;
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
      return false;
    }
    if (validateProvenance &&
        !decodeStoredProvenance(stmt, 9, 10, 11, error).has_value()) {
      return false;
    }
    stages.push_back(std::move(stage));
  }
  if (rc != SQLITE_DONE) {
    error = sqliteDatabaseError(sqlite3_db_handle(stmt));
    return false;
  }
  if (stages.empty()) {
    error = "course replay has no linked stages";
    return false;
  }
  return true;
}

bool courseReplayStagesHaveValidProvenance(sqlite3_stmt *stmt,
                                           int courseReplayId,
                                           std::string &error) {
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
} // namespace

ReplayDBHelper &ReplayDBHelper::GetInstance() {
  sqlite3_config(SQLITE_CONFIG_SERIALIZED);
  static ReplayDBHelper instance;
  return instance;
}

ReplayDBHelper::ReplayDBHelper(std::filesystem::path databasePath)
    : databasePath_(std::move(databasePath)) {}

void ReplayDBHelper::SetDatabasePath(std::filesystem::path databasePath) {
  databasePath_ = std::move(databasePath);
}

const std::filesystem::path &ReplayDBHelper::GetDatabasePath() const {
  return databasePath_;
}

sqlite3 *ReplayDBHelper::Connect() {
  const std::filesystem::path path =
      databasePath_.empty() ? Utils::GetDocumentsPath("db") / "replay.db"
                            : databasePath_;
  const std::filesystem::path directory = path.parent_path();
  std::error_code directoryError;
  if (!directory.empty() &&
      !Utils::EnsureDirectoryExists(directory, directoryError)) {
    SDL_Log("Can't create replay database directory %s: %s",
            fspath_to_utf8(directory).c_str(),
            directoryError.message().c_str());
    return nullptr;
  }

  std::string openError;
  sqlite3 *db = openValidatedSqliteDatabase(path, kReplayDatabaseSchemaVersion,
                                            true, openError);
  if (db == nullptr) {
    SDL_Log("Refusing to open replay database %s: %s",
            fspath_to_utf8(path).c_str(), openError.c_str());
    return nullptr;
  }
  return db;
}

void ReplayDBHelper::Close(sqlite3 *db) { closeSqliteDatabase(db); }

bool ReplayDBHelper::CreateReplayTables(sqlite3 *db) {
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
  return true;
}

bool ReplayDBHelper::EnsureSchema() {
  SqliteConnectionHandle connection(Connect());
  return connection.get() != nullptr && CreateReplayTables(connection.get());
}

std::optional<int> ReplayDBHelper::SaveReplay(const ReplayData &replay) {
  const auto provenanceJson =
      validatedProvenanceJson(replay.provenance, "replay");
  if (!provenanceJson.has_value()) {
    return std::nullopt;
  }

  SqliteConnectionHandle dbHandle(Connect());
  sqlite3 *db = dbHandle.get();
  if (db == nullptr) {
    return std::nullopt;
  }

  if (!CreateReplayTables(db)) {
    return std::nullopt;
  }

  std::string transactionError;
  SqliteTransactionHandle transaction(db, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    logSqlErrorText("starting replay save", transactionError);
    return std::nullopt;
  }

  const auto replayId = insertReplayRows(db, replay, *provenanceJson);
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
  if (replay.stages.empty()) {
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

  SqliteConnectionHandle dbHandle(Connect());
  sqlite3 *db = dbHandle.get();
  if (db == nullptr) {
    return std::nullopt;
  }

  if (!CreateReplayTables(db)) {
    return std::nullopt;
  }

  std::string transactionError;
  SqliteTransactionHandle transaction(db, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    logSqlErrorText("starting course replay save", transactionError);
    return std::nullopt;
  }

  const char *courseInsert =
      "INSERT INTO course_replays ("
      "course_id, course_name, course_group_name, constraint_json,"
      "gauge_type, gauge_profile, gauge_auto_shift, ln_mode,"
      "requested_play_option, assist_option, final_score, max_combo, "
      "final_gauge,"
      "clear_type, completed_charts, total_charts, ruleset_version,"
      "eligibility, provenance_json"
      ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

  SqliteStatementHandle courseStmt;
  if (!prepareSqliteStatementLogged(db, courseInsert, courseStmt,
                                    "preparing course replay insert",
                                    logSqlErrorText)) {
    return std::nullopt;
  }

  int bindIndex = 1;
  sqlite3_bind_int(courseStmt.get(), bindIndex++, replay.courseId);
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
  bindSqliteText(courseStmt.get(), bindIndex++, *courseProvenanceJson);

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
  std::vector<ReplaySummary> replays;
  SqliteConnectionHandle dbHandle(Connect());
  sqlite3 *db = dbHandle.get();
  if (db == nullptr) {
    return replays;
  }

  if (!CreateReplayTables(db)) {
    return replays;
  }

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

std::vector<ReplaySummary> ReplayDBHelper::ListCourseReplays(int courseId,
                                                             int limit) {
  std::vector<ReplaySummary> replays;
  SqliteConnectionHandle dbHandle(Connect());
  sqlite3 *db = dbHandle.get();
  if (db == nullptr || courseId <= 0) {
    return replays;
  }

  if (!CreateReplayTables(db)) {
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
      "WHERE cr.course_id = ? AND cr.id < ? "
      "ORDER BY cr.id DESC LIMIT ?";
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
  std::size_t inspected = 0;
  std::size_t rejected = 0;
  bool reachedEnd = false;
  bool scanFailed = false;
  while (validIds.size() < requestedCount && inspected < candidateBudget) {
    const std::size_t chunkSize = std::min<std::size_t>(
        replay_summary_scan::kChunkSize, candidateBudget - inspected);
    sqlite3_reset(candidateStmt.get());
    sqlite3_clear_bindings(candidateStmt.get());
    sqlite3_bind_int(candidateStmt.get(), 1, courseId);
    sqlite3_bind_int64(candidateStmt.get(), 2, beforeId);
    sqlite3_bind_int(candidateStmt.get(), 3, static_cast<int>(chunkSize));

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
      const bool stagesValid =
          aggregateValid &&
          courseReplayStagesHaveValidProvenance(
              stageProvenanceStmt.get(),
              sqlite3_column_int(candidateStmt.get(), 0), provenanceError);
      if (!aggregateValid || !stagesValid) {
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

  while (sqlite3_step(eventStmt.get()) == SQLITE_ROW) {
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

  while (sqlite3_step(touchSampleStmt.get()) == SQLITE_ROW) {
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

  while (sqlite3_step(laneCoverEventStmt.get()) == SQLITE_ROW) {
    ReplayLaneCoverEvent event;
    event.songTimeMicros = sqlite3_column_int64(laneCoverEventStmt.get(), 0);
    event.noteStartPositionPercent =
        sqlite3_column_int(laneCoverEventStmt.get(), 1);
    event.resetVisibleTimeReference =
        sqlite3_column_int(laneCoverEventStmt.get(), 2) != 0;
    replay->laneCoverEvents.push_back(event);
  }

  return replay;
}

std::optional<ReplayData>
ReplayDBHelper::LoadReplay(int replayId,
                           const bms_parser::ChartMeta &chartMeta) {
  SqliteConnectionHandle dbHandle(Connect());
  sqlite3 *db = dbHandle.get();
  if (db == nullptr || !CreateReplayTables(db)) {
    return std::nullopt;
  }

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
  SqliteConnectionHandle dbHandle(Connect());
  sqlite3 *db = dbHandle.get();
  if (db == nullptr) {
    return std::nullopt;
  }

  if (!CreateReplayTables(db)) {
    return std::nullopt;
  }

  std::string snapshotError;
  SqliteTransactionHandle readSnapshot(db, "BEGIN TRANSACTION", snapshotError);
  if (!readSnapshot.active()) {
    logSqlErrorText("starting course replay load snapshot", snapshotError);
    return std::nullopt;
  }

  const char *query =
      "SELECT id, course_id, course_name, course_group_name, constraint_json,"
      "gauge_type, gauge_profile, gauge_auto_shift, ln_mode,"
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
  courseReplay.courseName = readText(stmt.get(), 2);
  courseReplay.courseGroupName = readText(stmt.get(), 3);
  courseReplay.constraintJson = readText(stmt.get(), 4);
  courseReplay.initialGaugeType =
      gaugeTypeFromInt(sqlite3_column_int(stmt.get(), 5));
  courseReplay.gaugeProfile =
      gaugeProfileFromInt(sqlite3_column_int(stmt.get(), 6));
  courseReplay.gaugeAutoShift = sqlite3_column_int(stmt.get(), 7) != 0;
  courseReplay.longNoteMode =
      long_note_mode::normalizeValue(sqlite3_column_int(stmt.get(), 8));
  courseReplay.requestedPlayOption = readText(stmt.get(), 9);
  courseReplay.assistOption =
      assist_options::normalize(readText(stmt.get(), 10));
  courseReplay.finalScore = sqlite3_column_int(stmt.get(), 11);
  courseReplay.finalGauge =
      static_cast<float>(sqlite3_column_double(stmt.get(), 12));
  courseReplay.clearType = sqlite3_column_int(stmt.get(), 13);
  courseReplay.completedCharts = sqlite3_column_int(stmt.get(), 14);
  courseReplay.totalCharts = sqlite3_column_int(stmt.get(), 15);
  courseReplay.createdAt = readText(stmt.get(), 16);
  courseReplay.maxCombo = sqlite3_column_int(stmt.get(), 17);
  auto provenance =
      readStoredProvenance(stmt.get(), 18, 19, 20, "course replay");
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
  if (!readCourseReplayStageDescriptors(stageStmt.get(), replayId, true,
                                        pendingStages, stageError)) {
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
  const auto replays = ListReplays(chartMeta, 1);
  if (replays.empty()) {
    return std::nullopt;
  }
  return LoadReplay(replays.front().id, chartMeta);
}
