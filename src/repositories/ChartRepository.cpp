// Fill out your copyright notice in the Description page of Project Settings.

#include "ChartRepository.h"
#include "ChartRepositoryInternal.h"
#include "ChartSqlExpressions.h"
#include "ChartStorageIdentity.h"
#include "../LongNoteModeUtils.h"
#include "ScoreRepository.h"
#include "ScoreCacheQueries.h"
#include "SqliteRAII.h"
#include "../Utils.h"
#include <SDL2/SDL.h>
#include "../path.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <utility>
#include "../targets.h"
#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#endif

namespace {
using asobmshow::chart_sql::normalizedSqlHash;
constexpr int kChartDatabaseSchemaVersion = 3;

std::string columnString(sqlite3_stmt *stmt, int idx);

path_t readStoredPath(sqlite3_stmt *stmt, int idx) {
#ifdef _WIN32
  if (sqlite3_column_type(stmt, idx) == SQLITE_NULL) {
    return {};
  }
  const int size = sqlite3_column_bytes(stmt, idx);
  const auto utf8 = std::string(
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx)), size);
  return utf8_to_path_t(utf8);
#else
  const auto *text =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx));
  return text != nullptr ? path_t(text) : path_t();
#endif
}

bool pathIsInsideDirectory(const std::filesystem::path &path,
                           const std::filesystem::path &directory) {
  if (path.empty() || directory.empty()) {
    return false;
  }

  const std::filesystem::path normalizedPath = path.lexically_normal();
  const std::filesystem::path normalizedDirectory =
      directory.lexically_normal();
  if (normalizedPath == normalizedDirectory) {
    return false;
  }

  const std::filesystem::path relative =
      normalizedPath.lexically_relative(normalizedDirectory);
  if (relative.empty() || relative.is_absolute()) {
    return false;
  }

  const auto first = relative.begin();
  if (first == relative.end()) {
    return false;
  }
  return *first != std::filesystem::path("..") &&
         *first != std::filesystem::path(".");
}

void logSqlErrorText(const char *context, const std::string &error) {
  std::cerr << "SQL error while " << context << ": " << error << "\n";
}

void logSqlError(const char *context, sqlite3 *db) {
  logSqlErrorText(context, sqliteDatabaseError(db));
}

void logSdlSqlErrorText(const char *context, const std::string &error) {
  SDL_Log("SQL error while %s: %s", context, error.c_str());
}

void logSdlSqlError(const char *context, sqlite3 *db) {
  logSdlSqlErrorText(context, sqliteDatabaseError(db));
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
  const int rc = prepareSqliteStatement(db, "PRAGMA user_version", stmt);
  if (rc != SQLITE_OK) {
    logSqlError("reading chart database version", db);
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
  return execSql(db, query.c_str(), "updating chart database version");
}

bool sqliteTableExists(sqlite3 *db, const char *tableName, bool &exists,
                       const char *context) {
  if (const auto error = querySqliteTableExists(db, tableName, exists)) {
    logSqlErrorText(context, *error);
    return false;
  }
  return true;
}

bool createChartMetaTableSchema(sqlite3 *db) {
  const char *query =
      "CREATE TABLE IF NOT EXISTS chart_meta ("
      "path       TEXT primary key,"
      "md5        TEXT not null,"
      "sha256     TEXT not null,"
      "title      TEXT,"
      "subtitle   TEXT,"
      "genre      TEXT,"
      "artist     TEXT,"
      "sub_artist  TEXT,"
      "folder     TEXT,"
      "stage_file  TEXT,"
      "banner     TEXT,"
      "back_bmp    TEXT,"
      "preview    TEXT,"
      "level      REAL,"
      "difficulty INTEGER,"
      "total     REAL,"
      "has_total INTEGER NOT NULL DEFAULT 0,"
      "bpm       REAL,"
      "max_bpm     REAL,"
      "min_bpm     REAL,"
      "length     INTEGER,"
      "rank      INTEGER,"
      "player    INTEGER,"
      "keys     INTEGER,"
      "total_notes INTEGER,"
      "total_long_notes INTEGER,"
      "total_scratch_notes INTEGER,"
      "total_backspin_notes INTEGER,"
      "ln_mode INTEGER NOT NULL DEFAULT 0,"
      "source_priority INTEGER,"
      "source_archive_size INTEGER"
      ")";
  return execSql(db, query, "creating chart meta table");
}

std::optional<std::string> normalizedPathTextForStorage(
    const std::string &original) {
  if (original.empty()) {
    return std::nullopt;
  }

  std::filesystem::path path(utf8_to_path_t(original));
  if (path.empty()) {
    return std::nullopt;
  }
  chart_storage_identity::ToAbsolutePath(path);
  chart_storage_identity::ToRelativePath(path);
  path = path.lexically_normal();

  const std::string normalized = fspath_to_utf8(path);
  if (normalized == original) {
    return std::nullopt;
  }
  return normalized;
}

bool normalizedPathValueExists(sqlite3_stmt *stmt,
                               const std::string &normalized) {
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  bindSqliteText(stmt, 1, normalized);
  const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
  return exists;
}

bool normalizeStoredPathColumn(sqlite3 *db, const char *table,
                               const char *column, bool primaryKey) {
  struct PendingNormalization {
    sqlite3_int64 rowid = 0;
    std::string normalized;
  };

  std::string selectQuery = "SELECT rowid, ";
  selectQuery += column;
  selectQuery += " FROM ";
  selectQuery += table;
  selectQuery += " WHERE ";
  selectQuery += column;
  selectQuery += " IS NOT NULL AND ";
  selectQuery += column;
  selectQuery += " != ''";

  SqliteStatementHandle selectStmt;
  int rc = prepareSqliteStatement(db, selectQuery, selectStmt);
  if (rc != SQLITE_OK) {
    return false;
  }

  std::vector<PendingNormalization> pending;
  while (sqlite3_step(selectStmt.get()) == SQLITE_ROW) {
    const sqlite3_int64 rowid = sqlite3_column_int64(selectStmt.get(), 0);
    const auto *text = reinterpret_cast<const char *>(
        sqlite3_column_text(selectStmt.get(), 1));
    if (text == nullptr) {
      continue;
    }

    const auto normalized = normalizedPathTextForStorage(text);
    if (normalized.has_value()) {
      pending.push_back({.rowid = rowid, .normalized = *normalized});
    }
  }
  if (pending.empty()) {
    return false;
  }

  std::string updateQuery =
      std::string(primaryKey ? "UPDATE OR IGNORE " : "UPDATE ") + table +
      " SET " + column + " = ? WHERE rowid = ?";
  SqliteStatementHandle updateStmt;
  rc = prepareSqliteStatement(db, updateQuery, updateStmt);
  if (rc != SQLITE_OK) {
    return false;
  }

  SqliteStatementHandle existsStmt;
  SqliteStatementHandle deleteStmt;
  if (primaryKey) {
    std::string existsQuery = "SELECT 1 FROM ";
    existsQuery += table;
    existsQuery += " WHERE ";
    existsQuery += column;
    existsQuery += " = ? LIMIT 1";
    rc = prepareSqliteStatement(db, existsQuery, existsStmt);
    if (rc != SQLITE_OK) {
      return false;
    }

    std::string deleteQuery = "DELETE FROM ";
    deleteQuery += table;
    deleteQuery += " WHERE rowid = ?";
    rc = prepareSqliteStatement(db, deleteQuery, deleteStmt);
    if (rc != SQLITE_OK) {
      return false;
    }
  }

  bool changed = false;
  for (const auto &item : pending) {
    sqlite3_reset(updateStmt.get());
    sqlite3_clear_bindings(updateStmt.get());
    bindSqliteText(updateStmt.get(), 1, item.normalized);
    sqlite3_bind_int64(updateStmt.get(), 2, item.rowid);
    rc = sqlite3_step(updateStmt.get());
    if (rc == SQLITE_DONE && sqlite3_changes(db) > 0) {
      changed = true;
      continue;
    }

    if (!primaryKey ||
        !normalizedPathValueExists(existsStmt.get(), item.normalized)) {
      continue;
    }
    sqlite3_reset(deleteStmt.get());
    sqlite3_clear_bindings(deleteStmt.get());
    sqlite3_bind_int64(deleteStmt.get(), 1, item.rowid);
    if (sqlite3_step(deleteStmt.get()) == SQLITE_DONE &&
        sqlite3_changes(db) > 0) {
      changed = true;
    }
  }

  if (changed) {
    SDL_Log("Normalized stored app document paths in %s.%s", table, column);
  }
  return changed;
}

bool normalizeStoredHashColumnChecked(sqlite3 *db, const char *table,
                                      const char *column, bool &changed) {
  int changedCount = 0;
  if (!updateSqliteColumnWithExpressionLogged(
          db, table, column, normalizedSqlHash(column),
          "normalizing stored chart hash column", logSqlErrorText,
          &changedCount)) {
    return false;
  }

  if (changedCount > 0) {
    SDL_Log("Normalized %d stored chart hashes in %s.%s", changedCount, table,
            column);
    changed = true;
  }
  return true;
}

sqlite3_int64 clampSqlInteger(std::uint64_t value) {
  return value > static_cast<std::uint64_t>(
                     std::numeric_limits<sqlite3_int64>::max())
             ? std::numeric_limits<sqlite3_int64>::max()
             : static_cast<sqlite3_int64>(value);
}

bool createChartMetadataRebuildStateTable(sqlite3 *db) {
  const char *query =
      "CREATE TABLE IF NOT EXISTS chart_meta_rebuild_state ("
      "id INTEGER PRIMARY KEY CHECK(id = 1),"
      "required INTEGER NOT NULL DEFAULT 0,"
      "updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
      ")";
  return execSql(db, query, "creating chart metadata rebuild state table");
}

bool setChartMetadataRebuildRequired(sqlite3 *db, bool required) {
  if (!createChartMetadataRebuildStateTable(db)) {
    return false;
  }
  const char *query =
      "INSERT INTO chart_meta_rebuild_state (id, required, updated_at) "
      "VALUES (1, ?, CURRENT_TIMESTAMP) "
      "ON CONFLICT(id) DO UPDATE SET required = excluded.required, "
      "updated_at = CURRENT_TIMESTAMP";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing chart metadata rebuild state",
                                    logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(stmt.get(), 1, required ? 1 : 0);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    logSqlError("updating chart metadata rebuild state", db);
    return false;
  }
  return true;
}

bool clearChartMetadataRebuildRequiredIfPresent(sqlite3 *db) {
  bool tableExists = false;
  if (!sqliteTableExists(db, "chart_meta_rebuild_state", tableExists,
                         "checking chart metadata rebuild state table")) {
    return false;
  }
  if (!tableExists) {
    return true;
  }
  return setChartMetadataRebuildRequired(db, false);
}

bool invalidateChartMetadataForNormalScan(sqlite3 *db, bool &completed) {
  completed = false;
  if (!execSql(db, "SAVEPOINT chart_metadata_rebuild_migration",
               "starting chart metadata rebuild migration")) {
    return false;
  }

  bool ok = true;
  const char *queries[] = {
      "DROP TABLE IF EXISTS chart_meta",
      "DROP TABLE IF EXISTS solid_archives",
      "DROP TABLE IF EXISTS archive_scan_cache",
      "DROP TABLE IF EXISTS chart_scan_checkpoint",
  };
  for (const auto *query : queries) {
    if (!execSql(db, query, "invalidating chart metadata cache")) {
      ok = false;
      break;
    }
  }
  if (ok) {
    ok = createChartMetaTableSchema(db);
  }
  if (ok) {
    ok = setChartMetadataRebuildRequired(db, true);
  }

  if (ok) {
    ok = execSql(db, "RELEASE chart_metadata_rebuild_migration",
                 "committing chart metadata rebuild migration");
  } else {
    execSql(db, "ROLLBACK TO chart_metadata_rebuild_migration",
            "rolling back chart metadata rebuild migration");
    execSql(db, "RELEASE chart_metadata_rebuild_migration",
            "releasing chart metadata rebuild migration");
    return false;
  }

  SDL_Log("Chart metadata cache invalidated; normal library scan will rebuild");
  chart_repository_detail::BumpLibraryRevision();
  completed = true;
  return true;
}

class ChartDatabaseMigrationPass {
public:
  using RunFunction = bool (*)(sqlite3 *, bool &completed);

  constexpr ChartDatabaseMigrationPass(int targetVersion, const char *name,
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

bool migrateChartDatabaseToVersion1(sqlite3 *db, bool &completed) {
  return invalidateChartMetadataForNormalScan(db, completed);
}

bool normalizeExistingChartTablePaths(sqlite3 *db, const char *table,
                                      const char *column, bool primaryKey,
                                      bool &changed) {
  bool exists = false;
  if (!sqliteTableExists(db, table, exists,
                         "checking chart path normalization table")) {
    return false;
  }
  if (!exists) {
    return true;
  }
  changed = normalizeStoredPathColumn(db, table, column, primaryKey) || changed;
  return true;
}

bool normalizeExistingChartTableHashes(sqlite3 *db, const char *table,
                                       const char *md5Column,
                                       const char *sha256Column,
                                       bool &changed) {
  bool exists = false;
  if (!sqliteTableExists(db, table, exists,
                         "checking chart hash normalization table")) {
    return false;
  }
  if (!exists) {
    return true;
  }
  return normalizeStoredHashColumnChecked(db, table, md5Column, changed) &&
         normalizeStoredHashColumnChecked(db, table, sha256Column, changed);
}

bool migrateChartDatabaseToVersion2(sqlite3 *db, bool &completed) {
  bool changed = false;
  if (!normalizeExistingChartTablePaths(db, "chart_meta", "path", true,
                                        changed) ||
      !normalizeExistingChartTablePaths(db, "chart_meta", "folder", false,
                                        changed) ||
      !normalizeExistingChartTablePaths(db, "chart_favorites", "chart_path",
                                        true, changed) ||
      !normalizeExistingChartTablePaths(db, "solid_archives", "path", true,
                                        changed) ||
      !normalizeExistingChartTablePaths(db, "entries", "path", true,
                                        changed) ||
      !normalizeExistingChartTablePaths(db, "chart_scan_checkpoint",
                                        "last_path", false, changed) ||
      !normalizeExistingChartTablePaths(db, "chart_scan_checkpoint",
                                        "archive_path", false, changed) ||
      !normalizeExistingChartTablePaths(db, "archive_scan_cache", "path",
                                        true, changed) ||
      !normalizeExistingChartTableHashes(db, "chart_meta", "md5", "sha256",
                                         changed) ||
      !normalizeExistingChartTableHashes(db, "chart_favorites", "chart_md5",
                                         "chart_sha256", changed) ||
      !normalizeExistingChartTableHashes(db, "difficulty_table_entries", "md5",
                                         "sha256", changed) ||
      !normalizeExistingChartTableHashes(db, "difficulty_course_entries",
                                         "md5", "sha256", changed)) {
    return false;
  }
  if (changed) {
    chart_repository_detail::InvalidateDifficultyLabelCache();
    chart_repository_detail::BumpLibraryRevision();
  }
  completed = true;
  return true;
}

bool migrateChartDatabaseToVersion3(sqlite3 *db, bool &completed) {
  return invalidateChartMetadataForNormalScan(db, completed);
}

bool runChartDatabaseMigrationPasses(
    sqlite3 *db, const ChartDatabaseMigrationPass *passes,
    std::size_t passCount, int latestVersion) {
  int currentVersion = databaseUserVersion(db);
  if (currentVersion >= latestVersion) {
    return true;
  }

  for (std::size_t i = 0; i < passCount; ++i) {
    const ChartDatabaseMigrationPass &pass = passes[i];
    if (currentVersion >= pass.targetVersion()) {
      continue;
    }

    bool completed = false;
    if (!pass.run(db, completed)) {
      std::cerr << "Chart database migration failed for version "
                << pass.targetVersion() << " (" << pass.name() << ")\n";
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
    std::cerr << "No chart database migration pass reached version "
              << latestVersion << "\n";
    return false;
  }
  return true;
}

bool migrateChartDatabaseSchema(sqlite3 *db) {
  static constexpr ChartDatabaseMigrationPass kMigrationPasses[] = {
      {1, "chart metadata rebuild", migrateChartDatabaseToVersion1},
      {2, "normalize chart identity storage", migrateChartDatabaseToVersion2},
      {3, "persist authored TOTAL metadata", migrateChartDatabaseToVersion3},
  };
  return runChartDatabaseMigrationPasses(
      db, kMigrationPasses,
      sizeof(kMigrationPasses) / sizeof(kMigrationPasses[0]),
      kChartDatabaseSchemaVersion);
}

bool createChartScanCheckpointTable(sqlite3 *db) {
  const char *query =
      "CREATE TABLE IF NOT EXISTS chart_scan_checkpoint ("
      "id INTEGER PRIMARY KEY CHECK(id = 1),"
      "scan_signature TEXT NOT NULL DEFAULT '',"
      "phase TEXT NOT NULL DEFAULT '',"
      "next_index INTEGER NOT NULL DEFAULT 0,"
      "sub_index INTEGER NOT NULL DEFAULT 0,"
      "last_path TEXT NOT NULL DEFAULT '',"
      "archive_path TEXT NOT NULL DEFAULT '',"
      "archive_size INTEGER NOT NULL DEFAULT 0,"
      "archive_mtime_ns INTEGER NOT NULL DEFAULT 0,"
      "last_inner_path TEXT NOT NULL DEFAULT '',"
      "updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
      ")";
  if (!execSql(db, query, "creating chart scan checkpoint table")) {
    return false;
  }
  return true;
}

bool clearChartScanCheckpoint(sqlite3 *db) {
  if (!createChartScanCheckpointTable(db)) {
    return false;
  }
  return execSql(db, "DELETE FROM chart_scan_checkpoint",
                 "clearing chart scan checkpoint");
}

bool createArchiveScanCacheTable(sqlite3 *db) {
  const char *query =
      "CREATE TABLE IF NOT EXISTS archive_scan_cache ("
      "path TEXT PRIMARY KEY,"
      "archive_size INTEGER NOT NULL DEFAULT 0,"
      "mtime_ns INTEGER NOT NULL DEFAULT 0,"
      "solid INTEGER NOT NULL DEFAULT 0,"
      "uncompressed_size INTEGER NOT NULL DEFAULT 0,"
      "file_count INTEGER NOT NULL DEFAULT 0,"
      "chart_count INTEGER NOT NULL DEFAULT -1,"
      "updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
      ")";
  if (!execSql(db, query, "creating archive scan cache table")) {
    return false;
  }
  if (!execSqlAllowDuplicateColumn(
          db,
          "ALTER TABLE archive_scan_cache "
          "ADD COLUMN chart_count INTEGER NOT NULL DEFAULT -1",
          "migrating archive scan cache chart count")) {
    return false;
  }

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_archive_scan_cache_state "
      "ON archive_scan_cache(path, archive_size, mtime_ns)",
      "CREATE INDEX IF NOT EXISTS idx_archive_scan_cache_solid "
      "ON archive_scan_cache(solid)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating archive scan cache index")) {
      return false;
    }
  }
  return true;
}

std::vector<std::filesystem::path> selectArchiveScanCachePaths(sqlite3 *db) {
  std::vector<std::filesystem::path> paths;
  SqliteStatementHandle stmt;
  int rc =
      prepareSqliteStatement(db, "SELECT path FROM archive_scan_cache", stmt);
  if (rc != SQLITE_OK) {
    return paths;
  }
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const auto *text =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 0));
    if (text == nullptr) {
      continue;
    }
    std::filesystem::path path(utf8_to_path_t(text));
    chart_storage_identity::ToAbsolutePath(path);
    paths.push_back(path);
  }
  return paths;
}

bool deleteArchiveScanCache(sqlite3 *db,
                            const std::filesystem::path &archivePath) {
  const std::string pathText =
      chart_storage_identity::StoredPathText(archivePath);
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, "DELETE FROM archive_scan_cache WHERE path = ?", stmt,
          "preparing archive scan cache delete", logSqlErrorText)) {
    return false;
  }
  bindSqliteText(stmt.get(), 1, pathText);
  int rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    logSqlError("deleting archive scan cache", db);
    return false;
  }
  const bool changed = sqlite3_changes(db) > 0;
  return changed;
}

bool deleteSolidArchive(sqlite3 *db, const std::filesystem::path &archivePath) {
  const std::string pathText =
      chart_storage_identity::StoredPathText(archivePath);
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, "DELETE FROM solid_archives WHERE path = ?", stmt,
          "preparing solid archive delete", logSqlErrorText)) {
    return false;
  }
  bindSqliteText(stmt.get(), 1, pathText);
  int rc = sqlite3_step(stmt.get());
  if (rc != SQLITE_DONE) {
    logSqlError("deleting solid archive", db);
    return false;
  }
  const bool changed = sqlite3_changes(db) > 0;
  return changed;
}

bool deleteChartMetaInArchive(sqlite3 *db,
                              const std::filesystem::path &archivePath) {
  std::vector<std::filesystem::path> chartPaths;
  SqliteStatementHandle selectStmt;
  if (!prepareSqliteStatementLogged(db, "SELECT path FROM chart_meta",
                                    selectStmt,
                                    "selecting archive chart paths",
                                    logSqlErrorText)) {
    return false;
  }
  while (sqlite3_step(selectStmt.get()) == SQLITE_ROW) {
    std::filesystem::path path(utf8_to_path_t(
        sqliteColumnString(selectStmt.get(), 0)));
    chart_storage_identity::ToAbsolutePath(path);
    chartPaths.push_back(std::move(path));
  }

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, "DELETE FROM chart_meta WHERE path = ?", stmt,
          "preparing archive chart delete", logSqlErrorText)) {
    return false;
  }

  bool changed = false;
  const std::filesystem::path targetArchive = archivePath.lexically_normal();
  for (const auto &path : chartPaths) {
    if (!pathIsInsideDirectory(path, targetArchive)) {
      continue;
    }

    const std::string pathText = chart_storage_identity::StoredPathText(path);
    sqlite3_reset(stmt.get());
    sqlite3_clear_bindings(stmt.get());
    bindSqliteText(stmt.get(), 1, pathText);
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
      logSqlError("deleting archive chart", db);
      return changed;
    }
    changed = sqlite3_changes(db) > 0 || changed;
  }
  return changed;
}

std::vector<std::filesystem::path> selectSolidArchivePaths(sqlite3 *db) {
  std::vector<std::filesystem::path> paths;
  SqliteStatementHandle stmt;
  int rc = prepareSqliteStatement(db, "SELECT path FROM solid_archives", stmt);
  if (rc != SQLITE_OK) {
    return paths;
  }
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const auto *text =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 0));
    if (text == nullptr) {
      continue;
    }
    std::filesystem::path path(utf8_to_path_t(text));
    chart_storage_identity::ToAbsolutePath(path);
    paths.push_back(path);
  }
  return paths;
}

int columnInt(sqlite3_stmt *stmt, int idx) {
  return sqlite3_column_type(stmt, idx) == SQLITE_NULL
             ? 0
             : sqlite3_column_int(stmt, idx);
}

std::string columnString(sqlite3_stmt *stmt, int idx) {
  return sqliteColumnString(stmt, idx);
}

} // namespace

ChartRepository::Impl::Impl(std::filesystem::path path)
    : databasePath(std::move(path)) {}

ChartSessionStorage::ChartSessionStorage(sqlite3 *database)
    : connection(database) {}

sqlite3 *ChartSessionStorage::database() const { return connection.get(); }

ChartRepository::Session::Impl::Impl(
    ChartRepository &owner, sqlite3 *database, ScoreRepository *scoresValue)
    : repository(&owner), storage(std::make_shared<ChartSessionStorage>(database)),
      scores(scoresValue) {}

ScoreRepository &ChartRepository::Session::Impl::scoreRepository() {
  return scores != nullptr ? *scores : fallbackScores;
}

sqlite3 *ChartRepository::Session::Impl::database() const {
  return storage->database();
}

ChartRepository::Session::Session(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ChartRepository::Session::~Session() = default;
ChartRepository::Session::Session(Session &&) noexcept = default;
ChartRepository::Session &
ChartRepository::Session::operator=(Session &&) noexcept = default;

sqlite3 *ChartRepository::Session::NativeHandleForScoreRepository() const {
  return impl_->database();
}

bool ChartRepository::Session::EnsureSchema() {
  sqlite3 *db = impl_->database();
  return chart_repository_detail::EnsureCoreSchema(db) &&
         chart_repository_detail::EnsureDifficultySchema(db);
}

bool ChartRepository::Session::InsertChartMeta(
    bms_parser::ChartMeta &chartMeta) {
  return impl_->repository->InsertChartMeta(impl_->database(), chartMeta);
}

bool ChartRepository::Session::DeleteChartMeta(std::filesystem::path path) {
  return impl_->repository->DeleteChartMeta(impl_->database(),
                                             std::move(path));
}

int ChartRepository::Session::DeleteChartMetaInDirectory(
    const std::filesystem::path &directory) {
  return impl_->repository->DeleteChartMetaInDirectory(impl_->database(),
                                                        directory);
}

bool ChartRepository::Session::DeleteArchiveRecords(
    const std::filesystem::path &archivePath) {
  return impl_->repository->DeleteArchiveRecords(impl_->database(),
                                                  archivePath);
}

bool ChartRepository::Session::ClearChartMeta() {
  return impl_->repository->ClearChartMeta(impl_->database());
}

bool ChartRepository::Session::InsertEntry(
    const std::filesystem::path &path, const std::string &iosBookmark) {
  return impl_->repository->InsertEntry(impl_->database(), path,
                                        iosBookmark);
}

std::vector<ChartEntry> ChartRepository::Session::SelectAllEntries() {
  return impl_->repository->SelectAllEntries(impl_->database());
}

std::vector<ChartEntry> ChartRepository::Session::SelectEffectiveEntries() {
  return impl_->repository->SelectEffectiveEntries(impl_->database());
}

bool ChartRepository::Session::DeleteEntry(
    const std::filesystem::path &path) {
  return impl_->repository->DeleteEntry(impl_->database(), path);
}

bool ChartRepository::Session::DeleteEntryAndChartMetaInDirectory(
    const std::filesystem::path &path, int &removedChartCount) {
  removedChartCount = -1;
  std::string transactionError;
  SqliteTransactionHandle transaction(impl_->database(), "BEGIN",
                                      transactionError);
  if (!transaction.active()) {
    return false;
  }
  removedChartCount =
      impl_->repository->DeleteChartMetaInDirectory(impl_->database(),
                                                     path);
  if (removedChartCount < 0 ||
      !impl_->repository->DeleteEntry(impl_->database(), path)) {
    return false;
  }
  return transaction.commit(transactionError);
}

bool ChartRepository::Session::ClearEntries() {
  return impl_->repository->ClearEntries(impl_->database());
}

bool ChartRepository::EnsureReady() {
  std::lock_guard lock(impl_->readinessMutex);
  if (impl_->ready) {
    return true;
  }

  const std::filesystem::path directory = impl_->databasePath.parent_path();
  std::cout << "DB Directory: " << fspath_to_utf8(directory) << "\n";
  std::error_code directoryError;
  if (!directory.empty() &&
      !Utils::EnsureDirectoryExists(directory, directoryError)) {
    std::cerr << "Can't create chart database directory "
              << fspath_to_utf8(directory) << ": "
              << directoryError.message() << "\n";
    return false;
  }
  std::cout << "DB Path: " << fspath_to_utf8(impl_->databasePath) << "\n";

  std::string openError;
  SqliteConnectionHandle connection(openValidatedSqliteDatabase(
      impl_->databasePath, kChartDatabaseSchemaVersion,
      SqliteValidatedOpenPolicy{
          .enableForeignKeys = false,
          .disableCheckpointOnClose = false,
      },
      openError));
  if (!connection) {
    std::cerr << "Can't open chart database: " << openError << "\n";
    return false;
  }
  if (const auto pragmaError =
          applySqlitePragmas(connection.get(), {"PRAGMA synchronous=NORMAL"})) {
    std::cerr << "Could not configure chart database: " << *pragmaError
              << "\n";
    return false;
  }

  const bool ok = chart_repository_detail::EnsureCoreSchema(connection.get()) &&
                  chart_repository_detail::EnsureDifficultySchema(
                      connection.get());
  impl_->ready = ok;
  return ok;
}

std::optional<ChartRepository::Session>
ChartRepository::OpenSession(ScoreRepository *scores) {
  if (!EnsureReady()) {
    return std::nullopt;
  }

  sqlite3 *raw = nullptr;
  const std::string pathText = fspath_to_utf8(impl_->databasePath);
  const int openRc = sqlite3_open_v2(
      pathText.c_str(), &raw,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_PRIVATECACHE, nullptr);
  SqliteConnectionHandle connection(raw);
  if (openRc != SQLITE_OK || !connection) {
    std::cerr << "Can't open chart database session: "
              << (raw != nullptr ? sqlite3_errmsg(raw) : "unknown error")
              << "\n";
    return std::nullopt;
  }
  sqlite3_busy_timeout(connection.get(), 1000);

  std::string versionError;
  const auto version = readSqliteUserVersion(connection.get(), versionError);
  if (!version.has_value() || *version != kChartDatabaseSchemaVersion) {
    std::cerr << "Can't use chart database session: "
              << (versionError.empty() ? "unexpected schema version"
                                       : versionError)
              << "\n";
    return std::nullopt;
  }
  if (const auto pragmaError = applySqlitePragmas(
          connection.get(),
          {"PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL"})) {
    std::cerr << "Could not configure chart database session: "
              << *pragmaError << "\n";
  }

  sqlite3 *database = connection.release();
  return Session(std::make_unique<Session::Impl>(*this, database, scores));
}

const std::filesystem::path &ChartRepository::DatabasePath() const {
  return impl_->databasePath;
}

static bool createFavoritesTable(sqlite3 *db);

static bool createChartMetaTable(sqlite3 *db) {
  bool existingChartMetaTable = false;
  if (!sqliteTableExists(db, "chart_meta", existingChartMetaTable,
                         "checking chart meta table existence")) {
    return false;
  }

  if (!createChartMetaTableSchema(db)) {
    return false;
  }

  if (existingChartMetaTable) {
    if (!migrateChartDatabaseSchema(db)) {
      return false;
    }
  } else if (!setDatabaseUserVersion(db, kChartDatabaseSchemaVersion)) {
    return false;
  }

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_path ON chart_meta(path)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_folder ON chart_meta(folder)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_folder_source "
      "ON chart_meta(folder, source_priority, source_archive_size, path)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_title ON chart_meta(title)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_title_path "
      "ON chart_meta(title, path)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_sha256 ON chart_meta(sha256)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_md5 ON chart_meta(md5)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_sha256_source "
      "ON chart_meta(sha256, source_priority, source_archive_size, path)",
      "CREATE INDEX IF NOT EXISTS idx_chart_meta_md5_source "
      "ON chart_meta(md5, source_priority, source_archive_size, path)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating chart meta index")) {
      return false;
    }
  }
  return createFavoritesTable(db);
}

static bool createFavoritesTable(sqlite3 *db) {
  const char *query =
      "CREATE TABLE IF NOT EXISTS chart_favorites ("
      "chart_path TEXT PRIMARY KEY,"
      "chart_md5 TEXT NOT NULL DEFAULT '',"
      "chart_sha256 TEXT NOT NULL DEFAULT '',"
      "added_at TEXT DEFAULT CURRENT_TIMESTAMP"
      ")";
  if (!execSql(db, query, "creating chart favorite table")) {
    return false;
  }

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_chart_favorites_added_at "
      "ON chart_favorites(added_at)",
      "CREATE INDEX IF NOT EXISTS idx_chart_favorites_md5 "
      "ON chart_favorites(chart_md5)",
      "CREATE INDEX IF NOT EXISTS idx_chart_favorites_sha256 "
      "ON chart_favorites(chart_sha256)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating chart favorite index")) {
      return false;
    }
  }

  return migrateChartDatabaseSchema(db);
}

static bool createSolidArchiveTable(sqlite3 *db) {
  const char *query =
      "CREATE TABLE IF NOT EXISTS solid_archives ("
      "path TEXT PRIMARY KEY,"
      "name TEXT NOT NULL DEFAULT '',"
      "archive_size INTEGER NOT NULL DEFAULT 0,"
      "uncompressed_size INTEGER NOT NULL DEFAULT 0,"
      "file_count INTEGER NOT NULL DEFAULT 0,"
      "mtime_ns INTEGER NOT NULL DEFAULT 0,"
      "updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
      ")";
  if (!execSql(db, query, "creating solid archive table")) {
    return false;
  }

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_solid_archives_name "
      "ON solid_archives(name)",
      "CREATE INDEX IF NOT EXISTS idx_solid_archives_name_path "
      "ON solid_archives(name COLLATE NOCASE, path)",
      "CREATE INDEX IF NOT EXISTS idx_solid_archives_size "
      "ON solid_archives(uncompressed_size)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating solid archive index")) {
      return false;
    }
  }
  return true;
}

static bool createChartStateTables(sqlite3 *db) {
  if (db == nullptr) {
    return false;
  }

  bool ok = true;
  ok = createChartMetadataRebuildStateTable(db) && ok;
  ok = createChartScanCheckpointTable(db) && ok;
  ok = createArchiveScanCacheTable(db) && ok;
  return ok;
}

bool ChartRepository::DeleteChartMeta(sqlite3 *db, std::filesystem::path path) {
  // std::cout << "Deleting chart: " << path.string() << std::endl;
  chart_storage_identity::ToRelativePath(path);
  auto query = "DELETE FROM chart_meta WHERE path = @path";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing statement to delete a chart",
                                    logSqlErrorText)) {
    return false;
  }
  const auto target = fspath_to_utf8(path);
  SDL_Log("Deleting chart: %s", target.c_str());
  bindSqliteText(stmt, 1, target);
  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    logSqlError("deleting a chart", db);
    return false;
  }
  if (sqlite3_changes(db) > 0) {
    chart_repository_detail::BumpLibraryRevision();
  }
  return true;
}

int ChartRepository::DeleteChartMetaInDirectory(
    sqlite3 *db, const std::filesystem::path &directory) {
  if (directory.empty()) {
    return -1;
  }

  createSolidArchiveTable(db);
  createArchiveScanCacheTable(db);
  createChartScanCheckpointTable(db);

  std::filesystem::path targetDirectory = directory;
  chart_storage_identity::ToAbsolutePath(targetDirectory);
  const std::filesystem::path normalizedTarget =
      targetDirectory.lexically_normal();
  auto matchesTarget = [&](const std::filesystem::path &path) {
    return path.lexically_normal() == normalizedTarget ||
           pathIsInsideDirectory(path, targetDirectory);
  };

  std::vector<bms_parser::ChartMeta> chartMetas;
  chart_repository_detail::SelectAllChartMeta(db, chartMetas);

  auto query = "DELETE FROM chart_meta WHERE path = @path";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, query, stmt, "preparing statement to delete charts in directory",
          logSqlErrorText)) {
    return -1;
  }

  int deletedCount = 0;
  for (const auto &chartMeta : chartMetas) {
    if (!matchesTarget(chartMeta.BmsPath)) {
      continue;
    }

    const std::string target =
        chart_storage_identity::StoredPathText(chartMeta.BmsPath);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bindSqliteText(stmt, 1, target);
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      logSqlError("deleting a chart in directory", db);
      return -1;
    }
    if (sqlite3_changes(db) > 0) {
      ++deletedCount;
    }
  }

  for (const auto &solidArchivePath : selectSolidArchivePaths(db)) {
    if (!matchesTarget(solidArchivePath)) {
      continue;
    }
    if (deleteSolidArchive(db, solidArchivePath)) {
      ++deletedCount;
    }
  }
  for (const auto &archiveCachePath : selectArchiveScanCachePaths(db)) {
    if (!matchesTarget(archiveCachePath)) {
      continue;
    }
    if (deleteArchiveScanCache(db, archiveCachePath)) {
      ++deletedCount;
    }
  }
  if (deletedCount > 0) {
    clearChartScanCheckpoint(db);
    chart_repository_detail::BumpLibraryRevision();
  }
  return deletedCount;
}

bool ChartRepository::DeleteArchiveRecords(
    sqlite3 *db, const std::filesystem::path &archivePath) {
  if (db == nullptr || archivePath.empty()) {
    return false;
  }

  createChartMetaTable(db);
  createSolidArchiveTable(db);
  createArchiveScanCacheTable(db);
  createChartScanCheckpointTable(db);

  int changedCount = 0;
  std::string transactionError;
  SqliteTransactionHandle transaction(db, "BEGIN", transactionError);
  if (!transaction.active()) {
    SDL_Log("Failed to start archive record delete transaction: %s",
            transactionError.c_str());
    return false;
  }
  if (deleteChartMetaInArchive(db, archivePath)) {
    ++changedCount;
  }
  if (deleteSolidArchive(db, archivePath)) {
    ++changedCount;
  }
  if (deleteArchiveScanCache(db, archivePath)) {
    ++changedCount;
  }
  clearChartScanCheckpoint(db);
  if (!transaction.commit(transactionError)) {
    SDL_Log("Failed to commit archive record delete transaction: %s",
            transactionError.c_str());
    return false;
  }

  if (changedCount > 0) {
    chart_repository_detail::BumpLibraryRevision();
  }
  return changedCount > 0;
}

bool ChartRepository::ClearChartMeta(sqlite3 *db) {
  createSolidArchiveTable(db);
  createArchiveScanCacheTable(db);
  createChartScanCheckpointTable(db);

  auto query = "DELETE FROM chart_meta";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "clearing",
                                    logSqlErrorText)) {
    return false;
  }
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {

    logSqlError("clearing", db);
    return false;
  }
  const int chartChanges = sqlite3_changes(db);
  bool changed = chartChanges > 0;
  if (!execSql(db, "DELETE FROM solid_archives", "clearing solid archives")) {
    return false;
  }
  changed = sqlite3_changes(db) > 0 || changed;
  if (!execSql(db, "DELETE FROM archive_scan_cache",
               "clearing archive scan cache")) {
    return false;
  }
  changed = sqlite3_changes(db) > 0 || changed;
  if (!execSql(db, "DELETE FROM chart_scan_checkpoint",
               "clearing chart scan checkpoint")) {
    return false;
  }
  changed = sqlite3_changes(db) > 0 || changed;
  if (changed) {
    chart_repository_detail::BumpLibraryRevision();
  }
  return true;
}

static bool createEntriesTable(sqlite3 *db) {
  // save paths to search for charts
  if (!execSql(db,
               "CREATE TABLE IF NOT EXISTS entries ("
               "path       TEXT primary key,"
               "ios_bookmark TEXT DEFAULT ''"
               ")",
               "creating entries table")) {
    return false;
  }

  if (!execSqlAllowDuplicateColumn(
          db, "ALTER TABLE entries ADD COLUMN ios_bookmark TEXT DEFAULT ''",
          "migrating entries table")) {
    return false;
  }
  return true;
}

bool chart_repository_detail::EnsureCoreSchema(sqlite3 *database) {
  bool ok = true;
  ok = createChartMetaTable(database) && ok;
  ok = createSolidArchiveTable(database) && ok;
  ok = createFavoritesTable(database) && ok;
  ok = createEntriesTable(database) && ok;
  ok = createChartStateTables(database) && ok;
  return ok;
}

bool ChartRepository::InsertEntry(sqlite3 *db,
                                const std::filesystem::path &path,
                                const std::string &iosBookmark) {
  createChartScanCheckpointTable(db);
  auto query = "REPLACE INTO entries ("
               "path,"
               "ios_bookmark"
               ") VALUES("
               "@path,"
               "@ios_bookmark"
               ")";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing statement to insert an entry",
                                    logSqlErrorText)) {
    return false;
  }
  const std::string pathText = chart_storage_identity::StoredPathText(path);
  bindSqliteText(stmt, 1, pathText);
  bindSqliteText(stmt, 2, iosBookmark);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    logSqlError("inserting an entry", db);
    return false;
  }
  clearChartScanCheckpoint(db);
  chart_repository_detail::BumpLibraryRevision();
  return true;
}

std::vector<ChartEntry> ChartRepository::SelectAllEntries(sqlite3 *db) {
  auto query = "SELECT "
               "path,"
               "COALESCE(ios_bookmark, '')"
               " FROM entries";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "getting all entries",
                                    logSqlErrorText)) {
    return std::vector<ChartEntry>();
  }
  std::vector<ChartEntry> entries;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    ChartEntry entry;
    std::filesystem::path path(readStoredPath(stmt, 0));
    if (!path.empty()) {
      chart_storage_identity::ToAbsolutePath(path);
    }
    entry.path = fspath_to_path_t(path);
    entry.iosBookmark = sqliteColumnString(stmt, 1);
#if TARGET_OS_ANDROID
    RegisterAndroidChartFolder(path, entry.iosBookmark);
#endif
    entries.push_back(std::move(entry));
  }
  return entries;
}

std::filesystem::path ChartRepository::DefaultBmsFolderPath() {
  return Utils::GetDocumentsPath("BMS");
}

bool ChartRepository::IsDefaultBmsFolderPath(
    const std::filesystem::path &path) {
#if TARGET_OS_ANDROID
  if (path.empty()) {
    return false;
  }
  return path.lexically_normal() == DefaultBmsFolderPath().lexically_normal();
#else
  (void)path;
  return false;
#endif
}

std::vector<ChartEntry> ChartRepository::SelectEffectiveEntries(sqlite3 *db) {
  auto entries = SelectAllEntries(db);

#if TARGET_OS_ANDROID
  const auto defaultPath = DefaultBmsFolderPath();
  std::error_code errorCode;
  if (!Utils::EnsureDirectoryExists(defaultPath, errorCode)) {
    SDL_Log("Failed to create default BMS folder %s: %s",
            fspath_to_utf8(defaultPath).c_str(), errorCode.message().c_str());
  }

  bool hasDefaultEntry = false;
  for (auto &entry : entries) {
    if (IsDefaultBmsFolderPath(std::filesystem::path(entry.path))) {
      entry.removable = false;
      hasDefaultEntry = true;
    }
  }

  if (!hasDefaultEntry) {
    entries.push_back({
        .path = fspath_to_path_t(defaultPath),
        .iosBookmark = "",
        .removable = false,
    });
  }
#endif

  return entries;
}

bool ChartRepository::DeleteEntry(sqlite3 *db,
                                const std::filesystem::path &path) {
  createChartScanCheckpointTable(db);
  auto query = "DELETE FROM entries WHERE path = @path";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing statement to delete an entry",
                                    logSqlErrorText)) {
    return false;
  }
  const std::string pathText = chart_storage_identity::StoredPathText(path);
  bindSqliteText(stmt, 1, pathText);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    logSqlError("deleting an entry", db);
    return false;
  }
  if (sqlite3_changes(db) > 0) {
    clearChartScanCheckpoint(db);
    chart_repository_detail::BumpLibraryRevision();
  }
  return true;
}

bool ChartRepository::ClearEntries(sqlite3 *db) {
  createChartScanCheckpointTable(db);
  auto query = "DELETE FROM entries";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt, "clearing",
                                    logSqlErrorText)) {
    return false;
  }
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    logSqlError("clearing", db);
    return false;
  }
  if (sqlite3_changes(db) > 0) {
    clearChartScanCheckpoint(db);
    chart_repository_detail::BumpLibraryRevision();
  }
  return true;
}

ChartRepository::ChartRepository()
    : ChartRepository(Utils::GetDocumentsPath("db") / "chart.db") {}

ChartRepository::ChartRepository(std::filesystem::path databasePath)
    : impl_(std::make_unique<Impl>(std::move(databasePath))) {
  chart_storage_identity::ConfigureArchiveCachePathNormalization();
}

ChartRepository::~ChartRepository() = default;
