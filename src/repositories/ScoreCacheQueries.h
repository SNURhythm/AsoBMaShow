#pragma once

#include "../ProfileDatabaseActivity.h"
#include "../ScoreProvenance.h"
#include "ScoreRepositoryModels.h"
#include "SqliteRAII.h"
#include "../scene/play/RhythmState.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

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

inline std::string playbackPercentExpr(std::string_view alias) {
  const std::string prefix(alias);
  return "(CASE WHEN json_valid(" + prefix +
         ".provenance_json) THEN COALESCE(json_extract(" + prefix +
         ".provenance_json, '$.playback.percent'), 100) ELSE 0 END)";
}

inline std::string
fullComboClearRankExpr(std::string_view alias,
                       std::string playbackPercentExpression = {},
                       bool sourceAware = false) {
  const std::string prefix(alias);
  if (playbackPercentExpression.empty()) {
    playbackPercentExpression = playbackPercentExpr(alias);
  }
  const std::string importedBranch =
      sourceAware
          ? " WHEN " + prefix + ".score_source = " +
                std::to_string(
                    static_cast<int>(ScoreStorageSource::ImportedIr)) +
                " THEN " + prefix + ".clear_type"
          : std::string{};
  return "(CASE" + importedBranch + " WHEN " + prefix +
         ".clear_type >= " + std::to_string(kClearTypeAssistedEasyClearRank) +
         " AND " + playbackPercentExpression + " <> 100 THEN " +
         std::to_string(kClearTypeAssistedEasyClearRank) + " WHEN " + prefix +
         ".combo_break = 0 AND " + prefix +
         ".clear_type >= " + std::to_string(kClearTypeAssistedEasyClearRank) +
         " THEN " + std::to_string(kClearTypeFullComboRank) + " ELSE " +
         prefix + ".clear_type END)";
}

inline std::string playableLongNoteModesSql() {
  return "(SELECT 0 AS ln_mode UNION ALL SELECT 1 UNION ALL SELECT 2 UNION "
         "ALL SELECT 3)";
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

using BestScoreOrderKey = std::array<std::string, 4>;

inline BestScoreOrderKey
bestScoreOrderKey(std::string_view alias, std::string clearRankExpression,
                  std::string_view idColumn) {
  const std::string prefix(alias);
  return {prefix + ".score", clearRankExpression,
          prefix + ".created_at", prefix + "." + std::string(idColumn)};
}

inline std::string bestScoreOrderBySql(const BestScoreOrderKey &key) {
  return key[0] + " DESC, " + key[1] + " DESC, " + key[2] + " DESC, " +
         key[3] + " DESC";
}

inline std::string bestScoreCandidateWinsSql(
    const BestScoreOrderKey &candidate, const BestScoreOrderKey &incumbent) {
  return "(" + candidate[0] + ", " + candidate[1] + ", " + candidate[2] +
         ", " + candidate[3] + ") > (" + incumbent[0] + ", " +
         incumbent[1] + ", " + incumbent[2] + ", " + incumbent[3] + ")";
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

inline std::string scoreParticipatesInBestExpr(std::string_view alias) {
  return std::string(alias) + ".eligibility <> " +
         std::to_string(static_cast<int>(ScoreEligibility::Modified));
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

inline std::string clearRankUpsertSql(bool sourceAware) {
  return "INSERT INTO score_sha256_clear_rank_cache(chart_sha256, ln_mode, "
         "rank) SELECT lower(trim(NEW.chart_sha256)), modes.ln_mode, " +
         fullComboClearRankExpr("NEW", {}, sourceAware) + " FROM " +
         playableLongNoteModesSql() + " modes WHERE (NEW.ln_mode = -1 OR "
         "NEW.ln_mode = modes.ln_mode) AND " +
         keyHasValueExpr("NEW.chart_sha256") + " AND " +
         scoreParticipatesInBestExpr("NEW") +
         " ON CONFLICT(chart_sha256, ln_mode) DO UPDATE SET rank = "
         "max(score_sha256_clear_rank_cache.rank, excluded.rank);";
}

inline std::string bestScoreUpsertSql(bool sourceAware) {
  const auto candidate =
      bestScoreOrderKey("excluded", "excluded.clear_rank", "score_id");
  const auto incumbent = bestScoreOrderKey(
      "score_sha256_best_score_cache",
      "score_sha256_best_score_cache.clear_rank", "score_id");
  return "INSERT INTO score_sha256_best_score_cache("
         "chart_sha256, ln_mode, score_id, score, max_score, max_combo, "
         "combo_break, final_gauge, clear_type, clear_rank, score_rank, "
         "created_at) SELECT lower(trim(NEW.chart_sha256)), modes.ln_mode, "
         "NEW.id, NEW.score, NEW.max_score, NEW.max_combo, NEW.combo_break, "
         "NEW.final_gauge, NEW.clear_type, " +
         fullComboClearRankExpr("NEW", {}, sourceAware) + ", " +
         scoreRankLabelExpr("NEW") +
         ", NEW.created_at FROM " + playableLongNoteModesSql() +
         " modes WHERE (NEW.ln_mode = -1 OR NEW.ln_mode = modes.ln_mode) "
         "AND " + keyHasValueExpr("NEW.chart_sha256") +
         " AND " + scoreParticipatesInBestExpr("NEW") +
         " ON CONFLICT(chart_sha256, ln_mode) DO UPDATE SET "
         "score_id = excluded.score_id, score = excluded.score, "
         "max_score = excluded.max_score, max_combo = excluded.max_combo, "
         "combo_break = excluded.combo_break, "
         "final_gauge = excluded.final_gauge, clear_type = excluded.clear_type, "
         "clear_rank = excluded.clear_rank, score_rank = excluded.score_rank, "
         "created_at = excluded.created_at WHERE " +
         bestScoreCandidateWinsSql(candidate, incumbent) + ";";
}

inline std::string scoreIdentityCte(std::string_view schema,
                                    bool hasProvenance,
                                    bool sourceAware) {
  const std::string scores = qualifiedName(schema, "scores");
  const std::string playbackPercent =
      hasProvenance ? playbackPercentExpr("s") : "100";
  const std::string eligibilityFilter =
      hasProvenance ? " AND " + scoreParticipatesInBestExpr("s") : "";
  const std::string scoreSource =
      sourceAware ? "score_source" : "0 AS score_source";
  return "SELECT lower(trim(chart_sha256)) AS chart_sha256, modes.ln_mode, "
         "id AS score_id, score, max_score, max_combo, combo_break, "
         "final_gauge, clear_type, " + scoreSource + ", " +
         playbackPercent + " AS playback_percent, created_at FROM " + scores +
         " s JOIN " + playableLongNoteModesSql() +
         " modes ON s.ln_mode = -1 OR s.ln_mode = modes.ln_mode WHERE " +
         keyHasValueExpr("chart_sha256") + eligibilityFilter;
}

inline bool scoreTableHasColumn(sqlite3 *db, std::string_view schema,
                                std::string_view column) {
  const std::string query =
      "PRAGMA " + std::string(schema.empty() ? "" : std::string(schema) + ".") +
      "table_info(scores)";
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(db, query, stmt) != SQLITE_OK) {
    return false;
  }
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    if (sqliteColumnString(stmt.get(), 1) == column) {
      return true;
    }
  }
  return false;
}

inline bool scoreTableHasProvenance(sqlite3 *db, std::string_view schema) {
  return scoreTableHasColumn(db, schema, "provenance_json");
}

inline bool scoreTableHasSource(sqlite3 *db, std::string_view schema) {
  return scoreTableHasColumn(db, schema, "score_source");
}

} // namespace detail

inline std::optional<std::string>
inspectScoreDatabaseAttachment(sqlite3 *db, bool &attached,
                               std::filesystem::path &attachedPath) {
  attached = false;
  attachedPath.clear();
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(db, "PRAGMA database_list", stmt) != SQLITE_OK) {
    return sqliteDatabaseError(db);
  }

  int stepRc = SQLITE_OK;
  while ((stepRc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
    if (sqliteColumnString(stmt.get(), 1) == kScoreDatabaseSchema) {
      attached = true;
      attachedPath = sqliteColumnString(stmt.get(), 2);
    }
  }
  if (stepRc != SQLITE_DONE) {
    return sqliteDatabaseError(db);
  }
  return std::nullopt;
}

inline std::optional<std::string>
canonicalScoreDatabasePath(const std::filesystem::path &path,
                           std::filesystem::path &canonicalPath) {
  if (path.empty()) {
    return "score database path is empty";
  }
  std::error_code existsError;
  if (!std::filesystem::exists(path, existsError)) {
    if (existsError) {
      return "could not inspect score database path: " +
             existsError.message();
    }
    return "score database does not exist: " + fspath_to_utf8(path);
  }
  std::error_code canonicalError;
  canonicalPath = std::filesystem::canonical(path, canonicalError);
  if (canonicalError) {
    return "could not canonicalize score database path: " +
           canonicalError.message();
  }
  return std::nullopt;
}

inline std::optional<std::string>
attachScoreDatabaseIfNeeded(sqlite3 *db, const std::filesystem::path &path) {
  profile_database_activity::WriteGuard operation;
  std::filesystem::path targetPath;
  if (const auto error = canonicalScoreDatabasePath(path, targetPath)) {
    return error;
  }

  bool attached = false;
  std::filesystem::path attachedPath;
  if (const auto error =
          inspectScoreDatabaseAttachment(db, attached, attachedPath)) {
    return error;
  }
  if (attached) {
    if (!attachedPath.empty()) {
      std::error_code canonicalError;
      const std::filesystem::path canonicalAttachedPath =
          std::filesystem::canonical(attachedPath, canonicalError);
      if (canonicalError) {
        return "could not canonicalize attached score database path: " +
               canonicalError.message();
      }
      std::error_code equivalentError;
      const bool sameDatabase = std::filesystem::equivalent(
          canonicalAttachedPath, targetPath, equivalentError);
      if (equivalentError) {
        return "could not compare attached score database path: " +
               equivalentError.message();
      }
      if (sameDatabase) {
        return std::nullopt;
      }
    }

    const std::string detachSql =
        std::string("DETACH DATABASE ") + kScoreDatabaseSchema;
    if (const auto error = executeSqlite(db, detachSql.c_str())) {
      return "could not detach previous score database: " + *error;
    }
  }
  return attachSqliteDatabase(db, targetPath, kScoreDatabaseSchema);
}

inline std::optional<std::string>
detachScoreDatabaseIfAttached(sqlite3 *db) {
  profile_database_activity::WriteGuard operation;
  bool attached = false;
  std::filesystem::path attachedPath;
  if (const auto error =
          inspectScoreDatabaseAttachment(db, attached, attachedPath)) {
    return error;
  }
  if (!attached) {
    return std::nullopt;
  }

  const std::string detachSql =
      std::string("DETACH DATABASE ") + kScoreDatabaseSchema;
  return executeSqlite(db, detachSql.c_str());
}

inline std::optional<std::string>
ensureScoreSummarySchema(sqlite3 *db, std::string_view schema = {});

inline std::optional<std::string>
repairScoreSummaryTablesIfEmpty(sqlite3 *db, std::string_view schema);

inline std::optional<std::string>
prepareScoreQueryDatabase(sqlite3 *db, const std::filesystem::path &path) {
  profile_database_activity::WriteGuard operation;
  if (const auto attachError = attachScoreDatabaseIfNeeded(db, path)) {
    return "could not attach score database: " + *attachError;
  }

  if (const auto schemaError =
          ensureScoreSummarySchema(db, kScoreDatabaseSchema)) {
    return "could not prepare score summary schema: " + *schemaError;
  }

  if (const auto repairError =
          repairScoreSummaryTablesIfEmpty(db, kScoreDatabaseSchema)) {
    return repairError;
  }
  return std::nullopt;
}

inline std::optional<std::string>
ensureScoreSummarySchema(sqlite3 *db, std::string_view schema) {
  profile_database_activity::WriteGuard operation;
  if (db == nullptr) {
    return "database is not open";
  }
  const bool callerOwnsTransaction = sqlite3_get_autocommit(db) == 0;
  const char *beginQuery = callerOwnsTransaction
                               ? "SAVEPOINT asobmashow_score_summary_schema"
                               : "BEGIN";
  const char *commitQuery =
      callerOwnsTransaction
          ? "RELEASE asobmashow_score_summary_schema"
          : "COMMIT";
  const char *rollbackQuery =
      callerOwnsTransaction
          ? "ROLLBACK TO asobmashow_score_summary_schema; RELEASE "
            "asobmashow_score_summary_schema"
          : "ROLLBACK";
  std::string transactionError;
  SqliteTransactionHandle transaction(db, beginQuery, transactionError,
                                      commitQuery, rollbackQuery);
  if (!transaction.active()) {
    return transactionError;
  }

  const std::string clearTable = detail::clearRankSummaryTable(schema);
  const std::string bestTable = detail::bestScoreSummaryTable(schema);
  const std::string trigger =
      detail::qualifiedName(schema, "score_sha256_summary_after_insert");
  const std::string scores = detail::qualifiedName(schema, "scores");
  const bool sourceAware = detail::scoreTableHasSource(db, schema);

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
      detail::clearRankUpsertSql(sourceAware) +
      detail::bestScoreUpsertSql(sourceAware) + " END";
  if (const auto error = executeSqlite(db, createTrigger.c_str())) {
    return error;
  }
  if (!transaction.commit(transactionError)) {
    return transactionError;
  }
  return std::nullopt;
}

inline std::optional<std::string>
rebuildScoreSummaryTables(sqlite3 *db, std::string_view schema = {}) {
  profile_database_activity::WriteGuard operation;
  const std::string clearTable = detail::clearRankSummaryTable(schema);
  const std::string bestTable = detail::bestScoreSummaryTable(schema);
  const bool sourceAware = detail::scoreTableHasSource(db, schema);
  const std::string identityCte = detail::scoreIdentityCte(
      schema, detail::scoreTableHasProvenance(db, schema), sourceAware);

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
      detail::fullComboClearRankExpr("i", "i.playback_percent", sourceAware) +
      ") FROM identities i GROUP BY i.chart_sha256, i.ln_mode";
  if (const auto error = executeSqlite(db, insertClear.c_str())) {
    return error;
  }

  const auto bestOrder = detail::bestScoreOrderKey(
      "i", detail::fullComboClearRankExpr("i", "i.playback_percent",
                                           sourceAware),
      "score_id");
  const std::string insertBest =
      "WITH identities AS (" + identityCte +
      "), ranked AS ("
      "SELECT i.*, ROW_NUMBER() OVER (PARTITION BY i.chart_sha256, i.ln_mode "
      "ORDER BY " + detail::bestScoreOrderBySql(bestOrder) +
      ") AS row_number FROM identities i"
      ") INSERT INTO " +
      bestTable +
      "(chart_sha256, ln_mode, score_id, score, max_score, max_combo, "
      "combo_break, final_gauge, clear_type, clear_rank, score_rank, "
      "created_at) SELECT r.chart_sha256, r.ln_mode, r.score_id, r.score, "
      "r.max_score, r.max_combo, r.combo_break, r.final_gauge, "
      "r.clear_type, " +
      detail::fullComboClearRankExpr("r", "r.playback_percent", sourceAware) +
      ", " +
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
  profile_database_activity::WriteGuard operation;
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
  const std::string classicLongNote =
      detail::rankLookupForMode(sha256Expr, "1");
  const std::string legacy = detail::rankLookupForMode(sha256Expr, "0");
  return "(COALESCE(" + primary + ", CASE WHEN " + lnModeExpr +
         " != 0 THEN COALESCE(" + classicLongNote + ", " + legacy +
         ") END))";
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
