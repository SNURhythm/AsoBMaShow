#pragma once

#include "sqlite3.h"

#include <cassert>
#include <string>
#include <string_view>
#include <vector>

class SqliteInitializationTrace {
public:
  explicit SqliteInitializationTrace(sqlite3 *database) : database_(database) {
    assert(sqlite3_trace_v2(database_, SQLITE_TRACE_STMT, record, this) ==
           SQLITE_OK);
  }
  ~SqliteInitializationTrace() {
    sqlite3_trace_v2(database_, 0, nullptr, nullptr);
  }

  bool contains(std::string_view fragment) const {
    for (const auto &sql : statements) {
      if (sql.find(fragment) != std::string::npos) return true;
    }
    return false;
  }

  std::vector<std::string> statements;

private:
  static int record(unsigned, void *context, void *statement, void *) {
    const char *sql = sqlite3_sql(static_cast<sqlite3_stmt *>(statement));
    if (sql) {
      static_cast<SqliteInitializationTrace *>(context)->statements.emplace_back(sql);
    }
    return 0;
  }

  sqlite3 *database_;
};
