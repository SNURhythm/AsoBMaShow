#pragma once

#include "RAII.h"
#include "path.h"
#include "sqlite3.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

using SqliteConnectionHandle = UniqueResource<sqlite3, sqlite3_close>;

inline void closeSqliteDatabase(sqlite3 *db) {
  if (db != nullptr) {
    sqlite3_close(db);
  }
}

class SqliteErrorMessageHandle {
public:
  SqliteErrorMessageHandle() = default;
  SqliteErrorMessageHandle(const SqliteErrorMessageHandle &) = delete;
  SqliteErrorMessageHandle &
  operator=(const SqliteErrorMessageHandle &) = delete;
  ~SqliteErrorMessageHandle() { reset(); }

  const char *get() const { return message_; }
  char **out() {
    reset();
    return &message_;
  }
  void reset(char *message = nullptr) {
    if (message_ != nullptr) {
      sqlite3_free(message_);
    }
    message_ = message;
  }

private:
  char *message_ = nullptr;
};

class SqliteStatementHandle {
public:
  explicit SqliteStatementHandle(sqlite3_stmt *stmt = nullptr) : stmt_(stmt) {}
  SqliteStatementHandle(const SqliteStatementHandle &) = delete;
  SqliteStatementHandle &operator=(const SqliteStatementHandle &) = delete;

  sqlite3_stmt *get() const { return stmt_.get(); }
  void reset(sqlite3_stmt *stmt = nullptr) { stmt_.reset(stmt); }
  operator sqlite3_stmt *() const { return stmt_.get(); }

private:
  UniqueResource<sqlite3_stmt, sqlite3_finalize> stmt_;
};

inline int prepareSqliteStatement(sqlite3 *db, const char *query,
                                  SqliteStatementHandle &stmt) {
  sqlite3_stmt *rawStmt = nullptr;
  const int rc = sqlite3_prepare_v2(db, query, -1, &rawStmt, nullptr);
  stmt.reset(rawStmt);
  return rc;
}

inline int prepareSqliteStatement(sqlite3 *db, const std::string &query,
                                  SqliteStatementHandle &stmt) {
  return prepareSqliteStatement(db, query.c_str(), stmt);
}

inline bool bindSqliteText(sqlite3_stmt *stmt, int idx,
                           const std::string &value) {
  return sqlite3_bind_text(stmt, idx, value.c_str(), -1, SQLITE_TRANSIENT) ==
         SQLITE_OK;
}

inline std::string_view sqliteColumnTextView(sqlite3_stmt *stmt, int idx) {
  const auto *text =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
  if (text == nullptr) {
    return {};
  }
  return {text, static_cast<std::size_t>(sqlite3_column_bytes(stmt, idx))};
}

inline std::string sqliteColumnString(sqlite3_stmt *stmt, int idx) {
  const std::string_view text = sqliteColumnTextView(stmt, idx);
  return std::string(text);
}

inline std::string sqliteValueString(sqlite3_value *value) {
  if (value == nullptr || sqlite3_value_type(value) == SQLITE_NULL) {
    return "";
  }
  const unsigned char *text = sqlite3_value_text(value);
  return text != nullptr ? reinterpret_cast<const char *>(text) : "";
}

inline bool sqliteMessageContains(const char *message, const char *needle) {
  if (message == nullptr || needle == nullptr) {
    return false;
  }

  std::string lowerMessage(message);
  std::string lowerNeedle(needle);
  const auto lowerChar = [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  };
  std::transform(lowerMessage.begin(), lowerMessage.end(),
                 lowerMessage.begin(), lowerChar);
  std::transform(lowerNeedle.begin(), lowerNeedle.end(), lowerNeedle.begin(),
                 lowerChar);
  return lowerMessage.find(lowerNeedle) != std::string::npos;
}

inline std::optional<std::string>
executeSqlite(sqlite3 *db, const char *query,
              const char *allowedErrorNeedle = nullptr) {
  SqliteErrorMessageHandle errMsg;
  const int rc = sqlite3_exec(db, query, nullptr, nullptr, errMsg.out());
  if (rc == SQLITE_OK ||
      sqliteMessageContains(errMsg.get(), allowedErrorNeedle)) {
    return std::nullopt;
  }
  return errMsg.get() != nullptr ? std::string(errMsg.get())
                                 : std::string(sqlite3_errmsg(db));
}

inline std::optional<std::string>
attachSqliteDatabase(sqlite3 *db, const std::filesystem::path &path,
                     const char *schemaName) {
  const std::string pathText = fspath_to_utf8(path);
  char *query = sqlite3_mprintf("ATTACH DATABASE %Q AS \"%w\"",
                                pathText.c_str(), schemaName);
  if (query == nullptr) {
    return "could not allocate attach statement";
  }

  SqliteErrorMessageHandle errMsg;
  const int rc = sqlite3_exec(db, query, nullptr, nullptr, errMsg.out());
  sqlite3_free(query);
  if (rc == SQLITE_OK) {
    return std::nullopt;
  }
  return errMsg.get() != nullptr ? std::string(errMsg.get())
                                 : std::string(sqlite3_errmsg(db));
}

inline std::optional<std::string>
querySqliteTableExists(sqlite3 *db, const char *tableName, bool &exists) {
  exists = false;
  SqliteStatementHandle stmt;
  const int rc = prepareSqliteStatement(
      db,
      "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ? LIMIT 1",
      stmt);
  if (rc != SQLITE_OK) {
    return sqlite3_errmsg(db);
  }
  bindSqliteText(stmt, 1, tableName);

  const int stepRc = sqlite3_step(stmt.get());
  if (stepRc == SQLITE_ROW) {
    exists = true;
    return std::nullopt;
  }
  if (stepRc == SQLITE_DONE) {
    return std::nullopt;
  }
  return sqlite3_errmsg(db);
}

inline std::optional<std::string>
querySqliteTableHasColumn(sqlite3 *db, const char *tableName,
                          const char *columnName, bool &hasColumn) {
  hasColumn = false;
  char *query = sqlite3_mprintf("PRAGMA table_info(\"%w\")", tableName);
  if (query == nullptr) {
    return "could not allocate table_info statement";
  }

  SqliteStatementHandle stmt;
  const int rc = prepareSqliteStatement(db, query, stmt);
  sqlite3_free(query);
  if (rc != SQLITE_OK) {
    return sqlite3_errmsg(db);
  }

  int stepRc = SQLITE_OK;
  while ((stepRc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
    if (sqliteColumnString(stmt.get(), 1) == columnName) {
      hasColumn = true;
      return std::nullopt;
    }
  }
  if (stepRc != SQLITE_DONE) {
    return sqlite3_errmsg(db);
  }
  return std::nullopt;
}

inline sqlite3 *openSqliteDatabase(const std::filesystem::path &path,
                                   std::string &errorMessage,
                                   int busyTimeoutMs = 1000) {
  sqlite3 *db = nullptr;
  const std::string pathText = fspath_to_utf8(path);
  const int rc = sqlite3_open(pathText.c_str(), &db);
  if (db != nullptr) {
    sqlite3_busy_timeout(db, busyTimeoutMs);
  }
  if (rc != SQLITE_OK) {
    errorMessage = db != nullptr ? sqlite3_errmsg(db) : "unknown error";
    if (db != nullptr) {
      closeSqliteDatabase(db);
    }
    return nullptr;
  }
  return db;
}

inline std::optional<std::string>
applySqlitePragmas(sqlite3 *db, std::initializer_list<const char *> pragmas) {
  for (const char *pragma : pragmas) {
    if (const auto error = executeSqlite(db, pragma)) {
      return std::string(pragma) + ": " + *error;
    }
  }
  return std::nullopt;
}

class SqliteTransactionHandle {
public:
  SqliteTransactionHandle(sqlite3 *db, const char *beginQuery,
                          std::string &errorMessage)
      : db_(db) {
    if (db_ == nullptr) {
      errorMessage = "database is not open";
      return;
    }
    if (const auto error = executeSqlite(db_, beginQuery)) {
      errorMessage = *error;
      return;
    }
    active_ = true;
  }

  SqliteTransactionHandle(const SqliteTransactionHandle &) = delete;
  SqliteTransactionHandle &
  operator=(const SqliteTransactionHandle &) = delete;

  ~SqliteTransactionHandle() {
    if (active_) {
      sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
  }

  bool active() const { return active_; }

  bool commit(std::string &errorMessage) {
    if (!active_) {
      errorMessage = "transaction is not active";
      return false;
    }
    if (const auto error = executeSqlite(db_, "COMMIT")) {
      errorMessage = *error;
      return false;
    }
    active_ = false;
    return true;
  }

private:
  sqlite3 *db_ = nullptr;
  bool active_ = false;
};
