#include "ReplayRepository.h"
#include "ReplayRepositoryInternal.h"

#include "../BmsMetadataText.h"
#include "ChartSqlExpressions.h"
#include "../LongNoteModeUtils.h"
#include "../ProfileDatabaseActivity.h"
#include "SqliteRAII.h"
#include "../Uuid.h"
#include "../Utils.h"
#include "../path.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
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
    ReplayRepository::kCurrentSchemaVersion;
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
    sqlite3_stmt *stmt, sqlite3_int64 courseReplayId, int expectedCount,
    std::vector<course_identity::ChartIdentity> &charts, std::string &error) {
  charts.clear();
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  if (sqlite3_bind_int64(stmt, 1, courseReplayId) != SQLITE_OK ||
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
        sqlite3_column_int64(stmt, 1) <= 0 ||
        sqlite3_column_int64(stmt, 1) != sqlite3_column_int64(stmt, 2)) {
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
            stageStmt.get(), sqlite3_column_int64(rowStmt.get(), 0),
            totalCharts, charts, stageError);
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

constexpr const char *kReplayAttemptIdIndexSql =
    "CREATE UNIQUE INDEX idx_replays_attempt_id ON "
    "replays(attempt_id) WHERE attempt_id IS NOT NULL";

constexpr const char *kPendingChartScoreWritesTableSql =
    "CREATE TABLE pending_chart_score_writes ("
    "attempt_id TEXT PRIMARY KEY NOT NULL,"
    "replay_id INTEGER NOT NULL UNIQUE,"
    "chart_path TEXT NOT NULL,"
    "chart_md5 TEXT NOT NULL,"
    "chart_sha256 TEXT NOT NULL,"
    "chart_title TEXT NOT NULL,"
    "chart_artist TEXT NOT NULL,"
    "ln_mode INTEGER NOT NULL,"
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
    "ruleset_version INTEGER NOT NULL,"
    "eligibility INTEGER NOT NULL,"
    "provenance_json TEXT NOT NULL,"
    "created_at TEXT NOT NULL,"
    "recovery_attempts INTEGER NOT NULL DEFAULT 0,"
    "last_recovery_at TEXT,"
    "FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE"
    ")";

constexpr const char *kPendingChartScoreRecoveryIndexSql =
    "CREATE INDEX idx_pending_chart_score_created ON "
    "pending_chart_score_writes("
    "recovery_attempts, last_recovery_at, created_at, attempt_id)";

enum class ReplayResultOutboxSchemaState { Absent, Exact, Malformed };

struct NamedSchemaObjectInspection {
  bool present = false;
  bool exact = false;
};

unsigned char sqliteAsciiIdentifierFold(unsigned char character) {
  return character >= 'A' && character <= 'Z'
             ? static_cast<unsigned char>(character + ('a' - 'A'))
             : character;
}

bool sqliteAsciiIdentifiersEqual(std::string_view left,
                                 std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (sqliteAsciiIdentifierFold(static_cast<unsigned char>(left[i])) !=
        sqliteAsciiIdentifierFold(static_cast<unsigned char>(right[i]))) {
      return false;
    }
  }
  return true;
}

enum class SchemaSqlTokenKind {
  Word,
  QuotedIdentifier,
  Number,
  String,
  Symbol,
};

struct SchemaSqlToken {
  SchemaSqlTokenKind kind{};
  std::string text;
};

std::optional<std::vector<SchemaSqlToken>>
tokenizeSchemaSql(std::string_view sql) {
  std::vector<SchemaSqlToken> result;
  for (std::size_t i = 0; i < sql.size();) {
    const unsigned char character = static_cast<unsigned char>(sql[i]);
    if (std::isspace(character) != 0) {
      ++i;
      continue;
    }
    if (character == '"' || character == '`' || character == '[') {
      const char openingQuote = static_cast<char>(character);
      const char closingQuote = openingQuote == '[' ? ']' : openingQuote;
      std::string identifier;
      bool closed = false;
      for (++i; i < sql.size(); ++i) {
        if (sql[i] == closingQuote) {
          if (openingQuote != '[' && i + 1 < sql.size() &&
              sql[i + 1] == closingQuote) {
            identifier.push_back(closingQuote);
            ++i;
            continue;
          }
          ++i;
          closed = true;
          break;
        }
        identifier.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(sql[i]))));
      }
      if (!closed) {
        return std::nullopt;
      }
      result.push_back({.kind = SchemaSqlTokenKind::QuotedIdentifier,
                        .text = std::move(identifier)});
      continue;
    }
    if (character == '\'') {
      std::string value;
      bool closed = false;
      for (++i; i < sql.size(); ++i) {
        if (sql[i] == '\'') {
          if (i + 1 < sql.size() && sql[i + 1] == '\'') {
            value.push_back('\'');
            ++i;
            continue;
          }
          ++i;
          closed = true;
          break;
        }
        value.push_back(sql[i]);
      }
      if (!closed) {
        return std::nullopt;
      }
      result.push_back(
          {.kind = SchemaSqlTokenKind::String, .text = std::move(value)});
      continue;
    }
    if (std::isdigit(character) != 0) {
      const std::size_t begin = i++;
      while (i < sql.size() &&
             std::isdigit(static_cast<unsigned char>(sql[i])) != 0) {
        ++i;
      }
      result.push_back({.kind = SchemaSqlTokenKind::Number,
                        .text = std::string(sql.substr(begin, i - begin))});
      continue;
    }
    if (std::isalpha(character) != 0 || character == '_' || character == '$') {
      std::string word;
      do {
        word.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(sql[i++]))));
      } while (i < sql.size() &&
               (std::isalnum(static_cast<unsigned char>(sql[i])) != 0 ||
                sql[i] == '_' || sql[i] == '$'));
      result.push_back(
          {.kind = SchemaSqlTokenKind::Word, .text = std::move(word)});
      continue;
    }
    result.push_back({.kind = SchemaSqlTokenKind::Symbol,
                      .text = std::string(1, static_cast<char>(character))});
    ++i;
  }
  return result;
}

bool schemaSqlIsEquivalent(std::string_view actual, std::string_view expected) {
  const auto actualTokens = tokenizeSchemaSql(actual);
  const auto expectedTokens = tokenizeSchemaSql(expected);
  if (!actualTokens.has_value() || !expectedTokens.has_value() ||
      actualTokens->size() != expectedTokens->size()) {
    return false;
  }
  for (std::size_t i = 0; i < actualTokens->size(); ++i) {
    const SchemaSqlToken &actualToken = (*actualTokens)[i];
    const SchemaSqlToken &expectedToken = (*expectedTokens)[i];
    if (actualToken.text != expectedToken.text) {
      return false;
    }
    if (actualToken.kind == expectedToken.kind) {
      continue;
    }
    if (expectedToken.kind != SchemaSqlTokenKind::Word ||
        actualToken.kind != SchemaSqlTokenKind::QuotedIdentifier ||
        sqlite3_keyword_check(expectedToken.text.c_str(),
                              static_cast<int>(expectedToken.text.size())) !=
            0) {
      return false;
    }
  }
  return true;
}

bool inspectNamedSchemaObject(sqlite3 *db, std::string_view name,
                              std::string_view expectedType,
                              std::string_view expectedTable,
                              std::string_view expectedSql,
                              NamedSchemaObjectInspection &inspection,
                              const char *context) {
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db,
          "SELECT type, tbl_name, sql FROM sqlite_master "
          "WHERE name = ? COLLATE NOCASE",
          stmt, context, logSqlErrorText) ||
      name.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      sqlite3_bind_text(stmt.get(), 1, name.data(),
                        static_cast<int>(name.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    logSqlError(context, db);
    return false;
  }

  int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_DONE) {
    return true;
  }
  if (rc != SQLITE_ROW) {
    logSqlError(context, db);
    return false;
  }
  inspection.present = true;
  inspection.exact =
      sqlite3_column_type(stmt.get(), 0) == SQLITE_TEXT &&
      sqliteColumnTextView(stmt.get(), 0) == expectedType &&
      sqlite3_column_type(stmt.get(), 1) == SQLITE_TEXT &&
      sqliteAsciiIdentifiersEqual(sqliteColumnTextView(stmt.get(), 1),
                                  expectedTable) &&
      sqlite3_column_type(stmt.get(), 2) == SQLITE_TEXT &&
      schemaSqlIsEquivalent(sqliteColumnTextView(stmt.get(), 2), expectedSql);

  rc = sqlite3_step(stmt.get());
  while (rc == SQLITE_ROW) {
    inspection.exact = false;
    rc = sqlite3_step(stmt.get());
  }
  if (rc != SQLITE_DONE) {
    logSqlError(context, db);
    return false;
  }
  return true;
}

bool inspectNamedIndexShape(
    sqlite3 *db, std::string_view tableName, std::string_view indexName,
    bool expectedUnique, bool expectedPartial,
    const std::vector<std::string_view> &expectedColumns, bool &exact,
    const char *context) {
  exact = false;
  const std::string indexListQuery =
      "PRAGMA index_list(\"" + std::string(tableName) + "\")";
  SqliteStatementHandle indexList;
  if (!prepareSqliteStatementLogged(db, indexListQuery, indexList, context,
                                    logSqlErrorText)) {
    return false;
  }

  bool found = false;
  bool shapeExact = true;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(indexList.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(indexList.get(), 1) != SQLITE_TEXT ||
        !sqliteAsciiIdentifiersEqual(sqliteColumnTextView(indexList.get(), 1),
                                     indexName)) {
      continue;
    }
    if (found) {
      shapeExact = false;
    }
    found = true;
    shapeExact =
        shapeExact &&
        sqlite3_column_type(indexList.get(), 2) == SQLITE_INTEGER &&
        (sqlite3_column_int(indexList.get(), 2) != 0) == expectedUnique &&
        sqlite3_column_type(indexList.get(), 3) == SQLITE_TEXT &&
        sqliteColumnTextView(indexList.get(), 3) == "c" &&
        sqlite3_column_type(indexList.get(), 4) == SQLITE_INTEGER &&
        (sqlite3_column_int(indexList.get(), 4) != 0) == expectedPartial;
  }
  if (rc != SQLITE_DONE) {
    logSqlError(context, db);
    return false;
  }
  if (!found) {
    return true;
  }

  const std::string indexInfoQuery =
      "PRAGMA index_xinfo(\"" + std::string(indexName) + "\")";
  SqliteStatementHandle indexInfo;
  if (!prepareSqliteStatementLogged(db, indexInfoQuery, indexInfo, context,
                                    logSqlErrorText)) {
    return false;
  }
  std::size_t keyColumnCount = 0;
  std::size_t auxiliaryColumnCount = 0;
  while ((rc = sqlite3_step(indexInfo.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(indexInfo.get(), 5) != SQLITE_INTEGER) {
      shapeExact = false;
      continue;
    }
    if (sqlite3_column_int(indexInfo.get(), 5) != 0) {
      bool columnExact = false;
      if (keyColumnCount < expectedColumns.size()) {
        columnExact =
            sqlite3_column_type(indexInfo.get(), 0) == SQLITE_INTEGER &&
            sqlite3_column_int64(indexInfo.get(), 0) ==
                static_cast<sqlite3_int64>(keyColumnCount) &&
            sqlite3_column_type(indexInfo.get(), 1) == SQLITE_INTEGER &&
            sqlite3_column_int(indexInfo.get(), 1) >= 0 &&
            sqlite3_column_type(indexInfo.get(), 2) == SQLITE_TEXT &&
            sqliteAsciiIdentifiersEqual(
                sqliteColumnTextView(indexInfo.get(), 2),
                expectedColumns[keyColumnCount]) &&
            sqlite3_column_type(indexInfo.get(), 3) == SQLITE_INTEGER &&
            sqlite3_column_int(indexInfo.get(), 3) == 0 &&
            sqlite3_column_type(indexInfo.get(), 4) == SQLITE_TEXT &&
            sqliteColumnTextView(indexInfo.get(), 4) == "BINARY";
      }
      shapeExact = shapeExact && columnExact;
      ++keyColumnCount;
      continue;
    }
    const bool auxiliaryExact =
        sqlite3_column_type(indexInfo.get(), 0) == SQLITE_INTEGER &&
        sqlite3_column_int64(indexInfo.get(), 0) ==
            static_cast<sqlite3_int64>(expectedColumns.size()) &&
        sqlite3_column_type(indexInfo.get(), 1) == SQLITE_INTEGER &&
        sqlite3_column_int(indexInfo.get(), 1) == -1 &&
        sqlite3_column_type(indexInfo.get(), 2) == SQLITE_NULL &&
        sqlite3_column_type(indexInfo.get(), 3) == SQLITE_INTEGER &&
        sqlite3_column_int(indexInfo.get(), 3) == 0 &&
        sqlite3_column_type(indexInfo.get(), 4) == SQLITE_TEXT &&
        sqliteColumnTextView(indexInfo.get(), 4) == "BINARY";
    shapeExact = shapeExact && auxiliaryExact;
    ++auxiliaryColumnCount;
  }
  if (rc != SQLITE_DONE) {
    logSqlError(context, db);
    return false;
  }
  exact = shapeExact && keyColumnCount == expectedColumns.size() &&
          auxiliaryColumnCount == 1;
  return true;
}

bool inspectReplayResultOutboxSchema(sqlite3 *db,
                                     ReplayResultOutboxSchemaState &state) {
  bool hasAttemptIdColumn = false;
  bool hasExactAttemptIdColumn = false;
  bool hasAttemptFingerprintColumn = false;
  bool hasExactAttemptFingerprintColumn = false;
  SqliteStatementHandle columns;
  if (!prepareSqliteStatementLogged(db, "PRAGMA table_info(replays)", columns,
                                    "reading replay result outbox columns",
                                    logSqlErrorText)) {
    return false;
  }

  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(columns.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(columns.get(), 1) != SQLITE_TEXT) {
      continue;
    }
    const std::string_view name = sqliteColumnTextView(columns.get(), 1);
    bool *present = nullptr;
    bool *exact = nullptr;
    if (sqliteAsciiIdentifiersEqual(name, "attempt_id")) {
      present = &hasAttemptIdColumn;
      exact = &hasExactAttemptIdColumn;
    } else if (sqliteAsciiIdentifiersEqual(name, "attempt_fingerprint")) {
      present = &hasAttemptFingerprintColumn;
      exact = &hasExactAttemptFingerprintColumn;
    } else {
      continue;
    }
    *present = true;
    *exact =
        sqlite3_column_type(columns.get(), 2) == SQLITE_TEXT &&
        schemaSqlIsEquivalent(sqliteColumnTextView(columns.get(), 2), "TEXT") &&
        sqlite3_column_type(columns.get(), 3) == SQLITE_INTEGER &&
        sqlite3_column_int(columns.get(), 3) == 0 &&
        sqlite3_column_type(columns.get(), 4) == SQLITE_NULL &&
        sqlite3_column_type(columns.get(), 5) == SQLITE_INTEGER &&
        sqlite3_column_int(columns.get(), 5) == 0;
  }
  if (rc != SQLITE_DONE) {
    logSqlError("reading replay result outbox columns", db);
    return false;
  }

  NamedSchemaObjectInspection attemptIndex;
  NamedSchemaObjectInspection pendingTable;
  NamedSchemaObjectInspection recoveryIndex;
  bool attemptIndexShapeExact = false;
  bool recoveryIndexShapeExact = false;
  if (!inspectNamedSchemaObject(db, "idx_replays_attempt_id", "index",
                                "replays", kReplayAttemptIdIndexSql,
                                attemptIndex,
                                "reading replay attempt identity index") ||
      !inspectNamedSchemaObject(db, "pending_chart_score_writes", "table",
                                "pending_chart_score_writes",
                                kPendingChartScoreWritesTableSql, pendingTable,
                                "reading pending chart score outbox schema") ||
      !inspectNamedSchemaObject(
          db, "idx_pending_chart_score_created", "index",
          "pending_chart_score_writes", kPendingChartScoreRecoveryIndexSql,
          recoveryIndex, "reading pending chart score recovery index") ||
      !inspectNamedIndexShape(db, "replays", "idx_replays_attempt_id", true,
                              true, {"attempt_id"}, attemptIndexShapeExact,
                              "reading replay attempt identity index shape") ||
      !inspectNamedIndexShape(
          db, "pending_chart_score_writes", "idx_pending_chart_score_created",
          false, false,
          {"recovery_attempts", "last_recovery_at", "created_at", "attempt_id"},
          recoveryIndexShapeExact,
          "reading pending chart score recovery index shape")) {
    return false;
  }

  const bool entirelyAbsent =
      !hasAttemptIdColumn && !hasAttemptFingerprintColumn &&
      !attemptIndex.present && !pendingTable.present && !recoveryIndex.present;
  const bool exact =
      hasAttemptIdColumn && hasExactAttemptIdColumn &&
      hasAttemptFingerprintColumn && hasExactAttemptFingerprintColumn &&
      attemptIndex.present && attemptIndex.exact && attemptIndexShapeExact &&
      pendingTable.present && pendingTable.exact && recoveryIndex.present &&
      recoveryIndex.exact && recoveryIndexShapeExact;
  if (entirelyAbsent) {
    state = ReplayResultOutboxSchemaState::Absent;
  } else if (exact) {
    state = ReplayResultOutboxSchemaState::Exact;
  } else {
    state = ReplayResultOutboxSchemaState::Malformed;
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
    ReplayResultOutboxSchemaState resultOutboxState{};
    if (!inspectReplayResultOutboxSchema(db, resultOutboxState)) {
      return false;
    }
    if (resultOutboxState != ReplayResultOutboxSchemaState::Exact) {
      SDL_Log("Refusing current replay database with a partial or unexpected "
              "result outbox schema");
      return false;
    }
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
  if (*version < 5) {
    ReplayResultOutboxSchemaState resultOutboxState{};
    if (!inspectReplayResultOutboxSchema(db, resultOutboxState)) {
      return false;
    }
    if (resultOutboxState == ReplayResultOutboxSchemaState::Malformed) {
      SDL_Log("Refusing replay result outbox migration from a partial or "
              "unexpected schema");
      return false;
    }
    if (resultOutboxState == ReplayResultOutboxSchemaState::Absent &&
        (!execSql(db, "ALTER TABLE replays ADD COLUMN attempt_id TEXT",
                  "adding replay attempt ID column") ||
         !execSql(db, "ALTER TABLE replays ADD COLUMN attempt_fingerprint TEXT",
                  "adding replay attempt fingerprint column") ||
         !execSql(db, kReplayAttemptIdIndexSql,
                  "creating replay attempt ID index") ||
         !execSql(db, kPendingChartScoreWritesTableSql,
                  "creating pending chart score outbox") ||
         !execSql(db, kPendingChartScoreRecoveryIndexSql,
                  "creating pending chart score recovery index"))) {
      return false;
    }
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

} // namespace
bool replay_repository_detail::CreateReplayTablesOnConnection(sqlite3 *db) {
  if (db == nullptr || rejectFutureReplayDatabase(db)) {
    return false;
  }
  const bool callerOwnsTransaction = sqlite3_get_autocommit(db) == 0;
  const char *beginQuery = callerOwnsTransaction
                               ? "SAVEPOINT asobmashow_replay_schema_ensure"
                               : "BEGIN IMMEDIATE TRANSACTION";
  const char *commitQuery = callerOwnsTransaction
                                ? "RELEASE asobmashow_replay_schema_ensure"
                                : "COMMIT";
  const char *rollbackQuery =
      callerOwnsTransaction
          ? "ROLLBACK TO asobmashow_replay_schema_ensure; RELEASE "
            "asobmashow_replay_schema_ensure"
          : "ROLLBACK";
  std::string transactionError;
  SqliteTransactionHandle transaction(db, beginQuery, transactionError,
                                      commitQuery, rollbackQuery);
  if (!transaction.active()) {
    logSqlErrorText("starting replay schema ensure", transactionError);
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
  if (!execSql(db,
               "CREATE INDEX IF NOT EXISTS idx_course_replays_key_id ON "
               "course_replays(course_key, id)",
               "ensuring course replay content key index")) {
    return false;
  }
  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing replay schema ensure", transactionError);
    return false;
  }
  return true;
}

sqlite3 *replay_repository_detail::OpenDatabase(
    const std::filesystem::path &path, std::string &errorMessage) {
  return openReplayDatabase(path, errorMessage);
}

std::filesystem::path replay_repository_detail::ResolvedDatabasePath(
    const std::filesystem::path &databasePath) {
  return resolvedReplayDatabasePath(databasePath);
}

bool replay_repository_detail::EquivalentDatabasePaths(
    const std::filesystem::path &first,
    const std::filesystem::path &second) {
  return equivalentReplayDatabasePaths(first, second);
}

bool replay_repository_detail::MigrateSchema(sqlite3 *database) {
  return migrateReplayDatabaseSchema(database);
}
