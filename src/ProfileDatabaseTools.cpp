#include "ProfileDatabaseTools.h"

#include "sqlite3.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace {
struct ConnectionCloser {
  void operator()(sqlite3 *database) const {
    if (database != nullptr) {
      sqlite3_close(database);
    }
  }
};
using Connection = std::unique_ptr<sqlite3, ConnectionCloser>;

std::string pathUtf8(const std::filesystem::path &path) {
#ifdef _WIN32
  const auto value = path.u8string();
  return {reinterpret_cast<const char *>(value.data()), value.size()};
#else
  return path.string();
#endif
}

void setError(std::string &errorMessage, sqlite3 *database,
              std::string_view operation, int result) {
  errorMessage =
      std::string(operation) + " failed (" + std::to_string(result) + ")";
  if (database != nullptr) {
    errorMessage += ": ";
    errorMessage += sqlite3_errmsg(database);
  }
}

Connection openDatabase(const std::filesystem::path &path, int flags,
                        std::string &errorMessage) {
  sqlite3 *raw = nullptr;
  const std::string encoded = pathUtf8(path);
  const int result = sqlite3_open_v2(encoded.c_str(), &raw, flags, nullptr);
  if (result != SQLITE_OK) {
    setError(errorMessage, raw, "opening SQLite database", result);
    if (raw != nullptr) {
      sqlite3_close(raw);
    }
    return {};
  }
  sqlite3_busy_timeout(raw, 1000);
  return Connection(raw);
}

bool execute(sqlite3 *database, const char *sql, std::string &errorMessage,
             std::string_view operation) {
  char *rawError = nullptr;
  const int result = sqlite3_exec(database, sql, nullptr, nullptr, &rawError);
  if (result == SQLITE_OK) {
    return true;
  }
  errorMessage = std::string(operation) + " failed";
  if (rawError != nullptr) {
    errorMessage += ": ";
    errorMessage += rawError;
  } else {
    errorMessage += ": ";
    errorMessage += sqlite3_errmsg(database);
  }
  sqlite3_free(rawError);
  return false;
}

bool validIdentifier(std::string_view value) {
  if (value.empty() ||
      !(std::isalpha(static_cast<unsigned char>(value.front())) != 0 ||
        value.front() == '_')) {
    return false;
  }
  return std::ranges::all_of(value.substr(1), [](char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 ||
           character == '_';
  });
}

std::optional<std::int64_t> rowCount(sqlite3 *database, std::string_view table,
                                     std::string &errorMessage) {
  if (!validIdentifier(table)) {
    errorMessage = "invalid SQLite table identifier";
    return std::nullopt;
  }
  const std::string query =
      "SELECT COUNT(*) FROM \"" + std::string(table) + "\"";
  sqlite3_stmt *rawStatement = nullptr;
  int result =
      sqlite3_prepare_v2(database, query.c_str(), -1, &rawStatement, nullptr);
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
      rawStatement, sqlite3_finalize);
  if (result != SQLITE_OK) {
    setError(errorMessage, database, "preparing SQLite row count", result);
    return std::nullopt;
  }
  result = sqlite3_step(statement.get());
  if (result != SQLITE_ROW) {
    setError(errorMessage, database, "reading SQLite row count", result);
    return std::nullopt;
  }
  const std::int64_t count = sqlite3_column_int64(statement.get(), 0);
  result = sqlite3_step(statement.get());
  if (result != SQLITE_DONE) {
    setError(errorMessage, database, "finishing SQLite row count", result);
    return std::nullopt;
  }
  return count;
}

std::optional<std::int64_t>
standaloneLegacyReplayRowCount(sqlite3 *database, std::string &errorMessage) {
  constexpr const char *query =
      "SELECT COUNT(*) FROM replays AS replay "
      "WHERE NOT EXISTS(SELECT 1 FROM course_replay_stages AS stage "
      "WHERE stage.replay_id=replay.id)";
  sqlite3_stmt *rawStatement = nullptr;
  int result =
      sqlite3_prepare_v2(database, query, -1, &rawStatement, nullptr);
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
      rawStatement, sqlite3_finalize);
  if (result != SQLITE_OK) {
    setError(errorMessage, database,
             "preparing standalone legacy replay row count", result);
    return std::nullopt;
  }
  result = sqlite3_step(statement.get());
  if (result != SQLITE_ROW) {
    setError(errorMessage, database,
             "reading standalone legacy replay row count", result);
    return std::nullopt;
  }
  const std::int64_t count = sqlite3_column_int64(statement.get(), 0);
  result = sqlite3_step(statement.get());
  if (result != SQLITE_DONE) {
    setError(errorMessage, database,
             "finishing standalone legacy replay row count", result);
    return std::nullopt;
  }
  return count;
}

std::optional<std::map<std::string, std::int64_t>>
userTableRowCounts(sqlite3 *database, std::string &errorMessage) {
  constexpr const char *query =
      "SELECT name FROM sqlite_master WHERE type='table' "
      "AND name NOT LIKE 'sqlite_%' ORDER BY name";
  sqlite3_stmt *rawStatement = nullptr;
  int result = sqlite3_prepare_v2(database, query, -1, &rawStatement, nullptr);
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
      rawStatement, sqlite3_finalize);
  if (result != SQLITE_OK) {
    setError(errorMessage, database, "enumerating SQLite tables", result);
    return std::nullopt;
  }

  std::vector<std::string> tables;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    const auto *name = sqlite3_column_text(statement.get(), 0);
    if (name == nullptr) {
      errorMessage = "SQLite returned a null table name";
      return std::nullopt;
    }
    tables.emplace_back(reinterpret_cast<const char *>(name));
  }
  if (result != SQLITE_DONE) {
    setError(errorMessage, database, "reading SQLite table names", result);
    return std::nullopt;
  }

  std::map<std::string, std::int64_t> counts;
  for (const std::string &table : tables) {
    const auto count = rowCount(database, table, errorMessage);
    if (!count.has_value()) {
      return std::nullopt;
    }
    counts.emplace(table, *count);
  }
  return counts;
}

bool integrityCheck(sqlite3 *database, std::string &errorMessage) {
  sqlite3_stmt *rawStatement = nullptr;
  int result = sqlite3_prepare_v2(database, "PRAGMA integrity_check", -1,
                                  &rawStatement, nullptr);
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
      rawStatement, sqlite3_finalize);
  if (result != SQLITE_OK) {
    setError(errorMessage, database, "preparing SQLite integrity check",
             result);
    return false;
  }
  bool sawOk = false;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    const auto *text = sqlite3_column_text(statement.get(), 0);
    const std::string detail =
        text == nullptr ? std::string()
                        : std::string(reinterpret_cast<const char *>(text));
    if (!sawOk && detail == "ok") {
      sawOk = true;
      continue;
    }
    errorMessage = "SQLite integrity check failed: " + detail;
    return false;
  }
  if (result != SQLITE_DONE) {
    setError(errorMessage, database, "running SQLite integrity check", result);
    return false;
  }
  if (!sawOk) {
    errorMessage = "SQLite integrity check returned no result";
    return false;
  }
  return true;
}
} // namespace

bool snapshotSqliteDatabase(const std::filesystem::path &source,
                            const std::filesystem::path &destination,
                            std::string &errorMessage) {
  errorMessage.clear();
  std::error_code filesystemError;
  if (!std::filesystem::exists(source, filesystemError) || filesystemError) {
    errorMessage = filesystemError ? "unable to inspect SQLite source: " +
                                         filesystemError.message()
                                   : "SQLite source does not exist";
    return false;
  }
  if (!destination.parent_path().empty()) {
    std::filesystem::create_directories(destination.parent_path(),
                                        filesystemError);
    if (filesystemError) {
      errorMessage = "unable to create SQLite destination directory: " +
                     filesystemError.message();
      return false;
    }
  }

  const std::filesystem::path temporary =
      std::filesystem::path(destination.string() + ".snapshot.tmp");
  std::filesystem::remove(temporary, filesystemError);
  filesystemError.clear();

  Connection sourceDatabase = openDatabase(
      source, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, errorMessage);
  if (!sourceDatabase) {
    return false;
  }
  if (!execute(sourceDatabase.get(), "BEGIN", errorMessage,
               "starting SQLite snapshot transaction")) {
    return false;
  }

  Connection destinationDatabase = openDatabase(
      temporary,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
      errorMessage);
  if (!destinationDatabase) {
    execute(sourceDatabase.get(), "ROLLBACK", errorMessage,
            "rolling back SQLite snapshot transaction");
    return false;
  }

  sqlite3_backup *backup = sqlite3_backup_init(
      destinationDatabase.get(), "main", sourceDatabase.get(), "main");
  if (backup == nullptr) {
    setError(errorMessage, destinationDatabase.get(),
             "initializing SQLite backup",
             sqlite3_errcode(destinationDatabase.get()));
    execute(sourceDatabase.get(), "ROLLBACK", errorMessage,
            "rolling back SQLite snapshot transaction");
    destinationDatabase.reset();
    std::filesystem::remove(temporary, filesystemError);
    return false;
  }

  int result = SQLITE_OK;
  int busyRetries = 0;
  do {
    result = sqlite3_backup_step(backup, 128);
    if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
      if (++busyRetries > 100) {
        break;
      }
      sqlite3_sleep(10);
    }
  } while (result == SQLITE_OK || result == SQLITE_BUSY ||
           result == SQLITE_LOCKED);
  const int finishResult = sqlite3_backup_finish(backup);
  if (result != SQLITE_DONE || finishResult != SQLITE_OK) {
    setError(errorMessage, destinationDatabase.get(), "copying SQLite backup",
             result != SQLITE_DONE ? result : finishResult);
    execute(sourceDatabase.get(), "ROLLBACK", errorMessage,
            "rolling back SQLite snapshot transaction");
    destinationDatabase.reset();
    std::filesystem::remove(temporary, filesystemError);
    return false;
  }

  const auto sourceCounts =
      userTableRowCounts(sourceDatabase.get(), errorMessage);
  const auto destinationCounts =
      userTableRowCounts(destinationDatabase.get(), errorMessage);
  if (!sourceCounts || !destinationCounts ||
      *sourceCounts != *destinationCounts) {
    if (errorMessage.empty()) {
      errorMessage =
          "SQLite backup row counts do not match the source snapshot";
    }
    execute(sourceDatabase.get(), "ROLLBACK", errorMessage,
            "rolling back SQLite snapshot transaction");
    destinationDatabase.reset();
    std::filesystem::remove(temporary, filesystemError);
    return false;
  }
  if (!integrityCheck(destinationDatabase.get(), errorMessage)) {
    execute(sourceDatabase.get(), "ROLLBACK", errorMessage,
            "rolling back SQLite snapshot transaction");
    destinationDatabase.reset();
    std::filesystem::remove(temporary, filesystemError);
    return false;
  }
  if (!execute(sourceDatabase.get(), "COMMIT", errorMessage,
               "committing SQLite snapshot transaction")) {
    destinationDatabase.reset();
    std::filesystem::remove(temporary, filesystemError);
    return false;
  }

  destinationDatabase.reset();
  sourceDatabase.reset();
  const std::filesystem::path previous =
      std::filesystem::path(destination.string() + ".snapshot.previous");
  std::filesystem::remove(previous, filesystemError);
  filesystemError.clear();
  const bool hadDestination =
      std::filesystem::exists(destination, filesystemError);
  if (filesystemError) {
    errorMessage =
        "unable to inspect SQLite destination: " + filesystemError.message();
    std::filesystem::remove(temporary, filesystemError);
    return false;
  }
  if (hadDestination) {
    std::filesystem::rename(destination, previous, filesystemError);
    if (filesystemError) {
      errorMessage = "unable to stage previous SQLite destination: " +
                     filesystemError.message();
      std::filesystem::remove(temporary, filesystemError);
      return false;
    }
  }
  std::filesystem::rename(temporary, destination, filesystemError);
  if (filesystemError) {
    errorMessage =
        "unable to finalize SQLite snapshot: " + filesystemError.message();
    if (hadDestination) {
      std::error_code restoreError;
      std::filesystem::rename(previous, destination, restoreError);
      if (restoreError) {
        errorMessage += "; unable to restore previous destination: " +
                        restoreError.message();
      }
    }
    std::filesystem::remove(temporary, filesystemError);
    return false;
  }
  if (hadDestination) {
    std::filesystem::remove(previous, filesystemError);
  }
  return true;
}

bool sqliteIntegrityCheck(const std::filesystem::path &database,
                          std::string &errorMessage) {
  errorMessage.clear();
  Connection connection = openDatabase(
      database, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, errorMessage);
  return connection != nullptr &&
         integrityCheck(connection.get(), errorMessage);
}

std::optional<std::int64_t>
sqliteTableRowCount(const std::filesystem::path &database,
                    std::string_view table, std::string &errorMessage) {
  errorMessage.clear();
  Connection connection = openDatabase(
      database, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, errorMessage);
  if (!connection) {
    return std::nullopt;
  }
  return rowCount(connection.get(), table, errorMessage);
}

std::optional<std::int64_t> sqliteStandaloneLegacyReplayRowCount(
    const std::filesystem::path &database, std::string &errorMessage) {
  errorMessage.clear();
  Connection connection = openDatabase(
      database, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, errorMessage);
  if (!connection) {
    return std::nullopt;
  }
  return standaloneLegacyReplayRowCount(connection.get(), errorMessage);
}

std::optional<std::map<std::string, std::int64_t>>
sqliteUserTableRowCounts(const std::filesystem::path &database,
                         std::string &errorMessage) {
  errorMessage.clear();
  Connection connection = openDatabase(
      database, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, errorMessage);
  if (!connection) {
    return std::nullopt;
  }
  return userTableRowCounts(connection.get(), errorMessage);
}

std::optional<int>
sqliteDatabaseUserVersion(const std::filesystem::path &database,
                          std::string &errorMessage) {
  errorMessage.clear();
  Connection connection = openDatabase(
      database, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, errorMessage);
  if (!connection) {
    return std::nullopt;
  }
  sqlite3_stmt *rawStatement = nullptr;
  int result = sqlite3_prepare_v2(connection.get(), "PRAGMA user_version", -1,
                                  &rawStatement, nullptr);
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
      rawStatement, sqlite3_finalize);
  if (result != SQLITE_OK) {
    setError(errorMessage, connection.get(), "reading SQLite user version",
             result);
    return std::nullopt;
  }
  result = sqlite3_step(statement.get());
  if (result != SQLITE_ROW) {
    setError(errorMessage, connection.get(), "reading SQLite user version",
             result);
    return std::nullopt;
  }
  const int version = sqlite3_column_int(statement.get(), 0);
  result = sqlite3_step(statement.get());
  if (result != SQLITE_DONE) {
    setError(errorMessage, connection.get(), "finishing SQLite user version",
             result);
    return std::nullopt;
  }
  return version;
}
