#include "../src/ChartSqlExpressions.h"
#include "../src/sqlite3.h"

#include <cstdlib>
#include <iostream>
#include <string>

#define ASSERT_EQ(expected, actual, label)                                     \
  if ((expected) != (actual)) {                                                \
    std::cerr << label << " expected " << (expected) << " actual "            \
              << (actual) << std::endl;                                       \
    return 1;                                                                 \
  }

namespace {

int queryInt(sqlite3 *db, const std::string &sql) {
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    std::cerr << "prepare failed: " << sqlite3_errmsg(db) << "\n" << sql
              << std::endl;
    std::abort();
  }
  const int step = sqlite3_step(stmt);
  if (step != SQLITE_ROW) {
    std::cerr << "step failed: " << sqlite3_errmsg(db) << std::endl;
    std::abort();
  }
  const int value = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return value;
}

} // namespace

int main() {
  sqlite3 *db = nullptr;
  if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
    std::cerr << "open failed" << std::endl;
    return 1;
  }

  const char *setup =
      "CREATE TABLE chart_meta(path TEXT PRIMARY KEY, title TEXT);"
      "INSERT INTO chart_meta(path, title) VALUES('/b', 'B song');"
      "INSERT INTO chart_meta(path, title) VALUES('/a', 'a song');";
  char *error = nullptr;
  if (sqlite3_exec(db, setup, nullptr, nullptr, &error) != SQLITE_OK) {
    std::cerr << "setup failed: " << (error != nullptr ? error : "") << "\n";
    sqlite3_free(error);
    sqlite3_close(db);
    return 1;
  }

  const int orderedIndex =
      queryInt(db,
               "SELECT row_number - 1 FROM ("
               "SELECT path, ROW_NUMBER() OVER "
               "(ORDER BY title COLLATE NOCASE, path) AS row_number "
               "FROM chart_meta) WHERE path = '/a'");

  const std::string countSql =
      "WITH target AS (SELECT title AS target_title, path AS target_path "
      "FROM chart_meta WHERE path = '/a') "
      "SELECT COUNT(*) FROM chart_meta cm, target WHERE " +
      asobmshow::chart_sql::defaultChartMetaBeforeTargetPredicate("cm",
                                                                  "target");
  const int countedIndex = queryInt(db, countSql);
  sqlite3_close(db);

  ASSERT_EQ(orderedIndex, countedIndex,
            "default index predicate matches NOCASE title ordering");
  return 0;
}
