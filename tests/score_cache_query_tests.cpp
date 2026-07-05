#include "../src/ScoreCacheQueries.h"
#include "../src/sqlite3.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#define ASSERT_EQ(expected, actual, label)                                     \
  if ((expected) != (actual)) {                                                \
    std::cerr << label << " expected " << (expected) << " actual "            \
              << (actual) << std::endl;                                       \
    return 1;                                                                 \
  }

#define ASSERT_FALSE(value, label)                                             \
  if (value) {                                                                \
    std::cerr << label << " expected false" << std::endl;                     \
    return 1;                                                                 \
  }

#define ASSERT_TRUE(value, label)                                              \
  if (!(value)) {                                                              \
    std::cerr << label << " expected true" << std::endl;                      \
    return 1;                                                                 \
  }

namespace {

void execOrAbort(sqlite3 *db, const std::string &sql) {
  char *error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    std::cerr << "exec failed: " << (error != nullptr ? error : "") << "\n"
              << sql << std::endl;
    sqlite3_free(error);
    std::abort();
  }
}

bool execSucceeds(sqlite3 *db, const std::string &sql) {
  char *error = nullptr;
  const bool ok =
      sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) == SQLITE_OK;
  sqlite3_free(error);
  return ok;
}

std::string queryString(sqlite3 *db, const std::string &sql) {
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    std::cerr << "prepare failed: " << sqlite3_errmsg(db) << "\n" << sql
              << std::endl;
    std::abort();
  }
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    std::cerr << "step failed: " << sqlite3_errmsg(db) << std::endl;
    std::abort();
  }
  const unsigned char *text = sqlite3_column_text(stmt, 0);
  std::string value =
      text != nullptr ? reinterpret_cast<const char *>(text) : "";
  sqlite3_finalize(stmt);
  return value;
}

int scoreSha256NotNull(sqlite3 *db) {
  sqlite3_stmt *stmt = nullptr;
  const char *sql = "PRAGMA score_db.table_info(scores)";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    std::cerr << "prepare failed: " << sqlite3_errmsg(db) << "\n" << sql
              << std::endl;
    std::abort();
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *name = sqlite3_column_text(stmt, 1);
    if (name != nullptr &&
        std::string(reinterpret_cast<const char *>(name)) == "chart_sha256") {
      const int notNull = sqlite3_column_int(stmt, 3);
      sqlite3_finalize(stmt);
      return notNull;
    }
  }
  sqlite3_finalize(stmt);
  return 0;
}

std::string scoreSha256DefaultValue(sqlite3 *db) {
  sqlite3_stmt *stmt = nullptr;
  const char *sql = "PRAGMA score_db.table_info(scores)";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    std::cerr << "prepare failed: " << sqlite3_errmsg(db) << "\n" << sql
              << std::endl;
    std::abort();
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *name = sqlite3_column_text(stmt, 1);
    if (name != nullptr &&
        std::string(reinterpret_cast<const char *>(name)) == "chart_sha256") {
      const unsigned char *defaultValue = sqlite3_column_text(stmt, 4);
      std::string value = defaultValue != nullptr
                              ? reinterpret_cast<const char *>(defaultValue)
                              : "<null>";
      sqlite3_finalize(stmt);
      return value;
    }
  }
  sqlite3_finalize(stmt);
  return "<missing>";
}

int queryInt(sqlite3 *db, const std::string &sql) {
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    std::cerr << "prepare failed: " << sqlite3_errmsg(db) << "\n" << sql
              << std::endl;
    std::abort();
  }
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    std::cerr << "step failed: " << sqlite3_errmsg(db) << std::endl;
    std::abort();
  }
  const int value = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return value;
}

bool scoreColumnExists(sqlite3 *db, const std::string &column) {
  sqlite3_stmt *stmt = nullptr;
  const char *sql = "PRAGMA table_info(scores)";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    std::cerr << "prepare failed: " << sqlite3_errmsg(db) << "\n" << sql
              << std::endl;
    std::abort();
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *name = sqlite3_column_text(stmt, 1);
    if (name != nullptr &&
        std::string(reinterpret_cast<const char *>(name)) == column) {
      sqlite3_finalize(stmt);
      return true;
    }
  }
  sqlite3_finalize(stmt);
  return false;
}

void insertScore(sqlite3 *db, const std::string &sha256,
                 const std::string &md5, const std::string &path, int lnMode,
                 int score, int maxScore, int maxCombo, int comboBreak,
                 int clearType, const std::string &createdAt) {
  std::string sql =
      "INSERT INTO score_db.scores(chart_sha256, chart_md5, chart_path, "
      "ln_mode, score, max_score, max_combo, combo_break, clear_type, "
      "created_at) VALUES(";
  sql += "'" + sha256 + "', ";
  sql += "'" + md5 + "', ";
  sql += "'" + path + "', ";
  sql += std::to_string(lnMode) + ", ";
  sql += std::to_string(score) + ", ";
  sql += std::to_string(maxScore) + ", ";
  sql += std::to_string(maxCombo) + ", ";
  sql += std::to_string(comboBreak) + ", ";
  sql += std::to_string(clearType) + ", ";
  sql += "'" + createdAt + "')";
  execOrAbort(db, sql);
}

} // namespace

int main() {
  sqlite3 *db = nullptr;
  if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
    std::cerr << "open failed" << std::endl;
    return 1;
  }

  const auto attachError = score_cache_queries::attachEmptyScoreDatabase(db);
  ASSERT_FALSE(attachError.has_value(), "attach empty score database");
  ASSERT_EQ(1, scoreSha256NotNull(db), "score sha256 column is not null");
  ASSERT_EQ(std::string("<null>"), scoreSha256DefaultValue(db),
            "score sha256 column has no default");
  ASSERT_FALSE(
      execSucceeds(db,
                   "INSERT INTO score_db.scores(chart_sha256, ln_mode, score, "
                   "max_score, max_combo, combo_break, clear_type) "
                   "VALUES(NULL, 0, 1, 2, 1, 1, 200)"),
      "score sha256 column rejects null");
  ASSERT_FALSE(
      execSucceeds(db,
                   "INSERT INTO score_db.scores(ln_mode, score, max_score, "
                   "max_combo, combo_break, clear_type) "
                   "VALUES(0, 1, 2, 1, 1, 200)"),
      "score sha256 column rejects omitted value");

  insertScore(db, "abcdef", "md5-a", "BMS/fallback.bms", 0, 160, 200, 80, 0,
              400, "2026-01-01 00:00:00");
  insertScore(db, "abcdef", "md5-a", "BMS/fallback.bms", 0, 180, 200, 90, 0,
              400, "2026-01-02 00:00:00");
  insertScore(db, "", "md5-only", "BMS/md5-only.bms", 0, 190, 200, 95, 1,
              500, "2026-01-02 00:00:00");
  insertScore(db, "rank-only", "md5-rank", "", 0, 50, 200, 20, 1, 500,
              "2026-01-01 00:00:00");
  insertScore(db, "rank-only", "md5-rank", "", 0, 150, 200, 75, 1, 200,
              "2026-01-02 00:00:00");

  ASSERT_EQ(180,
            queryInt(db,
                     "SELECT " +
                         score_cache_queries::scoreBestLookupExpr(
                             "'abcdef'", "0", "score")),
            "best score uses sha256 key");
  ASSERT_EQ(std::string("AAA"),
            queryString(db,
                        "SELECT " +
                            score_cache_queries::scoreBestLookupExpr(
                                "'abcdef'", "0", "score_rank")),
            "score rank label");
  ASSERT_EQ(600,
            queryInt(db,
                     "SELECT " + score_cache_queries::scoreRankLookupExpr(
                                     "'abcdef'", "1")),
            "ln mode 1 falls back to mode 0 full combo clear rank");
  ASSERT_EQ(150,
            queryInt(db,
                     "SELECT " +
                         score_cache_queries::scoreBestLookupExpr(
                             "'rank-only'", "0", "score")),
            "best score summary chooses highest score");
  ASSERT_EQ(500,
            queryInt(db,
                     "SELECT " + score_cache_queries::scoreRankLookupExpr(
                                     "'rank-only'", "0")),
            "clear rank summary keeps highest clear lamp separately");
  ASSERT_EQ(0,
            queryInt(db,
                     "SELECT COUNT(*) FROM "
                     "score_db.score_sha256_best_score_cache "
                     "WHERE chart_sha256 = 'md5-only'"),
            "md5-only score does not create summary row");

  insertScore(db, "abcdef", "md5-a", "BMS/fallback.bms", 0, 200, 200, 100,
              0, 400, "2026-01-03 00:00:00");
  ASSERT_EQ(200,
            queryInt(db,
                     "SELECT " +
                         score_cache_queries::scoreBestLookupExpr(
                             "'abcdef'", "0", "score")),
            "score query reflects score rows without rebuilding a cache");

  ASSERT_EQ(0,
            queryInt(db,
                     "SELECT COUNT(*) FROM sqlite_temp_master WHERE name IN "
                     "('score_best_cache', 'score_clear_rank_cache')"),
            "score queries do not create temp cache objects");

  execOrAbort(db, "DELETE FROM score_db.score_sha256_clear_rank_cache");
  execOrAbort(db, "DELETE FROM score_db.score_sha256_best_score_cache");
  const auto rebuildError =
      score_cache_queries::rebuildScoreSummaryTables(db, "score_db");
  ASSERT_FALSE(rebuildError.has_value(), "rebuild score summaries");
  ASSERT_EQ(200,
            queryInt(db,
                     "SELECT " +
                         score_cache_queries::scoreBestLookupExpr(
                             "'abcdef'", "0", "score")),
            "rebuilt score summary keeps best score");
  ASSERT_EQ(500,
            queryInt(db,
                     "SELECT " + score_cache_queries::scoreRankLookupExpr(
                                     "'rank-only'", "0")),
            "rebuilt clear rank summary keeps best clear lamp");

  sqlite3_close(db);

  const std::filesystem::path scoreDbPath =
      std::filesystem::temp_directory_path() /
      "asobmashow_score_cache_query_test.sqlite";
  std::filesystem::remove(scoreDbPath);
  sqlite3 *scoreDb = nullptr;
  if (sqlite3_open(scoreDbPath.string().c_str(), &scoreDb) != SQLITE_OK) {
    std::cerr << "open score db failed" << std::endl;
    return 1;
  }
  execOrAbort(scoreDb,
              "CREATE TABLE scores ("
              "id INTEGER PRIMARY KEY AUTOINCREMENT,"
              "chart_sha256 TEXT NOT NULL,"
              "ln_mode INTEGER NOT NULL DEFAULT 0,"
              "score INTEGER NOT NULL,"
              "max_score INTEGER NOT NULL,"
              "max_combo INTEGER NOT NULL,"
              "combo_break INTEGER NOT NULL,"
              "final_gauge REAL NOT NULL DEFAULT 0,"
              "clear_type INTEGER NOT NULL,"
              "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
              ")");
  execOrAbort(scoreDb,
              "INSERT INTO scores(chart_sha256, ln_mode, score, max_score, "
              "max_combo, combo_break, clear_type, created_at) "
              "VALUES('prepared-sha', 0, 123, 200, 50, 1, 300, "
              "'2026-01-04 00:00:00')");
  sqlite3_close(scoreDb);

  sqlite3 *chartDb = nullptr;
  if (sqlite3_open(":memory:", &chartDb) != SQLITE_OK) {
    std::cerr << "open chart db failed" << std::endl;
    return 1;
  }
  const auto prepareError =
      score_cache_queries::prepareScoreQueryDatabase(chartDb, scoreDbPath);
  ASSERT_FALSE(prepareError.has_value(), "prepare score query database");
  ASSERT_EQ(123,
            queryInt(chartDb,
                     "SELECT " +
                         score_cache_queries::scoreBestLookupExpr(
                             "'prepared-sha'", "0", "score")),
            "prepared score query database backfills summaries");
  sqlite3_close(chartDb);
  std::filesystem::remove(scoreDbPath);

  const std::filesystem::path missingScoreDbPath =
      std::filesystem::temp_directory_path() /
      "asobmashow_missing_score_cache_query_test.sqlite";
  std::filesystem::remove(missingScoreDbPath);

  sqlite3 *missingChartDb = nullptr;
  if (sqlite3_open(":memory:", &missingChartDb) != SQLITE_OK) {
    std::cerr << "open missing chart db failed" << std::endl;
    return 1;
  }
  const auto missingPrepareError =
      score_cache_queries::prepareScoreQueryDatabase(missingChartDb,
                                                     missingScoreDbPath);
  ASSERT_FALSE(missingPrepareError.has_value(),
               "prepare missing score query database");
  sqlite3_close(missingChartDb);

  sqlite3 *createdScoreDb = nullptr;
  if (sqlite3_open(missingScoreDbPath.string().c_str(), &createdScoreDb) !=
      SQLITE_OK) {
    std::cerr << "open created score db failed" << std::endl;
    return 1;
  }
  ASSERT_TRUE(scoreColumnExists(createdScoreDb, "pgreat"),
              "created fallback score table has pgreat");
  ASSERT_TRUE(scoreColumnExists(createdScoreDb, "fast"),
              "created fallback score table has fast");
  ASSERT_TRUE(
      execSucceeds(
          createdScoreDb,
          "INSERT INTO scores(chart_path, chart_md5, chart_sha256, ln_mode, "
          "chart_title, chart_artist, score, max_score, max_combo, "
          "combo_break, pgreat, great, good, bad, poor, kpoor, fast, slow, "
          "final_gauge, clear_type, created_at) VALUES("
          "'BMS/new.bms', 'new-md5', 'new-sha', 0, 'New', 'Artist', 100, "
          "200, 50, 2, 1, 2, 3, 4, 5, 6, 7, 8, 12.5, 300, "
          "'2026-01-05 00:00:00')"),
      "created fallback score table accepts full score insert");
  sqlite3_close(createdScoreDb);
  std::filesystem::remove(missingScoreDbPath);
  return 0;
}
