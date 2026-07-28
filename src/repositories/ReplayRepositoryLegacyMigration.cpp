#include "ReplayRepositoryLegacyMigration.h"

#include "SqliteRAII.h"

#include "../ResultContracts.h"
#include "../ScoreProvenance.h"
#include "../replay/ReplayLimits.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace replay_repository_legacy {
namespace {

constexpr const char *kReceiptAttemptIndexSql =
    "CREATE INDEX idx_ir_submission_receipts_attempt ON "
    "ir_submission_receipts(provider_id, server_origin, attempt_id)";
constexpr const char *kReceiptRemoteScoreIndexSql =
    "CREATE INDEX idx_ir_submission_receipts_remote_score ON "
    "ir_submission_receipts(provider_id, server_origin, remote_score_id)";

bool run(sqlite3 *database, std::string_view sql, std::string_view context) {
  char *rawError = nullptr;
  const std::string statement(sql);
  const int rc =
      sqlite3_exec(database, statement.c_str(), nullptr, nullptr, &rawError);
  if (rc == SQLITE_OK) {
    return true;
  }
  SDL_Log("Replay summary migration failed while %s: %s",
          std::string(context).c_str(),
          rawError != nullptr ? rawError : sqlite3_errmsg(database));
  sqlite3_free(rawError);
  return false;
}

bool tableExists(sqlite3 *database, std::string_view table, bool &exists) {
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(
          database, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
          statement) != SQLITE_OK ||
      sqlite3_bind_text(statement.get(), 1, table.data(),
                        static_cast<int>(table.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    return false;
  }
  const int rc = sqlite3_step(statement.get());
  exists = rc == SQLITE_ROW;
  return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

std::string normalizedSchemaSql(std::string_view sql) {
  std::string normalized;
  normalized.reserve(sql.size());
  for (const unsigned char character : sql) {
    if (std::isspace(character) == 0) {
      normalized.push_back(
          static_cast<char>(std::tolower(static_cast<int>(character))));
    }
  }
  return normalized;
}

bool exactSchemaObject(sqlite3 *database, std::string_view name,
                       std::string_view type, std::string_view owner,
                       std::string_view expectedSql) {
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(
          database,
          "SELECT type,tbl_name,sql FROM sqlite_master WHERE name=? "
          "COLLATE NOCASE",
          statement) != SQLITE_OK ||
      sqlite3_bind_text(statement.get(), 1, name.data(),
                        static_cast<int>(name.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_step(statement.get()) != SQLITE_ROW ||
      sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT ||
      sqlite3_column_type(statement.get(), 1) != SQLITE_TEXT ||
      sqlite3_column_type(statement.get(), 2) != SQLITE_TEXT ||
      sqliteColumnTextView(statement.get(), 0) != type ||
      normalizedSchemaSql(sqliteColumnTextView(statement.get(), 1)) !=
          normalizedSchemaSql(owner) ||
      normalizedSchemaSql(sqliteColumnTextView(statement.get(), 2)) !=
          normalizedSchemaSql(expectedSql)) {
    return false;
  }
  return sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool hasColumns(sqlite3 *database, std::string_view table,
                std::span<const std::string_view> required) {
  SqliteStatementHandle statement;
  const std::string query = "PRAGMA table_info(\"" + std::string(table) + "\")";
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK) {
    return false;
  }
  std::vector<std::string> columns;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(statement.get(), 1) != SQLITE_TEXT) {
      return false;
    }
    columns.push_back(sqliteColumnString(statement.get(), 1));
  }
  if (rc != SQLITE_DONE) {
    return false;
  }
  return std::ranges::all_of(required, [&](std::string_view name) {
    return std::ranges::find(columns, name) != columns.end();
  });
}

bool validateSourceHeaders(sqlite3 *database) {
  constexpr std::array chartColumns{
      std::string_view("id"),
      std::string_view("chart_path"),
      std::string_view("chart_md5"),
      std::string_view("chart_sha256"),
      std::string_view("chart_title"),
      std::string_view("chart_artist"),
      std::string_view("ln_mode"),
      std::string_view("final_score"),
      std::string_view("max_combo"),
      std::string_view("final_gauge"),
      std::string_view("clear_type"),
      std::string_view("created_at"),
      std::string_view("ruleset_version"),
      std::string_view("eligibility"),
      std::string_view("provenance_json"),
  };
  constexpr std::array courseColumns{
      std::string_view("id"),
      std::string_view("course_id"),
      std::string_view("course_key"),
      std::string_view("course_name"),
      std::string_view("course_group_name"),
      std::string_view("constraint_json"),
      std::string_view("final_score"),
      std::string_view("max_combo"),
      std::string_view("final_gauge"),
      std::string_view("clear_type"),
      std::string_view("completed_charts"),
      std::string_view("total_charts"),
      std::string_view("created_at"),
      std::string_view("ruleset_version"),
      std::string_view("eligibility"),
      std::string_view("provenance_json"),
  };
  return hasColumns(database, "replays", chartColumns) &&
         hasColumns(database, "course_replays", courseColumns);
}

std::optional<std::string> textValue(sqlite3_stmt *source, int column,
                                     bool allowEmpty, bool &partial) {
  if (sqlite3_column_type(source, column) != SQLITE_TEXT) {
    partial = true;
    return std::nullopt;
  }
  const int bytes = sqlite3_column_bytes(source, column);
  if (bytes < 0 ||
      static_cast<std::size_t>(bytes) > replay::kReplayLimits.maxStringBytes) {
    partial = true;
    return std::nullopt;
  }
  std::string value = sqliteColumnString(source, column);
  if (!allowEmpty && value.empty()) {
    partial = true;
    return std::nullopt;
  }
  return value;
}

std::optional<int> intValue(sqlite3_stmt *source, int column, int minimum,
                            int maximum, bool &partial) {
  if (sqlite3_column_type(source, column) != SQLITE_INTEGER) {
    partial = true;
    return std::nullopt;
  }
  const sqlite3_int64 value = sqlite3_column_int64(source, column);
  if (value < minimum || value > maximum) {
    partial = true;
    return std::nullopt;
  }
  return static_cast<int>(value);
}

std::optional<double> numberValue(sqlite3_stmt *source, int column,
                                  double minimum, bool &partial) {
  const int type = sqlite3_column_type(source, column);
  if (type != SQLITE_INTEGER && type != SQLITE_FLOAT) {
    partial = true;
    return std::nullopt;
  }
  const double value = sqlite3_column_double(source, column);
  if (!std::isfinite(value) || value < minimum) {
    partial = true;
    return std::nullopt;
  }
  return value;
}

bool lowerHex(std::string_view value, std::size_t size) {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

std::optional<std::string> hashValue(sqlite3_stmt *source, int column,
                                     std::size_t size, bool &partial) {
  auto value = textValue(source, column, true, partial);
  if (!value || value->empty()) {
    return std::nullopt;
  }
  if (!lowerHex(*value, size)) {
    partial = true;
    return std::nullopt;
  }
  return value;
}

struct ProvenanceFields {
  std::optional<int> rulesetVersion;
  std::optional<int> eligibility;
  std::optional<std::string> json;
};

ProvenanceFields provenanceValue(sqlite3_stmt *source, int rulesetColumn,
                                 int eligibilityColumn, int jsonColumn,
                                 int sourceVersion, bool &partial) {
  if (sourceVersion < 3) {
    partial = true;
    return {};
  }
  bool localPartial = false;
  const auto ruleset = intValue(source, rulesetColumn, 0,
                                std::numeric_limits<int>::max(), localPartial);
  const auto eligibility = intValue(
      source, eligibilityColumn, static_cast<int>(ScoreEligibility::Verified),
      static_cast<int>(ScoreEligibility::LegacyUnverified), localPartial);
  const auto json = textValue(source, jsonColumn, false, localPartial);
  if (localPartial || !ruleset || !eligibility || !json) {
    partial = true;
    return {};
  }
  std::string error;
  const auto decoded = deserializeScoreProvenance(*json, error);
  if (!decoded || decoded->ruleset.version != *ruleset ||
      static_cast<int>(decoded->eligibility) != *eligibility) {
    partial = true;
    return {};
  }
  try {
    if (serializeScoreProvenance(*decoded) != *json) {
      partial = true;
      return {};
    }
  } catch (const std::runtime_error &) {
    // Older provenance schemas admitted incomplete stage facts that the
    // current canonical serializer rejects. They are an unavailable optional
    // header field, not a reason to lose the independently stored result row.
    partial = true;
    return {};
  }
  return {.rulesetVersion = ruleset, .eligibility = eligibility, .json = json};
}

bool bindTextOrNull(sqlite3_stmt *statement, int column,
                    const std::optional<std::string> &value) {
  return value ? sqlite3_bind_text(statement, column, value->c_str(),
                                   static_cast<int>(value->size()),
                                   SQLITE_TRANSIENT) == SQLITE_OK
               : sqlite3_bind_null(statement, column) == SQLITE_OK;
}

bool bindIntOrNull(sqlite3_stmt *statement, int column,
                   const std::optional<int> &value) {
  return value ? sqlite3_bind_int(statement, column, *value) == SQLITE_OK
               : sqlite3_bind_null(statement, column) == SQLITE_OK;
}

bool bindNumberOrNull(sqlite3_stmt *statement, int column,
                      const std::optional<double> &value) {
  return value ? sqlite3_bind_double(statement, column, *value) == SQLITE_OK
               : sqlite3_bind_null(statement, column) == SQLITE_OK;
}

bool copyChartHeaders(sqlite3 *database, int sourceVersion,
                      sqlite3_int64 &sourceCount) {
  constexpr const char *select =
      "SELECT id,chart_path,chart_md5,chart_sha256,chart_title,chart_artist,"
      "ln_mode,final_score,max_combo,final_gauge,clear_type,created_at,"
      "ruleset_version,eligibility,provenance_json FROM replays ORDER BY id";
  constexpr const char *insert =
      "INSERT INTO legacy_chart_result_summaries(legacy_replay_id,"
      "chart_path,chart_md5,chart_sha256,chart_title,chart_artist,"
      "long_note_mode,final_score,max_combo,final_gauge,clear_type,created_at,"
      "ruleset_version,eligibility,provenance_json,partial)"
      " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
  SqliteStatementHandle source;
  SqliteStatementHandle destination;
  if (prepareSqliteStatement(database, select, source) != SQLITE_OK ||
      prepareSqliteStatement(database, insert, destination) != SQLITE_OK) {
    return false;
  }
  sourceCount = 0;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(source.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(source.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_int64(source.get(), 0) <= 0) {
      return false;
    }
    bool partial = false;
    auto path = textValue(source.get(), 1, false, partial);
    auto md5 = hashValue(source.get(), 2, 32, partial);
    auto sha256 = hashValue(source.get(), 3, 64, partial);
    if (!md5 && !sha256) {
      partial = true;
    }
    auto title = textValue(source.get(), 4, false, partial);
    auto artist = textValue(source.get(), 5, true, partial);
    auto longNoteMode = intValue(source.get(), 6, 0, 3, partial);
    auto finalScore =
        intValue(source.get(), 7, 0, std::numeric_limits<int>::max(), partial);
    std::optional<int> maxCombo;
    if (sourceVersion >= 2) {
      maxCombo = intValue(source.get(), 8, 0, std::numeric_limits<int>::max(),
                          partial);
    } else {
      partial = true;
    }
    auto finalGauge = numberValue(source.get(), 9, 0.0, partial);
    auto clearType = intValue(source.get(), 10, std::numeric_limits<int>::min(),
                              std::numeric_limits<int>::max(), partial);
    if (clearType && !result_contract::isKnownClearRank(*clearType)) {
      clearType.reset();
      partial = true;
    }
    auto createdAt = textValue(source.get(), 11, false, partial);
    const auto provenance =
        provenanceValue(source.get(), 12, 13, 14, sourceVersion, partial);

    sqlite3_reset(destination.get());
    sqlite3_clear_bindings(destination.get());
    if (sqlite3_bind_int64(destination.get(), 1,
                           sqlite3_column_int64(source.get(), 0)) !=
            SQLITE_OK ||
        !bindTextOrNull(destination.get(), 2, path) ||
        !bindTextOrNull(destination.get(), 3, md5) ||
        !bindTextOrNull(destination.get(), 4, sha256) ||
        !bindTextOrNull(destination.get(), 5, title) ||
        !bindTextOrNull(destination.get(), 6, artist) ||
        !bindIntOrNull(destination.get(), 7, longNoteMode) ||
        !bindIntOrNull(destination.get(), 8, finalScore) ||
        !bindIntOrNull(destination.get(), 9, maxCombo) ||
        !bindNumberOrNull(destination.get(), 10, finalGauge) ||
        !bindIntOrNull(destination.get(), 11, clearType) ||
        !bindTextOrNull(destination.get(), 12, createdAt) ||
        !bindIntOrNull(destination.get(), 13, provenance.rulesetVersion) ||
        !bindIntOrNull(destination.get(), 14, provenance.eligibility) ||
        !bindTextOrNull(destination.get(), 15, provenance.json) ||
        sqlite3_bind_int(destination.get(), 16, partial ? 1 : 0) != SQLITE_OK ||
        sqlite3_step(destination.get()) != SQLITE_DONE ||
        sqlite3_changes(database) != 1) {
      return false;
    }
    ++sourceCount;
  }
  return rc == SQLITE_DONE;
}

bool copyCourseHeaders(sqlite3 *database, int sourceVersion,
                       sqlite3_int64 &sourceCount) {
  constexpr const char *select =
      "SELECT id,course_id,course_key,course_name,course_group_name,"
      "constraint_json,final_score,max_combo,final_gauge,clear_type,"
      "completed_charts,total_charts,created_at,ruleset_version,eligibility,"
      "provenance_json FROM course_replays ORDER BY id";
  constexpr const char *insert =
      "INSERT INTO legacy_course_result_summaries(legacy_course_replay_id,"
      "legacy_course_id,course_key,course_name,course_group_name,"
      "constraint_json,final_score,max_combo,final_gauge,clear_type,"
      "completed_charts,total_charts,created_at,ruleset_version,eligibility,"
      "provenance_json,partial) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
  SqliteStatementHandle source;
  SqliteStatementHandle destination;
  if (prepareSqliteStatement(database, select, source) != SQLITE_OK ||
      prepareSqliteStatement(database, insert, destination) != SQLITE_OK) {
    return false;
  }
  sourceCount = 0;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(source.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(source.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_int64(source.get(), 0) <= 0) {
      return false;
    }
    bool partial = false;
    auto courseId =
        intValue(source.get(), 1, 0, std::numeric_limits<int>::max(), partial);
    auto courseKey = textValue(source.get(), 2, false, partial);
    auto courseName = textValue(source.get(), 3, false, partial);
    auto groupName = textValue(source.get(), 4, true, partial);
    auto constraints = textValue(source.get(), 5, false, partial);
    auto finalScore =
        intValue(source.get(), 6, 0, std::numeric_limits<int>::max(), partial);
    std::optional<int> maxCombo;
    if (sourceVersion >= 2) {
      maxCombo = intValue(source.get(), 7, 0, std::numeric_limits<int>::max(),
                          partial);
    } else {
      partial = true;
    }
    auto finalGauge = numberValue(source.get(), 8, 0.0, partial);
    auto clearType = intValue(source.get(), 9, std::numeric_limits<int>::min(),
                              std::numeric_limits<int>::max(), partial);
    if (clearType && !result_contract::isKnownClearRank(*clearType)) {
      clearType.reset();
      partial = true;
    }
    const int maximumCourseStages =
        static_cast<int>(replay::kReplayLimits.maxCourseStages);
    auto completed =
        intValue(source.get(), 10, 0, maximumCourseStages, partial);
    auto total = intValue(source.get(), 11, 1, maximumCourseStages, partial);
    if (!completed || !total || *completed > *total || *completed < *total) {
      partial = true;
    }
    auto createdAt = textValue(source.get(), 12, false, partial);
    const auto provenance =
        provenanceValue(source.get(), 13, 14, 15, sourceVersion, partial);

    sqlite3_reset(destination.get());
    sqlite3_clear_bindings(destination.get());
    if (sqlite3_bind_int64(destination.get(), 1,
                           sqlite3_column_int64(source.get(), 0)) !=
            SQLITE_OK ||
        !bindIntOrNull(destination.get(), 2, courseId) ||
        !bindTextOrNull(destination.get(), 3, courseKey) ||
        !bindTextOrNull(destination.get(), 4, courseName) ||
        !bindTextOrNull(destination.get(), 5, groupName) ||
        !bindTextOrNull(destination.get(), 6, constraints) ||
        !bindIntOrNull(destination.get(), 7, finalScore) ||
        !bindIntOrNull(destination.get(), 8, maxCombo) ||
        !bindNumberOrNull(destination.get(), 9, finalGauge) ||
        !bindIntOrNull(destination.get(), 10, clearType) ||
        !bindIntOrNull(destination.get(), 11, completed) ||
        !bindIntOrNull(destination.get(), 12, total) ||
        !bindTextOrNull(destination.get(), 13, createdAt) ||
        !bindIntOrNull(destination.get(), 14, provenance.rulesetVersion) ||
        !bindIntOrNull(destination.get(), 15, provenance.eligibility) ||
        !bindTextOrNull(destination.get(), 16, provenance.json) ||
        sqlite3_bind_int(destination.get(), 17, partial ? 1 : 0) != SQLITE_OK ||
        sqlite3_step(destination.get()) != SQLITE_DONE ||
        sqlite3_changes(database) != 1) {
      return false;
    }
    ++sourceCount;
  }
  return rc == SQLITE_DONE;
}

bool countRows(sqlite3 *database, std::string_view table,
               sqlite3_int64 &count) {
  SqliteStatementHandle statement;
  const std::string query = "SELECT COUNT(*) FROM " + std::string(table);
  if (prepareSqliteStatement(database, query, statement) != SQLITE_OK ||
      sqlite3_step(statement.get()) != SQLITE_ROW ||
      sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER) {
    return false;
  }
  count = sqlite3_column_int64(statement.get(), 0);
  return count >= 0 && sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool rebuildReceipts(sqlite3 *database) {
  return run(database, "DROP INDEX idx_ir_submission_receipts_attempt",
             "dropping legacy receipt attempt index") &&
         run(database, "DROP INDEX idx_ir_submission_receipts_remote_score",
             "dropping legacy receipt score index") &&
         run(database,
             "ALTER TABLE ir_submission_receipts RENAME TO "
             "ir_submission_receipts_v13",
             "renaming legacy-owned receipts") &&
         run(database, kReceiptTableSql, "creating summary-owned receipts") &&
         run(database,
             "INSERT INTO ir_submission_receipts("
             "id,provider_id,server_origin,replay_id,modern_chart_result_id,"
             "attempt_id,chart_md5,chart_sha256,remote_user_id,remote_chart_id,"
             "remote_score_id,confirmation_source,observed_in_snapshot,"
             "confirmed_at_ms) SELECT id,provider_id,server_origin,replay_id,"
             "modern_chart_result_id,attempt_id,chart_md5,chart_sha256,"
             "remote_user_id,remote_chart_id,remote_score_id,"
             "confirmation_source,observed_in_snapshot,confirmed_at_ms "
             "FROM ir_submission_receipts_v13",
             "copying durable receipts") &&
         run(database, "DROP TABLE ir_submission_receipts_v13",
             "dropping legacy-owned receipt table") &&
         run(database, kReceiptAttemptIndexSql,
             "creating receipt attempt index") &&
         run(database, kReceiptRemoteScoreIndexSql,
             "creating receipt remote score index");
}

bool retireLegacyWork(sqlite3 *database) {
  return run(
      database,
      "UPDATE ir_outbox SET state=3,next_attempt_at_ms=NULL,"
      "next_request_user_intent=0,last_error_code='legacy_result_cutover',"
      "last_error_message='Submission remains historical because its legacy "
      "result cannot be activated after replay detail removal.' "
      "WHERE local_result_ready=0 AND EXISTS(SELECT 1 FROM replays r "
      "WHERE r.attempt_id=ir_outbox.attempt_id) AND NOT EXISTS(SELECT 1 FROM "
      "modern_chart_results m WHERE m.attempt_id=ir_outbox.attempt_id)",
      "retiring inactive legacy IR work");
}

bool dropRawTables(sqlite3 *database) {
  constexpr std::array statements{
      std::string_view("DROP TABLE pending_chart_score_writes"),
      std::string_view("DROP TABLE course_replay_stages"),
      std::string_view("DROP TABLE replay_events"),
      std::string_view("DROP TABLE replay_touch_samples"),
      std::string_view("DROP TABLE replay_lane_cover_events"),
      std::string_view("DROP TABLE course_replays"),
      std::string_view("DROP TABLE replays"),
  };
  return std::ranges::all_of(statements, [&](std::string_view statement) {
    return run(database, statement, "dropping legacy replay storage");
  });
}

bool noForeignKeyViolations(sqlite3 *database) {
  SqliteStatementHandle statement;
  return prepareSqliteStatement(database, "PRAGMA foreign_key_check",
                                statement) == SQLITE_OK &&
         sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool rawTablesAbsent(sqlite3 *database) {
  constexpr std::array rawTables{
      std::string_view("replays"),
      std::string_view("replay_events"),
      std::string_view("replay_touch_samples"),
      std::string_view("replay_lane_cover_events"),
      std::string_view("course_replays"),
      std::string_view("course_replay_stages"),
      std::string_view("pending_chart_score_writes"),
  };
  for (const auto table : rawTables) {
    bool exists = false;
    if (!tableExists(database, table, exists) || exists) {
      return false;
    }
  }
  return true;
}

bool receiptReferencesSummary(sqlite3 *database) {
  SqliteStatementHandle statement;
  if (prepareSqliteStatement(database,
                             "PRAGMA foreign_key_list("
                             "ir_submission_receipts)",
                             statement) != SQLITE_OK) {
    return false;
  }
  bool legacyOwner = false;
  bool modernOwner = false;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
    const std::string table = sqliteColumnString(statement.get(), 2);
    const std::string from = sqliteColumnString(statement.get(), 3);
    const std::string to = sqliteColumnString(statement.get(), 4);
    legacyOwner |= table == "legacy_chart_result_summaries" &&
                   from == "replay_id" && to == "legacy_replay_id";
    modernOwner |= table == "modern_chart_results" &&
                   from == "modern_chart_result_id" && to == "id";
  }
  return rc == SQLITE_DONE && legacyOwner && modernOwner;
}

} // namespace

bool migrateToSummarySchema(sqlite3 *database, int sourceVersion) {
  if (database == nullptr || sourceVersion < 0 || sourceVersion >= 14 ||
      !validateSourceHeaders(database) ||
      !run(database, kChartSummaryTableSql, "creating chart summaries") ||
      !run(database, kCourseSummaryTableSql, "creating course summaries") ||
      !run(database, kChartShaIndexSql, "indexing chart summary SHA-256") ||
      !run(database, kChartMd5IndexSql, "indexing chart summary MD5") ||
      !run(database, kChartPathIndexSql, "indexing chart summary path") ||
      !run(database, kCourseLookupIndexSql, "indexing course summaries")) {
    return false;
  }

  sqlite3_int64 chartSourceCount = 0;
  sqlite3_int64 courseSourceCount = 0;
  if (!copyChartHeaders(database, sourceVersion, chartSourceCount) ||
      !copyCourseHeaders(database, sourceVersion, courseSourceCount) ||
      !rebuildReceipts(database) || !retireLegacyWork(database) ||
      !dropRawTables(database)) {
    return false;
  }
  sqlite3_int64 chartSummaryCount = 0;
  sqlite3_int64 courseSummaryCount = 0;
  return countRows(database, "legacy_chart_result_summaries",
                   chartSummaryCount) &&
         countRows(database, "legacy_course_result_summaries",
                   courseSummaryCount) &&
         chartSourceCount == chartSummaryCount &&
         courseSourceCount == courseSummaryCount &&
         inspectCurrentSchema(database) && noForeignKeyViolations(database);
}

bool inspectCurrentSchema(sqlite3 *database) {
  constexpr std::array chartColumns{
      std::string_view("legacy_replay_id"), std::string_view("chart_path"),
      std::string_view("chart_md5"),        std::string_view("chart_sha256"),
      std::string_view("chart_title"),      std::string_view("chart_artist"),
      std::string_view("long_note_mode"),   std::string_view("final_score"),
      std::string_view("max_combo"),        std::string_view("final_gauge"),
      std::string_view("clear_type"),       std::string_view("created_at"),
      std::string_view("ruleset_version"),  std::string_view("eligibility"),
      std::string_view("provenance_json"),  std::string_view("partial"),
  };
  constexpr std::array courseColumns{
      std::string_view("legacy_course_replay_id"),
      std::string_view("legacy_course_id"),
      std::string_view("course_key"),
      std::string_view("course_name"),
      std::string_view("course_group_name"),
      std::string_view("constraint_json"),
      std::string_view("final_score"),
      std::string_view("max_combo"),
      std::string_view("final_gauge"),
      std::string_view("clear_type"),
      std::string_view("completed_charts"),
      std::string_view("total_charts"),
      std::string_view("created_at"),
      std::string_view("ruleset_version"),
      std::string_view("eligibility"),
      std::string_view("provenance_json"),
      std::string_view("partial"),
  };
  return database != nullptr && rawTablesAbsent(database) &&
         exactSchemaObject(database, "legacy_chart_result_summaries", "table",
                           "legacy_chart_result_summaries",
                           kChartSummaryTableSql) &&
         exactSchemaObject(database, "legacy_course_result_summaries", "table",
                           "legacy_course_result_summaries",
                           kCourseSummaryTableSql) &&
         exactSchemaObject(database, "idx_legacy_chart_summaries_sha256",
                           "index", "legacy_chart_result_summaries",
                           kChartShaIndexSql) &&
         exactSchemaObject(database, "idx_legacy_chart_summaries_md5", "index",
                           "legacy_chart_result_summaries",
                           kChartMd5IndexSql) &&
         exactSchemaObject(database, "idx_legacy_chart_summaries_path", "index",
                           "legacy_chart_result_summaries",
                           kChartPathIndexSql) &&
         exactSchemaObject(database, "idx_legacy_course_summaries_lookup",
                           "index", "legacy_course_result_summaries",
                           kCourseLookupIndexSql) &&
         hasColumns(database, "legacy_chart_result_summaries", chartColumns) &&
         hasColumns(database, "legacy_course_result_summaries",
                    courseColumns) &&
         receiptReferencesSummary(database);
}

} // namespace replay_repository_legacy
