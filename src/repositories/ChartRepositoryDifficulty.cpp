// Fill out your copyright notice in the Description page of Project Settings.

#include "ChartRepository.h"
#include "ChartRepositoryInternal.h"
#include "../BmsMetadataText.h"
#include "ChartMetaSql.h"
#include "ChartSqlExpressions.h"
#include "ChartStorageIdentity.h"
#include "../LongNoteModeUtils.h"
#include "ScoreRepository.h"
#include "ScoreCacheQueries.h"
#include "SqliteRAII.h"
#include "../Utils.h"
#include <SDL2/SDL.h>
#include "../path.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <fstream>
#include <iostream>
#include "../../yoga/lib/nlohmann/json.hpp"
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "../targets.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#elif TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#include "../CurlRAII.h"
#else
#include "../CurlRAII.h"
#endif

namespace {
using asobmshow::bms_metadata::normalizedHash;

struct UnambiguousHashEvidence {
  std::string value;
  bool conflicting = false;

  void observe(const std::string &candidate) {
    const std::string normalized = normalizedHash(candidate);
    if (normalized.empty()) {
      return;
    }
    if (value.empty()) {
      value = normalized;
    } else if (value != normalized) {
      conflicting = true;
    }
  }

  void merge(const UnambiguousHashEvidence &other) {
    observe(other.value);
    conflicting = conflicting || other.conflicting;
  }

  [[nodiscard]] std::string resolved() const {
    return conflicting ? std::string() : value;
  }
};

struct ChartHashEvidence {
  UnambiguousHashEvidence sha256;
  UnambiguousHashEvidence md5;

  void observeMatchingCandidate(
      const course_identity::ChartIdentity &stored,
      const course_identity::ChartIdentity &candidate) {
    const course_identity::ChartIdentity normalizedStored{
        .sha256 = normalizedHash(stored.sha256),
        .md5 = normalizedHash(stored.md5),
    };
    const course_identity::ChartIdentity normalizedCandidate{
        .sha256 = normalizedHash(candidate.sha256),
        .md5 = normalizedHash(candidate.md5),
    };
    if (normalizedStored.sha256.empty() &&
        !normalizedStored.md5.empty() &&
        normalizedCandidate.md5 == normalizedStored.md5) {
      sha256.observe(normalizedCandidate.sha256);
    }
    if (normalizedStored.md5.empty() &&
        !normalizedStored.sha256.empty() &&
        normalizedCandidate.sha256 == normalizedStored.sha256) {
      md5.observe(normalizedCandidate.md5);
    }
  }

  void enrichMissing(course_identity::ChartIdentity &stored) const {
    if (stored.sha256.empty()) {
      stored.sha256 = sha256.resolved();
    }
    if (stored.md5.empty()) {
      stored.md5 = md5.resolved();
    }
  }

  void merge(const ChartHashEvidence &other) {
    sha256.merge(other.sha256);
    md5.merge(other.md5);
  }
};

using ChartHashEvidenceLookup =
    std::unordered_map<std::string, ChartHashEvidence>;

std::string chartLookupKey(const std::string &kind, const std::string &hash) {
  return hash.empty() ? "" : kind + ":" + hash;
}

std::string missingCounterpartEvidenceKey(
    const course_identity::ChartIdentity &stored,
    const std::string &keyPrefix = "") {
  const std::string sha256 = normalizedHash(stored.sha256);
  const std::string md5 = normalizedHash(stored.md5);
  if (sha256.empty() && !md5.empty()) {
    return keyPrefix + chartLookupKey("md5", md5);
  }
  if (md5.empty() && !sha256.empty()) {
    return keyPrefix + chartLookupKey("sha256", sha256);
  }
  return {};
}

void logSqlErrorText(const char *context, const std::string &error) {
  std::cerr << "SQL error while " << context << ": " << error << "\n";
}

void logSqlError(const char *context, sqlite3 *db) {
  logSqlErrorText(context, sqliteDatabaseError(db));
}

void logSdlSqlErrorText(const char *context, const std::string &error) {
  SDL_Log("SQL error while %s: %s", context, error.c_str());
}

void logSdlSqlError(const char *context, sqlite3 *db) {
  logSdlSqlErrorText(context, sqliteDatabaseError(db));
}

bool execSql(sqlite3 *db, const char *query, const char *context) {
  return executeSqliteLogged(db, query, context, logSqlErrorText);
}

bool execSqlAllowDuplicateColumn(sqlite3 *db, const char *query,
                                 const char *context) {
  return executeSqliteLogged(db, query, context, logSqlErrorText,
                             "duplicate column name");
}
bool sqliteTableExists(sqlite3 *db, const char *tableName, bool &exists,
                       const char *context) {
  if (const auto error = querySqliteTableExists(db, tableName, exists)) {
    logSqlErrorText(context, *error);
    return false;
  }
  return true;
}

int columnInt(sqlite3_stmt *stmt, int idx) {
  return sqlite3_column_type(stmt, idx) == SQLITE_NULL
             ? 0
             : sqlite3_column_int(stmt, idx);
}

std::string columnString(sqlite3_stmt *stmt, int idx) {
  return sqliteColumnString(stmt, idx);
}
bool clearDifficultyTableContent(sqlite3 *db, int tableId) {
  auto deleteCourseEntries =
      "DELETE FROM difficulty_course_entries WHERE course_id IN "
      "(SELECT id FROM difficulty_courses WHERE table_id = @table_id)";
  SqliteStatementHandle stmt;
  int rc = prepareSqliteStatement(db, deleteCourseEntries, stmt);
  if (rc != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);
  rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    return false;
  }

  auto deleteCourses =
      "DELETE FROM difficulty_courses WHERE table_id = @table_id";
  rc = prepareSqliteStatement(db, deleteCourses, stmt);
  if (rc != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);
  rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    return false;
  }

  auto deleteEntries =
      "DELETE FROM difficulty_table_entries WHERE table_id = @table_id";
  rc = prepareSqliteStatement(db, deleteEntries, stmt);
  if (rc != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);
  rc = sqlite3_step(stmt.get());
  return rc == SQLITE_DONE;
}

static bool backfillDifficultyCourseKeys(sqlite3 *db) {
  const char *query =
      "SELECT dc.id, dc.constraint_json, dce.id, dce.sha256, dce.md5 "
      "FROM difficulty_courses dc "
      "LEFT JOIN difficulty_course_entries dce ON dce.course_id = dc.id "
      "WHERE COALESCE(TRIM(dc.course_key), '') = '' "
      "ORDER BY dc.table_id, dc.sort_order, dc.id, dce.sort_order, dce.id";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "reading blank difficulty course keys",
                                    logSqlErrorText)) {
    return false;
  }

  std::vector<course_identity::Definition> definitions;
  course_identity::Definition current;
  const auto retainCurrentCourse = [&]() {
    if (current.courseId <= 0) {
      return;
    }
    definitions.push_back(std::move(current));
    current = {};
  };

  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
    const int courseId = columnInt(stmt.get(), 0);
    if (courseId != current.courseId) {
      retainCurrentCourse();
      current.courseId = courseId;
      current.constraintJson = columnString(stmt.get(), 1);
    }
    if (sqlite3_column_type(stmt.get(), 2) != SQLITE_NULL) {
      current.charts.push_back({.sha256 = columnString(stmt.get(), 3),
                                .md5 = columnString(stmt.get(), 4)});
    }
  }
  if (rc != SQLITE_DONE) {
    return false;
  }
  retainCurrentCourse();

  const char *updateQuery =
      "UPDATE difficulty_courses SET course_key = @course_key "
      "WHERE id = @course_id AND COALESCE(TRIM(course_key), '') = ''";
  SqliteStatementHandle updateStmt;
  if (!prepareSqliteStatementLogged(db, updateQuery, updateStmt,
                                    "backfilling difficulty course keys",
                                    logSqlErrorText)) {
    return false;
  }
  for (const auto &definition : definitions) {
    const std::string courseKey = course_identity::makeCourseKey(
        definition.charts, definition.constraintJson);
    if (courseKey.empty()) {
      continue;
    }
    sqlite3_reset(updateStmt.get());
    sqlite3_clear_bindings(updateStmt.get());
    bindSqliteText(updateStmt.get(), 1, courseKey);
    sqlite3_bind_int(updateStmt.get(), 2, definition.courseId);
    if (sqlite3_step(updateStmt.get()) != SQLITE_DONE) {
      return false;
    }
  }
  return true;
}

bool ensureDifficultySchema(sqlite3 *db) {
  const char *createTables[] = {
      "CREATE TABLE IF NOT EXISTS difficulty_tables ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "name TEXT NOT NULL,"
      "symbol TEXT NOT NULL,"
      "data_url TEXT NOT NULL DEFAULT '',"
      "source_url TEXT NOT NULL DEFAULT '',"
      "updated_at TEXT,"
      "UNIQUE(name, symbol, source_url)"
      ")",
      "CREATE TABLE IF NOT EXISTS difficulty_table_entries ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "table_id INTEGER NOT NULL,"
      "level TEXT NOT NULL DEFAULT '',"
      "md5 TEXT NOT NULL DEFAULT '',"
      "sha256 TEXT NOT NULL DEFAULT '',"
      "title TEXT,"
      "subtitle TEXT,"
      "artist TEXT,"
      "subartist TEXT,"
      "url TEXT,"
      "url_diff TEXT,"
      "sort_order INTEGER NOT NULL DEFAULT 0"
      ")",
      "CREATE TABLE IF NOT EXISTS difficulty_courses ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "table_id INTEGER NOT NULL,"
      "name TEXT NOT NULL,"
      "group_name TEXT NOT NULL DEFAULT '',"
      "level TEXT NOT NULL DEFAULT '',"
      "constraint_json TEXT NOT NULL DEFAULT '[]',"
      "course_key TEXT NOT NULL DEFAULT '',"
      "sort_order INTEGER NOT NULL DEFAULT 0"
      ")",
      "CREATE TABLE IF NOT EXISTS difficulty_course_entries ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "course_id INTEGER NOT NULL,"
      "level TEXT NOT NULL DEFAULT '',"
      "md5 TEXT NOT NULL DEFAULT '',"
      "sha256 TEXT NOT NULL DEFAULT '',"
      "title TEXT,"
      "subtitle TEXT,"
      "artist TEXT,"
      "subartist TEXT,"
      "url TEXT,"
      "url_diff TEXT,"
      "sort_order INTEGER NOT NULL DEFAULT 0"
      ")",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_table_level "
      "ON difficulty_table_entries(table_id, level)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_table_sort_order "
      "ON difficulty_table_entries(table_id, sort_order)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_table_level_sort_order "
      "ON difficulty_table_entries(table_id, level, sort_order)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_table_sha256 "
      "ON difficulty_table_entries(table_id, sha256)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_table_md5 "
      "ON difficulty_table_entries(table_id, md5)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_md5 "
      "ON difficulty_table_entries(md5)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_entries_sha256 "
      "ON difficulty_table_entries(sha256)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_courses_group "
      "ON difficulty_courses(table_id, group_name)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_courses_table_sort_order "
      "ON difficulty_courses(table_id, sort_order, id)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_courses_group_sort_order "
      "ON difficulty_courses(table_id, group_name, sort_order, id)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_course_entries_course "
      "ON difficulty_course_entries(course_id)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_course_entries_course_sort_order "
      "ON difficulty_course_entries(course_id, sort_order, id)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_course_entries_course_sha256 "
      "ON difficulty_course_entries(course_id, sha256)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_course_entries_course_md5 "
      "ON difficulty_course_entries(course_id, md5)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_course_entries_md5 "
      "ON difficulty_course_entries(md5)",
      "CREATE INDEX IF NOT EXISTS idx_difficulty_course_entries_sha256 "
      "ON difficulty_course_entries(sha256)",
  };

  for (const auto *query : createTables) {
    if (!execSql(db, query, "creating difficulty table schema")) {
      return false;
    }
  }
  const char *courseEntryMigrations[] = {
      "ALTER TABLE difficulty_course_entries ADD COLUMN title TEXT",
      "ALTER TABLE difficulty_course_entries ADD COLUMN subtitle TEXT",
      "ALTER TABLE difficulty_course_entries ADD COLUMN artist TEXT",
      "ALTER TABLE difficulty_course_entries ADD COLUMN subartist TEXT",
      "ALTER TABLE difficulty_course_entries ADD COLUMN url TEXT",
      "ALTER TABLE difficulty_course_entries ADD COLUMN url_diff TEXT",
  };
  for (const auto *query : courseEntryMigrations) {
    if (!execSqlAllowDuplicateColumn(
            db, query, "migrating difficulty course entry schema")) {
      return false;
    }
  }
  if (!execSqlAllowDuplicateColumn(
          db,
          "ALTER TABLE difficulty_courses "
          "ADD COLUMN course_key TEXT NOT NULL DEFAULT ''",
          "migrating difficulty course key schema")) {
    return false;
  }
  if (!execSql(db,
               "CREATE INDEX IF NOT EXISTS idx_difficulty_courses_key "
               "ON difficulty_courses(course_key)",
               "creating difficulty course key index")) {
    return false;
  }
  return backfillDifficultyCourseKeys(db);
}

} // namespace


bool chart_repository_detail::EnsureDifficultySchema(sqlite3 *database) {
  return ensureDifficultySchema(database);
}

namespace {
bool deleteDifficultyTable(sqlite3 *database, int tableId);
std::vector<DifficultyTableInfo> selectDifficultyTables(sqlite3 *database);
std::vector<DifficultyLevelInfo> selectDifficultyLevels(sqlite3 *database,
                                                        int tableId);
std::vector<DifficultyCourseTableInfo>
selectDifficultyCourseTables(sqlite3 *database);
std::vector<DifficultyCourseGroupInfo>
selectDifficultyCourseGroups(sqlite3 *database, int tableId);
std::vector<DifficultyCourseInfo>
selectDifficultyCourses(sqlite3 *database, int tableId,
                        const std::string &groupName);
std::vector<course_identity::Definition>
selectDifficultyCourseDefinitions(sqlite3 *database);
} // namespace

bool ChartRepository::Session::DeleteDifficultyTable(int tableId) {
  return deleteDifficultyTable(impl_->database(), tableId);
}

std::vector<DifficultyTableInfo>
ChartRepository::Session::SelectDifficultyTables() {
  return selectDifficultyTables(impl_->database());
}

std::vector<DifficultyLevelInfo>
ChartRepository::Session::SelectDifficultyLevels(int tableId) {
  return selectDifficultyLevels(impl_->database(), tableId);
}

std::vector<DifficultyCourseTableInfo>
ChartRepository::Session::SelectDifficultyCourseTables() {
  return selectDifficultyCourseTables(impl_->database());
}

std::vector<DifficultyCourseGroupInfo>
ChartRepository::Session::SelectDifficultyCourseGroups(int tableId) {
  return selectDifficultyCourseGroups(impl_->database(), tableId);
}

std::vector<DifficultyCourseInfo>
ChartRepository::Session::SelectDifficultyCourses(
    int tableId, const std::string &groupName) {
  return selectDifficultyCourses(impl_->database(), tableId, groupName);
}

std::vector<course_identity::Definition>
ChartRepository::Session::SelectDifficultyCourseDefinitions() {
  return selectDifficultyCourseDefinitions(impl_->database());
}

namespace {
bool deleteDifficultyTable(sqlite3 *db, int tableId) {
  if (tableId <= 0 ||
      !chart_repository_detail::EnsureDifficultySchema(db)) {
    return false;
  }

  chart_repository_detail::InvalidateDifficultyLabelCache();
  std::string transactionError;
  SqliteTransactionHandle transaction(db, "BEGIN", transactionError);
  if (!transaction.active()) {
    SDL_Log("Failed to start difficulty table delete transaction: %s",
            transactionError.c_str());
    return false;
  }
  if (!clearDifficultyTableContent(db, tableId)) {
    return false;
  }

  auto query = "DELETE FROM difficulty_tables WHERE id = @id";
  SqliteStatementHandle stmt;
  int rc = prepareSqliteStatement(db, query, stmt);
  if (rc != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);
  rc = sqlite3_step(stmt.get());
  const bool deleted = rc == SQLITE_DONE && sqlite3_changes(db) > 0;
  if (!deleted) {
    return false;
  }

  if (!transaction.commit(transactionError)) {
    SDL_Log("Failed to commit difficulty table delete transaction: %s",
            transactionError.c_str());
    return false;
  }
  chart_repository_detail::BumpLibraryRevision();
  return true;
}

std::vector<DifficultyTableInfo>
selectDifficultyTables(sqlite3 *db) {
  if (!chart_repository_detail::EnsureDifficultySchema(db)) {
    return {};
  }
  auto query = "SELECT dt.id, dt.name, dt.symbol, dt.source_url, "
               "COUNT(dte.id) "
               "FROM difficulty_tables dt "
               "LEFT JOIN difficulty_table_entries dte ON dte.table_id = dt.id "
               "GROUP BY dt.id "
               "ORDER BY dt.name COLLATE NOCASE";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting difficulty tables",
                                    logSqlErrorText)) {
    return {};
  }

  std::vector<DifficultyTableInfo> tables;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    DifficultyTableInfo table;
    table.id = columnInt(stmt.get(), 0);
    table.name = columnString(stmt.get(), 1);
    table.symbol = columnString(stmt.get(), 2);
    table.sourceUrl = columnString(stmt.get(), 3);
    table.chartCount = columnInt(stmt.get(), 4);
    tables.push_back(std::move(table));
  }
  return tables;
}

std::vector<DifficultyLevelInfo>
selectDifficultyLevels(sqlite3 *db, int tableId) {
  if (!chart_repository_detail::EnsureDifficultySchema(db)) {
    return {};
  }
  auto query = "SELECT dte.table_id, dt.name, dt.symbol, dte.level, "
               "COUNT(dte.id), MIN(dte.sort_order) "
               "FROM difficulty_table_entries dte "
               "JOIN difficulty_tables dt ON dt.id = dte.table_id "
               "WHERE dte.table_id = @table_id "
               "GROUP BY dte.table_id, dte.level "
               "ORDER BY MIN(dte.sort_order), dte.level";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting difficulty levels",
                                    logSqlErrorText)) {
    return {};
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);

  std::vector<DifficultyLevelInfo> levels;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    DifficultyLevelInfo level;
    level.tableId = columnInt(stmt.get(), 0);
    level.tableName = columnString(stmt.get(), 1);
    level.tableSymbol = columnString(stmt.get(), 2);
    level.level = columnString(stmt.get(), 3);
    level.chartCount = columnInt(stmt.get(), 4);
    levels.push_back(std::move(level));
  }
  return levels;
}

std::vector<DifficultyCourseTableInfo>
selectDifficultyCourseTables(sqlite3 *db) {
  if (!chart_repository_detail::EnsureDifficultySchema(db)) {
    return {};
  }
  std::string query =
      "SELECT dc.table_id, dt.name, dt.symbol "
      "FROM difficulty_courses dc "
      "JOIN difficulty_tables dt ON dt.id = dc.table_id "
      "GROUP BY dc.table_id "
      "ORDER BY dt.name COLLATE NOCASE, MIN(dc.sort_order)";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting difficulty course tables",
                                    logSqlErrorText)) {
    return {};
  }

  std::vector<DifficultyCourseTableInfo> tables;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    DifficultyCourseTableInfo table;
    table.tableId = columnInt(stmt.get(), 0);
    table.tableName = columnString(stmt.get(), 1);
    table.tableSymbol = columnString(stmt.get(), 2);
    tables.push_back(std::move(table));
  }
  return tables;
}

std::vector<DifficultyCourseGroupInfo>
selectDifficultyCourseGroups(sqlite3 *db, int tableId) {
  if (!chart_repository_detail::EnsureDifficultySchema(db)) {
    return {};
  }
  std::string query =
      "SELECT dc.table_id, dt.name, dc.group_name, "
      "COUNT(dc.id), MIN(dc.id), MIN(dc.course_key), MIN(dc.level), "
      "MIN(dc.name), MIN(dc.constraint_json) "
      "FROM difficulty_courses dc "
      "JOIN difficulty_tables dt ON dt.id = dc.table_id "
      "WHERE dc.table_id = @table_id "
      "GROUP BY dc.table_id, dc.group_name "
      "ORDER BY MIN(dc.sort_order), dc.group_name COLLATE NOCASE";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting difficulty course groups",
                                    logSqlErrorText)) {
    return {};
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);

  std::vector<DifficultyCourseGroupInfo> groups;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    DifficultyCourseGroupInfo group;
    group.tableId = columnInt(stmt.get(), 0);
    group.tableName = columnString(stmt.get(), 1);
    group.groupName = columnString(stmt.get(), 2);
    group.courseCount = columnInt(stmt.get(), 3);
    group.singletonCourseId = group.courseCount == 1 ? columnInt(stmt.get(), 4)
                                                     : 0;
    group.singletonCourseKey =
        group.courseCount == 1 ? columnString(stmt.get(), 5) : "";
    group.singletonCourseLevel =
        group.courseCount == 1 ? columnString(stmt.get(), 6) : "";
    group.singletonCourseName =
        group.courseCount == 1 ? columnString(stmt.get(), 7) : "";
    group.singletonCourseConstraintJson =
        group.courseCount == 1 ? columnString(stmt.get(), 8) : "";
    groups.push_back(std::move(group));
  }
  return groups;
}

std::vector<DifficultyCourseInfo>
selectDifficultyCourses(sqlite3 *db, int tableId,
                        const std::string &groupName) {
  if (!chart_repository_detail::EnsureDifficultySchema(db)) {
    return {};
  }
  std::string query =
      "SELECT dc.id, dc.course_key, dc.table_id, dt.name, dc.group_name, "
      "dc.level, dc.name, dc.constraint_json "
      "FROM difficulty_courses dc "
      "JOIN difficulty_tables dt ON dt.id = dc.table_id "
      "WHERE dc.table_id = @table_id AND dc.group_name = @group_name "
      "ORDER BY dc.sort_order, dc.id";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting difficulty courses",
                                    logSqlErrorText)) {
    return {};
  }
  sqlite3_bind_int(stmt.get(), 1, tableId);
  bindSqliteText(stmt.get(), 2, groupName);

  std::vector<DifficultyCourseInfo> courses;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    DifficultyCourseInfo course;
    course.id = columnInt(stmt.get(), 0);
    course.courseKey = columnString(stmt.get(), 1);
    course.tableId = columnInt(stmt.get(), 2);
    course.tableName = columnString(stmt.get(), 3);
    course.groupName = columnString(stmt.get(), 4);
    course.level = columnString(stmt.get(), 5);
    course.name = columnString(stmt.get(), 6);
    course.constraintJson = columnString(stmt.get(), 7);
    courses.push_back(std::move(course));
  }
  return courses;
}

std::vector<course_identity::Definition>
selectDifficultyCourseDefinitions(sqlite3 *db) {
  if (!chart_repository_detail::EnsureDifficultySchema(db)) {
    return {};
  }

  bool chartMetaExists = false;
  if (!sqliteTableExists(db, "chart_meta", chartMetaExists,
                         "checking chart metadata for course definitions")) {
    return {};
  }

  const char *query =
      "SELECT dc.id, dc.course_key, dc.name, dc.group_name, "
      "dc.constraint_json, dce.id, dce.sha256, dce.md5, dc.table_id "
      "FROM difficulty_courses dc "
      "LEFT JOIN difficulty_course_entries dce ON dce.course_id = dc.id "
      "ORDER BY dc.table_id, dc.sort_order, dc.id, dce.sort_order, dce.id";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting difficulty course definitions",
                                    logSqlErrorText)) {
    return {};
  }

  struct EntryEvidenceLocation {
    std::size_t definitionIndex = 0;
    std::size_t chartIndex = 0;
    std::string tableEvidenceKey;
    std::string localEvidenceKey;
  };
  struct HashEvidenceRequest {
    int tableId = 0;
    bool matchSha256 = false;
    std::string hash;
  };
  std::vector<course_identity::Definition> definitions;
  std::vector<EntryEvidenceLocation> evidenceLocations;
  std::unordered_map<std::string, HashEvidenceRequest> tableEvidenceRequests;
  std::unordered_map<std::string, HashEvidenceRequest> localEvidenceRequests;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
    const int courseId = columnInt(stmt.get(), 0);
    if (definitions.empty() || definitions.back().courseId != courseId) {
      definitions.push_back({
          .courseId = courseId,
          .courseKey = columnString(stmt.get(), 1),
          .name = columnString(stmt.get(), 2),
          .groupName = columnString(stmt.get(), 3),
          .constraintJson = columnString(stmt.get(), 4),
      });
    }
    if (sqlite3_column_type(stmt.get(), 5) != SQLITE_NULL) {
      auto &definition = definitions.back();
      definition.charts.push_back(
          {.sha256 = normalizedHash(columnString(stmt.get(), 6)),
           .md5 = normalizedHash(columnString(stmt.get(), 7))});
      const auto &stored = definition.charts.back();
      const int tableId = columnInt(stmt.get(), 8);
      const std::string tablePrefix =
          std::to_string(tableId) + ":";
      const std::string tableEvidenceKey =
          missingCounterpartEvidenceKey(stored, tablePrefix);
      const std::string localEvidenceKey =
          missingCounterpartEvidenceKey(stored, "local:");
      if (!tableEvidenceKey.empty()) {
        const bool matchSha256 = !stored.sha256.empty();
        const std::string &hash =
            matchSha256 ? stored.sha256 : stored.md5;
        tableEvidenceRequests.try_emplace(
            tableEvidenceKey,
            HashEvidenceRequest{.tableId = tableId,
                                .matchSha256 = matchSha256,
                                .hash = hash});
        localEvidenceRequests.try_emplace(
            localEvidenceKey,
            HashEvidenceRequest{.matchSha256 = matchSha256, .hash = hash});
        evidenceLocations.push_back({
            .definitionIndex = definitions.size() - 1,
            .chartIndex = definition.charts.size() - 1,
            .tableEvidenceKey = tableEvidenceKey,
            .localEvidenceKey = localEvidenceKey,
        });
      }
    }
  }
  if (rc != SQLITE_DONE) {
    return {};
  }

  ChartHashEvidenceLookup evidenceByHash;
  if (!tableEvidenceRequests.empty()) {
    SqliteStatementHandle tableMd5Stmt;
    SqliteStatementHandle tableSha256Stmt;
    const char *tableMd5Query =
        "SELECT sha256, md5 FROM difficulty_table_entries "
        "WHERE table_id = @table_id AND md5 = @hash "
        "ORDER BY sort_order, id";
    const char *tableSha256Query =
        "SELECT sha256, md5 FROM difficulty_table_entries "
        "WHERE table_id = @table_id AND sha256 = @hash "
        "ORDER BY sort_order, id";
    if (!prepareSqliteStatementLogged(
            db, tableMd5Query, tableMd5Stmt,
            "preparing difficulty table MD5 evidence lookup",
            logSqlErrorText) ||
        !prepareSqliteStatementLogged(
            db, tableSha256Query, tableSha256Stmt,
            "preparing difficulty table SHA-256 evidence lookup",
            logSqlErrorText)) {
      return {};
    }
    for (const auto &[evidenceKey, request] : tableEvidenceRequests) {
      sqlite3_stmt *lookupStmt = request.matchSha256
                                     ? tableSha256Stmt.get()
                                     : tableMd5Stmt.get();
      sqlite3_reset(lookupStmt);
      sqlite3_clear_bindings(lookupStmt);
      sqlite3_bind_int(lookupStmt, 1, request.tableId);
      bindSqliteText(lookupStmt, 2, request.hash);
      const course_identity::ChartIdentity stored =
          request.matchSha256
              ? course_identity::ChartIdentity{.sha256 = request.hash}
              : course_identity::ChartIdentity{.md5 = request.hash};
      while ((rc = sqlite3_step(lookupStmt)) == SQLITE_ROW) {
        evidenceByHash[evidenceKey].observeMatchingCandidate(
            stored,
            {.sha256 = normalizedHash(columnString(lookupStmt, 0)),
             .md5 = normalizedHash(columnString(lookupStmt, 1))});
      }
      if (rc != SQLITE_DONE) {
        return {};
      }
    }
  }

  if (chartMetaExists && !localEvidenceRequests.empty()) {
    SqliteStatementHandle localMd5Stmt;
    SqliteStatementHandle localSha256Stmt;
    const char *localMd5Query =
        "SELECT sha256, md5 FROM chart_meta WHERE md5 = @hash ORDER BY path";
    const char *localSha256Query =
        "SELECT sha256, md5 FROM chart_meta WHERE sha256 = @hash ORDER BY path";
    if (!prepareSqliteStatementLogged(db, localMd5Query, localMd5Stmt,
                                      "preparing local MD5 evidence lookup",
                                      logSqlErrorText) ||
        !prepareSqliteStatementLogged(
            db, localSha256Query, localSha256Stmt,
            "preparing local SHA-256 evidence lookup", logSqlErrorText)) {
      return {};
    }
    for (const auto &[evidenceKey, request] : localEvidenceRequests) {
      sqlite3_stmt *lookupStmt = request.matchSha256
                                     ? localSha256Stmt.get()
                                     : localMd5Stmt.get();
      sqlite3_reset(lookupStmt);
      sqlite3_clear_bindings(lookupStmt);
      bindSqliteText(lookupStmt, 1, request.hash);
      const course_identity::ChartIdentity stored =
          request.matchSha256
              ? course_identity::ChartIdentity{.sha256 = request.hash}
              : course_identity::ChartIdentity{.md5 = request.hash};
      while ((rc = sqlite3_step(lookupStmt)) == SQLITE_ROW) {
        evidenceByHash[evidenceKey].observeMatchingCandidate(
            stored,
            {.sha256 = normalizedHash(columnString(lookupStmt, 0)),
             .md5 = normalizedHash(columnString(lookupStmt, 1))});
      }
      if (rc != SQLITE_DONE) {
        return {};
      }
    }
  }

  for (const auto &location : evidenceLocations) {
    ChartHashEvidence combined;
    if (const auto tableEvidence =
            evidenceByHash.find(location.tableEvidenceKey);
        tableEvidence != evidenceByHash.end()) {
      combined.merge(tableEvidence->second);
    }
    if (const auto localEvidence =
            evidenceByHash.find(location.localEvidenceKey);
        localEvidence != evidenceByHash.end()) {
      combined.merge(localEvidence->second);
    }
    combined.enrichMissing(definitions[location.definitionIndex]
                               .charts[location.chartIndex]);
  }
  return definitions;
}
} // namespace
