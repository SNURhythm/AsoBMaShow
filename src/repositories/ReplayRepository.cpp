#include "ReplayRepository.h"

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
  case 4:
    return ReplayEventAction::Gauge;
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
  case 7:
    return GaugeProfile::Standard5Keys;
  case 8:
    return GaugeProfile::Standard9Keys;
  case 9:
    return GaugeProfile::Standard24Keys;
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
  summary.gaugeAutoShift =
      gaugeAutoShiftModeFromValue(sqlite3_column_int(stmt, 7));
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

std::optional<int> insertReplayRows(
    sqlite3 *db, const ReplayData &replay, const std::string &provenanceJson,
    std::optional<std::string_view> attemptId,
    std::optional<std::string_view> attemptFingerprint) {
  const char *replayInsert =
      "INSERT INTO replays ("
      "chart_path, chart_md5, chart_sha256, chart_title, chart_artist,"
      "gauge_type, gauge_auto_shift, final_score, max_combo, final_gauge,"
      "clear_type,"
      "random_seed, random_prng, random_values, play_option, play_option_seed,"
      "play_option2, play_option2_seed, assist_option, ln_mode,"
      "ruleset_version, eligibility, provenance_json, attempt_id,"
      "attempt_fingerprint"
      ") VALUES ("
      "@chart_path, @chart_md5, @chart_sha256, @chart_title, @chart_artist,"
      "@gauge_type, @gauge_auto_shift, @final_score, @max_combo,"
      "@final_gauge, @clear_type, @random_seed, @random_prng, @random_values,"
      "@play_option, @play_option_seed, @play_option2, @play_option2_seed,"
      "@assist_option, @ln_mode, @ruleset_version, @eligibility,"
      "@provenance_json, @attempt_id, @attempt_fingerprint"
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
                   gaugeAutoShiftModeValue(replay.gaugeAutoShift));
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
  if (attemptId.has_value()) {
    sqlite3_bind_text(replayStmt.get(), bindIndex++, attemptId->data(),
                      static_cast<int>(attemptId->size()), SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(replayStmt.get(), bindIndex++);
  }
  if (attemptFingerprint.has_value()) {
    sqlite3_bind_text(replayStmt.get(), bindIndex++,
                      attemptFingerprint->data(),
                      static_cast<int>(attemptFingerprint->size()),
                      SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(replayStmt.get(), bindIndex++);
  }

  int rc = sqlite3_step(replayStmt.get());
  replayStmt.reset();
  if (rc != SQLITE_DONE) {
    logSqlError("saving replay", db);
    return std::nullopt;
  }

  const sqlite3_int64 storedReplayId = sqlite3_last_insert_rowid(db);
  if (storedReplayId <= 0 ||
      storedReplayId > static_cast<sqlite3_int64>(
                           std::numeric_limits<int>::max())) {
    SDL_Log("Refusing replay row ID outside the supported integer range");
    return std::nullopt;
  }
  const int replayId = static_cast<int>(storedReplayId);
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
  if (stage == nullptr || stage->longNoteMode <= long_note_mode::kUnknownValue ||
      long_note_mode::normalizeValue(stage->longNoteMode) !=
          stage->longNoteMode) {
    return reject(
        "score long-note mode does not match a unique provenance stage");
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

std::optional<std::string> validateChartResultAttempt(
    const result_persistence::ChartResultAttempt &attempt,
    std::string &diagnostic) {
  diagnostic.clear();
  if (!uuid::isCanonicalLowerV4(attempt.attemptId)) {
    diagnostic = "attempt ID is not a canonical version-4 UUID";
    return std::nullopt;
  }
  if (!isCanonicalLowerHex(attempt.payloadFingerprint, 64) ||
      attempt.payloadFingerprint != result_persistence::payloadFingerprint(
                                        attempt.replay, attempt.score)) {
    diagnostic = "attempt fingerprint is malformed or inconsistent";
    return std::nullopt;
  }

  const auto &score = attempt.score;
  if (!result_persistence::hasProjectableChartIdentity(score)) {
    diagnostic = "score chart identity is not projectable";
    return std::nullopt;
  }
  const std::string replayMd5 = normalizedHash(attempt.replay.chartMeta.MD5);
  const std::string replaySha256 =
      normalizedHash(attempt.replay.chartMeta.SHA256);
  const std::string replayPath =
      Utils::GetStoragePathUtf8RelativeToDocuments(
          attempt.replay.chartMeta.BmsPath, "BMS/");
  if (score.chartMd5 != replayMd5 || score.chartSha256 != replaySha256 ||
      score.chartPath != replayPath ||
      score.chartTitle != attempt.replay.chartMeta.Title ||
      score.chartArtist != attempt.replay.chartMeta.Artist) {
    diagnostic = "score and replay chart identities disagree";
    return std::nullopt;
  }
  if (score.provenance != attempt.replay.provenance) {
    diagnostic = "score and replay provenance disagree";
    return std::nullopt;
  }
  if (!validateChartScoreReplaySemantics(
          score, attempt.replay.chartMeta, attempt.replay.finalScore,
          attempt.replay.maxCombo, attempt.replay.finalGauge,
          attempt.replay.clearType, attempt.replay.chartMeta.TotalNotes,
          std::nullopt, diagnostic)) {
    return std::nullopt;
  }

  std::string provenanceError;
  auto provenanceJson =
      serializeValidatedScoreProvenance(score.provenance, provenanceError);
  if (!provenanceJson.has_value()) {
    diagnostic = "result provenance is invalid";
    return std::nullopt;
  }
  return provenanceJson;
}

enum class ExistingAttemptStatus {
  Found,
  NotFound,
  StorageFailure,
  IntegrityConflict,
};

struct ExistingAttempt {
  int replayId = 0;
  std::string fingerprint;
  std::string createdAt;
  bool scorePending = false;
};

struct ExistingAttemptOutcome {
  ExistingAttemptStatus status = ExistingAttemptStatus::StorageFailure;
  ExistingAttempt value;
  std::string diagnostic;
};

bool bindTextView(sqlite3_stmt *stmt, int index, std::string_view value) {
  if (value.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  return sqlite3_bind_text(stmt, index, value.data(),
                           static_cast<int>(value.size()), SQLITE_TRANSIENT) ==
         SQLITE_OK;
}

ExistingAttemptOutcome findExistingAttempt(sqlite3 *db,
                                           std::string_view attemptId) {
  SqliteStatementHandle replayStmt;
  if (!prepareSqliteStatementLogged(
          db,
          "SELECT id, attempt_id, attempt_fingerprint, created_at "
          "FROM replays WHERE attempt_id = ?",
          replayStmt, "preparing staged replay lookup", logSqlErrorText) ||
      !bindTextView(replayStmt.get(), 1, attemptId)) {
    return {.status = ExistingAttemptStatus::StorageFailure,
            .diagnostic = "could not query the staged replay"};
  }

  int rc = sqlite3_step(replayStmt.get());
  if (rc == SQLITE_DONE) {
    replayStmt.reset();
    SqliteStatementHandle orphanStmt;
    if (!prepareSqliteStatementLogged(
            db,
            "SELECT 1 FROM pending_chart_score_writes "
            "WHERE attempt_id = ? LIMIT 1",
            orphanStmt, "preparing orphaned pending score lookup",
            logSqlErrorText) ||
        !bindTextView(orphanStmt.get(), 1, attemptId)) {
      return {.status = ExistingAttemptStatus::StorageFailure,
              .diagnostic = "could not query pending score identity"};
    }
    rc = sqlite3_step(orphanStmt.get());
    if (rc == SQLITE_ROW) {
      return {.status = ExistingAttemptStatus::IntegrityConflict,
              .diagnostic =
                  "pending score identity has no matching staged replay"};
    }
    if (rc != SQLITE_DONE) {
      return {.status = ExistingAttemptStatus::StorageFailure,
              .diagnostic = "could not query pending score identity"};
    }

    SqliteStatementHandle noncanonicalStmt;
    if (!prepareSqliteStatementLogged(
            db,
            "SELECT 1 FROM replays WHERE attempt_id IS NOT NULL "
            "AND lower(attempt_id) = ? LIMIT 1",
            noncanonicalStmt, "preparing noncanonical replay identity lookup",
            logSqlErrorText) ||
        !bindTextView(noncanonicalStmt.get(), 1, attemptId)) {
      return {.status = ExistingAttemptStatus::StorageFailure,
              .diagnostic = "could not query staged replay identity"};
    }
    rc = sqlite3_step(noncanonicalStmt.get());
    if (rc == SQLITE_ROW) {
      return {.status = ExistingAttemptStatus::IntegrityConflict,
              .diagnostic = "stored replay attempt identity is malformed"};
    }
    if (rc != SQLITE_DONE) {
      return {.status = ExistingAttemptStatus::StorageFailure,
              .diagnostic = "could not query staged replay identity"};
    }
    return {.status = ExistingAttemptStatus::NotFound};
  }
  if (rc != SQLITE_ROW) {
    return {.status = ExistingAttemptStatus::StorageFailure,
            .diagnostic = "could not query the staged replay"};
  }

  const sqlite3_int64 replayId64 = sqlite3_column_int64(replayStmt.get(), 0);
  const bool validReplay =
      sqlite3_column_type(replayStmt.get(), 0) == SQLITE_INTEGER &&
      replayId64 > 0 &&
      replayId64 <=
          static_cast<sqlite3_int64>(std::numeric_limits<int>::max()) &&
      sqlite3_column_type(replayStmt.get(), 1) == SQLITE_TEXT &&
      sqliteColumnString(replayStmt.get(), 1) == attemptId &&
      sqlite3_column_type(replayStmt.get(), 2) == SQLITE_TEXT &&
      isCanonicalLowerHex(sqliteColumnTextView(replayStmt.get(), 2), 64) &&
      sqlite3_column_type(replayStmt.get(), 3) == SQLITE_TEXT &&
      !sqliteColumnTextView(replayStmt.get(), 3).empty();
  if (!validReplay) {
    return {.status = ExistingAttemptStatus::IntegrityConflict,
            .diagnostic = "stored replay attempt identity is malformed"};
  }

  ExistingAttempt existing{
      .replayId = static_cast<int>(replayId64),
      .fingerprint = sqliteColumnString(replayStmt.get(), 2),
      .createdAt = sqliteColumnString(replayStmt.get(), 3),
  };
  if (sqlite3_step(replayStmt.get()) != SQLITE_DONE) {
    return {.status = ExistingAttemptStatus::StorageFailure,
            .diagnostic = "staged replay identity lookup did not complete"};
  }
  replayStmt.reset();

  SqliteStatementHandle pendingStmt;
  if (!prepareSqliteStatementLogged(
          db,
          "SELECT attempt_id, replay_id FROM pending_chart_score_writes "
          "WHERE attempt_id = ? OR replay_id = ? ORDER BY attempt_id",
          pendingStmt, "preparing staged replay outbox linkage lookup",
          logSqlErrorText) ||
      !bindTextView(pendingStmt.get(), 1, attemptId) ||
      sqlite3_bind_int(pendingStmt.get(), 2, existing.replayId) != SQLITE_OK) {
    return {.status = ExistingAttemptStatus::StorageFailure,
            .diagnostic = "could not query staged replay outbox linkage"};
  }

  int linkedRows = 0;
  while ((rc = sqlite3_step(pendingStmt.get())) == SQLITE_ROW) {
    ++linkedRows;
    if (linkedRows > 1 ||
        sqlite3_column_type(pendingStmt.get(), 0) != SQLITE_TEXT ||
        sqliteColumnString(pendingStmt.get(), 0) != attemptId ||
        sqlite3_column_type(pendingStmt.get(), 1) != SQLITE_INTEGER ||
        sqlite3_column_int64(pendingStmt.get(), 1) != existing.replayId) {
      return {.status = ExistingAttemptStatus::IntegrityConflict,
              .diagnostic = "staged replay outbox linkage is malformed"};
    }
    existing.scorePending = true;
  }
  if (rc != SQLITE_DONE) {
    return {.status = ExistingAttemptStatus::StorageFailure,
            .diagnostic = "could not query staged replay outbox linkage"};
  }
  return {.status = ExistingAttemptStatus::Found,
          .value = std::move(existing)};
}

bool insertPendingChartScore(
    sqlite3 *db, const result_persistence::ChartResultAttempt &attempt,
    int replayId, std::string_view provenanceJson,
    std::string_view createdAt) {
  constexpr const char *query =
      "INSERT INTO pending_chart_score_writes ("
      "attempt_id, replay_id, chart_path, chart_md5, chart_sha256,"
      "chart_title, chart_artist, ln_mode, score, max_score, max_combo,"
      "combo_break, pgreat, great, good, bad, poor, kpoor, fast, slow,"
      "final_gauge, clear_type, ruleset_version, eligibility,"
      "provenance_json, created_at"
      ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
      "?, ?, ?, ?, ?, ?, ?)";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing pending chart score insert",
                                    logSqlErrorText)) {
    return false;
  }

  const auto &score = attempt.score;
  int index = 1;
  bool bound = bindTextView(stmt.get(), index++, attempt.attemptId);
  bound = bound && sqlite3_bind_int(stmt.get(), index++, replayId) == SQLITE_OK;
  bound = bound && bindTextView(stmt.get(), index++, score.chartPath);
  bound = bound && bindTextView(stmt.get(), index++, score.chartMd5);
  bound = bound && bindTextView(stmt.get(), index++, score.chartSha256);
  bound = bound && bindTextView(stmt.get(), index++, score.chartTitle);
  bound = bound && bindTextView(stmt.get(), index++, score.chartArtist);
  const auto bindInt = [&](int value) {
    bound = bound &&
            sqlite3_bind_int(stmt.get(), index++, value) == SQLITE_OK;
  };
  bindInt(score.longNoteMode);
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
  bound = bound &&
          sqlite3_bind_double(stmt.get(), index++, score.finalGauge) ==
              SQLITE_OK;
  bindInt(score.clearType);
  bindInt(score.provenance.ruleset.version);
  bindInt(static_cast<int>(score.provenance.eligibility));
  bound = bound && bindTextView(stmt.get(), index++, provenanceJson);
  bound = bound && bindTextView(stmt.get(), index++, createdAt);
  if (!bound || index != 27) {
    logSqlError("binding pending chart score", db);
    return false;
  }
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    logSqlError("saving pending chart score", db);
    return false;
  }
  return true;
}

std::optional<std::string> readStagedReplayTimestamp(
    sqlite3 *db, int replayId, std::string_view attemptId) {
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db,
          "SELECT created_at FROM replays WHERE id = ? AND attempt_id = ?",
          stmt, "preparing staged replay timestamp lookup", logSqlErrorText) ||
      sqlite3_bind_int(stmt.get(), 1, replayId) != SQLITE_OK ||
      !bindTextView(stmt.get(), 2, attemptId)) {
    return std::nullopt;
  }
  if (sqlite3_step(stmt.get()) != SQLITE_ROW ||
      sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
      sqliteColumnTextView(stmt.get(), 0).empty()) {
    return std::nullopt;
  }
  std::string createdAt = sqliteColumnString(stmt.get(), 0);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    return std::nullopt;
  }
  return createdAt;
}

constexpr const char *kPendingChartScoreSelect =
    "SELECT p.attempt_id, p.replay_id, p.chart_path, p.chart_md5,"
    "p.chart_sha256, p.chart_title, p.chart_artist, p.ln_mode, p.score,"
    "p.max_score, p.max_combo, p.combo_break, p.pgreat, p.great, p.good,"
    "p.bad, p.poor, p.kpoor, p.fast, p.slow, p.final_gauge, p.clear_type,"
    "p.ruleset_version, p.eligibility, p.provenance_json, p.created_at,"
    "p.recovery_attempts, p.last_recovery_at, r.id, r.attempt_id,"
    "r.attempt_fingerprint, r.chart_path, r.chart_md5, r.chart_sha256,"
    "r.chart_title, r.chart_artist, r.created_at, r.final_score,"
    "r.max_combo, r.final_gauge, r.clear_type, r.ruleset_version,"
    "r.eligibility, r.provenance_json, r.ln_mode "
    "FROM pending_chart_score_writes p "
    "LEFT JOIN replays r ON r.id = p.replay_id ";

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
  if (!readStrictInteger(stmt, 1, pending.replayId) ||
      pending.replayId <= 0) {
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
      linkedReplayId != pending.replayId ||
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
  ReplayResultOutboxSchemaState resultOutboxState{};
  if (!inspectReplayResultOutboxSchema(database.get(), resultOutboxState) ||
      resultOutboxState != ReplayResultOutboxSchemaState::Exact) {
    errorMessage = "trusted replay database result outbox schema changed";
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
    if (migrateReplayDatabaseSchema(sessionDatabase_)) {
      errorMessage.clear();
      return true;
    }
    errorMessage = "replay database validation failed";
    return false;
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
      return migrateReplayDatabaseSchema(sessionDatabase_);
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

  const auto replayId = insertReplayRows(db, replay, provenanceJson,
                                         std::nullopt, std::nullopt);
  if (!replayId.has_value()) {
    return std::nullopt;
  }

  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing replay save", transactionError);
    return std::nullopt;
  }

  return *replayId;
}

result_persistence::StageOutcome ReplayDBHelper::StageChartResult(
    const result_persistence::ChartResultAttempt &attempt) {
  using result_persistence::PendingReadStatus;
  using result_persistence::StageOutcome;
  using result_persistence::StageReceipt;
  using result_persistence::StageStatus;

  profile_database_activity::WriteGuard writeGuard;
  std::string validationDiagnostic;
  const auto provenanceJson =
      validateChartResultAttempt(attempt, validationDiagnostic);
  if (!provenanceJson.has_value()) {
    return {.status = StageStatus::IntegrityConflict,
            .diagnostic = std::move(validationDiagnostic)};
  }

  std::lock_guard lock(sessionMutex_);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }

  std::string transactionError;
  SqliteTransactionHandle transaction(sessionDatabase_,
                                      "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    logSqlErrorText("starting chart result staging", transactionError);
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not start result staging"};
  }

  ExistingAttemptOutcome existing =
      findExistingAttempt(sessionDatabase_, attempt.attemptId);
  if (existing.status == ExistingAttemptStatus::StorageFailure) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = std::move(existing.diagnostic)};
  }
  if (existing.status == ExistingAttemptStatus::IntegrityConflict) {
    return {.status = StageStatus::IntegrityConflict,
            .diagnostic = std::move(existing.diagnostic)};
  }
  if (existing.status == ExistingAttemptStatus::Found) {
    if (existing.value.fingerprint != attempt.payloadFingerprint) {
      return {.status = StageStatus::IntegrityConflict,
              .diagnostic =
                  "attempt ID already names a different result payload"};
    }
    if (existing.value.scorePending) {
      auto pending = loadPendingChartScoreOnConnection(sessionDatabase_,
                                                       attempt.attemptId);
      if (pending.status == PendingReadStatus::StorageFailure) {
        return {.status = StageStatus::StorageFailure,
                .diagnostic = pending.diagnostic.empty()
                                  ? "could not validate the pending score"
                                  : std::move(pending.diagnostic)};
      }
      if (pending.status != PendingReadStatus::Found ||
          !pending.value.has_value()) {
        return {.status = StageStatus::IntegrityConflict,
                .diagnostic = pending.diagnostic.empty()
                                  ? "staged result has no valid pending score"
                                  : std::move(pending.diagnostic)};
      }
      if (pending.value->attemptId != attempt.attemptId ||
          pending.value->replayId != existing.value.replayId ||
          pending.value->createdAt != existing.value.createdAt ||
          pending.value->score != attempt.score) {
        return {.status = StageStatus::IntegrityConflict,
                .diagnostic =
                    "pending score differs from the staged result payload"};
      }
    }
    if (!transaction.commit(transactionError)) {
      logSqlErrorText("finishing idempotent chart result staging",
                      transactionError);
      return {.status = StageStatus::StorageFailure,
              .diagnostic = "could not finish result staging"};
    }
    return {
        .status = StageStatus::AlreadyStaged,
        .receipt = StageReceipt{.attemptId = attempt.attemptId,
                                .replayId = existing.value.replayId,
                                .createdAt = existing.value.createdAt,
                                .scorePending = existing.value.scorePending},
    };
  }

  const auto replayId =
      insertReplayRows(sessionDatabase_, attempt.replay, *provenanceJson,
                       attempt.attemptId, attempt.payloadFingerprint);
  if (!replayId.has_value()) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not stage the replay"};
  }
  const auto createdAt = readStagedReplayTimestamp(
      sessionDatabase_, *replayId, attempt.attemptId);
  if (!createdAt.has_value()) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not read the staged result timestamp"};
  }
  if (!insertPendingChartScore(sessionDatabase_, attempt, *replayId,
                               *provenanceJson, *createdAt)) {
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not stage the pending score"};
  }
  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing chart result staging", transactionError);
    return {.status = StageStatus::StorageFailure,
            .diagnostic = "could not commit result staging"};
  }
  return {
      .status = StageStatus::Staged,
      .receipt = StageReceipt{.attemptId = attempt.attemptId,
                              .replayId = *replayId,
                              .createdAt = *createdAt,
                              .scorePending = true},
  };
}

result_persistence::PendingReadOutcome
ReplayDBHelper::LoadPendingChartScore(std::string_view attemptId) {
  using result_persistence::PendingReadStatus;
  profile_database_activity::ReadGuard readGuard;
  if (!uuid::isCanonicalLowerV4(attemptId)) {
    return {.status = PendingReadStatus::IntegrityConflict,
            .diagnostic = "attempt ID is not a canonical version-4 UUID"};
  }
  std::lock_guard lock(sessionMutex_);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = PendingReadStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  return loadPendingChartScoreOnConnection(sessionDatabase_, attemptId);
}

result_persistence::PendingBatchOutcome
ReplayDBHelper::ListPendingChartScores(std::size_t limit) {
  using result_persistence::PendingBatchOutcome;
  profile_database_activity::ReadGuard readGuard;
  std::lock_guard lock(sessionMutex_);
  if (!EnsureSessionDatabaseLocked()) {
    return {.storageAvailable = false,
            .diagnostic = "replay storage is unavailable"};
  }

  SqliteStatementHandle countStmt;
  if (!prepareSqliteStatementLogged(
          sessionDatabase_,
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
  if (!prepareSqliteStatementLogged(sessionDatabase_, query, stmt,
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
ReplayDBHelper::AcknowledgePendingChartScore(std::string_view attemptId,
                                             int replayId) {
  using result_persistence::AcknowledgeStatus;
  using result_persistence::PendingReadStatus;
  profile_database_activity::WriteGuard writeGuard;
  if (!uuid::isCanonicalLowerV4(attemptId) || replayId <= 0) {
    return {.status = AcknowledgeStatus::IntegrityConflict,
            .diagnostic = "pending score identity is malformed"};
  }

  std::lock_guard lock(sessionMutex_);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }
  std::string transactionError;
  SqliteTransactionHandle transaction(sessionDatabase_,
                                      "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = "could not start score acknowledgement"};
  }

  auto pending =
      loadPendingChartScoreOnConnection(sessionDatabase_, attemptId);
  if (pending.status == PendingReadStatus::StorageFailure) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = std::move(pending.diagnostic)};
  }
  if (pending.status == PendingReadStatus::IntegrityConflict) {
    return {.status = AcknowledgeStatus::IntegrityConflict,
            .diagnostic = std::move(pending.diagnostic)};
  }
  if (pending.status == PendingReadStatus::Found) {
    if (!pending.value.has_value() || pending.value->replayId != replayId) {
      return {.status = AcknowledgeStatus::IntegrityConflict,
              .diagnostic = "pending score names a different replay"};
    }
    SqliteStatementHandle deleteStmt;
    if (!prepareSqliteStatementLogged(
            sessionDatabase_,
            "DELETE FROM pending_chart_score_writes "
            "WHERE attempt_id = ? AND replay_id = ?",
            deleteStmt, "preparing pending chart score acknowledgement",
            logSqlErrorText) ||
        !bindTextView(deleteStmt.get(), 1, attemptId) ||
        sqlite3_bind_int(deleteStmt.get(), 2, replayId) != SQLITE_OK ||
        sqlite3_step(deleteStmt.get()) != SQLITE_DONE ||
        sqlite3_changes(sessionDatabase_) != 1) {
      return {.status = AcknowledgeStatus::StorageFailure,
              .diagnostic = "could not acknowledge the pending score"};
    }
    if (!transaction.commit(transactionError)) {
      return {.status = AcknowledgeStatus::StorageFailure,
              .diagnostic = "could not commit score acknowledgement"};
    }
    return {.status = AcknowledgeStatus::Acknowledged};
  }

  ExistingAttemptOutcome existing =
      findExistingAttempt(sessionDatabase_, attemptId);
  if (existing.status == ExistingAttemptStatus::StorageFailure) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = std::move(existing.diagnostic)};
  }
  if (existing.status != ExistingAttemptStatus::Found ||
      existing.value.replayId != replayId || existing.value.scorePending) {
    return {.status = AcknowledgeStatus::IntegrityConflict,
            .diagnostic =
                "acknowledged score has no matching durable replay identity"};
  }
  if (!transaction.commit(transactionError)) {
    return {.status = AcknowledgeStatus::StorageFailure,
            .diagnostic = "could not finish score acknowledgement"};
  }
  return {.status = AcknowledgeStatus::AlreadyAcknowledged};
}

result_persistence::RecoveryMarkOutcome
ReplayDBHelper::RecordPendingChartScoreRecoveryAttempt(
    std::string_view attemptId,
    result_persistence::RecoveryAttemptKind kind) {
  using result_persistence::RecoveryMarkStatus;
  profile_database_activity::WriteGuard writeGuard;
  (void)kind;
  std::lock_guard lock(sessionMutex_);
  if (!EnsureSessionDatabaseLocked()) {
    return {.status = RecoveryMarkStatus::StorageFailure,
            .diagnostic = "replay storage is unavailable"};
  }

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          sessionDatabase_,
          "UPDATE pending_chart_score_writes "
          "SET recovery_attempts = recovery_attempts + 1, "
          "last_recovery_at = CURRENT_TIMESTAMP WHERE attempt_id = ?",
          stmt, "preparing pending score recovery marker", logSqlErrorText) ||
      !bindTextView(stmt.get(), 1, attemptId) ||
      sqlite3_step(stmt.get()) != SQLITE_DONE) {
    return {.status = RecoveryMarkStatus::StorageFailure,
            .diagnostic = "could not record the pending score recovery"};
  }
  const int changes = sqlite3_changes(sessionDatabase_);
  if (changes == 0) {
    return {.status = RecoveryMarkStatus::NotFound};
  }
  if (changes != 1) {
    return {.status = RecoveryMarkStatus::StorageFailure,
            .diagnostic = "pending score recovery updated unexpected rows"};
  }
  return {.status = RecoveryMarkStatus::Recorded};
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
                   gaugeAutoShiftModeValue(replay.gaugeAutoShift));
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
    auto stageReplayId = insertReplayRows(
        db, replay.stages[i].replay, stageProvenanceJson[i], std::nullopt,
        std::nullopt);
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
      "r.ruleset_version, r.eligibility, r.provenance_json "
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
    const auto provenance =
        readStoredProvenance(detailStmt.get(), 20, 21, 22, "replay summary");
    if (!provenance.has_value()) {
      continue;
    }
    summary.playback = provenance->playback;
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
      const sqlite3_int64 candidateId =
          sqlite3_column_int64(candidateStmt.get(), 0);
      beforeId = candidateId;
      if (candidateId <= 0 ||
          candidateId > std::numeric_limits<int>::max()) {
        ++rejected;
        continue;
      }
      const int publicId = static_cast<int>(candidateId);
      std::string provenanceError;
      const bool aggregateValid =
          decodeStoredProvenance(candidateStmt.get(), 1, 2, 3, provenanceError)
              .has_value();
      const CourseReplayStageDescriptorReadResult stageResult =
          aggregateValid
              ? courseReplayStageProvenanceStatus(
                    stageProvenanceStmt.get(), publicId, provenanceError)
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
      validIds.push_back(publicId);
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
      "cr.ruleset_version, cr.eligibility, cr.provenance_json "
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
    summary.gaugeAutoShift = gaugeAutoShiftModeFromValue(
        sqlite3_column_int(detailStmt.get(), 2));
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
    const auto provenance = readStoredProvenance(detailStmt.get(), 15, 16, 17,
                                                 "course replay summary");
    if (!provenance.has_value()) {
      continue;
    }
    summary.playback = provenance->playback;
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
            stageStmt.get(), sqlite3_column_int64(row, 0), completedCharts,
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
    loaded.gaugeAutoShift =
        gaugeAutoShiftModeFromValue(sqlite3_column_int(stmt.get(), 7));
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
    loaded.gaugeAutoShiftLowerBound =
        loaded.provenance.gaugeAutoShiftLowerBound;
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
  courseReplay.gaugeAutoShift =
      gaugeAutoShiftModeFromValue(sqlite3_column_int(stmt.get(), 8));
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
  courseReplay.gaugeAutoShiftLowerBound =
      courseReplay.provenance.gaugeAutoShiftLowerBound;
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
