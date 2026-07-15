#pragma once

#include "../RAII.h"
#include "../path.h"
#include "../sqlite3.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

using SqliteConnectionHandle = UniqueResource<sqlite3, sqlite3_close>;

inline void closeSqliteDatabase(sqlite3 *db) {
  if (db != nullptr) {
    sqlite3_close(db);
  }
}

inline std::string sqliteDatabaseError(sqlite3 *db) {
  return db != nullptr ? sqlite3_errmsg(db) : "database is not open";
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

template <typename LogSqlError>
inline bool prepareSqliteStatementLogged(sqlite3 *db, const char *query,
                                         SqliteStatementHandle &stmt,
                                         const char *context,
                                         const LogSqlError &logSqlError) {
  if (prepareSqliteStatement(db, query, stmt) != SQLITE_OK) {
    logSqlError(context, sqliteDatabaseError(db));
    return false;
  }
  return true;
}

template <typename LogSqlError>
inline bool prepareSqliteStatementLogged(sqlite3 *db, const std::string &query,
                                         SqliteStatementHandle &stmt,
                                         const char *context,
                                         const LogSqlError &logSqlError) {
  return prepareSqliteStatementLogged(db, query.c_str(), stmt, context,
                                      logSqlError);
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
  std::transform(lowerMessage.begin(), lowerMessage.end(), lowerMessage.begin(),
                 lowerChar);
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
                                 : sqliteDatabaseError(db);
}

template <typename LogSqlError>
inline bool executeSqliteLogged(sqlite3 *db, const char *query,
                                const char *context,
                                const LogSqlError &logSqlError,
                                const char *allowedErrorNeedle = nullptr) {
  if (const auto error = executeSqlite(db, query, allowedErrorNeedle)) {
    logSqlError(context, *error);
    return false;
  }
  return true;
}

template <typename LogSqlError>
inline bool updateSqliteColumnWithExpressionLogged(
    sqlite3 *db, const char *tableName, const char *columnName,
    const std::string &expression, const char *context,
    const LogSqlError &logSqlError, int *changedRows = nullptr) {
  std::string query = "UPDATE ";
  query += tableName;
  query += " SET ";
  query += columnName;
  query += " = ";
  query += expression;
  query += " WHERE ";
  query += columnName;
  query += " != ";
  query += expression;

  if (!executeSqliteLogged(db, query.c_str(), context, logSqlError)) {
    return false;
  }
  if (changedRows != nullptr) {
    *changedRows = sqlite3_changes(db);
  }
  return true;
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
                                 : sqliteDatabaseError(db);
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
    return sqliteDatabaseError(db);
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
  return sqliteDatabaseError(db);
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
    return sqliteDatabaseError(db);
  }

  int stepRc = SQLITE_OK;
  while ((stepRc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
    if (sqliteColumnString(stmt.get(), 1) == columnName) {
      hasColumn = true;
      return std::nullopt;
    }
  }
  if (stepRc != SQLITE_DONE) {
    return sqliteDatabaseError(db);
  }
  return std::nullopt;
}

template <typename LogSqlError>
inline bool ensureSqliteTableColumnLogged(sqlite3 *db, const char *tableName,
                                          const char *columnName,
                                          const char *alterQuery,
                                          const char *schemaContext,
                                          const char *alterContext,
                                          const LogSqlError &logSqlError) {
  bool hasColumn = false;
  if (const auto error =
          querySqliteTableHasColumn(db, tableName, columnName, hasColumn)) {
    logSqlError(schemaContext, *error);
    return false;
  }
  return hasColumn ||
         executeSqliteLogged(db, alterQuery, alterContext, logSqlError);
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

struct SqliteDatabaseFamilyFileState {
  bool exists = false;
  std::uintmax_t size = 0;
  std::filesystem::file_time_type writeTime{};

  bool operator==(const SqliteDatabaseFamilyFileState &) const = default;
};

using SqliteDatabaseFamilyState = std::array<SqliteDatabaseFamilyFileState, 4>;

inline std::optional<SqliteDatabaseFamilyState>
readSqliteDatabaseFamilyState(const std::filesystem::path &path,
                              std::string &errorMessage) {
  constexpr std::array<const char *, 4> suffixes = {"", "-journal", "-wal",
                                                    "-shm"};
  SqliteDatabaseFamilyState state;
  for (std::size_t i = 0; i < suffixes.size(); ++i) {
    std::filesystem::path familyPath = path;
    familyPath += suffixes[i];
    std::error_code existsError;
    state[i].exists = std::filesystem::exists(familyPath, existsError);
    if (existsError) {
      errorMessage = "could not inspect " + fspath_to_utf8(familyPath) + ": " +
                     existsError.message();
      return std::nullopt;
    }
    if (!state[i].exists) {
      continue;
    }

    std::error_code regularFileError;
    if (!std::filesystem::is_regular_file(familyPath, regularFileError) ||
        regularFileError) {
      errorMessage = "database family member is not a regular file: " +
                     fspath_to_utf8(familyPath);
      return std::nullopt;
    }
    std::error_code sizeError;
    state[i].size = std::filesystem::file_size(familyPath, sizeError);
    if (sizeError) {
      errorMessage = "could not read size of " + fspath_to_utf8(familyPath) +
                     ": " + sizeError.message();
      return std::nullopt;
    }
    std::error_code writeTimeError;
    state[i].writeTime =
        std::filesystem::last_write_time(familyPath, writeTimeError);
    if (writeTimeError) {
      errorMessage = "could not read write time of " +
                     fspath_to_utf8(familyPath) + ": " +
                     writeTimeError.message();
      return std::nullopt;
    }
  }
  return state;
}

inline std::optional<int>
readRawSqliteUserVersion(const std::filesystem::path &path,
                         std::uintmax_t fileSize, std::string &errorMessage) {
  if (fileSize == 0) {
    return 0;
  }
  constexpr std::size_t kHeaderSize = 100;
  if (fileSize < kHeaderSize) {
    errorMessage = "database file is shorter than the SQLite header";
    return std::nullopt;
  }

  std::array<unsigned char, kHeaderSize> header{};
  std::ifstream input(path, std::ios::binary);
  if (!input.read(reinterpret_cast<char *>(header.data()), header.size())) {
    errorMessage = "could not read SQLite database header";
    return std::nullopt;
  }
  constexpr std::array<unsigned char, 16> magic = {'S', 'Q', 'L', 'i', 't', 'e',
                                                   ' ', 'f', 'o', 'r', 'm', 'a',
                                                   't', ' ', '3', 0};
  if (!std::equal(magic.begin(), magic.end(), header.begin())) {
    errorMessage = "database file has an invalid SQLite header";
    return std::nullopt;
  }

  const unsigned int encodedPageSize =
      (static_cast<unsigned int>(header[16]) << 8U) | header[17];
  const unsigned int pageSize = encodedPageSize == 1 ? 65536 : encodedPageSize;
  if (pageSize < 512 || pageSize > 65536 || (pageSize & (pageSize - 1U)) != 0) {
    errorMessage = "database file has an invalid SQLite page size";
    return std::nullopt;
  }
  if ((header[18] != 1 && header[18] != 2) ||
      (header[19] != 1 && header[19] != 2)) {
    errorMessage = "database file has an unsupported SQLite format";
    return std::nullopt;
  }

  const std::uint32_t rawVersion =
      (static_cast<std::uint32_t>(header[60]) << 24U) |
      (static_cast<std::uint32_t>(header[61]) << 16U) |
      (static_cast<std::uint32_t>(header[62]) << 8U) |
      static_cast<std::uint32_t>(header[63]);
  const std::int64_t signedVersion =
      rawVersion <= 0x7fffffffU
          ? static_cast<std::int64_t>(rawVersion)
          : static_cast<std::int64_t>(rawVersion) - 0x100000000LL;
  return static_cast<int>(signedVersion);
}

inline constexpr std::uintmax_t kMaximumSqliteSnapshotBytes =
    512ULL * 1024ULL * 1024ULL;
inline constexpr std::uintmax_t kSqliteSnapshotAuxiliaryReserveBytes =
    64ULL * 1024ULL;

inline constexpr bool sqliteSnapshotFitsBudget(std::uintmax_t mainBytes,
                                               std::uintmax_t walBytes) {
  if (mainBytes >
      kMaximumSqliteSnapshotBytes - kSqliteSnapshotAuxiliaryReserveBytes) {
    return false;
  }
  return walBytes <= kMaximumSqliteSnapshotBytes -
                         kSqliteSnapshotAuxiliaryReserveBytes - mainBytes;
}

inline bool copySqliteFileExactly(const std::filesystem::path &source,
                                  const std::filesystem::path &destination,
                                  std::uintmax_t expectedBytes,
                                  std::string &errorMessage) {
  std::ifstream input(source, std::ios::binary);
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!input || !output) {
    errorMessage = "could not open schema preflight copy files";
    return false;
  }

  std::array<char, 64 * 1024> buffer{};
  std::uintmax_t remaining = expectedBytes;
  while (remaining > 0) {
    const auto requested = static_cast<std::streamsize>(
        std::min<std::uintmax_t>(remaining, buffer.size()));
    input.read(buffer.data(), requested);
    if (input.gcount() != requested) {
      errorMessage = "schema preflight source changed while copying";
      return false;
    }
    if (!output.write(buffer.data(), requested)) {
      errorMessage = "could not write schema preflight copy";
      return false;
    }
    remaining -= static_cast<std::uintmax_t>(requested);
  }

  char extra = 0;
  input.read(&extra, 1);
  if (input.gcount() != 0 || input.bad()) {
    errorMessage = "schema preflight source grew while copying";
    return false;
  }
  output.close();
  if (!output) {
    errorMessage = "could not finish schema preflight copy";
    return false;
  }
  return true;
}

inline bool sqliteFilesHaveEqualBytes(const std::filesystem::path &left,
                                      const std::filesystem::path &right,
                                      std::uintmax_t expectedBytes,
                                      std::string &errorMessage) {
  std::ifstream leftInput(left, std::ios::binary);
  std::ifstream rightInput(right, std::ios::binary);
  if (!leftInput || !rightInput) {
    errorMessage = "could not compare schema preflight files";
    return false;
  }
  std::array<char, 64 * 1024> leftBytes{};
  std::array<char, 64 * 1024> rightBytes{};
  std::uintmax_t remaining = expectedBytes;
  while (remaining > 0) {
    const auto requested = static_cast<std::streamsize>(
        std::min<std::uintmax_t>(remaining, leftBytes.size()));
    leftInput.read(leftBytes.data(), requested);
    rightInput.read(rightBytes.data(), requested);
    if (leftInput.gcount() != requested || rightInput.gcount() != requested ||
        !std::equal(leftBytes.begin(), leftBytes.begin() + requested,
                    rightBytes.begin())) {
      errorMessage = "database family changed while comparing snapshot";
      return false;
    }
    remaining -= static_cast<std::uintmax_t>(requested);
  }

  char leftExtra = 0;
  char rightExtra = 0;
  leftInput.read(&leftExtra, 1);
  rightInput.read(&rightExtra, 1);
  if (leftInput.gcount() != 0 || rightInput.gcount() != 0 || leftInput.bad() ||
      rightInput.bad()) {
    errorMessage = "schema preflight file size changed while comparing";
    return false;
  }
  return true;
}

inline std::optional<int> readSqliteUserVersion(sqlite3 *db,
                                                std::string &errorMessage) {
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(db, "PRAGMA user_version", stmt) != SQLITE_OK) {
    errorMessage = sqliteDatabaseError(db);
    return std::nullopt;
  }
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    errorMessage = sqliteDatabaseError(db);
    return std::nullopt;
  }
  const int version = sqlite3_column_int(stmt.get(), 0);
  if (version < 0) {
    errorMessage = "database schema version is negative";
    return std::nullopt;
  }
  return version;
}

inline bool validateSqliteSchemaUsability(sqlite3 *db,
                                          std::string &errorMessage) {
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(db, "SELECT count(*) FROM sqlite_schema", stmt) !=
      SQLITE_OK) {
    errorMessage = sqliteDatabaseError(db);
    return false;
  }
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    errorMessage = sqliteDatabaseError(db);
    return false;
  }
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    errorMessage = sqliteDatabaseError(db);
    return false;
  }
  return true;
}

inline std::optional<int> readSqliteUserVersionFromIsolatedSnapshot(
    const std::filesystem::path &path,
    const SqliteDatabaseFamilyState &expectedFamily,
    std::string &errorMessage) {
  const bool hasWal = expectedFamily[2].exists;
  const std::uintmax_t walBytes = hasWal ? expectedFamily[2].size : 0;
  if (!sqliteSnapshotFitsBudget(expectedFamily[0].size, walBytes)) {
    errorMessage = "database family exceeds schema preflight snapshot budget";
    return std::nullopt;
  }

  std::error_code temporaryRootError;
  const std::filesystem::path temporaryRoot =
      std::filesystem::temp_directory_path(temporaryRootError);
  if (temporaryRootError) {
    errorMessage =
        "could not locate temporary directory: " + temporaryRootError.message();
    return std::nullopt;
  }

  std::filesystem::path snapshotDirectory;
  for (int attempt = 0; attempt < 32 && snapshotDirectory.empty(); ++attempt) {
    std::uint64_t randomValue = 0;
    sqlite3_randomness(sizeof(randomValue), &randomValue);
    const auto candidate = temporaryRoot / ("asobmashow-sqlite-preflight-" +
                                            std::to_string(randomValue));
    std::error_code createError;
    if (std::filesystem::create_directory(candidate, createError)) {
      std::error_code permissionsError;
      std::filesystem::permissions(candidate, std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::replace,
                                   permissionsError);
      if (permissionsError) {
        std::error_code ignored;
        std::filesystem::remove(candidate, ignored);
        errorMessage = "could not make schema preflight directory private: " +
                       permissionsError.message();
        return std::nullopt;
      }
      snapshotDirectory = candidate;
    } else if (createError && createError != std::errc::file_exists) {
      errorMessage = "could not create schema preflight directory: " +
                     createError.message();
      return std::nullopt;
    }
  }
  if (snapshotDirectory.empty()) {
    errorMessage = "could not allocate a unique schema preflight directory";
    return std::nullopt;
  }
  struct SnapshotDirectoryCleanup {
    std::filesystem::path path;
    ~SnapshotDirectoryCleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{snapshotDirectory};

  const std::filesystem::path snapshotPath =
      snapshotDirectory / path.filename();
  if (!copySqliteFileExactly(path, snapshotPath, expectedFamily[0].size,
                             errorMessage)) {
    return std::nullopt;
  }

  std::filesystem::path walPath = path;
  walPath += "-wal";
  std::filesystem::path snapshotWalPath = snapshotPath;
  snapshotWalPath += "-wal";
  if (hasWal) {
    if (!copySqliteFileExactly(walPath, snapshotWalPath, walBytes,
                               errorMessage) ||
        !sqliteFilesHaveEqualBytes(walPath, snapshotWalPath, walBytes,
                                   errorMessage)) {
      return std::nullopt;
    }
  }
  if (!sqliteFilesHaveEqualBytes(path, snapshotPath, expectedFamily[0].size,
                                 errorMessage)) {
    return std::nullopt;
  }

  sqlite3 *rawDb = nullptr;
  const std::string pathText = fspath_to_utf8(snapshotPath);
  const int openRc = sqlite3_open_v2(
      pathText.c_str(), &rawDb,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_PRIVATECACHE, nullptr);
  SqliteConnectionHandle db(rawDb);
  if (openRc != SQLITE_OK || !db) {
    errorMessage = rawDb != nullptr ? sqlite3_errmsg(rawDb)
                                    : "could not open SQLite schema snapshot";
    return std::nullopt;
  }
  int noCheckpointOnClose = 0;
  if (sqlite3_db_config(db.get(), SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1,
                        &noCheckpointOnClose) != SQLITE_OK ||
      noCheckpointOnClose != 1) {
    errorMessage = "could not disable snapshot checkpoint-on-close";
    return std::nullopt;
  }

  const auto version = readSqliteUserVersion(db.get(), errorMessage);
  if (!version.has_value() ||
      !validateSqliteSchemaUsability(db.get(), errorMessage)) {
    return std::nullopt;
  }
  db.reset();

  if (!sqliteFilesHaveEqualBytes(path, snapshotPath, expectedFamily[0].size,
                                 errorMessage)) {
    return std::nullopt;
  }
  if (hasWal) {
    if (!sqliteFilesHaveEqualBytes(walPath, snapshotWalPath, walBytes,
                                   errorMessage)) {
      return std::nullopt;
    }
  }

  auto afterSnapshot = readSqliteDatabaseFamilyState(path, errorMessage);
  if (!afterSnapshot.has_value() || *afterSnapshot != expectedFamily) {
    if (errorMessage.empty()) {
      errorMessage = "database family changed during schema snapshot";
    }
    return std::nullopt;
  }
  return *version;
}

// Determines the WAL-visible schema version without opening the original
// database read-write. Existing databases are queried through a bounded
// isolated family. Rollback-journal recovery is intentionally ambiguous and
// fails closed.
inline std::optional<int>
preflightSqliteUserVersion(const std::filesystem::path &path,
                           int maximumSupportedVersion,
                           std::string &errorMessage) {
  errorMessage.clear();
  auto before = readSqliteDatabaseFamilyState(path, errorMessage);
  if (!before.has_value()) {
    return std::nullopt;
  }
  if (!(*before)[0].exists) {
    if ((*before)[1].exists || (*before)[2].exists || (*before)[3].exists) {
      errorMessage = "database sidecar exists without a main database";
      return std::nullopt;
    }
    return 0;
  }
  if ((*before)[0].size == 0) {
    if ((*before)[1].exists || (*before)[2].exists || (*before)[3].exists) {
      errorMessage = "database sidecar exists beside an empty main database";
      return std::nullopt;
    }
    return 0;
  }
  if ((*before)[1].exists) {
    errorMessage = "database has a rollback journal requiring recovery";
    return std::nullopt;
  }
  if (!(*before)[2].exists) {
    const auto first =
        readRawSqliteUserVersion(path, (*before)[0].size, errorMessage);
    if (!first.has_value()) {
      return std::nullopt;
    }
    const auto second =
        readRawSqliteUserVersion(path, (*before)[0].size, errorMessage);
    if (!second.has_value() || *first != *second) {
      errorMessage = "database header changed during schema preflight";
      return std::nullopt;
    }
    if (*first < 0) {
      errorMessage = "database schema version is negative";
      return std::nullopt;
    }
    if (*first > maximumSupportedVersion) {
      errorMessage = "database schema version " + std::to_string(*first) +
                     " is newer than supported version " +
                     std::to_string(maximumSupportedVersion);
      return std::nullopt;
    }
  } else if (!readRawSqliteUserVersion(path, (*before)[0].size, errorMessage)
                  .has_value()) {
    return std::nullopt;
  }

  const auto version =
      readSqliteUserVersionFromIsolatedSnapshot(path, *before, errorMessage);
  if (!version.has_value()) {
    return std::nullopt;
  }

  auto after = readSqliteDatabaseFamilyState(path, errorMessage);
  if (!after.has_value()) {
    return std::nullopt;
  }
  if (*before != *after) {
    errorMessage = "database family changed during schema preflight";
    return std::nullopt;
  }
  if (*version > maximumSupportedVersion) {
    errorMessage = "database schema version " + std::to_string(*version) +
                   " is newer than supported version " +
                   std::to_string(maximumSupportedVersion);
    return std::nullopt;
  }
  return version;
}

struct SqliteValidatedOpenHooks {
  void *context = nullptr;
  void (*afterSnapshotValidated)(void *) = nullptr;
  bool (*configureNoCheckpoint)(sqlite3 *, void *, std::string &) = nullptr;
};

inline bool configureSqliteNoCheckpoint(sqlite3 *db,
                                        const SqliteValidatedOpenHooks &hooks,
                                        std::string &errorMessage) {
  if (hooks.configureNoCheckpoint != nullptr) {
    return hooks.configureNoCheckpoint(db, hooks.context, errorMessage);
  }
  int enabled = 0;
  if (sqlite3_db_config(db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, &enabled) !=
          SQLITE_OK ||
      enabled != 1) {
    errorMessage = "could not disable checkpoint-on-close";
    return false;
  }
  return true;
}

inline bool setSqliteJournalModeWal(sqlite3 *db, std::string &errorMessage) {
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(db, "PRAGMA journal_mode=WAL", stmt) !=
      SQLITE_OK) {
    errorMessage = sqliteDatabaseError(db);
    return false;
  }
  if (sqlite3_step(stmt.get()) != SQLITE_ROW ||
      sqliteColumnString(stmt.get(), 0) != "wal") {
    errorMessage = sqliteDatabaseError(db);
    if (errorMessage == "not an error") {
      errorMessage = "SQLite did not enter WAL journal mode";
    }
    return false;
  }
  return true;
}

inline sqlite3 *
openValidatedSqliteDatabase(const std::filesystem::path &path,
                            int maximumSupportedVersion, bool enableForeignKeys,
                            std::string &errorMessage,
                            const SqliteValidatedOpenHooks &hooks = {}) {
  const auto snapshotVersion =
      preflightSqliteUserVersion(path, maximumSupportedVersion, errorMessage);
  if (!snapshotVersion.has_value()) {
    return nullptr;
  }
  if (hooks.afterSnapshotValidated != nullptr) {
    hooks.afterSnapshotValidated(hooks.context);
  }

  const std::string pathText = fspath_to_utf8(path);
  sqlite3 *rawGuard = nullptr;
  const int guardOpenRc = sqlite3_open_v2(
      pathText.c_str(), &rawGuard,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_PRIVATECACHE,
      nullptr);
  SqliteConnectionHandle guard(rawGuard);
  if (guardOpenRc != SQLITE_OK || !guard) {
    errorMessage = rawGuard != nullptr ? sqlite3_errmsg(rawGuard)
                                       : "could not open database guard";
    return nullptr;
  }
  sqlite3_busy_timeout(guard.get(), 0);
  if (!configureSqliteNoCheckpoint(guard.get(), hooks, errorMessage)) {
    return nullptr;
  }
  if (const auto lockError = executeSqlite(
          guard.get(), "PRAGMA locking_mode=EXCLUSIVE; BEGIN EXCLUSIVE")) {
    const int primaryError = sqlite3_extended_errcode(guard.get()) & 0xff;
    errorMessage = primaryError == SQLITE_BUSY || primaryError == SQLITE_LOCKED
                       ? "database has an active writer"
                       : *lockError;
    return nullptr;
  }

  const auto ownedVersion = readSqliteUserVersion(guard.get(), errorMessage);
  if (!ownedVersion.has_value() || *ownedVersion > maximumSupportedVersion) {
    if (ownedVersion.has_value()) {
      errorMessage = "database schema version " +
                     std::to_string(*ownedVersion) +
                     " is newer than supported version " +
                     std::to_string(maximumSupportedVersion);
    }
    executeSqlite(guard.get(), "ROLLBACK");
    return nullptr;
  }
  if (*ownedVersion != *snapshotVersion) {
    errorMessage = "database schema version changed while acquiring ownership";
    executeSqlite(guard.get(), "ROLLBACK");
    return nullptr;
  }
  if (const auto rollbackError = executeSqlite(guard.get(), "ROLLBACK")) {
    errorMessage =
        "could not finish database ownership check: " + *rollbackError;
    return nullptr;
  }
  if (!setSqliteJournalModeWal(guard.get(), errorMessage)) {
    return nullptr;
  }

  sqlite3 *rawProduction = nullptr;
  const int productionOpenRc = sqlite3_open_v2(
      pathText.c_str(), &rawProduction,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_PRIVATECACHE,
      nullptr);
  SqliteConnectionHandle production(rawProduction);
  if (productionOpenRc != SQLITE_OK || !production) {
    errorMessage = rawProduction != nullptr ? sqlite3_errmsg(rawProduction)
                                            : "could not open database";
    return nullptr;
  }
  int noCheckpointOnClose = 0;
  if (sqlite3_db_config(production.get(), SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1,
                        &noCheckpointOnClose) != SQLITE_OK ||
      noCheckpointOnClose != 1) {
    errorMessage = "could not configure production checkpoint-on-close";
    return nullptr;
  }
  if (enableForeignKeys) {
    int foreignKeysEnabled = 0;
    if (sqlite3_db_config(production.get(), SQLITE_DBCONFIG_ENABLE_FKEY, 1,
                          &foreignKeysEnabled) != SQLITE_OK ||
        foreignKeysEnabled != 1) {
      errorMessage = "could not enable foreign keys";
      return nullptr;
    }
  }
  sqlite3_busy_timeout(production.get(), 1000);
  guard.reset();
  return production.release();
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
                          std::string &errorMessage,
                          const char *commitQuery = "COMMIT",
                          const char *rollbackQuery = "ROLLBACK")
      : db_(db), commitQuery_(commitQuery), rollbackQuery_(rollbackQuery) {
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
  SqliteTransactionHandle &operator=(const SqliteTransactionHandle &) = delete;

  ~SqliteTransactionHandle() {
    if (active_) {
      sqlite3_exec(db_, rollbackQuery_.c_str(), nullptr, nullptr, nullptr);
    }
  }

  bool active() const { return active_; }

  bool commit(std::string &errorMessage) {
    if (!active_) {
      errorMessage = "transaction is not active";
      return false;
    }
    if (const auto error = executeSqlite(db_, commitQuery_.c_str())) {
      errorMessage = *error;
      return false;
    }
    active_ = false;
    return true;
  }

private:
  sqlite3 *db_ = nullptr;
  std::string commitQuery_;
  std::string rollbackQuery_;
  bool active_ = false;
};
