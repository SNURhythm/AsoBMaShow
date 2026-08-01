#pragma once

#include "../src/sqlite3.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace repository_test {

struct RawDatabaseFamilySnapshot {
  std::array<std::optional<std::string>, 4> files;

  bool operator==(const RawDatabaseFamilySnapshot &) const = default;
};

inline std::string readFileBytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  assert(input);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

inline RawDatabaseFamilySnapshot
rawDatabaseFamilySnapshot(const std::filesystem::path &databasePath) {
  constexpr std::array<const char *, 4> suffixes{"", "-journal", "-wal",
                                                 "-shm"};
  RawDatabaseFamilySnapshot snapshot;
  for (std::size_t i = 0; i < suffixes.size(); ++i) {
    const std::filesystem::path path = databasePath.string() + suffixes[i];
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    assert(!error);
    if (exists) {
      snapshot.files[i] = readFileBytes(path);
    }
  }
  return snapshot;
}

struct StatementTrace {
  int count = 0;
  std::vector<std::string> sql;

  static int callback(unsigned mask, void *context, void *statement, void *) {
    if ((mask & SQLITE_TRACE_STMT) == 0 || context == nullptr ||
        statement == nullptr) {
      return 0;
    }
    auto &trace = *static_cast<StatementTrace *>(context);
    ++trace.count;
    const char *text = sqlite3_sql(static_cast<sqlite3_stmt *>(statement));
    trace.sql.emplace_back(text != nullptr ? text : "");
    return 0;
  }
};

class ScopedStatementTrace {
public:
  ScopedStatementTrace(sqlite3 *database, StatementTrace &trace)
      : database_(database) {
    assert(database_ != nullptr);
    assert(sqlite3_trace_v2(database_, SQLITE_TRACE_STMT,
                            StatementTrace::callback, &trace) == SQLITE_OK);
  }

  ~ScopedStatementTrace() { sqlite3_trace_v2(database_, 0, nullptr, nullptr); }

  ScopedStatementTrace(const ScopedStatementTrace &) = delete;
  ScopedStatementTrace &operator=(const ScopedStatementTrace &) = delete;

private:
  sqlite3 *database_;
};

inline std::vector<std::string> explainPlan(sqlite3 *database,
                                            std::string_view query) {
  std::vector<std::string> details;
  const std::string sql = "EXPLAIN QUERY PLAN " + std::string(query);
  sqlite3_stmt *raw = nullptr;
  assert(sqlite3_prepare_v2(database, sql.c_str(), -1, &raw, nullptr) ==
         SQLITE_OK);
  while (sqlite3_step(raw) == SQLITE_ROW) {
    const auto *text = sqlite3_column_text(raw, 3);
    details.emplace_back(text != nullptr ? reinterpret_cast<const char *>(text)
                                         : "");
  }
  sqlite3_finalize(raw);
  return details;
}

inline bool planContains(const std::vector<std::string> &plan,
                         std::string_view text) {
  for (const std::string &line : plan) {
    if (line.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

} // namespace repository_test
