#pragma once

#include "RAII.h"
#include "sqlite3.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

using SqliteConnectionHandle = UniqueResource<sqlite3, sqlite3_close>;

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
