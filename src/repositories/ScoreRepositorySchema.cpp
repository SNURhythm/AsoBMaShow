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
#include <bit>
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
constexpr int kLegacyScoreDatabaseSchemaVersion = 4;
constexpr int kScoreProvenanceSchemaVersion = 5;
constexpr int kScoreCourseIdentitySchemaVersion = 6;
constexpr int kScoreSummarySemanticsSchemaVersion = 7;
constexpr int kScoreBestEligibilitySchemaVersion = 8;
constexpr int kScoreAttemptIdentitySchemaVersion = 9;
constexpr int kScoreDatabaseSchemaVersion =
    ScoreRepository::kCurrentSchemaVersion;
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

bool scoreAttemptIdentitySchemaIsExact(sqlite3 *db);
bool currentScoreAttemptIdentitySchemaIsValid(sqlite3 *db);

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

bool inspectScoreAttemptIdentitySchema(sqlite3 *db,
                                       ScoreAttemptIdentitySchemaState &state) {
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

bool scoreAttemptIdentitySchemaIsExact(sqlite3 *db) {
  ScoreAttemptIdentitySchemaState state{};
  if (!inspectScoreAttemptIdentitySchema(db, state)) {
    return false;
  }
  if (state != ScoreAttemptIdentitySchemaState::Exact) {
    SDL_Log("Refusing current score database with a partial or unexpected "
            "attempt identity schema");
    return false;
  }
  return true;
}

bool currentScoreAttemptIdentitySchemaIsValid(sqlite3 *db) {
  std::string versionError;
  const auto version = readSqliteUserVersion(db, versionError);
  if (!version.has_value()) {
    logSqlErrorText("reading current score schema version", versionError);
    return false;
  }
  if (*version != kScoreDatabaseSchemaVersion) {
    return false;
  }
  return scoreAttemptIdentitySchemaIsExact(db);
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
    return scoreAttemptIdentitySchemaIsExact(db);
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
    score_repository_detail::IncrementRevision();
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

bool attachChartDatabaseForScoreMigration(
    sqlite3 *db, const std::filesystem::path &chartPath) {
  if (const auto error =
          attachSqliteDatabase(db, chartPath, kScoreMigrationChartSchema)) {
    logSqlErrorText("attaching chart database for score migration", *error);
    return false;
  }
  return true;
}

void detachChartDatabaseForScoreMigration(sqlite3 *db) {
  // SQLite cannot detach a database while the caller's schema transaction is
  // active. The attachment is connection-local and is released with the
  // migration connection, so avoid reporting a false migration failure.
  if (sqlite3_get_autocommit(db) == 0) {
    return;
  }
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

bool migrateLegacyScoreLongNoteModes(
    sqlite3 *db, const std::filesystem::path &chartDatabasePath,
    bool &completed) {
  completed = false;
  if (!attachChartDatabaseForScoreMigration(db, chartDatabasePath)) {
    return false;
  }

  const int scoreCount = selectScalarInt(db, "SELECT COUNT(*) FROM scores", 0);
  if (scoreCount > 0 && chartDatabaseRebuildRequiredForScoreMigration(db)) {
    detachChartDatabaseForScoreMigration(db);
    SDL_Log("Preserving unclassified legacy score ln_mode values because "
            "chart metadata is scheduled for rebuild");
    completed = true;
    return true;
  }
  if (scoreCount > 0 && !chartDatabaseHasRowsForScoreMigration(db)) {
    detachChartDatabaseForScoreMigration(db);
    SDL_Log("Preserving unclassified legacy score ln_mode values because "
            "chart metadata is empty");
    completed = true;
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
  const std::string effectiveModeExpr =
      "CASE WHEN COALESCE(cm.total_long_notes, 0) + "
      "COALESCE(cm.total_backspin_notes, 0) <= 0 THEN 0 ELSE " +
      std::to_string(long_note_mode::kLnValue) + " END";
  const std::string updateQuery =
      "UPDATE scores SET ln_mode = COALESCE((SELECT " + effectiveModeExpr +
      " FROM " + chartTable + " cm WHERE " + bestMatchPredicate +
      " ORDER BY cm.path LIMIT 1), ln_mode) WHERE ln_mode = 0";

  bool ok = execSql(db, "SAVEPOINT score_lnmode_migration",
                    "starting score ln_mode migration");
  int changedRows = 0;
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
    score_repository_detail::IncrementRevision();
  }
  completed = ok;
  return ok;
}

class ScoreDatabaseMigrationPass {
public:
  using RunFunction = bool (*)(sqlite3 *, const std::filesystem::path &,
                               bool &completed);

  constexpr ScoreDatabaseMigrationPass(int targetVersion, const char *name,
                                       RunFunction run)
      : targetVersion_(targetVersion), name_(name), run_(run) {}

  int targetVersion() const { return targetVersion_; }
  const char *name() const { return name_; }

  bool run(sqlite3 *db, const std::filesystem::path &chartDatabasePath,
           bool &completed) const {
    return run_(db, chartDatabasePath, completed);
  }

private:
  int targetVersion_;
  const char *name_;
  RunFunction run_;
};

bool migrateScoreDatabaseToVersion1(
    sqlite3 *db, const std::filesystem::path &chartDatabasePath,
    bool &completed) {
  completed = false;
  if (!execSqlAllowDuplicateColumn(
          db,
          "ALTER TABLE scores ADD COLUMN ln_mode INTEGER NOT NULL DEFAULT 0",
          "migrating score long note mode")) {
    return false;
  }
  return migrateLegacyScoreLongNoteModes(db, chartDatabasePath, completed);
}

bool migrateScoreDatabaseToVersion2(
    sqlite3 *db, const std::filesystem::path &chartDatabasePath,
    bool &completed) {
  return migrateLegacyScoreLongNoteModes(db, chartDatabasePath, completed);
}

bool migrateScoreDatabaseToVersion3(sqlite3 *db,
                                    const std::filesystem::path &,
                                    bool &completed) {
  completed = normalizeScoreChartIdentityHashes(db);
  return completed;
}

bool migrateScoreDatabaseToVersion4(sqlite3 *db,
                                    const std::filesystem::path &,
                                    bool &completed) {
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
                                     std::size_t passCount, int latestVersion,
                                     const std::filesystem::path
                                         &chartDatabasePath) {
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
    if (!pass.run(db, chartDatabasePath, completed)) {
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

bool migrateScoreDatabaseSchema(
    sqlite3 *db, const std::filesystem::path &chartDatabasePath) {
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
                                         kLegacyScoreDatabaseSchemaVersion,
                                         chartDatabasePath);
}
} // namespace
bool score_repository_detail::CreateScoreTableOnConnection(
    sqlite3 *db, const std::filesystem::path &chartDatabasePath) {
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
    if (!migrateScoreDatabaseSchema(db, chartDatabasePath)) {
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

bool score_repository_detail::CreateCourseScoreTableOnConnection(sqlite3 *db) {
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

bool score_repository_detail::EnsureSchemaOnConnection(
    sqlite3 *db, const std::filesystem::path &chartDatabasePath) {
  if (db == nullptr || rejectFutureScoreDatabase(db)) {
    return false;
  }
  const bool callerOwnsTransaction = sqlite3_get_autocommit(db) == 0;
  const char *beginQuery = callerOwnsTransaction
                               ? "SAVEPOINT asobmashow_score_schema_ensure"
                               : "BEGIN IMMEDIATE TRANSACTION";
  const char *commitQuery = callerOwnsTransaction
                                ? "RELEASE asobmashow_score_schema_ensure"
                                : "COMMIT";
  const char *rollbackQuery =
      callerOwnsTransaction
          ? "ROLLBACK TO asobmashow_score_schema_ensure; RELEASE "
            "asobmashow_score_schema_ensure"
          : "ROLLBACK";
  std::string transactionError;
  SqliteTransactionHandle transaction(db, beginQuery, transactionError,
                                      commitQuery, rollbackQuery);
  if (!transaction.active()) {
    logSqlErrorText("starting score schema ensure", transactionError);
    return false;
  }
  if (!CreateScoreTableOnConnection(db, chartDatabasePath) ||
      !CreateCourseScoreTableOnConnection(db)) {
    return false;
  }
  if (!migrateScoreDatabaseToVersion5(db) ||
      !migrateScoreDatabaseToVersion6(db) ||
      !migrateScoreDatabaseToVersion7(db) ||
      !migrateScoreDatabaseToVersion8(db) ||
      !migrateScoreDatabaseToVersion9(db)) {
    return false;
  }
  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing score schema ensure", transactionError);
    return false;
  }
  return true;
}

sqlite3 *score_repository_detail::OpenDatabase(
    const std::filesystem::path &path, std::string &errorMessage) {
  return openScoreDatabase(path, errorMessage);
}

void score_repository_detail::LogDatabaseOpenFailure(
    const std::filesystem::path &path, const std::string &errorMessage) {
  logScoreDatabaseOpenFailure(path, errorMessage);
}

std::filesystem::path score_repository_detail::ResolvedDatabasePath(
    const std::filesystem::path &databasePath) {
  return resolvedScoreDatabasePath(databasePath);
}

bool score_repository_detail::EquivalentDatabasePaths(
    const std::filesystem::path &first,
    const std::filesystem::path &second) {
  return equivalentScoreDatabasePaths(first, second);
}

bool score_repository_detail::CurrentSchemaIsValid(sqlite3 *database) {
  return currentScoreAttemptIdentitySchemaIsValid(database);
}
