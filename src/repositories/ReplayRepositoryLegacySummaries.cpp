#include "ReplayRepository.h"
#include "ReplayRepositoryInternal.h"

#include "ChartSqlExpressions.h"
#include "SqliteRAII.h"

#include "../BmsMetadataText.h"
#include "../ProfileDatabaseActivity.h"
#include "../ResultContracts.h"
#include "../Utils.h"
#include "../replay/ReplayFormat.h"
#include "../replay/ReplayLimits.h"

#include <SDL2/SDL.h>

#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using asobmshow::bms_metadata::normalizedHash;
using asobmshow::bms_metadata::trimCopy;
using asobmshow::chart_sql::boundNormalizedHashMatchCondition;
using asobmshow::chart_sql::boundStoredOrLegacyBmsPathMatchCondition;

void logReadError(const char *context, sqlite3 *database) {
  SDL_Log("SQL error while %s: %s", context,
          sqliteDatabaseError(database).c_str());
}

bool validLimit(std::size_t limit) noexcept {
  return limit > 0 && limit <= kMaximumLegacyResultSummaryRows &&
         limit <= static_cast<std::size_t>(
                      std::numeric_limits<sqlite3_int64>::max());
}

std::optional<std::string> optionalText(sqlite3_stmt *statement, int column,
                                        bool &partial) {
  if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
    return std::nullopt;
  }
  if (sqlite3_column_type(statement, column) != SQLITE_TEXT ||
      sqlite3_column_bytes(statement, column) < 0 ||
      static_cast<std::size_t>(sqlite3_column_bytes(statement, column)) >
          replay::kReplayLimits.maxStringBytes) {
    partial = true;
    return std::nullopt;
  }
  return sqliteColumnString(statement, column);
}

std::optional<int> optionalInt(sqlite3_stmt *statement, int column, int minimum,
                               int maximum, bool &partial) {
  if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
    return std::nullopt;
  }
  if (sqlite3_column_type(statement, column) != SQLITE_INTEGER) {
    partial = true;
    return std::nullopt;
  }
  const sqlite3_int64 value = sqlite3_column_int64(statement, column);
  if (value < minimum || value > maximum) {
    partial = true;
    return std::nullopt;
  }
  return static_cast<int>(value);
}

std::optional<double> optionalNumber(sqlite3_stmt *statement, int column,
                                     bool &partial) {
  if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
    return std::nullopt;
  }
  const int type = sqlite3_column_type(statement, column);
  const double value = sqlite3_column_double(statement, column);
  if ((type != SQLITE_INTEGER && type != SQLITE_FLOAT) ||
      !std::isfinite(value) || value < 0.0) {
    partial = true;
    return std::nullopt;
  }
  return value;
}

std::optional<ScoreEligibility> optionalEligibility(sqlite3_stmt *statement,
                                                    int column, bool &partial) {
  const auto value = optionalInt(
      statement, column, static_cast<int>(ScoreEligibility::Verified),
      static_cast<int>(ScoreEligibility::LegacyUnverified), partial);
  return value ? std::optional(static_cast<ScoreEligibility>(*value))
               : std::nullopt;
}

bool storedPartial(sqlite3_stmt *statement, int column) {
  return sqlite3_column_type(statement, column) != SQLITE_INTEGER ||
         sqlite3_column_int(statement, column) != 0;
}

LegacyChartResultSummary readChartSummary(sqlite3_stmt *statement) {
  LegacyChartResultSummary result;
  result.legacyReplayId = sqlite3_column_int(statement, 0);
  bool partial = storedPartial(statement, 15);
  result.chartPath = optionalText(statement, 1, partial);
  result.chartMd5 = optionalText(statement, 2, partial);
  if (result.chartMd5 && !replay::isCanonicalLowerHex(*result.chartMd5, 32)) {
    result.chartMd5.reset();
    partial = true;
  }
  result.chartSha256 = optionalText(statement, 3, partial);
  if (result.chartSha256 &&
      !replay::isCanonicalLowerHex(*result.chartSha256, 64)) {
    result.chartSha256.reset();
    partial = true;
  }
  result.chartTitle = optionalText(statement, 4, partial);
  result.chartArtist = optionalText(statement, 5, partial);
  result.longNoteMode = optionalInt(statement, 6, 0, 3, partial);
  result.finalScore =
      optionalInt(statement, 7, 0, std::numeric_limits<int>::max(), partial);
  result.maxCombo =
      optionalInt(statement, 8, 0, std::numeric_limits<int>::max(), partial);
  result.finalGauge = optionalNumber(statement, 9, partial);
  result.clearType = optionalInt(statement, 10, std::numeric_limits<int>::min(),
                                 std::numeric_limits<int>::max(), partial);
  if (result.clearType &&
      !result_contract::isKnownClearRank(*result.clearType)) {
    result.clearType.reset();
    partial = true;
  }
  result.createdAt = optionalText(statement, 11, partial);
  result.rulesetVersion =
      optionalInt(statement, 12, 0, std::numeric_limits<int>::max(), partial);
  result.eligibility = optionalEligibility(statement, 13, partial);
  result.provenanceJson = optionalText(statement, 14, partial);
  result.partial = partial;
  return result;
}

LegacyCourseResultSummary readCourseSummary(sqlite3_stmt *statement) {
  LegacyCourseResultSummary result;
  result.legacyCourseReplayId = sqlite3_column_int(statement, 0);
  bool partial = storedPartial(statement, 16);
  result.legacyCourseId =
      optionalInt(statement, 1, 0, std::numeric_limits<int>::max(), partial);
  result.courseKey = optionalText(statement, 2, partial);
  result.courseName = optionalText(statement, 3, partial);
  result.courseGroupName = optionalText(statement, 4, partial);
  result.constraintJson = optionalText(statement, 5, partial);
  result.finalScore =
      optionalInt(statement, 6, 0, std::numeric_limits<int>::max(), partial);
  result.maxCombo =
      optionalInt(statement, 7, 0, std::numeric_limits<int>::max(), partial);
  result.finalGauge = optionalNumber(statement, 8, partial);
  result.clearType = optionalInt(statement, 9, std::numeric_limits<int>::min(),
                                 std::numeric_limits<int>::max(), partial);
  if (result.clearType &&
      !result_contract::isKnownClearRank(*result.clearType)) {
    result.clearType.reset();
    partial = true;
  }
  const int maximumStages =
      static_cast<int>(replay::kReplayLimits.maxCourseStages);
  result.completedCharts =
      optionalInt(statement, 10, 0, maximumStages, partial);
  result.totalCharts = optionalInt(statement, 11, 1, maximumStages, partial);
  if (result.completedCharts && result.totalCharts &&
      *result.completedCharts > *result.totalCharts) {
    result.completedCharts.reset();
    result.totalCharts.reset();
    partial = true;
  }
  result.createdAt = optionalText(statement, 12, partial);
  result.rulesetVersion =
      optionalInt(statement, 13, 0, std::numeric_limits<int>::max(), partial);
  result.eligibility = optionalEligibility(statement, 14, partial);
  result.provenanceJson = optionalText(statement, 15, partial);
  result.partial = partial;
  return result;
}

int bindChartIdentity(sqlite3_stmt *statement, int index,
                      std::string_view sha256, std::string_view md5,
                      std::string_view path) {
  auto bind = [&](std::string_view value) {
    sqlite3_bind_text(statement, index++, value.data(),
                      static_cast<int>(value.size()), SQLITE_TRANSIENT);
  };
  bind(sha256);
  bind(sha256);
  bind(md5);
  bind(md5);
  bind(path);
  bind(path);
  bind(path);
  bind(path);
  return index;
}

} // namespace

std::vector<LegacyChartResultSummary>
ReplayRepository::ListLegacyChartSummaries(
    const bms_parser::ChartMeta &chartMeta, std::size_t limit) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {};
  }
  return replay_repository_detail::ListLegacyChartSummariesOnConnection(
      impl_->sessionDatabase, chartMeta, limit);
}

std::vector<LegacyChartResultSummary>
replay_repository_detail::ListLegacyChartSummariesOnConnection(
    sqlite3 *database, const bms_parser::ChartMeta &chartMeta,
    std::size_t limit) {
  if (database == nullptr || !validLimit(limit)) {
    return {};
  }
  const std::string sha256 = normalizedHash(chartMeta.SHA256);
  const std::string md5 = normalizedHash(chartMeta.MD5);
  const std::string path =
      Utils::GetStoragePathUtf8RelativeToDocuments(chartMeta.BmsPath, "BMS/");
  if (sha256.empty() && md5.empty() && trimCopy(path).empty()) {
    return {};
  }

  std::string snapshotError;
  SqliteTransactionHandle snapshot(database, "BEGIN TRANSACTION",
                                   snapshotError);
  if (!snapshot.active()) {
    return {};
  }
  std::string query =
      "SELECT legacy_replay_id,chart_path,chart_md5,chart_sha256,chart_title,"
      "chart_artist,long_note_mode,final_score,max_combo,final_gauge,clear_"
      "type,"
      "created_at,ruleset_version,eligibility,provenance_json,partial FROM "
      "legacy_chart_result_summaries WHERE ((";
  query += boundNormalizedHashMatchCondition("chart_sha256");
  query += ") OR (" + boundNormalizedHashMatchCondition("chart_md5") +
           ") OR (" + boundStoredOrLegacyBmsPathMatchCondition("chart_path") +
           ")) ORDER BY legacy_replay_id DESC LIMIT ?";
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK) {
    logReadError("preparing legacy chart summaries", database);
    return {};
  }
  const int limitIndex =
      bindChartIdentity(statement.get(), 1, sha256, md5, path);
  if (sqlite3_bind_int64(statement.get(), limitIndex,
                         static_cast<sqlite3_int64>(limit)) != SQLITE_OK) {
    return {};
  }

  std::vector<LegacyChartResultSummary> rows;
  rows.reserve(limit);
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
    rows.push_back(readChartSummary(statement.get()));
  }
  if (rc != SQLITE_DONE || !snapshot.commit(snapshotError)) {
    logReadError("reading legacy chart summaries", database);
    return {};
  }
  return rows;
}

std::vector<LegacyCourseResultSummary>
ReplayRepository::ListLegacyCourseSummaries(const CourseReplayLookup &lookup,
                                            std::size_t limit) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(impl_->sessionMutex);
  if (!EnsureSessionDatabaseLocked()) {
    return {};
  }
  return replay_repository_detail::ListLegacyCourseSummariesOnConnection(
      impl_->sessionDatabase, lookup, limit);
}

std::vector<LegacyCourseResultSummary>
replay_repository_detail::ListLegacyCourseSummariesOnConnection(
    sqlite3 *database, const CourseReplayLookup &lookup, std::size_t limit) {
  if (database == nullptr || !validLimit(limit) ||
      (lookup.courseKey.empty() && lookup.legacyCourseId <= 0)) {
    return {};
  }
  std::string snapshotError;
  SqliteTransactionHandle snapshot(database, "BEGIN TRANSACTION",
                                   snapshotError);
  if (!snapshot.active()) {
    return {};
  }
  constexpr const char *query =
      "SELECT legacy_course_replay_id,legacy_course_id,course_key,course_name,"
      "course_group_name,constraint_json,final_score,max_combo,final_gauge,"
      "clear_type,completed_charts,total_charts,created_at,ruleset_version,"
      "eligibility,provenance_json,partial FROM "
      "legacy_course_result_summaries WHERE ((?1<>'' AND course_key=?1) OR "
      "(?2>0 AND legacy_course_id=?2 AND "
      "(course_key IS NULL OR course_key=''))) ORDER BY "
      "legacy_course_replay_id DESC LIMIT ?3";
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK ||
      sqlite3_bind_text(statement.get(), 1, lookup.courseKey.c_str(),
                        static_cast<int>(lookup.courseKey.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_int(statement.get(), 2, lookup.legacyCourseId) !=
          SQLITE_OK ||
      sqlite3_bind_int64(statement.get(), 3,
                         static_cast<sqlite3_int64>(limit)) != SQLITE_OK) {
    logReadError("preparing legacy course summaries", database);
    return {};
  }

  std::vector<LegacyCourseResultSummary> rows;
  rows.reserve(limit);
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
    rows.push_back(readCourseSummary(statement.get()));
  }
  if (rc != SQLITE_DONE || !snapshot.commit(snapshotError)) {
    logReadError("reading legacy course summaries", database);
    return {};
  }
  return rows;
}
