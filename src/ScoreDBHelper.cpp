#include "ScoreDBHelper.h"

#include "BmsMetadataText.h"
#include "ChartDBHelper.h"
#include "ChartSqlExpressions.h"
#include "CoursePlaySession.h"
#include "LongNoteModeUtils.h"
#include "ScoreCacheQueries.h"
#include "SqliteRAII.h"
#include "Utils.h"
#include "path.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
std::atomic<std::uint64_t> gScoreRevision{1};
constexpr int kLegacyScoreDatabaseSchemaVersion = 4;
constexpr int kScoreDatabaseSchemaVersion = 5;
constexpr const char *kScoreMigrationChartSchema = "score_migration_chart";
constexpr const char *kLegacyProvenanceJson =
    "{\"schemaVersion\":1,\"ruleset\":{\"version\":0},\"stages\":[],"
    "\"eligibility\":\"legacy-unverified\"}";

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

bool execSql(sqlite3 *db, const char *query, const char *context) {
  return executeSqliteLogged(db, query, context, logSqlErrorText);
}

bool execSqlAllowDuplicateColumn(sqlite3 *db, const char *query,
                                 const char *context) {
  return executeSqliteLogged(db, query, context, logSqlErrorText,
                             "duplicate column name");
}

int databaseUserVersion(sqlite3 *db) {
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, "PRAGMA user_version", stmt,
                                    "reading score database version",
                                    logSqlErrorText)) {
    return 0;
  }
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    return 0;
  }
  return sqlite3_column_int(stmt.get(), 0);
}

bool setDatabaseUserVersion(sqlite3 *db, int version) {
  const std::string query =
      "PRAGMA user_version = " + std::to_string(std::max(0, version));
  return execSql(db, query.c_str(), "updating score database version");
}

bool rejectFutureScoreDatabase(sqlite3 *db) {
  const char *filename =
      db != nullptr ? sqlite3_db_filename(db, "main") : nullptr;
  if (db != nullptr && sqlite3_get_autocommit(db) != 0 && filename != nullptr &&
      filename[0] != '\0') {
    std::string error;
    if (!preflightSqliteUserVersion(filename, kScoreDatabaseSchemaVersion,
                                    error)
             .has_value()) {
      SDL_Log("Refusing score database schema preflight: %s", error.c_str());
      return true;
    }
    return false;
  }
  const int version = databaseUserVersion(db);
  if (version <= kScoreDatabaseSchemaVersion) {
    return false;
  }
  SDL_Log("Refusing future score database version %d (supported: %d)", version,
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
  const int version = databaseUserVersion(db);
  if (version > kScoreDatabaseSchemaVersion) {
    return false;
  }
  if (version >= kScoreDatabaseSchemaVersion) {
    return true;
  }
  if (version < kLegacyScoreDatabaseSchemaVersion) {
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
      !setDatabaseUserVersion(db, kScoreDatabaseSchemaVersion)) {
    return false;
  }
  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing score provenance migration", transactionError);
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

std::string stableChartKey(const bms_parser::ChartMeta &chartMeta) {
  const std::string sha256 = normalizedHash(chartMeta.SHA256);
  if (!sha256.empty()) {
    return "sha256:" + sha256;
  }
  const std::string md5 = normalizedHash(chartMeta.MD5);
  if (!md5.empty()) {
    return "md5:" + md5;
  }
  const std::string chartPath =
      Utils::GetStoragePathUtf8RelativeToDocuments(chartMeta.BmsPath, "BMS/");
  return "path:" + chartPath;
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
  std::ostringstream key;
  key << "course:" << session.courseName << "\n";
  key << "constraint:" << session.constraintJson << "\n";
  for (const auto &entry : session.entries) {
    key << stableChartKey(entry.meta) << "\n";
  }
  return key.str();
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

void storeBestCourseRank(CourseScoreRankMap &ranks, const std::string &key,
                         int rank) {
  if (key.empty()) {
    return;
  }
  auto it = ranks.find(key);
  if (it == ranks.end() || rank > it->second) {
    ranks[key] = rank;
  }
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

std::string bestClearMarkRankExpr() {
  return "MAX(CASE WHEN combo_break = 0 AND clear_type >= " +
         std::to_string(kClearTypeAssistedEasyClearRank) + " THEN " +
         std::to_string(kClearTypeFullComboRank) + " ELSE clear_type END)";
}

void loadBestChartRanks(sqlite3 *db, ScoreClearRankCache &cache) {
  const char *query = "SELECT chart_sha256, ln_mode, rank "
                      "FROM score_sha256_clear_rank_cache";
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

void loadBestChartScores(sqlite3 *db, ScoreBestCache &cache) {
  const char *query =
      "SELECT chart_sha256, ln_mode, score, max_score, max_combo, combo_break, "
      "final_gauge, clear_type, created_at "
      "FROM score_sha256_best_score_cache";
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

void loadBestCourseRanks(sqlite3 *db, CourseScoreRankMap &ranks) {
  const std::string query =
      "SELECT course_id, " + bestClearMarkRankExpr() +
      " FROM course_scores WHERE course_id IS NOT NULL AND course_id > 0 "
      "GROUP BY course_id";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "loading course score clear ranks",
                                    logSqlErrorText)) {
    return;
  }

  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const int courseId = sqlite3_column_int(stmt.get(), 0);
    const int rank = sqlite3_column_int(stmt.get(), 1);
    storeBestCourseRank(ranks, std::to_string(courseId), rank);
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
  int currentVersion = databaseUserVersion(db);
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

int ScoreClearRankCache::bestCourseRankForId(int courseId) const {
  if (courseId <= 0) {
    return kNoClearTypeRank;
  }
  const auto it = rankByCourseId.find(std::to_string(courseId));
  return it == rankByCourseId.end() ? kNoClearTypeRank : it->second;
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
  sqlite3_config(SQLITE_CONFIG_SERIALIZED);
  static ScoreDBHelper instance;
  return instance;
}

ScoreDBHelper::ScoreDBHelper(std::filesystem::path databasePath)
    : databasePath_(std::move(databasePath)) {}

void ScoreDBHelper::SetDatabasePath(std::filesystem::path databasePath) {
  databasePath_ = std::move(databasePath);
  gScoreRevision.fetch_add(1, std::memory_order_relaxed);
}

const std::filesystem::path &ScoreDBHelper::GetDatabasePath() const {
  return databasePath_;
}

sqlite3 *ScoreDBHelper::Connect() {
  const std::filesystem::path path =
      databasePath_.empty() ? Utils::GetDocumentsPath("db") / "score.db"
                            : databasePath_;
  const std::filesystem::path directory = path.parent_path();
  std::error_code directoryError;
  if (!directory.empty() &&
      !Utils::EnsureDirectoryExists(directory, directoryError)) {
    SDL_Log("Can't create score database directory %s: %s",
            fspath_to_utf8(directory).c_str(),
            directoryError.message().c_str());
    return nullptr;
  }

  std::string openError;
  sqlite3 *db = openValidatedSqliteDatabase(path, kScoreDatabaseSchemaVersion,
                                            false, openError);
  if (db == nullptr) {
    SDL_Log("Refusing to open score database %s: %s",
            fspath_to_utf8(path).c_str(), openError.c_str());
    return nullptr;
  }
  return db;
}

void ScoreDBHelper::Close(sqlite3 *db) { closeSqliteDatabase(db); }

bool ScoreDBHelper::CreateScoreTable(sqlite3 *db) {
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
  if (const auto error = score_cache_queries::ensureScoreSummarySchema(db)) {
    logSqlErrorText("creating score identity summary schema", *error);
    return false;
  }
  if (existingScoreTable) {
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
  if (db == nullptr || rejectFutureScoreDatabase(db)) {
    return false;
  }
  const char *query = "CREATE TABLE IF NOT EXISTS course_scores ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "course_id INTEGER,"
                      "course_key TEXT,"
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
  if (db == nullptr || rejectFutureScoreDatabase(db)) {
    return false;
  }
  if (!CreateScoreTable(db) || !CreateCourseScoreTable(db)) {
    return false;
  }
  return migrateScoreDatabaseToVersion5(db);
}

bool ScoreDBHelper::EnsureSchema() {
  SqliteConnectionHandle connection(Connect());
  return connection.get() != nullptr && EnsureSchema(connection.get());
}

bool ScoreDBHelper::InsertScore(sqlite3 *db,
                                const bms_parser::ChartMeta &chartMeta,
                                const RhythmState &state,
                                const ScoreProvenance &provenance) {
  std::string provenanceError;
  const auto provenanceJson =
      serializeValidatedScoreProvenance(provenance, provenanceError);
  if (!provenanceJson.has_value()) {
    SDL_Log("Refusing to save score with invalid provenance: %s",
            provenanceError.c_str());
    return false;
  }
  if (!EnsureSchema(db)) {
    return false;
  }

  const char *query =
      "INSERT INTO scores ("
      "chart_path, chart_md5, chart_sha256, ln_mode, chart_title, "
      "chart_artist,"
      "score, max_score, max_combo, combo_break,"
      "pgreat, great, good, bad, poor, kpoor, fast, slow, final_gauge,"
      "clear_type, ruleset_version, eligibility, provenance_json"
      ") VALUES ("
      "@chart_path, @chart_md5, @chart_sha256, @ln_mode, @chart_title, "
      "@chart_artist,"
      "@score, @max_score, @max_combo, @combo_break,"
      "@pgreat, @great, @good, @bad, @poor, @kpoor, @fast, @slow,"
      "@final_gauge, @clear_type, @ruleset_version, @eligibility,"
      "@provenance_json"
      ")";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "preparing score insert",
                                    logSqlErrorText)) {
    return false;
  }

  const auto chartPath =
      Utils::GetStoragePathUtf8RelativeToDocuments(chartMeta.BmsPath, "BMS/");
  const std::string chartSha256 = normalizedHash(chartMeta.SHA256);
  if (chartSha256.empty()) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                    "Refusing to save score without chart SHA256: %s",
                    chartMeta.BmsPath.c_str());
    std::abort();
  }
  int bindIndex = 1;
  bindSqliteText(stmt.get(), bindIndex++, chartPath);
  bindSqliteText(stmt.get(), bindIndex++, normalizedHash(chartMeta.MD5));
  bindSqliteText(stmt.get(), bindIndex++, chartSha256);
  sqlite3_bind_int(stmt.get(), bindIndex++,
                   scoreLongNoteModeForClearLamp(chartMeta));
  bindSqliteText(stmt.get(), bindIndex++, chartMeta.Title);
  bindSqliteText(stmt.get(), bindIndex++, chartMeta.Artist);
  sqlite3_bind_int(stmt.get(), bindIndex++, state.getScore());
  sqlite3_bind_int(stmt.get(), bindIndex++, chartMeta.TotalNotes * 2);
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
  sqlite3_bind_int(stmt.get(), bindIndex++, state.getClearTypeRank());
  sqlite3_bind_int(stmt.get(), bindIndex++, provenance.ruleset.version);
  sqlite3_bind_int(stmt.get(), bindIndex++,
                   static_cast<int>(provenance.eligibility));
  bindSqliteText(stmt.get(), bindIndex++, *provenanceJson);

  int rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    logSqlError("saving score", db);
    if (sqlite3_extended_errcode(db) == SQLITE_CONSTRAINT_NOTNULL) {
      std::abort();
    }
    return false;
  }
  return true;
}

bool ScoreDBHelper::InsertCourseScore(sqlite3 *db,
                                      const CoursePlaySession &session,
                                      const RhythmState &state,
                                      int completedCharts, int totalCharts,
                                      const ScoreProvenance &provenance) {
  std::string provenanceError;
  const auto provenanceJson =
      serializeValidatedScoreProvenance(provenance, provenanceError);
  if (!provenanceJson.has_value()) {
    SDL_Log("Refusing to save course score with invalid provenance: %s",
            provenanceError.c_str());
    return false;
  }
  if (!EnsureSchema(db)) {
    return false;
  }

  const char *query =
      "INSERT INTO course_scores ("
      "course_id, course_key, course_name, course_group_name, constraint_json,"
      "gauge_type, gauge_profile, gauge_auto_shift, play_option, assist_option,"
      "completed_charts, total_charts,"
      "score, max_score, max_combo, combo_break,"
      "pgreat, great, good, bad, poor, kpoor, fast, slow, final_gauge,"
      "clear_type, ruleset_version, eligibility, provenance_json"
      ") VALUES ("
      "@course_id, @course_key, @course_name, @course_group_name,"
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
  bindSqliteText(stmt.get(), bindIndex++, courseKeyForSession(session));
  bindSqliteText(stmt.get(), bindIndex++, session.courseName);
  bindSqliteText(stmt.get(), bindIndex++, session.courseGroupName);
  bindSqliteText(stmt.get(), bindIndex++, session.constraintJson);
  sqlite3_bind_int(stmt.get(), bindIndex++, gaugeTypeIndex(session.gaugeType));
  sqlite3_bind_int(stmt.get(), bindIndex++,
                   static_cast<int>(session.gaugeProfile));
  sqlite3_bind_int(stmt.get(), bindIndex++, session.gaugeAutoShift ? 1 : 0);
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
  if (completedCharts == totalCharts && totalCharts > 0 &&
      state.currentGauge > 0.0f && state.comboBreak == 0 &&
      state.maxCombo >= courseTotalNotes) {
    clearRank = kClearTypeFullComboRank;
  }
  sqlite3_bind_int(stmt.get(), bindIndex++, clearRank);
  sqlite3_bind_int(stmt.get(), bindIndex++, provenance.ruleset.version);
  sqlite3_bind_int(stmt.get(), bindIndex++,
                   static_cast<int>(provenance.eligibility));
  bindSqliteText(stmt.get(), bindIndex++, *provenanceJson);

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
  std::string provenanceError;
  if (!serializeValidatedScoreProvenance(provenance, provenanceError)
           .has_value()) {
    SDL_Log("Refusing to save score with invalid provenance: %s",
            provenanceError.c_str());
    return false;
  }

  SqliteConnectionHandle connection(Connect());
  if (connection.get() == nullptr) {
    return false;
  }

  const bool result =
      InsertScore(connection.get(), chartMeta, state, provenance);
  if (result) {
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

bool ScoreDBHelper::SaveCourseScore(const CoursePlaySession &session,
                                    const RhythmState &state,
                                    int completedCharts, int totalCharts,
                                    const ScoreProvenance &provenance) {
  std::string provenanceError;
  if (!serializeValidatedScoreProvenance(provenance, provenanceError)
           .has_value()) {
    SDL_Log("Refusing to save course score with invalid provenance: %s",
            provenanceError.c_str());
    return false;
  }

  SqliteConnectionHandle connection(Connect());
  if (connection.get() == nullptr) {
    return false;
  }

  const bool result =
      InsertCourseScore(connection.get(), session, state, completedCharts,
                        totalCharts, provenance);
  if (result) {
    gScoreRevision.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

std::optional<ScoreBestSnapshot> ScoreDBHelper::LoadBestScore(
    const bms_parser::ChartMeta &chartMeta,
    const std::optional<std::string> &beforeCreatedAt) {
  SqliteConnectionHandle connection(Connect());
  if (connection.get() == nullptr) {
    return std::nullopt;
  }

  if (!EnsureSchema(connection.get())) {
    return std::nullopt;
  }

  const auto match = scoreChartMatchFor(chartMeta);
  const std::string cutoff = beforeCreatedAt.value_or("");
  const int longNoteMode = scoreLongNoteModeForClearLamp(chartMeta);
  const bool legacyLongNoteModeFallback = longNoteMode == 1;

  std::string query =
      "SELECT score, max_score, max_combo, combo_break, final_gauge,"
      "clear_type, created_at "
      "FROM scores WHERE ";
  query += scoreChartMatchPredicate();
  query += " AND (ln_mode = ? OR (? != 0 AND ln_mode = 0)) "
           "AND (? = '' OR created_at < ?) "
           "ORDER BY CASE WHEN ln_mode = ? THEN 0 ELSE 1 END, "
           "score DESC, clear_type DESC, created_at DESC, id DESC "
           "LIMIT 1";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(connection.get(), query, stmt,
                                    "loading best score", logSqlErrorText)) {
    return std::nullopt;
  }

  int bindIndex = 1;
  bindIndex = bindScoreChartMatch(stmt.get(), bindIndex, match);
  sqlite3_bind_int(stmt.get(), bindIndex++, longNoteMode);
  sqlite3_bind_int(stmt.get(), bindIndex++, legacyLongNoteModeFallback ? 1 : 0);
  bindSqliteText(stmt.get(), bindIndex++, cutoff);
  bindSqliteText(stmt.get(), bindIndex++, cutoff);
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
  SqliteConnectionHandle connection(Connect());
  if (connection.get() == nullptr) {
    return std::nullopt;
  }

  if (!EnsureSchema(connection.get())) {
    return std::nullopt;
  }

  const std::string courseKey = courseKeyForSession(session);
  const char *query =
      "SELECT score, max_score, max_combo, combo_break, final_gauge,"
      "clear_type, created_at "
      "FROM course_scores "
      "WHERE ((? != '' AND course_key = ?) OR (? > 0 AND course_id = ?)) "
      "ORDER BY score DESC, clear_type DESC, created_at DESC, id DESC "
      "LIMIT 1";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(connection.get(), query, stmt,
                                    "loading best course score",
                                    logSqlErrorText)) {
    return std::nullopt;
  }

  int bindIndex = 1;
  bindSqliteText(stmt.get(), bindIndex++, courseKey);
  bindSqliteText(stmt.get(), bindIndex++, courseKey);
  sqlite3_bind_int(stmt.get(), bindIndex++, session.courseId);
  sqlite3_bind_int(stmt.get(), bindIndex++, session.courseId);

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

ScoreClearRankCache ScoreDBHelper::LoadBestClearRanks() {
  ScoreClearRankCache cache;
  SqliteConnectionHandle connection(Connect());
  if (connection.get() == nullptr) {
    return cache;
  }

  if (EnsureSchema(connection.get())) {
    loadBestChartRanks(connection.get(), cache);
    loadBestCourseRanks(connection.get(), cache.rankByCourseId);
  }
  return cache;
}

ScoreBestCache ScoreDBHelper::LoadBestScores() {
  ScoreBestCache cache;
  SqliteConnectionHandle connection(Connect());
  if (connection.get() == nullptr) {
    return cache;
  }

  if (EnsureSchema(connection.get())) {
    loadBestChartScores(connection.get(), cache);
  }
  return cache;
}

std::uint64_t ScoreDBHelper::GetRevision() const {
  return gScoreRevision.load(std::memory_order_relaxed);
}
