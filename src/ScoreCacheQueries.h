#pragma once

#include "SqliteRAII.h"
#include "scene/play/RhythmState.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace score_cache_queries {

inline constexpr const char *kScoreDatabaseSchema = "score_db";

namespace detail {

inline std::string qualifiedName(std::string_view schema,
                                 std::string_view name) {
  return std::string(schema.empty() ? "" : std::string(schema) + ".") +
         std::string(name);
}

inline std::string clearRankSummaryTable(std::string_view schema = {}) {
  return qualifiedName(schema, "score_sha256_clear_rank_cache");
}

inline std::string bestScoreSummaryTable(std::string_view schema = {}) {
  return qualifiedName(schema, "score_sha256_best_score_cache");
}

inline std::string fullComboClearRankExpr(std::string_view alias) {
  const std::string prefix(alias);
  return "(CASE WHEN " + prefix + ".combo_break = 0 AND " + prefix +
         ".clear_type >= " +
         std::to_string(kClearTypeAssistedEasyClearRank) + " THEN " +
         std::to_string(kClearTypeFullComboRank) + " ELSE " + prefix +
         ".clear_type END)";
}

inline std::string scoreRankLabelExpr(std::string_view alias) {
  const std::string prefix(alias);
  const std::string score = "max(0, " + prefix + ".score)";
  const std::string maxScore = prefix + ".max_score";
  return "(CASE "
         "WHEN " +
         maxScore + " <= 0 THEN '' "
         "WHEN " +
         score + " >= " + maxScore + " THEN 'MAX' "
         "WHEN " +
         score + " >= ((" + maxScore + " * 26 + 26) / 27) THEN 'MAX -' "
         "WHEN " +
         score + " >= ((" + maxScore + " * 24 + 26) / 27) THEN 'AAA' "
         "WHEN " +
         score + " >= ((" + maxScore + " * 21 + 26) / 27) THEN 'AA' "
         "WHEN " +
         score + " >= ((" + maxScore + " * 18 + 26) / 27) THEN 'A' "
         "WHEN " +
         score + " >= ((" + maxScore + " * 15 + 26) / 27) THEN 'B' "
         "WHEN " +
         score + " >= ((" + maxScore + " * 12 + 26) / 27) THEN 'C' "
         "WHEN " +
         score + " >= ((" + maxScore + " * 9 + 26) / 27) THEN 'D' "
         "WHEN " +
         score + " >= ((" + maxScore + " * 6 + 26) / 27) THEN 'E' "
         "ELSE 'F' END)";
}

inline std::string scoreColumnExpr(const std::string &column) {
  if (column == "clear_rank") {
    return "s.clear_rank";
  }
  if (column == "score_rank") {
    return "s.score_rank";
  }
  return "s." + column;
}

inline std::string keyHasValueExpr(std::string_view keyExpr) {
  return "NULLIF(trim(" + std::string(keyExpr) + "), '') IS NOT NULL";
}

inline std::string rankLookupForMode(const std::string &sha256Expr,
                                     const std::string &lnModeExpr) {
  return "(SELECT s.rank FROM " + clearRankSummaryTable(kScoreDatabaseSchema) +
         " s WHERE s.chart_sha256 = " + sha256Expr +
         " AND s.ln_mode = " + lnModeExpr + " LIMIT 1)";
}

inline std::string bestLookupForMode(const std::string &sha256Expr,
                                     const std::string &lnModeExpr,
                                     const std::string &column) {
  return "(SELECT " + scoreColumnExpr(column) + " FROM " +
         bestScoreSummaryTable(kScoreDatabaseSchema) +
         " s WHERE s.chart_sha256 = " + sha256Expr +
         " AND s.ln_mode = " + lnModeExpr + " LIMIT 1)";
}

inline std::string clearRankUpsertSql() {
  return "INSERT INTO score_sha256_clear_rank_cache(chart_sha256, ln_mode, "
         "rank) SELECT lower(trim(NEW.chart_sha256)), NEW.ln_mode, " +
         fullComboClearRankExpr("NEW") + " WHERE " +
         keyHasValueExpr("NEW.chart_sha256") +
         " ON CONFLICT(chart_sha256, ln_mode) DO UPDATE SET rank = "
         "max(score_sha256_clear_rank_cache.rank, excluded.rank);";
}

inline std::string bestScoreUpsertSql() {
  return "INSERT INTO score_sha256_best_score_cache("
         "chart_sha256, ln_mode, score_id, score, max_score, max_combo, "
         "combo_break, final_gauge, clear_type, clear_rank, score_rank, "
         "created_at) SELECT lower(trim(NEW.chart_sha256)), NEW.ln_mode, "
         "NEW.id, NEW.score, NEW.max_score, NEW.max_combo, NEW.combo_break, "
         "NEW.final_gauge, NEW.clear_type, " +
         fullComboClearRankExpr("NEW") + ", " + scoreRankLabelExpr("NEW") +
         ", NEW.created_at WHERE " + keyHasValueExpr("NEW.chart_sha256") +
         " ON CONFLICT(chart_sha256, ln_mode) DO UPDATE SET "
         "score_id = excluded.score_id, score = excluded.score, "
         "max_score = excluded.max_score, max_combo = excluded.max_combo, "
         "combo_break = excluded.combo_break, "
         "final_gauge = excluded.final_gauge, clear_type = excluded.clear_type, "
         "clear_rank = excluded.clear_rank, score_rank = excluded.score_rank, "
         "created_at = excluded.created_at WHERE "
         "excluded.score > score_sha256_best_score_cache.score OR "
         "(excluded.score = score_sha256_best_score_cache.score AND "
         "excluded.clear_type > score_sha256_best_score_cache.clear_type) OR "
         "(excluded.score = score_sha256_best_score_cache.score AND "
         "excluded.clear_type = score_sha256_best_score_cache.clear_type AND "
         "excluded.created_at > score_sha256_best_score_cache.created_at) OR "
         "(excluded.score = score_sha256_best_score_cache.score AND "
         "excluded.clear_type = score_sha256_best_score_cache.clear_type AND "
         "excluded.created_at = score_sha256_best_score_cache.created_at AND "
         "excluded.score_id > score_sha256_best_score_cache.score_id);";
}

inline std::string scoreIdentityCte(std::string_view schema) {
  const std::string scores = qualifiedName(schema, "scores");
  return "SELECT lower(trim(chart_sha256)) AS chart_sha256, ln_mode, "
         "id AS score_id, score, max_score, max_combo, combo_break, "
         "final_gauge, clear_type, created_at FROM " +
         scores + " WHERE " + keyHasValueExpr("chart_sha256");
}

} // namespace detail

inline std::optional<std::string>
isScoreDatabaseAttached(sqlite3 *db, bool &attached) {
  attached = false;
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(db, "PRAGMA database_list", stmt) != SQLITE_OK) {
    return sqliteDatabaseError(db);
  }

  int stepRc = SQLITE_OK;
  while ((stepRc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
    if (sqliteColumnString(stmt.get(), 1) == kScoreDatabaseSchema) {
      attached = true;
      return std::nullopt;
    }
  }
  if (stepRc != SQLITE_DONE) {
    return sqliteDatabaseError(db);
  }
  return std::nullopt;
}

inline std::optional<std::string>
attachScoreDatabaseIfNeeded(sqlite3 *db, const std::filesystem::path &path) {
  bool attached = false;
  if (const auto error = isScoreDatabaseAttached(db, attached)) {
    return error;
  }
  if (attached) {
    return std::nullopt;
  }
  return attachSqliteDatabase(db, path, kScoreDatabaseSchema);
}

inline std::optional<std::string>
ensureScoreSummarySchema(sqlite3 *db, std::string_view schema = {});

inline std::optional<std::string>
repairScoreSummaryTablesIfEmpty(sqlite3 *db, std::string_view schema);

inline std::optional<std::string> attachEmptyScoreDatabase(sqlite3 *db) {
  bool attached = false;
  if (const auto error = isScoreDatabaseAttached(db, attached)) {
    return error;
  }
  if (!attached) {
    if (const auto error =
            executeSqlite(db, "ATTACH ':memory:' AS score_db")) {
      return error;
    }
  }
  if (const auto error =
          executeSqlite(db,
                        "CREATE TABLE IF NOT EXISTS score_db.scores ("
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
                        "pgreat INTEGER NOT NULL DEFAULT 0,"
                        "great INTEGER NOT NULL DEFAULT 0,"
                        "good INTEGER NOT NULL DEFAULT 0,"
                        "bad INTEGER NOT NULL DEFAULT 0,"
                        "poor INTEGER NOT NULL DEFAULT 0,"
                        "kpoor INTEGER NOT NULL DEFAULT 0,"
                        "fast INTEGER NOT NULL DEFAULT 0,"
                        "slow INTEGER NOT NULL DEFAULT 0,"
                        "final_gauge REAL NOT NULL DEFAULT 0,"
                        "clear_type INTEGER NOT NULL,"
                        "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                        ")")) {
    return error;
  }
  return ensureScoreSummarySchema(db, kScoreDatabaseSchema);
}

inline std::optional<std::string>
prepareScoreQueryDatabase(sqlite3 *db, const std::filesystem::path &path) {
  if (const auto attachError = attachScoreDatabaseIfNeeded(db, path)) {
    if (const auto emptyError = attachEmptyScoreDatabase(db)) {
      return "could not attach score database: " + *attachError +
             "; could not prepare empty score database: " + *emptyError;
    }
    return std::nullopt;
  }

  if (const auto schemaError = ensureScoreSummarySchema(db, kScoreDatabaseSchema)) {
    if (const auto emptyError = attachEmptyScoreDatabase(db)) {
      return "could not prepare score summary schema: " + *schemaError +
             "; could not prepare empty score database: " + *emptyError;
    }
    return std::nullopt;
  }

  if (const auto repairError =
          repairScoreSummaryTablesIfEmpty(db, kScoreDatabaseSchema)) {
    return repairError;
  }
  return std::nullopt;
}

inline std::optional<std::string>
ensureScoreSummarySchema(sqlite3 *db, std::string_view schema) {
  const std::string clearTable = detail::clearRankSummaryTable(schema);
  const std::string bestTable = detail::bestScoreSummaryTable(schema);
  const std::string trigger =
      detail::qualifiedName(schema, "score_sha256_summary_after_insert");
  const std::string scores = detail::qualifiedName(schema, "scores");

  const std::string createClearTable =
      "CREATE TABLE IF NOT EXISTS " + clearTable +
      " (chart_sha256 TEXT NOT NULL, ln_mode INTEGER NOT NULL, "
      "rank INTEGER NOT NULL, PRIMARY KEY(chart_sha256, ln_mode)) WITHOUT ROWID";
  if (const auto error = executeSqlite(db, createClearTable.c_str())) {
    return error;
  }

  const std::string createBestTable =
      "CREATE TABLE IF NOT EXISTS " + bestTable +
      " (chart_sha256 TEXT NOT NULL, ln_mode INTEGER NOT NULL, "
      "score_id INTEGER NOT NULL, score INTEGER NOT NULL, "
      "max_score INTEGER NOT NULL, max_combo INTEGER NOT NULL, "
      "combo_break INTEGER NOT NULL, final_gauge REAL NOT NULL, "
      "clear_type INTEGER NOT NULL, clear_rank INTEGER NOT NULL, "
      "score_rank TEXT NOT NULL, created_at TEXT NOT NULL, "
      "PRIMARY KEY(chart_sha256, ln_mode)) WITHOUT ROWID";
  if (const auto error = executeSqlite(db, createBestTable.c_str())) {
    return error;
  }

  const std::string dropTrigger = "DROP TRIGGER IF EXISTS " + trigger;
  if (const auto error = executeSqlite(db, dropTrigger.c_str())) {
    return error;
  }

  const std::string createTrigger =
      "CREATE TRIGGER " + trigger + " AFTER INSERT ON " + scores + " BEGIN " +
      detail::clearRankUpsertSql() + detail::bestScoreUpsertSql() + " END";
  return executeSqlite(db, createTrigger.c_str());
}

inline std::optional<std::string>
rebuildScoreSummaryTables(sqlite3 *db, std::string_view schema = {}) {
  const std::string clearTable = detail::clearRankSummaryTable(schema);
  const std::string bestTable = detail::bestScoreSummaryTable(schema);
  const std::string identityCte = detail::scoreIdentityCte(schema);

  const std::string deleteClear = "DELETE FROM " + clearTable;
  if (const auto error = executeSqlite(db, deleteClear.c_str())) {
    return error;
  }
  const std::string deleteBest = "DELETE FROM " + bestTable;
  if (const auto error = executeSqlite(db, deleteBest.c_str())) {
    return error;
  }

  const std::string insertClear =
      "WITH identities AS (" + identityCte + ") INSERT INTO " + clearTable +
      "(chart_sha256, ln_mode, rank) SELECT i.chart_sha256, i.ln_mode, MAX(" +
      detail::fullComboClearRankExpr("i") +
      ") FROM identities i GROUP BY i.chart_sha256, i.ln_mode";
  if (const auto error = executeSqlite(db, insertClear.c_str())) {
    return error;
  }

  const std::string insertBest =
      "WITH identities AS (" + identityCte + "), ranked AS ("
      "SELECT i.*, ROW_NUMBER() OVER (PARTITION BY i.chart_sha256, i.ln_mode "
      "ORDER BY i.score DESC, i.clear_type DESC, i.created_at DESC, "
      "i.score_id DESC) AS row_number FROM identities i"
      ") INSERT INTO " +
      bestTable +
      "(chart_sha256, ln_mode, score_id, score, max_score, max_combo, "
      "combo_break, final_gauge, clear_type, clear_rank, score_rank, "
      "created_at) SELECT r.chart_sha256, r.ln_mode, r.score_id, r.score, "
      "r.max_score, r.max_combo, r.combo_break, r.final_gauge, "
      "r.clear_type, " +
      detail::fullComboClearRankExpr("r") + ", " +
      detail::scoreRankLabelExpr("r") +
      ", r.created_at FROM ranked r WHERE r.row_number = 1";
  return executeSqlite(db, insertBest.c_str());
}

inline std::optional<std::string>
queryHasRows(sqlite3 *db, const std::string &query, bool &hasRows) {
  hasRows = false;
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(db, query, stmt) != SQLITE_OK) {
    return sqliteDatabaseError(db);
  }
  const int stepRc = sqlite3_step(stmt.get());
  if (stepRc == SQLITE_ROW) {
    hasRows = true;
    return std::nullopt;
  }
  if (stepRc == SQLITE_DONE) {
    return std::nullopt;
  }
  return sqliteDatabaseError(db);
}

inline std::optional<std::string>
repairScoreSummaryTablesIfEmpty(sqlite3 *db, std::string_view schema = {}) {
  const std::string scores = detail::qualifiedName(schema, "scores");
  const std::string hasScoreIdentityQuery =
      "SELECT 1 FROM " + scores + " WHERE " +
      detail::keyHasValueExpr("chart_sha256") + " LIMIT 1";
  bool hasScoreIdentity = false;
  if (const auto error =
          queryHasRows(db, hasScoreIdentityQuery, hasScoreIdentity)) {
    return error;
  }
  if (!hasScoreIdentity) {
    return std::nullopt;
  }

  bool hasClearSummary = false;
  if (const auto error = queryHasRows(
          db, "SELECT 1 FROM " + detail::clearRankSummaryTable(schema) +
                  " LIMIT 1",
          hasClearSummary)) {
    return error;
  }
  bool hasBestSummary = false;
  if (const auto error = queryHasRows(
          db, "SELECT 1 FROM " + detail::bestScoreSummaryTable(schema) +
                  " LIMIT 1",
          hasBestSummary)) {
    return error;
  }
  if (hasClearSummary && hasBestSummary) {
    return std::nullopt;
  }
  return rebuildScoreSummaryTables(db, schema);
}

inline std::string scoreRankLookupExpr(const std::string &sha256Expr,
                                       const std::string &lnModeExpr) {
  const std::string primary = detail::rankLookupForMode(sha256Expr, lnModeExpr);
  const std::string fallback = detail::rankLookupForMode(sha256Expr, "0");
  return "(COALESCE(" + primary + ", CASE WHEN " + lnModeExpr +
         " = 1 THEN " + fallback + " END))";
}

inline std::string scoreBestLookupExpr(const std::string &sha256Expr,
                                       const std::string &lnModeExpr,
                                       const std::string &column) {
  const std::string primary =
      detail::bestLookupForMode(sha256Expr, lnModeExpr, column);
  const std::string fallback =
      detail::bestLookupForMode(sha256Expr, "0", column);
  return "(COALESCE(" + primary + ", CASE WHEN " + lnModeExpr +
         " = 1 THEN " + fallback + " END))";
}

} // namespace score_cache_queries
