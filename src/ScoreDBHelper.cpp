#include "ScoreDBHelper.h"

#include "BmsMetadataText.h"
#include "ChartDBHelper.h"
#include "ChartSqlExpressions.h"
#include "CoursePlaySession.h"
#include "LongNoteModeUtils.h"
#include "ProfileDatabaseActivity.h"
#include "ReplayDBHelper.h"
#include "ResultPersistenceModel.h"
#include "ScoreCacheQueries.h"
#include "SqliteRAII.h"
#include "Utils.h"
#include "Uuid.h"
#include "path.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
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
constexpr int kLegacyScoreDatabaseSchemaVersion = 4;
constexpr int kScoreProvenanceSchemaVersion = 5;
constexpr int kScoreCourseIdentitySchemaVersion = 6;
constexpr int kScoreSummarySemanticsSchemaVersion = 7;
constexpr int kScoreBestEligibilitySchemaVersion = 8;
constexpr int kScoreAttemptIdentitySchemaVersion = 9;
constexpr int kScoreDatabaseSchemaVersion =
    ScoreDBHelper::kCurrentSchemaVersion;
constexpr const char *kScoreMigrationChartSchema = "score_migration_chart";
constexpr const char *kLegacyProvenanceJson =
    "{\"schemaVersion\":1,\"ruleset\":{\"version\":0},\"stages\":[],"
    "\"eligibility\":\"legacy-unverified\"}";

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

bool ensureScoreDatabaseDirectory(const std::filesystem::path &path,
                                  std::string &errorMessage) {
  const std::filesystem::path directory = path.parent_path();
  std::error_code directoryError;
  if (!directory.empty() &&
      !Utils::EnsureDirectoryExists(directory, directoryError)) {
    errorMessage =
        "can't create score database directory: " + directoryError.message();
    return false;
  }
  return true;
}

sqlite3 *openScoreDatabase(const std::filesystem::path &path,
                           std::string &errorMessage) {
  if (!ensureScoreDatabaseDirectory(path, errorMessage)) {
    return nullptr;
  }

  return openValidatedSqliteDatabase(path, kScoreDatabaseSchemaVersion, false,
                                     errorMessage);
}

sqlite3 *openTrustedScoreDatabase(const std::filesystem::path &path,
                                  std::string &errorMessage) {
  sqlite3 *raw = nullptr;
  const std::string pathText = fspath_to_utf8(path);
  const int openRc = sqlite3_open_v2(
      pathText.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_PRIVATECACHE,
      nullptr);
  SqliteConnectionHandle connection(raw);
  if (openRc != SQLITE_OK || connection.get() == nullptr) {
    errorMessage = raw != nullptr ? sqlite3_errmsg(raw)
                                  : "could not open trusted score database";
    return nullptr;
  }

  int noCheckpointOnClose = 0;
  if (sqlite3_db_config(connection.get(), SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1,
                        &noCheckpointOnClose) != SQLITE_OK ||
      noCheckpointOnClose != 1) {
    errorMessage = "could not configure score checkpoint-on-close";
    return nullptr;
  }
  sqlite3_busy_timeout(connection.get(), 1000);
  std::string versionError;
  const auto version = readSqliteUserVersion(connection.get(), versionError);
  if (!version.has_value() || *version != kScoreDatabaseSchemaVersion) {
    errorMessage =
        version.has_value()
            ? "trusted score database schema version changed"
            : "could not read trusted score database version: " + versionError;
    return nullptr;
  }
  SqliteStatementHandle journalMode;
  if (prepareSqliteStatement(connection.get(), "PRAGMA journal_mode",
                             journalMode) != SQLITE_OK ||
      sqlite3_step(journalMode.get()) != SQLITE_ROW ||
      sqliteColumnString(journalMode.get(), 0) != "wal") {
    errorMessage = "trusted score database is not in WAL mode";
    return nullptr;
  }
  return connection.release();
}

void logScoreDatabaseOpenFailure(const std::filesystem::path &path,
                                 const std::string &errorMessage) {
  SDL_Log("Refusing to open score database %s: %s",
          fspath_to_utf8(path).c_str(), errorMessage.c_str());
}

std::filesystem::path
resolvedScoreDatabasePath(const std::filesystem::path &databasePath) {
  return databasePath.empty() ? Utils::GetDocumentsPath("db") / "score.db"
                              : databasePath;
}

std::filesystem::path
normalizedScoreDatabasePath(const std::filesystem::path &databasePath) {
  std::filesystem::path path = resolvedScoreDatabasePath(databasePath);
  std::error_code absoluteError;
  const std::filesystem::path absolutePath =
      std::filesystem::absolute(path, absoluteError);
  if (!absoluteError) {
    path = absolutePath;
  }
  std::error_code canonicalError;
  const std::filesystem::path canonicalPath =
      std::filesystem::weakly_canonical(path, canonicalError);
  return (canonicalError ? path : canonicalPath).lexically_normal();
}

bool equivalentScoreDatabasePaths(const std::filesystem::path &first,
                                  const std::filesystem::path &second) {
  const std::filesystem::path firstResolved = resolvedScoreDatabasePath(first);
  const std::filesystem::path secondResolved =
      resolvedScoreDatabasePath(second);
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
  return normalizedScoreDatabasePath(first) ==
         normalizedScoreDatabasePath(second);
}

bool execSql(sqlite3 *db, const char *query, const char *context) {
  return executeSqliteLogged(db, query, context, logSqlErrorText);
}

bool execSqlAllowDuplicateColumn(sqlite3 *db, const char *query,
                                 const char *context) {
  return executeSqliteLogged(db, query, context, logSqlErrorText,
                             "duplicate column name");
}

bool setDatabaseUserVersion(sqlite3 *db, int version) {
  const std::string query =
      "PRAGMA user_version = " + std::to_string(std::max(0, version));
  return execSql(db, query.c_str(), "updating score database version");
}

bool rejectFutureScoreDatabase(sqlite3 *db) {
  // Connect() performs the guarded path-level preflight before returning an
  // owned handle. Schema helpers must inspect that handle directly instead of
  // snapshotting the same database family again.
  std::string error;
  const auto version = readSqliteUserVersion(db, error);
  if (!version.has_value()) {
    SDL_Log("Refusing score database with unreadable version: %s",
            error.c_str());
    return true;
  }
  if (*version <= kScoreDatabaseSchemaVersion) {
    return false;
  }
  SDL_Log("Refusing future score database version %d (supported: %d)", *version,
          kScoreDatabaseSchemaVersion);
  return true;
}

bool ensureScoreProvenanceColumns(sqlite3 *db, const char *tableName) {
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
  return ensureSqliteTableColumnLogged(
             db, tableName, "ruleset_version", addRuleset.c_str(),
             "reading score provenance schema",
             "adding score ruleset version column", logSqlErrorText) &&
         ensureSqliteTableColumnLogged(
             db, tableName, "eligibility", addEligibility.c_str(),
             "reading score provenance schema",
             "adding score eligibility column", logSqlErrorText) &&
         ensureSqliteTableColumnLogged(
             db, tableName, "provenance_json", addProvenance.c_str(),
             "reading score provenance schema",
             "adding score provenance JSON column", logSqlErrorText);
}

bool migrateScoreDatabaseToVersion5(sqlite3 *db) {
  std::string versionError;
  const auto version = readSqliteUserVersion(db, versionError);
  if (!version.has_value()) {
    logSqlErrorText("reading score provenance migration version", versionError);
    return false;
  }
  if (*version > kScoreDatabaseSchemaVersion) {
    return false;
  }
  if (*version >= kScoreProvenanceSchemaVersion) {
    return true;
  }
  if (*version < kLegacyScoreDatabaseSchemaVersion) {
    SDL_Log("Score database must reach version %d before provenance migration",
            kLegacyScoreDatabaseSchemaVersion);
    return false;
  }

  const bool callerOwnsTransaction = sqlite3_get_autocommit(db) == 0;
  const char *beginQuery = callerOwnsTransaction
                               ? "SAVEPOINT asobmashow_score_provenance_v5"
                               : "BEGIN IMMEDIATE TRANSACTION";
  const char *commitQuery = callerOwnsTransaction
                                ? "RELEASE asobmashow_score_provenance_v5"
                                : "COMMIT";
  const char *rollbackQuery =
      callerOwnsTransaction
          ? "ROLLBACK TO asobmashow_score_provenance_v5; RELEASE "
            "asobmashow_score_provenance_v5"
          : "ROLLBACK";
  std::string transactionError;
  SqliteTransactionHandle transaction(db, beginQuery, transactionError,
                                      commitQuery, rollbackQuery);
  if (!transaction.active()) {
    logSqlErrorText("starting score provenance migration", transactionError);
    return false;
  }
  if (!ensureScoreProvenanceColumns(db, "scores") ||
      !ensureScoreProvenanceColumns(db, "course_scores") ||
      !setDatabaseUserVersion(db, kScoreProvenanceSchemaVersion)) {
    return false;
  }
  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing score provenance migration", transactionError);
    return false;
  }
  return true;
}

bool migrateScoreDatabaseToVersion6(sqlite3 *db) {
  std::string versionError;
  const auto version = readSqliteUserVersion(db, versionError);
  if (!version.has_value()) {
    logSqlErrorText("reading course score identity migration version",
                    versionError);
    return false;
  }
  if (*version > kScoreDatabaseSchemaVersion) {
    return false;
  }
  if (*version >= kScoreCourseIdentitySchemaVersion) {
    return true;
  }
  if (*version < kScoreProvenanceSchemaVersion) {
    SDL_Log("Score database must reach version %d before course identity "
            "migration",
            kScoreProvenanceSchemaVersion);
    return false;
  }

  const bool callerOwnsTransaction = sqlite3_get_autocommit(db) == 0;
  const char *beginQuery = callerOwnsTransaction
                               ? "SAVEPOINT asobmashow_score_course_identity_v6"
                               : "BEGIN IMMEDIATE TRANSACTION";
  const char *commitQuery = callerOwnsTransaction
                                ? "RELEASE asobmashow_score_course_identity_v6"
                                : "COMMIT";
  const char *rollbackQuery =
      callerOwnsTransaction
          ? "ROLLBACK TO asobmashow_score_course_identity_v6; RELEASE "
            "asobmashow_score_course_identity_v6"
          : "ROLLBACK";
  std::string transactionError;
  SqliteTransactionHandle transaction(db, beginQuery, transactionError,
                                      commitQuery, rollbackQuery);
  if (!transaction.active()) {
    logSqlErrorText("starting course score identity migration",
                    transactionError);
    return false;
  }

  if (!ensureSqliteTableColumnLogged(
          db, "course_scores", "legacy_course_key",
          "ALTER TABLE course_scores ADD COLUMN legacy_course_key TEXT NOT "
          "NULL DEFAULT ''",
          "reading course score identity schema",
          "adding legacy course score key column", logSqlErrorText) ||
      !ensureSqliteTableColumnLogged(
          db, "course_scores", "ln_mode",
          "ALTER TABLE course_scores ADD COLUMN ln_mode INTEGER NOT NULL "
          "DEFAULT -1",
          "reading course score identity schema",
          "adding course score long note mode column", logSqlErrorText)) {
    return false;
  }

  struct CourseKeyMigrationRow {
    sqlite3_int64 id = 0;
    std::string courseKey;
    std::string legacyCourseKey;
  };
  std::vector<CourseKeyMigrationRow> rows;
  SqliteStatementHandle selectStmt;
  if (!prepareSqliteStatementLogged(
          db,
          "SELECT id, COALESCE(course_key, ''), legacy_course_key FROM "
          "course_scores ORDER BY id",
          selectStmt, "reading legacy course score keys", logSqlErrorText)) {
    return false;
  }
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(selectStmt.get())) == SQLITE_ROW) {
    rows.push_back(
        {.id = sqlite3_column_int64(selectStmt.get(), 0),
         .courseKey = sqliteColumnString(selectStmt.get(), 1),
         .legacyCourseKey = sqliteColumnString(selectStmt.get(), 2)});
  }
  if (rc != SQLITE_DONE) {
    logSqlError("reading legacy course score keys", db);
    return false;
  }
  selectStmt.reset();

  SqliteStatementHandle updateStmt;
  if (!prepareSqliteStatementLogged(
          db,
          "UPDATE course_scores SET "
          "legacy_course_key = CASE WHEN ?1 THEN ?2 ELSE legacy_course_key "
          "END, course_key = CASE WHEN ?3 THEN ?4 ELSE course_key END, "
          "ln_mode = -1 WHERE id = ?5",
          updateStmt, "preparing course score identity migration",
          logSqlErrorText)) {
    return false;
  }
  for (const auto &row : rows) {
    const bool noncanonical =
        !row.courseKey.empty() && !isCanonicalCourseKey(row.courseKey);
    const bool copyLegacy = noncanonical && row.legacyCourseKey.empty();
    const auto parsed =
        noncanonical ? course_identity::parseLegacyScoreKey(row.courseKey)
                     : std::nullopt;
    const bool replaceCourseKey = parsed.has_value();

    sqlite3_bind_int(updateStmt.get(), 1, copyLegacy ? 1 : 0);
    bindSqliteText(updateStmt.get(), 2, row.courseKey);
    sqlite3_bind_int(updateStmt.get(), 3, replaceCourseKey ? 1 : 0);
    bindSqliteText(updateStmt.get(), 4,
                   parsed.has_value() ? parsed->courseKey : row.courseKey);
    sqlite3_bind_int64(updateStmt.get(), 5, row.id);
    if (sqlite3_step(updateStmt.get()) != SQLITE_DONE) {
      logSqlError("migrating course score identity", db);
      return false;
    }
    sqlite3_reset(updateStmt.get());
    sqlite3_clear_bindings(updateStmt.get());
  }

  if (!execSql(db,
               "CREATE INDEX IF NOT EXISTS "
               "idx_course_scores_key_ln_mode_clear_type ON "
               "course_scores(course_key, ln_mode, clear_type)",
               "creating course score identity index") ||
      !setDatabaseUserVersion(db, kScoreCourseIdentitySchemaVersion)) {
    return false;
  }
  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing course score identity migration",
                    transactionError);
    return false;
  }
  return true;
}

bool migrateScoreDatabaseToVersion7(sqlite3 *db) {
  std::string versionError;
  const auto version = readSqliteUserVersion(db, versionError);
  if (!version.has_value()) {
    logSqlErrorText("reading score summary semantics migration version",
                    versionError);
    return false;
  }
  if (*version > kScoreDatabaseSchemaVersion) {
    return false;
  }
  if (*version >= kScoreSummarySemanticsSchemaVersion) {
    return true;
  }
  if (*version < kScoreCourseIdentitySchemaVersion) {
    SDL_Log("Score database must reach version %d before summary semantics "
            "migration",
            kScoreCourseIdentitySchemaVersion);
    return false;
  }

  const bool callerOwnsTransaction = sqlite3_get_autocommit(db) == 0;
  const char *beginQuery =
      callerOwnsTransaction ? "SAVEPOINT asobmashow_score_summary_semantics_v7"
                            : "BEGIN IMMEDIATE TRANSACTION";
  const char *commitQuery =
      callerOwnsTransaction ? "RELEASE asobmashow_score_summary_semantics_v7"
                            : "COMMIT";
  const char *rollbackQuery =
      callerOwnsTransaction
          ? "ROLLBACK TO asobmashow_score_summary_semantics_v7; RELEASE "
            "asobmashow_score_summary_semantics_v7"
          : "ROLLBACK";
  std::string transactionError;
  SqliteTransactionHandle transaction(db, beginQuery, transactionError,
                                      commitQuery, rollbackQuery);
  if (!transaction.active()) {
    logSqlErrorText("starting score summary semantics migration",
                    transactionError);
    return false;
  }

  if (const auto error = score_cache_queries::ensureScoreSummarySchema(db)) {
    logSqlErrorText("recreating score summary trigger", *error);
    return false;
  }
  if (const auto error = score_cache_queries::rebuildScoreSummaryTables(db)) {
    logSqlErrorText("rebuilding score summaries for assisted clear caps",
                    *error);
    return false;
  }
  if (!setDatabaseUserVersion(db, kScoreSummarySemanticsSchemaVersion)) {
    return false;
  }
  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing score summary semantics migration",
                    transactionError);
    return false;
  }
  return true;
}

bool reclassifyStoredScoreEligibility(sqlite3 *db, const char *tableName) {
  struct StoredProvenance {
    sqlite3_int64 id = 0;
    int eligibility = static_cast<int>(ScoreEligibility::LegacyUnverified);
    ScoreProvenance provenance;
  };

  std::vector<StoredProvenance> rows;
  const std::string selectQuery =
      "SELECT id, eligibility, provenance_json FROM " + std::string(tableName) +
      " ORDER BY id";
  SqliteStatementHandle selectStmt;
  if (!prepareSqliteStatementLogged(db, selectQuery, selectStmt,
                                    "reading stored score eligibility",
                                    logSqlErrorText)) {
    return false;
  }
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(selectStmt.get())) == SQLITE_ROW) {
    std::string error;
    auto provenance = deserializeScoreProvenance(
        sqliteColumnString(selectStmt.get(), 2), error);
    if (!provenance.has_value()) {
      SDL_Log("Leaving unreadable %s provenance eligibility unchanged: %s",
              tableName, error.c_str());
      continue;
    }
    rows.push_back({.id = sqlite3_column_int64(selectStmt.get(), 0),
                    .eligibility = sqlite3_column_int(selectStmt.get(), 1),
                    .provenance = std::move(*provenance)});
  }
  if (rc != SQLITE_DONE) {
    logSqlError("reading stored score eligibility", db);
    return false;
  }
  selectStmt.reset();

  const std::string updateQuery =
      "UPDATE " + std::string(tableName) +
      " SET eligibility = ?1, provenance_json = ?2 WHERE id = ?3";
  SqliteStatementHandle updateStmt;
  if (!prepareSqliteStatementLogged(db, updateQuery, updateStmt,
                                    "preparing score eligibility update",
                                    logSqlErrorText)) {
    return false;
  }
  for (auto &row : rows) {
    if (row.provenance.ruleset.version <= 0) {
      continue;
    }
    const ScoreEligibility eligibility =
        scoreEligibilityForProvenance(row.provenance);
    if (row.eligibility == static_cast<int>(eligibility) &&
        row.provenance.eligibility == eligibility) {
      continue;
    }
    row.provenance.eligibility = eligibility;
    std::string error;
    const auto serialized =
        serializeValidatedScoreProvenance(row.provenance, error);
    if (!serialized.has_value()) {
      SDL_Log("Could not reclassify %s provenance: %s", tableName,
              error.c_str());
      return false;
    }
    sqlite3_bind_int(updateStmt.get(), 1, static_cast<int>(eligibility));
    bindSqliteText(updateStmt.get(), 2, *serialized);
    sqlite3_bind_int64(updateStmt.get(), 3, row.id);
    if (sqlite3_step(updateStmt.get()) != SQLITE_DONE) {
      logSqlError("updating stored score eligibility", db);
      return false;
    }
    sqlite3_reset(updateStmt.get());
    sqlite3_clear_bindings(updateStmt.get());
  }
  return true;
}

bool migrateScoreDatabaseToVersion8(sqlite3 *db) {
  std::string versionError;
  const auto version = readSqliteUserVersion(db, versionError);
  if (!version.has_value()) {
    logSqlErrorText("reading score eligibility migration version",
                    versionError);
    return false;
  }
  if (*version > kScoreDatabaseSchemaVersion) {
    return false;
  }
  if (*version >= kScoreBestEligibilitySchemaVersion) {
    return true;
  }
  if (*version < kScoreSummarySemanticsSchemaVersion) {
    SDL_Log("Score database must reach version %d before best-score "
            "eligibility migration",
            kScoreSummarySemanticsSchemaVersion);
    return false;
  }

  const bool callerOwnsTransaction = sqlite3_get_autocommit(db) == 0;
  const char *beginQuery = callerOwnsTransaction
                               ? "SAVEPOINT asobmashow_score_eligibility_v8"
                               : "BEGIN IMMEDIATE TRANSACTION";
  const char *commitQuery = callerOwnsTransaction
                                ? "RELEASE asobmashow_score_eligibility_v8"
                                : "COMMIT";
  const char *rollbackQuery =
      callerOwnsTransaction
          ? "ROLLBACK TO asobmashow_score_eligibility_v8; RELEASE "
            "asobmashow_score_eligibility_v8"
          : "ROLLBACK";
  std::string transactionError;
  SqliteTransactionHandle transaction(db, beginQuery, transactionError,
                                      commitQuery, rollbackQuery);
  if (!transaction.active()) {
    logSqlErrorText("starting score eligibility migration", transactionError);
    return false;
  }

  if (!reclassifyStoredScoreEligibility(db, "scores") ||
      !reclassifyStoredScoreEligibility(db, "course_scores")) {
    return false;
  }
  if (const auto error = score_cache_queries::ensureScoreSummarySchema(db)) {
    logSqlErrorText("recreating eligibility-aware score summary trigger",
                    *error);
    return false;
  }
  if (const auto error = score_cache_queries::rebuildScoreSummaryTables(db)) {
    logSqlErrorText("rebuilding eligibility-aware score summaries", *error);
    return false;
  }
  if (!setDatabaseUserVersion(db, kScoreBestEligibilitySchemaVersion)) {
    return false;
  }
  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing score eligibility migration", transactionError);
    return false;
  }
  return true;
}

enum class ScoreAttemptIdentitySchemaState { Absent, Exact, Malformed };

bool inspectScoreAttemptIdentitySchema(
    sqlite3 *db, ScoreAttemptIdentitySchemaState &state) {
  bool hasAttemptIdColumn = false;
  bool hasExactAttemptIdColumn = false;
  SqliteStatementHandle columns;
  if (!prepareSqliteStatementLogged(db, "PRAGMA table_info(scores)", columns,
                                    "reading score attempt identity column",
                                    logSqlErrorText)) {
    return false;
  }

  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(columns.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(columns.get(), 1) != SQLITE_TEXT ||
        sqliteColumnString(columns.get(), 1) != "attempt_id") {
      continue;
    }
    hasAttemptIdColumn = true;
    hasExactAttemptIdColumn =
        sqlite3_column_type(columns.get(), 2) == SQLITE_TEXT &&
        sqliteColumnString(columns.get(), 2) == "TEXT" &&
        sqlite3_column_type(columns.get(), 3) == SQLITE_INTEGER &&
        sqlite3_column_int(columns.get(), 3) == 0 &&
        sqlite3_column_type(columns.get(), 4) == SQLITE_NULL &&
        sqlite3_column_type(columns.get(), 5) == SQLITE_INTEGER &&
        sqlite3_column_int(columns.get(), 5) == 0;
  }
  if (rc != SQLITE_DONE) {
    logSqlError("reading score attempt identity column", db);
    return false;
  }

  constexpr std::string_view expectedIndexSql =
      "CREATE UNIQUE INDEX idx_scores_attempt_id ON scores(attempt_id) WHERE "
      "attempt_id IS NOT NULL";
  bool hasAttemptIdIndex = false;
  bool hasExactAttemptIdIndex = false;
  SqliteStatementHandle index;
  if (!prepareSqliteStatementLogged(
          db,
          "SELECT sql FROM sqlite_master WHERE type = 'index' AND "
          "name = 'idx_scores_attempt_id'",
          index, "reading score attempt identity index", logSqlErrorText)) {
    return false;
  }
  rc = sqlite3_step(index.get());
  if (rc == SQLITE_ROW) {
    hasAttemptIdIndex = true;
    hasExactAttemptIdIndex =
        sqlite3_column_type(index.get(), 0) == SQLITE_TEXT &&
        sqliteColumnString(index.get(), 0) == expectedIndexSql;
    rc = sqlite3_step(index.get());
  }
  if (rc != SQLITE_DONE) {
    logSqlError("reading score attempt identity index", db);
    return false;
  }

  if (!hasAttemptIdColumn && !hasAttemptIdIndex) {
    state = ScoreAttemptIdentitySchemaState::Absent;
  } else if (hasAttemptIdColumn && hasExactAttemptIdColumn &&
             hasAttemptIdIndex && hasExactAttemptIdIndex) {
    state = ScoreAttemptIdentitySchemaState::Exact;
  } else {
    state = ScoreAttemptIdentitySchemaState::Malformed;
  }
  return true;
}

bool migrateScoreDatabaseToVersion9(sqlite3 *db) {
  std::string versionError;
  const auto version = readSqliteUserVersion(db, versionError);
  if (!version.has_value()) {
    logSqlErrorText("reading score attempt identity migration version",
                    versionError);
    return false;
  }
  if (*version > kScoreDatabaseSchemaVersion) {
    return false;
  }
  if (*version >= kScoreAttemptIdentitySchemaVersion) {
    return true;
  }
  if (*version < kScoreBestEligibilitySchemaVersion) {
    SDL_Log("Score database must reach version %d before attempt identity "
            "migration",
            kScoreBestEligibilitySchemaVersion);
    return false;
  }

  const bool callerOwnsTransaction = sqlite3_get_autocommit(db) == 0;
  const char *beginQuery =
      callerOwnsTransaction ? "SAVEPOINT asobmashow_score_attempt_identity_v9"
                            : "BEGIN IMMEDIATE TRANSACTION";
  const char *commitQuery = callerOwnsTransaction
                                ? "RELEASE asobmashow_score_attempt_identity_v9"
                                : "COMMIT";
  const char *rollbackQuery =
      callerOwnsTransaction
          ? "ROLLBACK TO asobmashow_score_attempt_identity_v9; RELEASE "
            "asobmashow_score_attempt_identity_v9"
          : "ROLLBACK";
  std::string transactionError;
  SqliteTransactionHandle transaction(db, beginQuery, transactionError,
                                      commitQuery, rollbackQuery);
  if (!transaction.active()) {
    logSqlErrorText("starting score attempt identity migration",
                    transactionError);
    return false;
  }

  ScoreAttemptIdentitySchemaState schemaState{};
  if (!inspectScoreAttemptIdentitySchema(db, schemaState)) {
    return false;
  }
  if (schemaState == ScoreAttemptIdentitySchemaState::Malformed) {
    SDL_Log("Refusing score attempt identity migration from a partial or "
            "unexpected schema");
    return false;
  }
  if (schemaState == ScoreAttemptIdentitySchemaState::Absent &&
      (!execSql(db, "ALTER TABLE scores ADD COLUMN attempt_id TEXT",
                "adding score attempt identity") ||
       !execSql(db,
                "CREATE UNIQUE INDEX idx_scores_attempt_id "
                "ON scores(attempt_id) WHERE attempt_id IS NOT NULL",
                "creating score attempt identity index"))) {
    return false;
  }
  if (!setDatabaseUserVersion(db, kScoreAttemptIdentitySchemaVersion)) {
    return false;
  }
  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing score attempt identity migration",
                    transactionError);
    return false;
  }
  return true;
}

bool sqliteTableExists(sqlite3 *db, const char *tableName, bool &exists,
                       const char *context) {
  if (const auto error = querySqliteTableExists(db, tableName, exists)) {
    logSqlErrorText(context, *error);
    return false;
  }
  return true;
}

std::string createScoreTableSql(std::string_view tableName) {
  return "CREATE TABLE IF NOT EXISTS " + std::string(tableName) +
         " ("
         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "chart_path TEXT,"
         "chart_md5 TEXT,"
         "chart_sha256 TEXT NOT NULL,"
         "ln_mode INTEGER NOT NULL DEFAULT 0,"
         "chart_title TEXT,"
         "chart_artist TEXT,"
         "score INTEGER NOT NULL,"
         "max_score INTEGER NOT NULL,"
         "max_combo INTEGER NOT NULL,"
         "combo_break INTEGER NOT NULL,"
         "pgreat INTEGER NOT NULL,"
         "great INTEGER NOT NULL,"
         "good INTEGER NOT NULL,"
         "bad INTEGER NOT NULL,"
         "poor INTEGER NOT NULL,"
         "kpoor INTEGER NOT NULL,"
         "fast INTEGER NOT NULL,"
         "slow INTEGER NOT NULL,"
         "final_gauge REAL NOT NULL,"
         "clear_type INTEGER NOT NULL,"
         "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
         ")";
}

bool ensureScoreChartIdentityColumns(sqlite3 *db) {
  return ensureSqliteTableColumnLogged(
             db, "scores", "chart_path",
             "ALTER TABLE scores ADD COLUMN chart_path TEXT",
             "reading score schema", "adding score chart path column",
             logSqlErrorText) &&
         ensureSqliteTableColumnLogged(
             db, "scores", "chart_md5",
             "ALTER TABLE scores ADD COLUMN chart_md5 TEXT",
             "reading score schema", "adding score chart md5 column",
             logSqlErrorText) &&
         ensureSqliteTableColumnLogged(
             db, "scores", "chart_sha256",
             "ALTER TABLE scores ADD COLUMN chart_sha256 TEXT",
             "reading score schema", "adding score chart sha256 column",
             logSqlErrorText);
}

bool ensureScoreChartMetadataColumns(sqlite3 *db) {
  return ensureSqliteTableColumnLogged(
             db, "scores", "chart_title",
             "ALTER TABLE scores ADD COLUMN chart_title TEXT",
             "reading score schema", "adding score chart title column",
             logSqlErrorText) &&
         ensureSqliteTableColumnLogged(
             db, "scores", "chart_artist",
             "ALTER TABLE scores ADD COLUMN chart_artist TEXT",
             "reading score schema", "adding score chart artist column",
             logSqlErrorText);
}

bool normalizeScoreChartIdentityHashes(sqlite3 *db) {
  int changedRows = 0;
  int totalChangedRows = 0;
  if (!updateSqliteColumnWithExpressionLogged(
          db, "scores", "chart_md5", normalizedSqlHash("chart_md5"),
          "normalizing stored score md5 hashes", logSqlErrorText,
          &changedRows)) {
    return false;
  }
  totalChangedRows += changedRows;
  if (!updateSqliteColumnWithExpressionLogged(
          db, "scores", "chart_sha256",
          "COALESCE(" + normalizedSqlHash("chart_sha256") + ", '')",
          "normalizing stored score sha256 hashes", logSqlErrorText,
          &changedRows)) {
    return false;
  }
  totalChangedRows += changedRows;
  if (totalChangedRows > 0) {
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
}

bool scoreSha256ColumnIsNotNull(sqlite3 *db, bool &notNull) {
  notNull = false;
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, "PRAGMA table_info(\"scores\")", stmt,
                                    "reading score schema", logSqlErrorText)) {
    return false;
  }

  int stepRc = SQLITE_OK;
  while ((stepRc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
    if (sqliteColumnString(stmt.get(), 1) == "chart_sha256") {
      notNull = sqlite3_column_int(stmt.get(), 3) != 0;
      return true;
    }
  }
  if (stepRc != SQLITE_DONE) {
    logSqlError("reading score schema", db);
    return false;
  }
  SDL_Log("SQL error while reading score schema: chart_sha256 column missing");
  return false;
}

bool rebuildScoreTableWithRequiredSha256(sqlite3 *db) {
  bool ok = execSql(db, "SAVEPOINT score_sha256_required_migration",
                    "starting score sha256 required migration");
  if (ok) {
    ok = execSql(db, "DROP TABLE IF EXISTS scores_sha256_rebuild",
                 "dropping stale score sha256 rebuild table");
  }
  if (ok) {
    const std::string createRebuildTable =
        createScoreTableSql("scores_sha256_rebuild");
    ok = execSql(db, createRebuildTable.c_str(),
                 "creating score sha256 rebuild table");
  }
  if (ok) {
    ok = execSql(db,
                 "INSERT INTO scores_sha256_rebuild ("
                 "id, chart_path, chart_md5, chart_sha256, ln_mode, "
                 "chart_title, chart_artist, score, max_score, max_combo, "
                 "combo_break, pgreat, great, good, bad, poor, kpoor, fast, "
                 "slow, final_gauge, clear_type, created_at) "
                 "SELECT id, chart_path, chart_md5, "
                 "COALESCE(chart_sha256, ''), ln_mode, chart_title, "
                 "chart_artist, score, max_score, max_combo, combo_break, "
                 "pgreat, great, good, bad, poor, kpoor, fast, slow, "
                 "final_gauge, clear_type, created_at FROM scores",
                 "copying scores into sha256 required table");
  }
  if (ok) {
    ok = execSql(db, "DROP TABLE scores", "dropping nullable score table");
  }
  if (ok) {
    ok = execSql(db, "ALTER TABLE scores_sha256_rebuild RENAME TO scores",
                 "renaming sha256 required score table");
  }

  if (ok) {
    return execSql(db, "RELEASE score_sha256_required_migration",
                   "committing score sha256 required migration");
  }
  execSql(db, "ROLLBACK TO score_sha256_required_migration",
          "rolling back score sha256 required migration");
  execSql(db, "RELEASE score_sha256_required_migration",
          "releasing score sha256 required migration");
  return false;
}

bool ensureScoreSha256Required(sqlite3 *db) {
  bool notNull = false;
  if (!scoreSha256ColumnIsNotNull(db, notNull)) {
    return false;
  }
  if (notNull) {
    return true;
  }
  return rebuildScoreTableWithRequiredSha256(db);
}

int selectScalarInt(sqlite3 *db, const std::string &query, int fallback = 0) {
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, query, stmt, "reading score migration value", logSqlErrorText)) {
    return fallback;
  }
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    return fallback;
  }
  return sqlite3_column_int(stmt.get(), 0);
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

enum class ScoreWriteStatus {
  Inserted,
  AttemptIdentityCollision,
  StorageFailure,
};

struct ScoreWriteOutcome {
  ScoreWriteStatus status = ScoreWriteStatus::StorageFailure;
  std::string diagnostic;
};

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
         diagnostic.find("UNIQUE constraint failed: scores.attempt_id") !=
             std::string_view::npos;
}

ScoreWriteOutcome
insertScoreWriteOnConnection(sqlite3 *db,
                             const result_persistence::ChartScoreWrite &score,
                             std::optional<std::string_view> attemptId,
                             std::optional<std::string_view> createdAt,
                             const std::string &provenanceJson) {
  if (score.chartSha256.empty()) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                    "Refusing to save score without chart SHA256: %s",
                    score.chartPath.c_str());
    std::abort();
  }

  std::string query =
      "INSERT INTO scores ("
      "chart_path, chart_md5, chart_sha256, ln_mode, chart_title, "
      "chart_artist, score, max_score, max_combo, combo_break, pgreat, great, "
      "good, bad, poor, kpoor, fast, slow, final_gauge, clear_type, "
      "ruleset_version, eligibility, provenance_json, attempt_id";
  if (createdAt.has_value()) {
    query += ", created_at";
  }
  query += ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
           "?, ?, ?, ?, ?, ?";
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
  if (createdAt.has_value()) {
    bindText(*createdAt);
  }
  const int expectedBindIndex = createdAt.has_value() ? 26 : 25;
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
  if (!attemptId.has_value() && extendedError == SQLITE_CONSTRAINT_NOTNULL) {
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

bool sameFloatBits(float left, float right) {
  return std::bit_cast<std::uint32_t>(left) ==
         std::bit_cast<std::uint32_t>(right);
}

bool sameChartScoreWrite(const result_persistence::ChartScoreWrite &left,
                         const result_persistence::ChartScoreWrite &right) {
  return left.chartPath == right.chartPath && left.chartMd5 == right.chartMd5 &&
         left.chartSha256 == right.chartSha256 &&
         left.chartTitle == right.chartTitle &&
         left.chartArtist == right.chartArtist &&
         left.longNoteMode == right.longNoteMode && left.score == right.score &&
         left.maxScore == right.maxScore && left.maxCombo == right.maxCombo &&
         left.comboBreak == right.comboBreak && left.pGreat == right.pGreat &&
         left.great == right.great && left.good == right.good &&
         left.bad == right.bad && left.poor == right.poor &&
         left.kPoor == right.kPoor && left.fast == right.fast &&
         left.slow == right.slow &&
         sameFloatBits(left.finalGauge, right.finalGauge) &&
         left.clearType == right.clearType &&
         left.provenance == right.provenance;
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

  if (storedAttemptId != pending.attemptId ||
      storedCreatedAt != pending.createdAt ||
      !sameChartScoreWrite(stored, pending.score)) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic =
                "stored projected score does not match the attempted payload"};
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
  if (!current.has_value()) {
    return true;
  }
  if (candidate.score != current->score) {
    return candidate.score > current->score;
  }
  if (candidate.clearType != current->clearType) {
    return candidate.clearType > current->clearType;
  }
  return candidate.createdAt > current->createdAt;
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

bool ensureChartDatabaseReadyForScoreMigration() {
  ChartDBHelper &chartDbHelper = ChartDBHelper::GetInstance();
  SqliteConnectionHandle chartConnection(chartDbHelper.Connect());
  if (chartConnection.get() == nullptr) {
    return false;
  }
  return chartDbHelper.CreateChartMetaTable(chartConnection.get());
}

bool attachChartDatabaseForScoreMigration(sqlite3 *db) {
  const std::filesystem::path chartPath =
      Utils::GetDocumentsPath("db") / "chart.db";
  if (const auto error =
          attachSqliteDatabase(db, chartPath, kScoreMigrationChartSchema)) {
    logSqlErrorText("attaching chart database for score migration", *error);
    return false;
  }
  return true;
}

void detachChartDatabaseForScoreMigration(sqlite3 *db) {
  const std::string query =
      std::string("DETACH DATABASE ") + kScoreMigrationChartSchema;
  execSql(db, query.c_str(), "detaching chart database after score migration");
}

bool chartDatabaseHasRowsForScoreMigration(sqlite3 *db) {
  const std::string query = std::string("SELECT COUNT(*) FROM ") +
                            kScoreMigrationChartSchema + ".chart_meta";
  return selectScalarInt(db, query, 0) > 0;
}

bool chartDatabaseRebuildRequiredForScoreMigration(sqlite3 *db) {
  const std::string tableExistsQuery =
      std::string("SELECT 1 FROM ") + kScoreMigrationChartSchema +
      ".sqlite_master WHERE type = 'table' AND "
      "name = 'chart_meta_rebuild_state' LIMIT 1";
  if (selectScalarInt(db, tableExistsQuery, 0) <= 0) {
    return false;
  }
  const std::string requiredQuery =
      std::string("SELECT COALESCE(MAX(required), 0) FROM ") +
      kScoreMigrationChartSchema + ".chart_meta_rebuild_state";
  return selectScalarInt(db, requiredQuery, 0) > 0;
}

std::string scoreMigrationHashHasValue(std::string_view columnName) {
  const std::string column(columnName);
  return "scores." + column + " IS NOT NULL AND trim(scores." + column +
         ") != ''";
}

std::string scoreMigrationHashMatchCondition(std::string_view scoreColumn,
                                             std::string_view chartAlias,
                                             std::string_view chartColumn) {
  const std::string score(scoreColumn);
  const std::string alias(chartAlias);
  const std::string chart(chartColumn);
  return scoreMigrationHashHasValue(score) + " AND lower(trim(" + alias + "." +
         chart + ")) = lower(trim(scores." + score + "))";
}

std::string scoreMigrationPathMatchCondition(std::string_view chartAlias) {
  const std::string alias(chartAlias);
  return storedOrLegacyBmsPathMatchCondition("scores.chart_path",
                                             alias + ".path");
}

std::string scoreMigrationChartMatchPredicate(std::string_view chartAlias) {
  return "((" +
         scoreMigrationHashMatchCondition("chart_sha256", chartAlias,
                                          "sha256") +
         ") OR (" +
         scoreMigrationHashMatchCondition("chart_md5", chartAlias, "md5") +
         ") OR (" + scoreMigrationPathMatchCondition(chartAlias) + "))";
}

std::string scoreMigrationChartMatchRankExpr(std::string_view chartAlias) {
  return "(CASE WHEN " +
         scoreMigrationHashMatchCondition("chart_sha256", chartAlias,
                                          "sha256") +
         " THEN 0 WHEN " +
         scoreMigrationHashMatchCondition("chart_md5", chartAlias, "md5") +
         " THEN 1 WHEN " + scoreMigrationPathMatchCondition(chartAlias) +
         " THEN 2 ELSE 3 END)";
}

bool migrateLegacyScoreLongNoteModes(sqlite3 *db, bool &completed) {
  completed = false;
  if (!ensureChartDatabaseReadyForScoreMigration()) {
    return false;
  }
  if (!attachChartDatabaseForScoreMigration(db)) {
    return false;
  }

  const int scoreCount = selectScalarInt(db, "SELECT COUNT(*) FROM scores", 0);
  if (scoreCount > 0 && chartDatabaseRebuildRequiredForScoreMigration(db)) {
    detachChartDatabaseForScoreMigration(db);
    SDL_Log("Deferred score ln_mode migration because chart metadata is "
            "scheduled for rebuild");
    return true;
  }
  if (scoreCount > 0 && !chartDatabaseHasRowsForScoreMigration(db)) {
    detachChartDatabaseForScoreMigration(db);
    SDL_Log("Deferred score ln_mode migration because chart metadata is empty");
    return true;
  }

  const std::string chartTable =
      std::string(kScoreMigrationChartSchema) + ".chart_meta";
  const std::string matchPredicate = scoreMigrationChartMatchPredicate("cm");
  const std::string matchRank = scoreMigrationChartMatchRankExpr("cm");
  const std::string betterMatchRank =
      scoreMigrationChartMatchRankExpr("cm_better");
  const std::string sourcePriority = chartSourcePriorityExpr("cm");
  const std::string betterSourcePriority = chartSourcePriorityExpr("cm_better");
  const std::string sourceArchiveSize = chartSourceArchiveSizeExpr("cm");
  const std::string betterSourceArchiveSize =
      chartSourceArchiveSizeExpr("cm_better");
  const std::string betterMatchPredicate =
      "NOT EXISTS (SELECT 1 FROM " + chartTable + " cm_better WHERE " +
      scoreMigrationChartMatchPredicate("cm_better") + " AND (" +
      betterMatchRank + " < " + matchRank + " OR (" + betterMatchRank + " = " +
      matchRank + " AND (" + betterSourcePriority + " < " + sourcePriority +
      " OR (" + betterSourcePriority + " = " + sourcePriority + " AND (" +
      betterSourceArchiveSize + " < " + sourceArchiveSize + " OR (" +
      betterSourceArchiveSize + " = " + sourceArchiveSize +
      " AND cm_better.path < cm.path)))))))";
  const std::string bestMatchPredicate =
      matchPredicate + " AND " + betterMatchPredicate;
  const std::string matchedLnModeExpr = "COALESCE(cm.ln_mode, 0)";
  const std::string matchedForcedModeExpr =
      "(SELECT CASE WHEN COALESCE(cm.total_long_notes, 0) + "
      "COALESCE(cm.total_backspin_notes, 0) > 0 "
      "AND " +
      long_note_mode::sqlValidValuePredicate(matchedLnModeExpr) +
      " THEN cm.ln_mode ELSE 0 END FROM " + chartTable + " cm WHERE " +
      bestMatchPredicate + " ORDER BY cm.path LIMIT 1)";
  const std::string effectiveModeExpr =
      "CASE WHEN COALESCE(cm.total_long_notes, 0) + "
      "COALESCE(cm.total_backspin_notes, 0) <= 0 THEN 0 ELSE " +
      std::to_string(long_note_mode::kLnValue) + " END";
  const std::string purgeQuery = "DELETE FROM scores WHERE COALESCE(" +
                                 matchedForcedModeExpr + ", 0) IN (2, 3)";
  const std::string updateQuery =
      "UPDATE scores SET ln_mode = COALESCE((SELECT " + effectiveModeExpr +
      " FROM " + chartTable + " cm WHERE " + bestMatchPredicate +
      " ORDER BY cm.path LIMIT 1), ln_mode) WHERE ln_mode = 0";

  bool ok = execSql(db, "SAVEPOINT score_lnmode_migration",
                    "starting score ln_mode migration");
  int changedRows = 0;
  if (ok) {
    ok = execSql(db, purgeQuery.c_str(),
                 "purging legacy scores with incompatible long note modes");
    if (ok) {
      changedRows += sqlite3_changes(db);
    }
  }
  if (ok) {
    ok = execSql(db, updateQuery.c_str(), "migrating score long note modes");
    if (ok) {
      changedRows += sqlite3_changes(db);
    }
  }
  if (ok) {
    ok = execSql(db, "RELEASE score_lnmode_migration",
                 "committing score ln_mode migration");
  } else {
    execSql(db, "ROLLBACK TO score_lnmode_migration",
            "rolling back score ln_mode migration");
    execSql(db, "RELEASE score_lnmode_migration",
            "releasing score ln_mode migration");
  }

  detachChartDatabaseForScoreMigration(db);
  if (ok && changedRows > 0) {
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
  }
  completed = ok;
  return ok;
}

class ScoreDatabaseMigrationPass {
public:
  using RunFunction = bool (*)(sqlite3 *, bool &completed);

  constexpr ScoreDatabaseMigrationPass(int targetVersion, const char *name,
                                       RunFunction run)
      : targetVersion_(targetVersion), name_(name), run_(run) {}

  int targetVersion() const { return targetVersion_; }
  const char *name() const { return name_; }

  bool run(sqlite3 *db, bool &completed) const { return run_(db, completed); }

private:
  int targetVersion_;
  const char *name_;
  RunFunction run_;
};

bool migrateScoreDatabaseToVersion1(sqlite3 *db, bool &completed) {
  completed = false;
  if (!execSqlAllowDuplicateColumn(
          db,
          "ALTER TABLE scores ADD COLUMN ln_mode INTEGER NOT NULL DEFAULT 0",
          "migrating score long note mode")) {
    return false;
  }
  return migrateLegacyScoreLongNoteModes(db, completed);
}

bool migrateScoreDatabaseToVersion2(sqlite3 *db, bool &completed) {
  return migrateLegacyScoreLongNoteModes(db, completed);
}

bool migrateScoreDatabaseToVersion3(sqlite3 *db, bool &completed) {
  completed = normalizeScoreChartIdentityHashes(db);
  return completed;
}

bool migrateScoreDatabaseToVersion4(sqlite3 *db, bool &completed) {
  completed = false;
  if (const auto error = score_cache_queries::ensureScoreSummarySchema(db)) {
    logSqlErrorText("creating score identity summary schema", *error);
    return false;
  }
  if (const auto error = score_cache_queries::rebuildScoreSummaryTables(db)) {
    logSqlErrorText("backfilling score identity summaries", *error);
    return false;
  }
  completed = true;
  return true;
}

bool runScoreDatabaseMigrationPasses(sqlite3 *db,
                                     const ScoreDatabaseMigrationPass *passes,
                                     std::size_t passCount, int latestVersion) {
  std::string versionError;
  const auto storedVersion = readSqliteUserVersion(db, versionError);
  if (!storedVersion.has_value()) {
    logSqlErrorText("reading score migration version", versionError);
    return false;
  }
  int currentVersion = *storedVersion;
  if (currentVersion >= latestVersion) {
    return true;
  }

  for (std::size_t i = 0; i < passCount; ++i) {
    const ScoreDatabaseMigrationPass &pass = passes[i];
    if (currentVersion >= pass.targetVersion()) {
      continue;
    }

    bool completed = false;
    if (!pass.run(db, completed)) {
      SDL_Log("Score database migration failed for version %d (%s)",
              pass.targetVersion(), pass.name());
      return false;
    }
    if (!completed) {
      return true;
    }
    if (!setDatabaseUserVersion(db, pass.targetVersion())) {
      return false;
    }
    currentVersion = pass.targetVersion();
  }

  if (currentVersion < latestVersion) {
    SDL_Log("No score database migration pass reached version %d",
            latestVersion);
    return false;
  }
  return true;
}

bool migrateScoreDatabaseSchema(sqlite3 *db) {
  static constexpr ScoreDatabaseMigrationPass kMigrationPasses[] = {
      {1, "score long note modes", migrateScoreDatabaseToVersion1},
      {2, "repair legacy score long note modes",
       migrateScoreDatabaseToVersion2},
      {3, "normalize score chart hashes", migrateScoreDatabaseToVersion3},
      {4, "score identity summaries", migrateScoreDatabaseToVersion4},
  };
  return runScoreDatabaseMigrationPasses(db, kMigrationPasses,
                                         sizeof(kMigrationPasses) /
                                             sizeof(kMigrationPasses[0]),
                                         kLegacyScoreDatabaseSchemaVersion);
}
} // namespace

std::size_t
TransparentStringHash::operator()(std::string_view value) const noexcept {
  return std::hash<std::string_view>{}(value);
}

int ScoreRankByLongNoteMode::bestRankForMode(int lnMode) const {
  const int mode = long_note_mode::normalizeValue(lnMode);
  const int rank = ranks[static_cast<size_t>(mode)];
  if (rank != kNoClearTypeRank || mode != 1) {
    return rank;
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
  return bestRankForHash(
      chartMeta.SHA256,
      scoreLongNoteModeForClearLamp(chartMeta, selectedLongNoteMode));
}

int ScoreClearRankCache::bestRankForHash(const std::string &sha256,
                                         int longNoteMode) const {
  const std::string normalizedSha = normalizedHash(sha256);
  return bestRankForStoredKey(normalizedSha, longNoteMode);
}

int ScoreClearRankCache::bestRankForStoredKey(std::string_view sha256,
                                              int longNoteMode) const {
  const auto shaIt = rankBySha256.find(sha256);
  if (shaIt != rankBySha256.end()) {
    return shaIt->second.bestRankForMode(longNoteMode);
  }

  return kNoClearTypeRank;
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
  return bestForHash(chartMeta.SHA256, scoreLongNoteModeForClearLamp(
                                           chartMeta, selectedLongNoteMode));
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
  if (shaIt != scoreBySha256.end()) {
    const auto snapshot = shaIt->second.bestForMode(longNoteMode);
    if (snapshot.has_value()) {
      return snapshot;
    }
  }

  return std::nullopt;
}

ScoreDBHelper &ScoreDBHelper::GetInstance() {
  static ScoreDBHelper instance;
  return instance;
}

ScoreDBHelper::ScoreDBHelper(std::filesystem::path databasePath)
    : databasePath_(std::move(databasePath)) {}

ScoreDBHelper::~ScoreDBHelper() {
  std::lock_guard lock(sessionMutex_);
  CloseSessionDatabaseLocked();
}

void ScoreDBHelper::SetDatabasePath(std::filesystem::path databasePath) {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(sessionMutex_);
  if (!equivalentScoreDatabasePaths(databasePath_, databasePath)) {
    CloseSessionDatabaseLocked();
  }
  databasePath_ = std::move(databasePath);
  gScoreRevision.fetch_add(1, std::memory_order_relaxed);
}

std::filesystem::path ScoreDBHelper::GetDatabasePath() const {
  std::lock_guard lock(sessionMutex_);
  return databasePath_;
}

std::filesystem::path ScoreDBHelper::GetResolvedDatabasePath() const {
  std::lock_guard lock(sessionMutex_);
  return GetResolvedDatabasePathLocked();
}

std::filesystem::path ScoreDBHelper::GetResolvedDatabasePathLocked() const {
  return resolvedScoreDatabasePath(databasePath_);
}

struct ScoreDBHelper::PreparedScoreQueryDatabase::State {
  State(const ScoreDBHelper &helper, sqlite3 *chartDatabase)
      : sessionLock(helper.sessionMutex_) {
    const std::filesystem::path path = helper.GetResolvedDatabasePathLocked();
    error = score_cache_queries::prepareScoreQueryDatabase(chartDatabase, path);
  }

  profile_database_activity::WriteGuard operation;
  std::unique_lock<std::mutex> sessionLock;
  std::optional<std::string> error;
};

ScoreDBHelper::PreparedScoreQueryDatabase::PreparedScoreQueryDatabase(
    const ScoreDBHelper &helper, sqlite3 *chartDatabase)
    : state_(std::make_unique<State>(helper, chartDatabase)) {}

ScoreDBHelper::PreparedScoreQueryDatabase::~PreparedScoreQueryDatabase() =
    default;

const std::optional<std::string> &
ScoreDBHelper::PreparedScoreQueryDatabase::error() const {
  return state_->error;
}

ScoreDBHelper::PreparedScoreQueryDatabase
ScoreDBHelper::PrepareScoreQueryDatabase(sqlite3 *chartDatabase) const {
  return PreparedScoreQueryDatabase(*this, chartDatabase);
}

bool ScoreDBHelper::BindDatabasePath(std::filesystem::path databasePath,
                                     std::string &errorMessage) {
  profile_database_activity::WriteGuard operation;
  if (databasePath.empty()) {
    errorMessage = "score database path is empty";
    return false;
  }

  std::lock_guard lock(sessionMutex_);
  if (sessionDatabase_ != nullptr &&
      sqlite3_get_autocommit(sessionDatabase_) == 0) {
    SDL_Log("Discarding score database with an unfinished transaction");
    CloseSessionDatabaseLocked();
  }
  if (sessionDatabase_ != nullptr &&
      equivalentScoreDatabasePaths(databasePath_, databasePath)) {
    databasePath_ = std::move(databasePath);
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
    errorMessage.clear();
    return true;
  }

  std::string openError;
  SqliteConnectionHandle candidate(openScoreDatabase(databasePath, openError));
  if (candidate.get() == nullptr) {
    logScoreDatabaseOpenFailure(databasePath, openError);
    errorMessage = "score database validation failed";
    return false;
  }
  if (!EnsureSchemaOnConnection(candidate.get())) {
    errorMessage = "score database validation failed";
    return false;
  }

  sqlite3 *previous = sessionDatabase_;
  databasePath_ = std::move(databasePath);
  sessionDatabase_ = candidate.release();
  closeSqliteDatabase(previous);
  gScoreRevision.fetch_add(1, std::memory_order_relaxed);
  errorMessage.clear();
  return true;
}

bool ScoreDBHelper::HasActiveReads() {
  return profile_database_activity::readsActive();
}

bool ScoreDBHelper::HasActiveWrites() {
  return profile_database_activity::writesActive();
}

sqlite3 *ScoreDBHelper::Connect() {
  if (this == &GetInstance()) {
    SDL_Log("Raw score connections are unavailable on the runtime singleton");
    return nullptr;
  }
  std::filesystem::path path;
  bool trustedSessionPath = false;
  {
    std::lock_guard lock(sessionMutex_);
    path = GetResolvedDatabasePathLocked();
    trustedSessionPath = sessionDatabase_ != nullptr;
  }

  std::string openError;
  sqlite3 *db = trustedSessionPath ? openTrustedScoreDatabase(path, openError)
                                   : openScoreDatabase(path, openError);
  if (db == nullptr) {
    logScoreDatabaseOpenFailure(path, openError);
    return nullptr;
  }
  return db;
}

void ScoreDBHelper::Close(sqlite3 *db) { closeSqliteDatabase(db); }

void ScoreDBHelper::CloseSessionDatabaseLocked() {
  closeSqliteDatabase(sessionDatabase_);
  sessionDatabase_ = nullptr;
}

void ScoreDBHelper::Shutdown() {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(sessionMutex_);
  CloseSessionDatabaseLocked();
}

sqlite3 *ScoreDBHelper::EnsureSessionDatabaseLocked() {
  if (sessionDatabase_ != nullptr) {
    if (sqlite3_get_autocommit(sessionDatabase_) != 0) {
      return sessionDatabase_;
    }
    SDL_Log("Discarding score database with an unfinished transaction");
    CloseSessionDatabaseLocked();
  }

  const std::filesystem::path path = GetResolvedDatabasePathLocked();
  std::string openError;
  SqliteConnectionHandle candidate(openScoreDatabase(path, openError));
  if (candidate.get() == nullptr) {
    logScoreDatabaseOpenFailure(path, openError);
    return nullptr;
  }
  if (!EnsureSchemaOnConnection(candidate.get())) {
    return nullptr;
  }

  sessionDatabase_ = candidate.release();
  return sessionDatabase_;
}

bool ScoreDBHelper::CreateScoreTable(sqlite3 *db) {
  profile_database_activity::WriteGuard operation;
  return CreateScoreTableOnConnection(db);
}

bool ScoreDBHelper::CreateScoreTableOnConnection(sqlite3 *db) {
  if (db == nullptr || rejectFutureScoreDatabase(db)) {
    return false;
  }
  bool existingScoreTable = false;
  if (!sqliteTableExists(db, "scores", existingScoreTable,
                         "checking score table existence")) {
    return false;
  }

  const std::string query = createScoreTableSql("scores");
  if (!execSql(db, query.c_str(), "creating score table")) {
    return false;
  }

  if (!ensureScoreChartIdentityColumns(db) ||
      !ensureScoreChartMetadataColumns(db)) {
    return false;
  }

  if (existingScoreTable) {
    if (!migrateScoreDatabaseSchema(db)) {
      return false;
    }
  }
  if (!ensureScoreSha256Required(db)) {
    return false;
  }

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_sha256 ON "
      "scores(chart_sha256)",
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_md5 ON scores(chart_md5)",
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_path ON scores(chart_path)",
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_sha256_clear_type ON "
      "scores(chart_sha256, clear_type)",
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_md5_clear_type ON "
      "scores(chart_md5, clear_type)",
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_path_clear_type ON "
      "scores(chart_path, clear_type)",
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_sha256_ln_mode ON "
      "scores(chart_sha256, ln_mode)",
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_md5_ln_mode ON "
      "scores(chart_md5, ln_mode)",
      "CREATE INDEX IF NOT EXISTS idx_scores_chart_path_ln_mode ON "
      "scores(chart_path, ln_mode)",
      "CREATE INDEX IF NOT EXISTS idx_scores_identity_ln_mode ON "
      "scores(chart_sha256, chart_md5, chart_path, ln_mode)",
      "CREATE INDEX IF NOT EXISTS idx_scores_created_at ON scores(created_at)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating score index")) {
      return false;
    }
  }
  bool ensureCurrentSummarySchema = !existingScoreTable;
  if (existingScoreTable) {
    std::string versionError;
    const auto version = readSqliteUserVersion(db, versionError);
    if (!version.has_value()) {
      logSqlErrorText("reading score summary schema version", versionError);
      return false;
    }
    ensureCurrentSummarySchema = *version >= kScoreBestEligibilitySchemaVersion;
  }
  if (ensureCurrentSummarySchema) {
    if (const auto error = score_cache_queries::ensureScoreSummarySchema(db)) {
      logSqlErrorText("creating score identity summary schema", *error);
      return false;
    }
  }
  if (existingScoreTable && ensureCurrentSummarySchema) {
    if (const auto error =
            score_cache_queries::repairScoreSummaryTablesIfEmpty(db)) {
      logSqlErrorText("repairing score identity summaries", *error);
      return false;
    }
  }
  if (!existingScoreTable &&
      !setDatabaseUserVersion(db, kLegacyScoreDatabaseSchemaVersion)) {
    return false;
  }
  return true;
}

bool ScoreDBHelper::CreateCourseScoreTable(sqlite3 *db) {
  profile_database_activity::WriteGuard operation;
  return CreateCourseScoreTableOnConnection(db);
}

bool ScoreDBHelper::CreateCourseScoreTableOnConnection(sqlite3 *db) {
  if (db == nullptr || rejectFutureScoreDatabase(db)) {
    return false;
  }
  const char *query = "CREATE TABLE IF NOT EXISTS course_scores ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "course_id INTEGER,"
                      "course_key TEXT,"
                      "legacy_course_key TEXT NOT NULL DEFAULT '',"
                      "ln_mode INTEGER NOT NULL DEFAULT -1,"
                      "course_name TEXT,"
                      "course_group_name TEXT,"
                      "constraint_json TEXT,"
                      "gauge_type INTEGER NOT NULL,"
                      "gauge_profile INTEGER NOT NULL,"
                      "gauge_auto_shift INTEGER NOT NULL,"
                      "play_option TEXT,"
                      "assist_option TEXT,"
                      "completed_charts INTEGER NOT NULL,"
                      "total_charts INTEGER NOT NULL,"
                      "score INTEGER NOT NULL,"
                      "max_score INTEGER NOT NULL,"
                      "max_combo INTEGER NOT NULL,"
                      "combo_break INTEGER NOT NULL,"
                      "pgreat INTEGER NOT NULL,"
                      "great INTEGER NOT NULL,"
                      "good INTEGER NOT NULL,"
                      "bad INTEGER NOT NULL,"
                      "poor INTEGER NOT NULL,"
                      "kpoor INTEGER NOT NULL,"
                      "fast INTEGER NOT NULL,"
                      "slow INTEGER NOT NULL,"
                      "final_gauge REAL NOT NULL,"
                      "clear_type INTEGER NOT NULL,"
                      "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                      ")";
  if (!execSql(db, query, "creating course score table")) {
    return false;
  }

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_course_scores_course_id ON "
      "course_scores(course_id)",
      "CREATE INDEX IF NOT EXISTS idx_course_scores_course_id_clear_type ON "
      "course_scores(course_id, clear_type)",
      "CREATE INDEX IF NOT EXISTS idx_course_scores_course_key ON "
      "course_scores(course_key)",
      "CREATE INDEX IF NOT EXISTS idx_course_scores_clear_type ON "
      "course_scores(clear_type)",
      "CREATE INDEX IF NOT EXISTS idx_course_scores_created_at ON "
      "course_scores(created_at)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating course score index")) {
      return false;
    }
  }
  return true;
}

bool ScoreDBHelper::EnsureSchema(sqlite3 *db) {
  profile_database_activity::WriteGuard operation;
  return EnsureSchemaOnConnection(db);
}

bool ScoreDBHelper::EnsureSchemaOnConnection(sqlite3 *db) {
  if (db == nullptr || rejectFutureScoreDatabase(db)) {
    return false;
  }
  if (!CreateScoreTableOnConnection(db) ||
      !CreateCourseScoreTableOnConnection(db)) {
    return false;
  }
  return migrateScoreDatabaseToVersion5(db) &&
         migrateScoreDatabaseToVersion6(db) &&
         migrateScoreDatabaseToVersion7(db) &&
         migrateScoreDatabaseToVersion8(db) &&
         migrateScoreDatabaseToVersion9(db);
}

bool ScoreDBHelper::EnsureSchema() {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(sessionMutex_);
  return EnsureSessionDatabaseLocked() != nullptr;
}

bool ScoreDBHelper::InsertScore(sqlite3 *db,
                                const bms_parser::ChartMeta &chartMeta,
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
  if (!EnsureSchema(db)) {
    return false;
  }
  return insertScoreWriteOnConnection(db, score, std::nullopt, std::nullopt,
                                      provenanceJson)
             .status == ScoreWriteStatus::Inserted;
}

bool ScoreDBHelper::InsertCourseScore(sqlite3 *db,
                                      const CoursePlaySession &session,
                                      const RhythmState &state,
                                      int completedCharts, int totalCharts,
                                      const ScoreProvenance &provenance) {
  profile_database_activity::WriteGuard writeGuard;
  if (courseKeyForSession(session).empty()) {
    SDL_Log("Refusing to save course score without a durable course key");
    return false;
  }
  std::string provenanceJson;
  if (!serializeProvenanceForWrite(provenance, "course score",
                                   provenanceJson)) {
    return false;
  }
  if (!EnsureSchema(db)) {
    return false;
  }
  return InsertCourseScoreOnConnection(db, session, state, completedCharts,
                                       totalCharts, provenance, provenanceJson);
}

bool ScoreDBHelper::InsertCourseScoreOnConnection(
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

bool ScoreDBHelper::SaveScore(const bms_parser::ChartMeta &chartMeta,
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

  std::lock_guard lock(sessionMutex_);
  sqlite3 *db = EnsureSessionDatabaseLocked();
  if (db == nullptr) {
    return false;
  }

  const bool result = insertScoreWriteOnConnection(db, score, std::nullopt,
                                                   std::nullopt, provenanceJson)
                          .status == ScoreWriteStatus::Inserted;
  if (result) {
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

result_persistence::ProjectionOutcome ScoreDBHelper::SaveProjectedScore(
    const result_persistence::PendingChartScoreWrite &pending) {
  using result_persistence::ProjectionOutcome;
  using result_persistence::ProjectionStatus;

  profile_database_activity::WriteGuard writeGuard;
  if (!uuid::isCanonicalLowerV4(pending.attemptId)) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic =
                "score projection attempt ID is not a canonical v4 UUID"};
  }
  if (pending.createdAt.empty()) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic = "score projection timestamp is empty"};
  }
  if (pending.score.chartSha256.empty()) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic = "score projection chart SHA256 is empty"};
  }

  std::string provenanceError;
  const auto provenanceJson = serializeValidatedScoreProvenance(
      pending.score.provenance, provenanceError);
  if (!provenanceJson.has_value()) {
    return {.status = ProjectionStatus::IntegrityConflict,
            .diagnostic =
                "score projection provenance is invalid: " + provenanceError};
  }

  std::lock_guard lock(sessionMutex_);
  sqlite3 *db = EnsureSessionDatabaseLocked();
  if (db == nullptr) {
    return {.status = ProjectionStatus::StorageFailure,
            .diagnostic = "score storage is unavailable"};
  }

  const ScoreWriteOutcome inserted = insertScoreWriteOnConnection(
      db, pending.score, pending.attemptId, pending.createdAt, *provenanceJson);
  if (inserted.status == ScoreWriteStatus::Inserted) {
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
    return {.status = ProjectionStatus::Inserted};
  }
  if (inserted.status == ScoreWriteStatus::AttemptIdentityCollision) {
    return classifyProjectedScoreCollision(db, pending, *provenanceJson);
  }
  return {.status = ProjectionStatus::StorageFailure,
          .diagnostic = inserted.diagnostic};
}

bool ScoreDBHelper::SaveCourseScore(const CoursePlaySession &session,
                                    const RhythmState &state,
                                    int completedCharts, int totalCharts,
                                    const ScoreProvenance &provenance) {
  profile_database_activity::WriteGuard writeGuard;
  if (courseKeyForSession(session).empty()) {
    SDL_Log("Refusing to save course score without a durable course key");
    return false;
  }
  std::lock_guard lock(sessionMutex_);
  std::string provenanceJson;
  if (!serializeProvenanceForWrite(provenance, "course score",
                                   provenanceJson)) {
    return false;
  }

  sqlite3 *db = EnsureSessionDatabaseLocked();
  if (db == nullptr) {
    return false;
  }

  const bool result =
      InsertCourseScoreOnConnection(db, session, state, completedCharts,
                                    totalCharts, provenance, provenanceJson);
  if (result) {
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

std::optional<ScoreBestSnapshot> ScoreDBHelper::LoadBestScore(
    const bms_parser::ChartMeta &chartMeta,
    const std::optional<std::string> &beforeCreatedAt,
    const std::optional<std::string> &excludeAttemptId) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(sessionMutex_);
  sqlite3 *db = EnsureSessionDatabaseLocked();
  if (db == nullptr) {
    return std::nullopt;
  }
  return LoadBestScoreOnConnection(db, chartMeta, beforeCreatedAt,
                                   excludeAttemptId);
}

std::optional<ScoreBestSnapshot> ScoreDBHelper::LoadBestScoreOnConnection(
    sqlite3 *db, const bms_parser::ChartMeta &chartMeta,
    const std::optional<std::string> &beforeCreatedAt,
    const std::optional<std::string> &excludeAttemptId) {
  const auto match = scoreChartMatchFor(chartMeta);
  const std::string cutoff = beforeCreatedAt.value_or("");
  const int longNoteMode = scoreLongNoteModeForClearLamp(chartMeta);
  const bool legacyLongNoteModeFallback = longNoteMode == 1;
  const std::string effectiveClearRank =
      score_cache_queries::detail::fullComboClearRankExpr("s");
  const auto bestOrder = score_cache_queries::detail::bestScoreOrderKey(
      "s", effectiveClearRank, "id");

  std::string query =
      "SELECT score, max_score, max_combo, combo_break, final_gauge, ";
  query += effectiveClearRank + ", created_at FROM scores s WHERE ";
  query += scoreChartMatchPredicate();
  query += " AND " +
           score_cache_queries::detail::scoreParticipatesInBestExpr("s") +
           " AND (ln_mode = ? OR (? != 0 AND ln_mode = 0)) "
           "AND (? = '' OR created_at < ?) ";
  if (excludeAttemptId.has_value()) {
    query += "AND (attempt_id IS NULL OR attempt_id <> ?) ";
  }
  query += "ORDER BY CASE WHEN ln_mode = ? THEN 0 ELSE 1 END, " +
           score_cache_queries::detail::bestScoreOrderBySql(bestOrder) +
           " LIMIT 1";

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
  sqlite3_bind_int(stmt.get(), bindIndex++, longNoteMode);

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

std::optional<ScoreBestSnapshot>
ScoreDBHelper::LoadBestCourseScore(const CoursePlaySession &session) {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(sessionMutex_);
  sqlite3 *db = EnsureSessionDatabaseLocked();
  if (db == nullptr) {
    return std::nullopt;
  }
  return LoadBestCourseScoreOnConnection(db, session);
}

std::optional<ScoreBestSnapshot> ScoreDBHelper::LoadBestCourseScoreOnConnection(
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

CourseScoreRecoveryResult ScoreDBHelper::RecoverCourseRecords(
    std::span<const course_identity::Definition> definitions) {
  profile_database_activity::WriteGuard operation;
  std::lock_guard lock(sessionMutex_);
  sqlite3 *db = EnsureSessionDatabaseLocked();
  if (db == nullptr) {
    return {.errorMessage = "score database validation failed"};
  }
  return RecoverCourseRecordsOnConnection(db, definitions);
}

CourseScoreRecoveryResult ScoreDBHelper::RecoverCourseRecordsOnConnection(
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

ScoreClearRankCache ScoreDBHelper::LoadBestClearRanks() {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(sessionMutex_);
  ScoreClearRankCache cache;
  sqlite3 *db = EnsureSessionDatabaseLocked();
  if (db == nullptr) {
    return cache;
  }

  loadBestChartRanks(db, cache);
  loadBestCourseRanks(db, cache);
  return cache;
}

ScoreClearRankCache ScoreDBHelper::LoadBestClearRanks(sqlite3 *db,
                                                      std::string_view schema) {
  profile_database_activity::ReadGuard operation;
  ScoreClearRankCache cache;
  if (db != nullptr) {
    loadBestChartRanks(db, cache, schema);
    loadBestCourseRanks(db, cache, schema);
  }
  return cache;
}

ScoreBestCache ScoreDBHelper::LoadBestScores() {
  profile_database_activity::ReadGuard operation;
  std::lock_guard lock(sessionMutex_);
  ScoreBestCache cache;
  sqlite3 *db = EnsureSessionDatabaseLocked();
  if (db == nullptr) {
    return cache;
  }

  loadBestChartScores(db, cache);
  return cache;
}

ScoreBestCache ScoreDBHelper::LoadBestScores(sqlite3 *db,
                                             std::string_view schema) {
  profile_database_activity::ReadGuard operation;
  ScoreBestCache cache;
  if (db != nullptr) {
    loadBestChartScores(db, cache, schema);
  }
  return cache;
}

std::uint64_t ScoreDBHelper::GetRevision() const {
  return gScoreRevision.load(std::memory_order_relaxed);
}
