#include "../src/repositories/ScoreCacheQueries.h"
#include "../src/sqlite3.h"
#include "RepositorySqliteTestSupport.h"

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

// These cache fixtures represent historical rows without a known ruleset.
// Their zero indexed revision preserves the legacy participation policy.
constexpr const char *kNeutralProvenanceJson =
    R"({"playback":{"percent":100}})";

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

std::filesystem::path attachedDatabasePath(sqlite3 *db,
                                           const std::string &schema) {
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA database_list", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    std::cerr << "database_list prepare failed: " << sqlite3_errmsg(db)
              << std::endl;
    std::abort();
  }
  std::filesystem::path result;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *name = sqlite3_column_text(stmt, 1);
    const unsigned char *file = sqlite3_column_text(stmt, 2);
    if (name != nullptr && reinterpret_cast<const char *>(name) == schema) {
      result = file != nullptr ? reinterpret_cast<const char *>(file) : "";
      break;
    }
  }
  sqlite3_finalize(stmt);
  return result;
}

struct DenyNextAttach {
  bool pending = true;
};

int denyNextAttach(void *rawState, int action, const char *, const char *,
                   const char *, const char *) {
  auto &state = *static_cast<DenyNextAttach *>(rawState);
  if (action == SQLITE_ATTACH && state.pending) {
    state.pending = false;
    return SQLITE_DENY;
  }
  return SQLITE_OK;
}

bool createScoreDatabase(const std::filesystem::path &path,
                         const std::string &sha256, int score) {
  sqlite3 *database = nullptr;
  if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
    if (database != nullptr) {
      sqlite3_close(database);
    }
    return false;
  }
  const bool created =
      execSucceeds(database,
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
                   "ruleset_version INTEGER NOT NULL DEFAULT 0,"
                   "eligibility INTEGER NOT NULL DEFAULT 2,"
                   "provenance_json TEXT NOT NULL DEFAULT "
                   "'{\"playback\":{\"percent\":100}}',"
                   "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                   ")") &&
      execSucceeds(database, "INSERT INTO scores(chart_sha256, ln_mode, score, "
                             "max_score, max_combo, combo_break, clear_type, "
                             "created_at) VALUES('" +
                                 sha256 + "', 0, " + std::to_string(score) +
                                 ", 200, 50, 1, 300, "
                                 "'2026-01-04 00:00:00')");
  sqlite3_close(database);
  return created;
}

void insertScore(sqlite3 *db, const std::string &sha256, const std::string &md5,
                 const std::string &path, int lnMode, int score, int maxScore,
                 int maxCombo, int comboBreak, int clearType,
                 const std::string &createdAt,
                 const std::string &provenanceJson = kNeutralProvenanceJson,
                 int eligibility =
                     static_cast<int>(ScoreEligibility::LegacyUnverified)) {
  std::string sql =
      "INSERT INTO score_db.scores(chart_sha256, chart_md5, chart_path, "
      "ln_mode, score, max_score, max_combo, combo_break, clear_type, "
      "created_at, provenance_json, eligibility) VALUES(";
  sql += "'" + sha256 + "', ";
  sql += "'" + md5 + "', ";
  sql += "'" + path + "', ";
  sql += std::to_string(lnMode) + ", ";
  sql += std::to_string(score) + ", ";
  sql += std::to_string(maxScore) + ", ";
  sql += std::to_string(maxCombo) + ", ";
  sql += std::to_string(comboBreak) + ", ";
  sql += std::to_string(clearType) + ", ";
  sql += "'" + createdAt + "', ";
  sql += "'" + provenanceJson + "', ";
  sql += std::to_string(eligibility) + ")";
  execOrAbort(db, sql);
}

void attachScoreDatabaseForTest(sqlite3 *db) {
  execOrAbort(db, "ATTACH ':memory:' AS score_db");
  execOrAbort(db, "CREATE TABLE score_db.scores ("
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
                  "ruleset_version INTEGER NOT NULL DEFAULT 0,"
                  "eligibility INTEGER NOT NULL DEFAULT 2,"
                  "provenance_json TEXT NOT NULL DEFAULT "
                  "'{\"playback\":{\"percent\":100}}',"
                  "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                  ")");
  const auto summaryError =
      score_cache_queries::ensureScoreSummarySchema(db, "score_db");
  if (summaryError.has_value()) {
    std::cerr << "prepare test score summary schema failed: " << *summaryError
              << std::endl;
    std::abort();
  }
}

int testHistoricalOnlyAttachedCachesStayEmptyWithoutRebuilding() {
  const auto path = std::filesystem::temp_directory_path() /
                    "asobmashow_historical_attached_cache_test.sqlite";
  std::filesystem::remove(path);
  ASSERT_TRUE(createScoreDatabase(path, std::string(64, 'a'), 100),
              "historical score fixture initializes");
  sqlite3 *scores = nullptr;
  ASSERT_EQ(SQLITE_OK, sqlite3_open(path.string().c_str(), &scores), "open history");
  execOrAbort(scores, "UPDATE scores SET ruleset_version=3,eligibility=0,"
      "provenance_json='{\"playback\":{\"percent\":100},\"ruleset\":{"
      "\"id\":\"lr2\",\"version\":3,\"scoringModel\":\"asobmashow-v1\","
      "\"judgementModel\":\"lr2-v1\",\"gaugeModel\":\"lr2-gauge-v1\"}}'");
  // A large historical profile must not be rescanned for each menu lookup.
  execOrAbort(scores, "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<1000) "
      "INSERT INTO scores(chart_sha256,score,max_score,max_combo,combo_break,clear_type,"
      "ruleset_version,eligibility,provenance_json) SELECT chart_sha256,score,max_score,"
      "max_combo,combo_break,clear_type,ruleset_version,eligibility,provenance_json FROM scores,n");
  sqlite3_close(scores);
  sqlite3 *charts = nullptr;
  ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &charts), "open chart connection");
  ASSERT_FALSE(score_cache_queries::prepareScoreQueryDatabase(charts, path).has_value(),
               "first attached history prepare succeeds");
  struct RepairWork { int rebuilds = 0; int scannedRows = 0; } work;
  sqlite3_trace_v2(charts, SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE,
      [](unsigned mask, void *count, void *raw, void *) {
        const std::string_view sql = sqlite3_sql(static_cast<sqlite3_stmt *>(raw));
        auto &work = *static_cast<RepairWork *>(count);
        if (mask == SQLITE_TRACE_STMT && sql.starts_with("DELETE FROM") &&
            sql.find("score_sha256_best_score_cache") != std::string_view::npos) {
          ++work.rebuilds;
        }
        if (mask == SQLITE_TRACE_PROFILE && sql.starts_with("SELECT 1 FROM") &&
            sql.find("score_db.scores") != std::string_view::npos) {
          work.scannedRows += sqlite3_stmt_status(static_cast<sqlite3_stmt *>(raw),
                                                 SQLITE_STMTSTATUS_FULLSCAN_STEP, 0);
        }
        return 0;
      }, &work);
  for (int repeat = 0; repeat < 3; ++repeat) {
    ASSERT_FALSE(score_cache_queries::prepareScoreQueryDatabase(charts, path).has_value(),
                 "repeated attached history prepare succeeds");
  }
  ASSERT_EQ(0, work.rebuilds, "legitimately empty attached caches do not rebuild");
  ASSERT_EQ(0, work.scannedRows, "empty policy index avoids rescanning historical rows");
  ASSERT_EQ(0, queryInt(charts, "SELECT COUNT(*) FROM score_db.score_sha256_best_score_cache"),
            "old-policy scores remain excluded");
  // Eligibility can change independently of the cached rows. Real missing
  // summaries must still be repaired instead of trusting an empty cache forever.
  execOrAbort(charts, "UPDATE score_db.scores SET ruleset_version=0 WHERE id=1");
  ASSERT_FALSE(score_cache_queries::prepareScoreQueryDatabase(charts, path).has_value(),
               "eligible history repair succeeds");
  ASSERT_EQ(1, work.rebuilds, "eligible rows repair missing summaries once");
  ASSERT_EQ(1, queryInt(charts, "SELECT COUNT(*) FROM score_db.score_sha256_best_score_cache"),
            "repaired attached cache contains eligible score");
  sqlite3_close(charts);
  std::filesystem::remove(path);
  return 0;
}

} // namespace

int main() {
  if (testHistoricalOnlyAttachedCachesStayEmptyWithoutRebuilding() != 0) return 1;
  sqlite3 *db = nullptr;
  if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
    std::cerr << "open failed" << std::endl;
    return 1;
  }

  attachScoreDatabaseForTest(db);

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
  insertScore(db, "assisted", "md5-assisted", "", 0, 200, 200, 100, 0,
              kClearTypeHardClearRank, "2026-01-03 00:00:00",
              R"({"playback":{"percent":75}})");
  insertScore(db, "effective-tie", "md5-effective-tie", "", 0, 180, 200,
              90, 1, kClearTypeEasyClearRank, "2026-01-01 00:00:00");
  insertScore(db, "effective-tie", "md5-effective-tie", "", 0, 180, 200,
              90, 1, kClearTypeHardClearRank, "2026-01-02 00:00:00",
              R"({"playback":{"percent":75}})");
  insertScore(db, "eligibility", "md5-eligibility", "", 0, 160, 200, 80, 1,
              kClearTypeEasyClearRank, "2026-01-01 00:00:00");
  insertScore(db, "eligibility", "md5-eligibility", "", 0, 200, 200, 100,
              0, kClearTypeFullComboRank, "2026-01-02 00:00:00",
              kNeutralProvenanceJson,
              static_cast<int>(ScoreEligibility::Modified));
  insertScore(db, "historical-ln", "md5-historical-ln", "", 1, 170, 200,
              85, 1, kClearTypeHardClearRank, "2026-01-01 00:00:00");

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
  ASSERT_EQ(kClearTypeHardClearRank,
            queryInt(db,
                     "SELECT " + score_cache_queries::scoreRankLookupExpr(
                                     "'historical-ln'", "2")),
            "CN lamp falls back to a historical classic-LN result");
  ASSERT_EQ(kClearTypeHardClearRank,
            queryInt(db,
                     "SELECT " + score_cache_queries::scoreRankLookupExpr(
                                     "'historical-ln'", "3")),
            "HCN lamp falls back to a historical classic-LN result");
  ASSERT_EQ(1,
            queryInt(db,
                     "SELECT " + score_cache_queries::scoreBestLookupExpr(
                                     "'historical-ln'", "2", "score") +
                         " IS NULL"),
            "CN best score does not inherit a historical classic-LN score");
  insertScore(db, "historical-ln", "md5-historical-ln", "", 2, 120, 200,
              60, 1, kClearTypeEasyClearRank, "2026-01-02 00:00:00");
  ASSERT_EQ(kClearTypeEasyClearRank,
            queryInt(db,
                     "SELECT " + score_cache_queries::scoreRankLookupExpr(
                                     "'historical-ln'", "2")),
            "exact CN lamp takes priority over historical classic-LN");
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
  ASSERT_EQ(kClearTypeLightAssistedEasyClearRank,
            queryInt(db, "SELECT " + score_cache_queries::scoreRankLookupExpr(
                                         "'assisted'", "0")),
            "altered-playback zero-break score cache remains light-assisted");
  ASSERT_EQ(kClearTypeEasyClearRank,
            queryInt(db,
                     "SELECT clear_type FROM "
                     "score_db.score_sha256_best_score_cache WHERE "
                     "chart_sha256 = 'effective-tie' AND ln_mode = 0"),
            "equal score keeps the higher effective clear rank");
  const auto directBestOrder = score_cache_queries::detail::bestScoreOrderBySql(
      score_cache_queries::detail::bestScoreOrderKey(
          "s", score_cache_queries::detail::fullComboClearRankExpr("s"),
          "id"));
  ASSERT_EQ(kClearTypeEasyClearRank,
            queryInt(db,
                     "SELECT clear_type FROM score_db.scores s WHERE "
                     "chart_sha256 = 'effective-tie' AND ln_mode = 0 ORDER BY " +
                         directBestOrder + " LIMIT 1"),
            "direct best score uses the shared effective-rank ordering");
  ASSERT_EQ(160,
            queryInt(db,
                     "SELECT " +
                         score_cache_queries::scoreBestLookupExpr(
                             "'eligibility'", "0", "score")),
            "modified score does not replace legacy best score");
  ASSERT_EQ(kClearTypeEasyClearRank,
            queryInt(db, "SELECT " +
                             score_cache_queries::scoreRankLookupExpr(
                                 "'eligibility'", "0")),
            "modified score does not replace legacy clear rank");
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
  ASSERT_EQ(kClearTypeLightAssistedEasyClearRank,
            queryInt(db, "SELECT " + score_cache_queries::scoreRankLookupExpr(
                                         "'assisted'", "0")),
            "rebuilt altered-playback clear rank remains light-assisted");
  ASSERT_EQ(kClearTypeEasyClearRank,
            queryInt(db,
                     "SELECT clear_type FROM "
                     "score_db.score_sha256_best_score_cache WHERE "
                     "chart_sha256 = 'effective-tie' AND ln_mode = 0"),
            "rebuilt equal score keeps the higher effective clear rank");

  sqlite3_close(db);

  const std::filesystem::path scoreDbPath =
      std::filesystem::temp_directory_path() /
      "asobmashow_score_cache_query_test.sqlite";
  std::filesystem::remove(scoreDbPath);
  ASSERT_TRUE(createScoreDatabase(scoreDbPath, "prepared-sha", 123),
              "create first persistent score database");

  const std::filesystem::path secondScoreDbPath =
      std::filesystem::temp_directory_path() /
      "asobmashow_score_cache_query_test_second.sqlite";
  std::filesystem::remove(secondScoreDbPath);
  ASSERT_TRUE(createScoreDatabase(secondScoreDbPath, "prepared-sha", 187),
              "create second persistent score database");

  sqlite3 *chartDb = nullptr;
  if (sqlite3_open(":memory:", &chartDb) != SQLITE_OK) {
    std::cerr << "open chart db failed" << std::endl;
    return 1;
  }
  repository_test::StatementTrace trace;
  {
    repository_test::ScopedStatementTrace observation(chartDb, trace);
    ASSERT_FALSE(score_cache_queries::prepareScoreQueryDatabase(chartDb,
                                                                 scoreDbPath)
                     .has_value(),
                 "prepare score query database");
    ASSERT_FALSE(score_cache_queries::prepareScoreQueryDatabase(chartDb,
                                                                 scoreDbPath)
                     .has_value(),
                 "equivalent score attachment is reused");
  }
  int attachmentCount = 0;
  for (const std::string &sql : trace.sql) {
    attachmentCount +=
        sql.find("ATTACH DATABASE") != std::string::npos ? 1 : 0;
  }
  ASSERT_EQ(1, attachmentCount,
            "equivalent preparation does not reattach");

  const auto clearPlan = repository_test::explainPlan(
      chartDb,
      "SELECT chart_sha256, ln_mode, rank "
      "FROM score_db.score_sha256_clear_rank_cache "
      "WHERE chart_sha256 = 'abcdef' AND ln_mode = 0");
  ASSERT_TRUE(repository_test::planContains(clearPlan, "PRIMARY KEY"),
              "WITHOUT ROWID score lookup retains its composite primary-key "
              "search");
  ASSERT_EQ(123,
            queryInt(chartDb,
                     "SELECT " +
                         score_cache_queries::scoreBestLookupExpr(
                             "'prepared-sha'", "0", "score")),
            "prepared score query database backfills summaries");

  const std::filesystem::path nonCanonicalSecondPath =
      secondScoreDbPath.parent_path() / "." / secondScoreDbPath.filename();
  const auto secondPrepareError =
      score_cache_queries::prepareScoreQueryDatabase(chartDb,
                                                     nonCanonicalSecondPath);
  ASSERT_FALSE(secondPrepareError.has_value(),
               "persistent connection reattaches second score database");
  ASSERT_EQ(
      std::filesystem::canonical(secondScoreDbPath),
      std::filesystem::canonical(attachedDatabasePath(chartDb, "score_db")),
      "persistent connection exposes canonical second score path");
  ASSERT_EQ(
      187,
      queryInt(chartDb, "SELECT " + score_cache_queries::scoreBestLookupExpr(
                                        "'prepared-sha'", "0", "score")),
      "persistent connection reads second profile score data");

  const auto restoreFirstError =
      score_cache_queries::prepareScoreQueryDatabase(chartDb, scoreDbPath);
  ASSERT_FALSE(restoreFirstError.has_value(),
               "persistent connection restores first score database");
  DenyNextAttach denyState;
  ASSERT_EQ(SQLITE_OK,
            sqlite3_set_authorizer(chartDb, denyNextAttach, &denyState),
            "install one-shot attach failure");
  const auto deniedPrepareError =
      score_cache_queries::prepareScoreQueryDatabase(chartDb,
                                                     secondScoreDbPath);
  ASSERT_TRUE(deniedPrepareError.has_value(),
              "persistent reattach reports target attach failure");
  ASSERT_EQ(SQLITE_OK, sqlite3_set_authorizer(chartDb, nullptr, nullptr),
            "remove one-shot attach failure");
  const auto rollbackPrepareError =
      score_cache_queries::prepareScoreQueryDatabase(chartDb, scoreDbPath);
  ASSERT_FALSE(rollbackPrepareError.has_value(),
               "source score database reattaches after target failure");
  ASSERT_EQ(
      123,
      queryInt(chartDb, "SELECT " + score_cache_queries::scoreBestLookupExpr(
                                        "'prepared-sha'", "0", "score")),
      "source score data is restored after target attach failure");
  sqlite3_close(chartDb);
  std::filesystem::remove(scoreDbPath);
  std::filesystem::remove(secondScoreDbPath);

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
  ASSERT_TRUE(missingPrepareError.has_value(),
              "prepare missing score query database returns error");
  ASSERT_FALSE(std::filesystem::exists(missingScoreDbPath),
               "prepare missing score query database does not create file");
  sqlite3_close(missingChartDb);
  std::filesystem::remove(missingScoreDbPath);
  return 0;
}
