#include "MusicPlaylistRepository.h"

#include "../BmsMetadataText.h"
#include "ChartMetaSql.h"
#include "ChartSqlExpressions.h"
#include "ChartStorageIdentity.h"
#include "SqliteRAII.h"
#include "../Utils.h"
#include "../path.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace {

constexpr const char *kPlaylistDatabaseFileName = "music_playlist.db";
constexpr const char *kChartDatabaseFileName = "chart.db";
constexpr const char *kChartDatabaseSchema = "chart_library";
constexpr const char *kChartMetaTable = "chart_library.chart_meta";
constexpr int kMusicPlaylistDatabaseSchemaVersion = 1;

using asobmshow::bms_metadata::normalizedHash;
using asobmshow::bms_metadata::trimCopy;
using asobmshow::chart_sql::chartArtworkOrderBy;
using asobmshow::chart_sql::chartSourceOrderBy;
using asobmshow::chart_sql::kChartMetaColumnCount;
using asobmshow::chart_sql::kChartMetaSelectColumns;
using asobmshow::chart_sql::kMaxSqlIntegerText;
using asobmshow::chart_sql::kStoredDocumentsBmsPrefix;
using asobmshow::chart_sql::normalizedSqlHash;

int parseIntOr(const std::string &value, int fallback) {
  if (value.empty()) {
    return fallback;
  }
  char *end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0') {
    return fallback;
  }
  if (parsed < std::numeric_limits<int>::min() ||
      parsed > std::numeric_limits<int>::max()) {
    return fallback;
  }
  return static_cast<int>(parsed);
}

int stateInt(const std::unordered_map<std::string, std::string> &values,
             const char *key, int fallback) {
  const auto it = values.find(key);
  if (it == values.end()) {
    return fallback;
  }
  return parseIntOr(it->second, fallback);
}

std::string stateString(
    const std::unordered_map<std::string, std::string> &values,
    const char *key) {
  const auto it = values.find(key);
  return it == values.end() ? std::string{} : it->second;
}

std::string columnString(sqlite3_stmt *stmt, int idx) {
  return sqliteColumnString(stmt, idx);
}

void sqliteArtistHasObjectNotation(sqlite3_context *context, int argc,
                                   sqlite3_value **argv) {
  const std::string artist = argc > 0 ? sqliteValueString(argv[0]) : "";
  const std::string subArtist = argc > 1 ? sqliteValueString(argv[1]) : "";
  sqlite3_result_int(
      context,
      asobmshow::bms_metadata::hasArtistObjectNotation(artist, subArtist) ? 1
                                                                          : 0);
}

void logSqlErrorText(const char *context, const std::string &error) {
  std::cerr << "SQL error while " << context << ": " << error << "\n";
}

void logSqlError(const char *context, sqlite3 *db) {
  logSqlErrorText(context, sqliteDatabaseError(db));
}

void registerMusicPlaylistSqliteFunctions(sqlite3 *db) {
  if (db == nullptr) {
    return;
  }
  const int rc = sqlite3_create_function_v2(
      db, "bms_artist_has_object_notation", 2,
      SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
      sqliteArtistHasObjectNotation, nullptr, nullptr, nullptr);
  if (rc != SQLITE_OK) {
    logSqlError("registering music playlist functions", db);
  }
}

bool execSql(sqlite3 *db, const char *query, const char *context) {
  return executeSqliteLogged(db, query, context, logSqlErrorText);
}

int databaseUserVersion(sqlite3 *db) {
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, "PRAGMA user_version", stmt,
                                    "reading music playlist database version",
                                    logSqlErrorText)) {
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
  return execSql(db, query.c_str(),
                 "updating music playlist database version");
}

bool addMissingColumn(sqlite3 *db, const char *table, const char *column,
                      const char *definition) {
  std::string query = "ALTER TABLE ";
  query += table;
  query += " ADD COLUMN ";
  query += definition;
  return ensureSqliteTableColumnLogged(
      db, table, column, query.c_str(), "checking music playlist schema",
      "adding music playlist identity column", logSqlErrorText);
}

bool ensureStoredMusicTrackIdentityColumns(sqlite3 *db, const char *table) {
  return addMissingColumn(db, table, "chart_path",
                          "chart_path TEXT NOT NULL DEFAULT ''") &&
         addMissingColumn(db, table, "chart_md5",
                          "chart_md5 TEXT NOT NULL DEFAULT ''") &&
         addMissingColumn(db, table, "chart_sha256",
                          "chart_sha256 TEXT NOT NULL DEFAULT ''");
}

bool normalizeStoredHashColumn(sqlite3 *db, const char *table,
                               const char *column) {
  return updateSqliteColumnWithExpressionLogged(
      db, table, column, normalizedSqlHash(column),
      "normalizing stored playlist hash column", logSqlErrorText);
}

bool attachChartDatabase(sqlite3 *db, const std::filesystem::path &path) {
  if (const auto error = attachSqliteDatabase(db, path, kChartDatabaseSchema)) {
    logSqlErrorText("attaching chart database", *error);
    return false;
  }
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(
          db,
          "SELECT 1 FROM chart_library.sqlite_master WHERE type = 'table' "
          "AND name = 'chart_meta' LIMIT 1",
          stmt) != SQLITE_OK ||
      sqlite3_step(stmt) != SQLITE_ROW) {
    logSqlErrorText("checking attached chart database",
                    sqliteDatabaseError(db));
    return false;
  }
  return true;
}

bool validateChartDatabase(const std::filesystem::path &path,
                           std::string &errorMessage) {
  std::error_code filesystemError;
  if (!std::filesystem::exists(path, filesystemError)) {
    errorMessage = filesystemError
                       ? "could not inspect chart database: " +
                             filesystemError.message()
                       : "chart database does not exist";
    return false;
  }
  if (!std::filesystem::is_regular_file(path, filesystemError) ||
      filesystemError) {
    errorMessage = filesystemError
                       ? "could not inspect chart database: " +
                             filesystemError.message()
                       : "chart database is not a regular file";
    return false;
  }

  sqlite3 *rawDatabase = nullptr;
  const std::string pathText = fspath_to_utf8(path);
  const int openResult = sqlite3_open_v2(
      pathText.c_str(), &rawDatabase,
      SQLITE_OPEN_READONLY | SQLITE_OPEN_PRIVATECACHE, nullptr);
  SqliteConnectionHandle database(rawDatabase);
  if (openResult != SQLITE_OK || !database) {
    errorMessage = rawDatabase != nullptr ? sqlite3_errmsg(rawDatabase)
                                          : "could not open chart database";
    return false;
  }
  sqlite3_busy_timeout(database.get(), 1000);

  bool chartMetaExists = false;
  if (const auto queryError =
          querySqliteTableExists(database.get(), "chart_meta", chartMetaExists)) {
    errorMessage = *queryError;
    return false;
  }
  if (!chartMetaExists) {
    errorMessage = "chart database does not contain chart_meta";
    return false;
  }
  return true;
}

bool compactPlaylistPositions(sqlite3 *db, int playlistId) {
  const char *selectQuery =
      "SELECT id FROM music_playlist_items WHERE playlist_id = ?1 "
      "ORDER BY position, id";
  SqliteStatementHandle selectStmt;
  if (!prepareSqliteStatementLogged(
          db, selectQuery, selectStmt,
          "preparing music playlist compact select", logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(selectStmt, 1, playlistId);

  std::vector<int> ids;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(selectStmt)) == SQLITE_ROW) {
    ids.push_back(sqlite3_column_int(selectStmt, 0));
  }
  if (rc != SQLITE_DONE) {
    logSqlError("selecting music playlist positions", db);
    return false;
  }

  const char *updateQuery =
      "UPDATE music_playlist_items SET position = ?1 WHERE id = ?2";
  SqliteStatementHandle updateStmt;
  if (!prepareSqliteStatementLogged(
          db, updateQuery, updateStmt,
          "preparing music playlist compact update", logSqlErrorText)) {
    return false;
  }

  for (std::size_t i = 0; i < ids.size(); ++i) {
    sqlite3_reset(updateStmt);
    sqlite3_clear_bindings(updateStmt);
    sqlite3_bind_int(updateStmt, 1, static_cast<int>(i));
    sqlite3_bind_int(updateStmt, 2, ids[i]);
    rc = sqlite3_step(updateStmt);
    if (rc != SQLITE_DONE) {
      logSqlError("compacting music playlist positions", db);
      return false;
    }
  }
  return true;
}

void touchPlaylistUpdatedAt(sqlite3 *db, int playlistId) {
  const char *query =
      "UPDATE music_playlists SET updated_at = CURRENT_TIMESTAMP WHERE id = ?1";
  SqliteStatementHandle stmt;
  if (prepareSqliteStatement(db, query, stmt) == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, playlistId);
    sqlite3_step(stmt);
  }
}

std::filesystem::path pathFromDbText(const std::string &value) {
  return std::filesystem::path(utf8_to_path_t(value));
}

std::filesystem::path absolutePathFromColumn(sqlite3_stmt *stmt, int idx) {
  auto path = pathFromDbText(columnString(stmt, idx));
  if (!path.empty()) {
    chart_storage_identity::ToAbsolutePath(path);
  }
  return path;
}

std::filesystem::path relativePathFromColumn(sqlite3_stmt *stmt, int idx) {
  return pathFromDbText(columnString(stmt, idx));
}

std::string storedPathTextForDbValue(const std::string &value) {
  return chart_storage_identity::StoredPathText(pathFromDbText(value));
}

struct PendingStoredPathNormalization {
  sqlite3_int64 rowid = 0;
  int playlistId = 0;
  std::string normalized;
};

bool normalizeStoredPathColumn(sqlite3 *db, const char *table,
                               const char *column,
                               const char *extraWhere = nullptr) {
  std::string selectQuery = "SELECT rowid, ";
  selectQuery += column;
  selectQuery += " FROM ";
  selectQuery += table;
  selectQuery += " WHERE ";
  selectQuery += column;
  selectQuery += " IS NOT NULL AND ";
  selectQuery += column;
  selectQuery += " != ''";
  if (extraWhere != nullptr && extraWhere[0] != '\0') {
    selectQuery += " AND ";
    selectQuery += extraWhere;
  }

  SqliteStatementHandle selectStmt;
  if (!prepareSqliteStatementLogged(
          db, selectQuery, selectStmt,
          "preparing stored music path normalization select",
          logSqlErrorText)) {
    return false;
  }

  std::vector<PendingStoredPathNormalization> pending;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(selectStmt)) == SQLITE_ROW) {
    const std::string original = columnString(selectStmt, 1);
    const std::string normalized = storedPathTextForDbValue(original);
    if (!normalized.empty() && normalized != original) {
      pending.push_back({.rowid = sqlite3_column_int64(selectStmt, 0),
                         .normalized = normalized});
    }
  }
  if (rc != SQLITE_DONE) {
    logSqlError("selecting stored music paths to normalize", db);
    return false;
  }
  if (pending.empty()) {
    return true;
  }

  std::string updateQuery = "UPDATE ";
  updateQuery += table;
  updateQuery += " SET ";
  updateQuery += column;
  updateQuery += " = ?1 WHERE rowid = ?2";
  SqliteStatementHandle updateStmt;
  if (!prepareSqliteStatementLogged(
          db, updateQuery, updateStmt,
          "preparing stored music path normalization update",
          logSqlErrorText)) {
    return false;
  }

  for (const auto &item : pending) {
    sqlite3_reset(updateStmt);
    sqlite3_clear_bindings(updateStmt);
    bindSqliteText(updateStmt, 1, item.normalized);
    sqlite3_bind_int64(updateStmt, 2, item.rowid);
    if (sqlite3_step(updateStmt) != SQLITE_DONE) {
      logSqlError("normalizing stored music path", db);
      return false;
    }
  }
  return true;
}

bool backfillStoredChartPathFromMusicKey(sqlite3 *db, const char *table) {
  std::string query = "UPDATE ";
  query += table;
  query += " SET chart_path = music_key WHERE music_key_type = 'path' "
           "AND chart_path = '' AND music_key != ''";
  return execSql(db, query.c_str(), "backfilling stored music chart paths");
}

bool playlistMusicKeyExists(sqlite3_stmt *stmt, int playlistId,
                            sqlite3_int64 rowid,
                            const std::string &musicKey) {
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_int(stmt, 1, playlistId);
  bindSqliteText(stmt, 2, musicKey);
  sqlite3_bind_int64(stmt, 3, rowid);
  return sqlite3_step(stmt) == SQLITE_ROW;
}

bool normalizePlaylistItemMusicKeys(sqlite3 *db) {
  const char *selectQuery =
      "SELECT rowid, playlist_id, music_key FROM music_playlist_items "
      "WHERE music_key_type = 'path' AND music_key != ''";
  SqliteStatementHandle selectStmt;
  if (!prepareSqliteStatementLogged(
          db, selectQuery, selectStmt,
          "preparing music playlist key normalization select",
          logSqlErrorText)) {
    return false;
  }

  std::vector<PendingStoredPathNormalization> pending;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(selectStmt)) == SQLITE_ROW) {
    const std::string original = columnString(selectStmt, 2);
    const std::string normalized = storedPathTextForDbValue(original);
    if (!normalized.empty() && normalized != original) {
      pending.push_back({.rowid = sqlite3_column_int64(selectStmt, 0),
                         .playlistId = sqlite3_column_int(selectStmt, 1),
                         .normalized = normalized});
    }
  }
  if (rc != SQLITE_DONE) {
    logSqlError("selecting music playlist keys to normalize", db);
    return false;
  }
  if (pending.empty()) {
    return true;
  }

  const char *updateQuery =
      "UPDATE OR IGNORE music_playlist_items SET music_key = ?1 "
      "WHERE rowid = ?2";
  SqliteStatementHandle updateStmt;
  if (!prepareSqliteStatementLogged(
          db, updateQuery, updateStmt,
          "preparing music playlist key normalization update",
          logSqlErrorText)) {
    return false;
  }

  const char *existsQuery =
      "SELECT 1 FROM music_playlist_items WHERE playlist_id = ?1 "
      "AND music_key_type = 'path' AND music_key = ?2 AND rowid != ?3 "
      "LIMIT 1";
  SqliteStatementHandle existsStmt;
  if (!prepareSqliteStatementLogged(
          db, existsQuery, existsStmt,
          "preparing music playlist key normalization duplicate select",
          logSqlErrorText)) {
    return false;
  }

  const char *deleteQuery =
      "DELETE FROM music_playlist_items WHERE rowid = ?1";
  SqliteStatementHandle deleteStmt;
  if (!prepareSqliteStatementLogged(
          db, deleteQuery, deleteStmt,
          "preparing music playlist key normalization duplicate delete",
          logSqlErrorText)) {
    return false;
  }

  std::vector<int> affectedPlaylists;
  for (const auto &item : pending) {
    sqlite3_reset(updateStmt);
    sqlite3_clear_bindings(updateStmt);
    bindSqliteText(updateStmt, 1, item.normalized);
    sqlite3_bind_int64(updateStmt, 2, item.rowid);
    if (sqlite3_step(updateStmt) != SQLITE_DONE) {
      logSqlError("normalizing music playlist key", db);
      return false;
    }
    if (sqlite3_changes(db) > 0) {
      affectedPlaylists.push_back(item.playlistId);
      continue;
    }

    if (!playlistMusicKeyExists(existsStmt, item.playlistId, item.rowid,
                                item.normalized)) {
      continue;
    }
    sqlite3_reset(deleteStmt);
    sqlite3_clear_bindings(deleteStmt);
    sqlite3_bind_int64(deleteStmt, 1, item.rowid);
    if (sqlite3_step(deleteStmt) != SQLITE_DONE) {
      logSqlError("deleting duplicate normalized music playlist key", db);
      return false;
    }
    if (sqlite3_changes(db) > 0) {
      affectedPlaylists.push_back(item.playlistId);
    }
  }

  std::sort(affectedPlaylists.begin(), affectedPlaylists.end());
  affectedPlaylists.erase(
      std::unique(affectedPlaylists.begin(), affectedPlaylists.end()),
      affectedPlaylists.end());
  for (int playlistId : affectedPlaylists) {
    if (!compactPlaylistPositions(db, playlistId)) {
      return false;
    }
  }
  return true;
}

bool migrateMusicPlaylistDatabaseSchema(sqlite3 *db) {
  if (databaseUserVersion(db) >= kMusicPlaylistDatabaseSchemaVersion) {
    return true;
  }

  if (!backfillStoredChartPathFromMusicKey(db, "music_playlist_items") ||
      !backfillStoredChartPathFromMusicKey(db, "music_now_playing_items") ||
      !normalizeStoredPathColumn(db, "music_playlist_items", "chart_path") ||
      !normalizePlaylistItemMusicKeys(db) ||
      !normalizeStoredPathColumn(db, "music_now_playing_items", "chart_path") ||
      !normalizeStoredPathColumn(db, "music_now_playing_items", "music_key",
                                 "music_key_type = 'path'") ||
      !normalizeStoredHashColumn(db, "music_playlist_items", "chart_md5") ||
      !normalizeStoredHashColumn(db, "music_playlist_items", "chart_sha256") ||
      !normalizeStoredHashColumn(db, "music_now_playing_items", "chart_md5") ||
      !normalizeStoredHashColumn(db, "music_now_playing_items",
                                 "chart_sha256")) {
    return false;
  }

  return setDatabaseUserVersion(db, kMusicPlaylistDatabaseSchemaVersion);
}

struct StoredMusicTrackIdentity {
  std::string keyType;
  std::string musicKey;
  std::string chartPath;
  std::string md5;
  std::string sha256;
};

StoredMusicTrackIdentity
storedMusicTrackIdentity(const bms_parser::ChartMeta &chartMeta) {
  StoredMusicTrackIdentity identity;
  identity.chartPath =
      chart_storage_identity::StoredPathText(chartMeta.BmsPath);
  identity.md5 = normalizedHash(chartMeta.MD5);
  identity.sha256 = normalizedHash(chartMeta.SHA256);

  if (!identity.chartPath.empty()) {
    identity.keyType = "path";
    identity.musicKey = identity.chartPath;
  }
  return identity;
}

void bindStoredMusicTrackKey(sqlite3_stmt *stmt, int firstIndex,
                             const StoredMusicTrackIdentity &identity) {
  bindSqliteText(stmt, firstIndex, identity.keyType);
  bindSqliteText(stmt, firstIndex + 1, identity.musicKey);
}

void bindStoredMusicTrackIdentity(sqlite3_stmt *stmt, int firstIndex,
                                  const StoredMusicTrackIdentity &identity) {
  bindStoredMusicTrackKey(stmt, firstIndex, identity);
  bindSqliteText(stmt, firstIndex + 2, identity.chartPath);
  bindSqliteText(stmt, firstIndex + 3, identity.md5);
  bindSqliteText(stmt, firstIndex + 4, identity.sha256);
}

std::string sqlParam(int index) { return "?" + std::to_string(index); }

std::string storedMusicTrackPathParamPredicate(const std::string &pathParam) {
  const std::string prefix(kStoredDocumentsBmsPrefix);
  return pathParam + " != '' AND (chart_path = " + pathParam +
         " OR chart_path = '" + prefix + "' || " + pathParam + " OR " +
         "(" + pathParam + " LIKE '" + prefix +
         "%' AND chart_path = substr(" + pathParam + ", length('" + prefix +
         "') + 1)))";
}

std::string storedMusicTrackRowPredicate(int firstIndex) {
  const std::string keyTypeParam = sqlParam(firstIndex);
  const std::string musicKeyParam = sqlParam(firstIndex + 1);
  const std::string chartPathParam = sqlParam(firstIndex + 2);
  const std::string md5Param = sqlParam(firstIndex + 3);
  const std::string sha256Param = sqlParam(firstIndex + 4);
  return "((music_key_type = " + keyTypeParam + " AND music_key = " +
         musicKeyParam + ") OR (" + sha256Param +
         " != '' AND chart_sha256 = " + sha256Param + ") OR (" + md5Param +
         " != '' AND chart_md5 = " + md5Param + ") OR (" +
         storedMusicTrackPathParamPredicate(chartPathParam) + "))";
}

std::string storedMusicTrackRowPreferenceOrderBy(int firstIndex) {
  const std::string keyTypeParam = sqlParam(firstIndex);
  const std::string musicKeyParam = sqlParam(firstIndex + 1);
  const std::string chartPathParam = sqlParam(firstIndex + 2);
  const std::string md5Param = sqlParam(firstIndex + 3);
  const std::string sha256Param = sqlParam(firstIndex + 4);
  return "CASE WHEN music_key_type = " + keyTypeParam + " AND music_key = " +
         musicKeyParam + " THEN 0 WHEN " + sha256Param +
         " != '' AND chart_sha256 = " + sha256Param + " THEN 1 WHEN " +
         md5Param + " != '' AND chart_md5 = " + md5Param + " THEN 2 WHEN " +
         storedMusicTrackPathParamPredicate(chartPathParam) +
         " THEN 3 ELSE 4 END";
}

int selectStoredMusicTrackRowId(sqlite3 *db, int playlistId,
                                const StoredMusicTrackIdentity &identity) {
  std::string query = "SELECT id FROM music_playlist_items "
                      "WHERE playlist_id = ?1 AND ";
  query += storedMusicTrackRowPredicate(2);
  query += " ORDER BY ";
  query += storedMusicTrackRowPreferenceOrderBy(2);
  query += ", position, id LIMIT 1";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, query, stmt, "selecting music playlist track identity",
          logSqlErrorText)) {
    return 0;
  }
  sqlite3_bind_int(stmt, 1, playlistId);
  bindStoredMusicTrackIdentity(stmt, 2, identity);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    return 0;
  }
  return sqlite3_column_int(stmt, 0);
}

bool updateStoredMusicTrackRowIdentity(
    sqlite3 *db, int rowId, const StoredMusicTrackIdentity &identity) {
  const char *query =
      "UPDATE music_playlist_items SET music_key_type = ?1, music_key = ?2, "
      "chart_path = ?3, chart_md5 = ?4, chart_sha256 = ?5, "
      "added_at = CURRENT_TIMESTAMP WHERE id = ?6";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, query, stmt, "updating music playlist track identity",
          logSqlErrorText)) {
    return false;
  }
  bindStoredMusicTrackIdentity(stmt, 1, identity);
  sqlite3_bind_int(stmt, 6, rowId);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    logSqlError("updating music playlist track identity", db);
    return false;
  }
  return true;
}

bool deleteDuplicateStoredMusicTrackRows(
    sqlite3 *db, int playlistId, int keepRowId,
    const StoredMusicTrackIdentity &identity) {
  std::string query = "DELETE FROM music_playlist_items "
                      "WHERE playlist_id = ?1 AND id != ?7 AND ";
  query += storedMusicTrackRowPredicate(2);

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, query, stmt, "deleting duplicate music playlist tracks",
          logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(stmt, 1, playlistId);
  bindStoredMusicTrackIdentity(stmt, 2, identity);
  sqlite3_bind_int(stmt, 7, keepRowId);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    logSqlError("deleting duplicate music playlist tracks", db);
    return false;
  }
  return true;
}

std::string joinedTextExpr(const std::string &alias,
                           std::initializer_list<const char *> columns) {
  std::string expr;
  bool first = true;
  for (const char *column : columns) {
    if (!first) {
      expr += " || ' ' || ";
    }
    first = false;
    expr += "COALESCE(" + alias + "." + column + ", '')";
  }
  return expr;
}

std::string lowerTextExpr(const std::string &alias,
                          std::initializer_list<const char *> columns) {
  return "LOWER(" + joinedTextExpr(alias, columns) + ")";
}

std::string trimmedTextExpr(const std::string &alias,
                            std::initializer_list<const char *> columns) {
  return "TRIM(" + joinedTextExpr(alias, columns) + ")";
}

std::string textLengthOrderExpr(const std::string &alias,
                                std::initializer_list<const char *> columns) {
  const std::string text = trimmedTextExpr(alias, columns);
  return "CASE WHEN " + text + " = '' THEN " + kMaxSqlIntegerText +
         " ELSE LENGTH(" + text + ") END";
}

std::string likeAnyExpr(const std::string &expr,
                        std::initializer_list<const char *> patterns) {
  std::string result = "(";
  bool first = true;
  for (const char *pattern : patterns) {
    if (!first) {
      result += " OR ";
    }
    first = false;
    result += expr + " LIKE '" + pattern + "'";
  }
  result += ")";
  return result;
}

std::string musicTitleTextExpr(const std::string &alias) {
  return lowerTextExpr(alias, {"title", "subtitle"});
}

std::string titleContainsExpr(const std::string &alias,
                              std::initializer_list<const char *> patterns) {
  return likeAnyExpr(musicTitleTextExpr(alias), patterns);
}

std::string musicRepresentativeOrderBy(const std::string &alias) {
  return "CASE WHEN bms_artist_has_object_notation(" + alias + ".artist, " +
         alias + ".sub_artist) != 0 THEN 1 ELSE 0 END, CASE WHEN " +
         titleContainsExpr(alias, {"%delay%"}) +
         " THEN 1 ELSE 0 END, CASE WHEN " +
         titleContainsExpr(alias, {"%endless%"}) +
         " THEN 1 ELSE 0 END, CASE WHEN " +
         titleContainsExpr(alias,
                           {"%normal%", "%hyper%", "%insane%",
                            "%another%"}) +
         " THEN 0 ELSE 1 END, " +
         textLengthOrderExpr(alias, {"artist", "sub_artist"}) + ", " +
         textLengthOrderExpr(alias, {"title", "subtitle"});
}

struct StoredMusicTrackSelectQuery {
  const char *itemTable;
  const char *itemAlias;
  const char *positionAlias;
  const char *keyTypeAlias;
  const char *wherePrefix;
};

std::string storedMusicTrackItemColumn(const StoredMusicTrackSelectQuery &config,
                                       const char *column) {
  std::string result = config.itemAlias;
  result += ".";
  result += column;
  return result;
}

std::string storedMusicTrackCandidateBranch(
    const StoredMusicTrackSelectQuery &config,
    const std::string &matchCondition, int identityRank) {
  std::string query = "SELECT cm.*, ";
  query += config.itemAlias;
  query += ".id AS item_id, ";
  query += config.itemAlias;
  query += ".position AS ";
  query += config.positionAlias;
  query += ", ";
  query += config.itemAlias;
  query += ".music_key_type AS ";
  query += config.keyTypeAlias;
  query += ", ";
  query += std::to_string(identityRank);
  query += " AS identity_rank FROM ";
  query += config.itemTable;
  query += " ";
  query += config.itemAlias;
  query += " JOIN ";
  query += kChartMetaTable;
  query += " cm ON ";
  query += matchCondition;
  query += " WHERE ";
  if (config.wherePrefix != nullptr) {
    query += config.wherePrefix;
  }
  query += config.itemAlias;
  query += ".music_key_type = 'path'";
  return query;
}

std::string storedMusicTrackCandidateQuery(
    const StoredMusicTrackSelectQuery &config) {
  const std::string prefix(kStoredDocumentsBmsPrefix);
  const std::string itemSha256 =
      storedMusicTrackItemColumn(config, "chart_sha256");
  const std::string itemMd5 = storedMusicTrackItemColumn(config, "chart_md5");
  const std::string itemPath =
      storedMusicTrackItemColumn(config, "chart_path");
  const std::string shaMatch =
      itemSha256 + " != '' AND cm.sha256 = " + itemSha256;
  const std::string md5Match = itemMd5 + " != '' AND cm.md5 = " + itemMd5;
  const std::string nonHashMatch =
      "NOT (" + shaMatch + ") AND NOT (" + md5Match + ")";

  std::vector<std::string> branches;
  branches.push_back(storedMusicTrackCandidateBranch(config, shaMatch, 0));
  branches.push_back(storedMusicTrackCandidateBranch(
      config, md5Match + " AND NOT (" + shaMatch + ")", 1));
  branches.push_back(storedMusicTrackCandidateBranch(
      config, itemPath + " != '' AND cm.path = " + itemPath + " AND " +
                  nonHashMatch,
      2));
  branches.push_back(storedMusicTrackCandidateBranch(
      config, itemPath + " != '' AND cm.path = '" + prefix + "' || " +
                  itemPath + " AND " + nonHashMatch,
      2));
  branches.push_back(storedMusicTrackCandidateBranch(
      config, itemPath + " LIKE '" + prefix + "%' AND cm.path = substr(" +
                  itemPath + ", length('" + prefix + "') + 1) AND " +
                  nonHashMatch,
      2));

  std::string query;
  for (std::size_t i = 0; i < branches.size(); ++i) {
    if (i > 0) {
      query += " UNION ALL ";
    }
    query += branches[i];
  }
  return query;
}

std::string
storedMusicTrackSelectQuery(const StoredMusicTrackSelectQuery &config) {
  std::string representativeOrder = chartArtworkOrderBy("choice");
  representativeOrder +=
      ", choice.identity_rank, choice.total_notes DESC, choice.length DESC, ";
  representativeOrder += chartSourceOrderBy("choice");
  representativeOrder += ", choice.title COLLATE NOCASE, choice.path";

  std::string query =
      "WITH candidates AS (";
  query += storedMusicTrackCandidateQuery(config);
  query += "), preferred_candidates AS (SELECT * FROM candidates pc WHERE ";
  query += asobmshow::chart_sql::preferredChartPredicate("pc",
                                                         kChartMetaTable);
  query +=
      "), item_choices AS (SELECT pc.item_id, COUNT(*) AS music_chart_count, "
      "(SELECT choice.path FROM preferred_candidates choice WHERE "
      "choice.item_id = pc.item_id ORDER BY ";
  query += representativeOrder;
  query += " LIMIT 1) AS representative_path FROM preferred_candidates pc "
           "GROUP BY pc.item_id) SELECT ";
  query += kChartMetaSelectColumns;
  query += ", item_choices.music_chart_count, cm.item_id, cm.";
  query += config.keyTypeAlias;
  query +=
      " FROM preferred_candidates cm JOIN item_choices ON "
      "item_choices.item_id = cm.item_id AND "
      "item_choices.representative_path = cm.path ORDER BY cm.";
  query += config.positionAlias;
  query += ", cm.title COLLATE NOCASE, cm.path";
  return query;
}

bms_parser::ChartMeta readChartMeta(sqlite3_stmt *stmt) {
  return asobmshow::chart_sql::readChartMeta(
      stmt, absolutePathFromColumn, relativePathFromColumn);
}

void readStoredMusicTrackRows(sqlite3_stmt *stmt,
                              std::vector<MusicTrackRecord> &tracks) {
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    MusicTrackRecord record;
    record.representativeChart = readChartMeta(stmt);
    record.chartCount =
        std::max(1, sqlite3_column_int(stmt, kChartMetaColumnCount));
    record.storedItemId =
        sqlite3_column_int(stmt, kChartMetaColumnCount + 1);
    record.useChartPathIdentity =
        columnString(stmt, kChartMetaColumnCount + 2) == "path";
    tracks.push_back(std::move(record));
  }
}

} // namespace

struct MusicPlaylistRepository::Impl {
  Impl(std::filesystem::path databasePath,
       std::filesystem::path chartDatabasePath)
      : databasePath(std::move(databasePath)),
        chartDatabasePath(std::move(chartDatabasePath)) {}
  ~Impl();

  bool EnsureDatabase();
  void Shutdown();
  sqlite3 *Connect();
  void Close(sqlite3 *db);
  bool CreateTables(sqlite3 *db);
  int EnsurePlaylist(sqlite3 *db, const std::string &name);
  bool RenamePlaylist(sqlite3 *db, int playlistId, const std::string &name);
  std::vector<MusicPlaylistInfo> SelectPlaylists(sqlite3 *db);
  bool InsertTrack(sqlite3 *db, int playlistId,
                   const bms_parser::ChartMeta &chartMeta);
  bool DeleteTrack(sqlite3 *db, int playlistId,
                   const bms_parser::ChartMeta &chartMeta,
                   int storedItemId);
  bool MoveTrack(sqlite3 *db, int playlistId,
                 const bms_parser::ChartMeta &chartMeta, int delta,
                 int storedItemId);
  bool ClearPlaylist(sqlite3 *db, int playlistId);
  bool DeletePlaylist(sqlite3 *db, int playlistId);
  MusicPlayerStateRecord SelectPlayerState(sqlite3 *db);
  bool SavePlayerState(sqlite3 *db, const MusicPlayerStateRecord &state);
  bool ReplaceNowPlayingTracks(
      sqlite3 *db, const std::vector<bms_parser::ChartMeta> &tracks);
  void SelectLibraryTracks(sqlite3 *db,
                           std::vector<MusicTrackRecord> &tracks);
  void SelectLibraryGroupTracks(sqlite3 *db,
                                const bms_parser::ChartMeta &chartMeta,
                                std::vector<MusicTrackRecord> &tracks);
  void SelectNowPlayingTracks(sqlite3 *db,
                              std::vector<MusicTrackRecord> &tracks);
  void SelectTracks(sqlite3 *db, int playlistId,
                    std::vector<MusicTrackRecord> &tracks);

  std::filesystem::path databasePath;
  std::filesystem::path chartDatabasePath;
  std::mutex mutex;
  sqlite3 *database = nullptr;
  sqlite3 *schemaDatabase = nullptr;
};

MusicPlaylistRepository::Impl::~Impl() { Shutdown(); }

bool MusicPlaylistRepository::Impl::EnsureDatabase() {
  if (database != nullptr) {
    if (sqlite3_get_autocommit(database) != 0) {
      return true;
    }
    std::cerr << "Discarding music playlist database with an unfinished "
                 "transaction.\n";
    Shutdown();
  }

  database = Connect();
  if (database == nullptr) {
    return false;
  }
  if (!CreateTables(database)) {
    Shutdown();
    return false;
  }
  return true;
}

void MusicPlaylistRepository::Impl::Shutdown() {
  sqlite3 *connection = database;
  database = nullptr;
  Close(connection);
}

sqlite3 *MusicPlaylistRepository::Impl::Connect() {
  std::string chartValidationError;
  if (!validateChartDatabase(chartDatabasePath, chartValidationError)) {
    std::cerr << "Can't use chart database for music playlists: "
              << chartValidationError << "\n";
    return nullptr;
  }

  const std::filesystem::path directory = databasePath.parent_path();
  std::error_code directoryError;
  if (!directory.empty() &&
      !Utils::EnsureDirectoryExists(directory, directoryError)) {
    std::cerr << "Can't create music playlist database directory "
              << fspath_to_utf8(directory) << ": " << directoryError.message()
              << "\n";
    return nullptr;
  }

  std::string openError;
  sqlite3 *db = openValidatedSqliteDatabase(
      databasePath, kMusicPlaylistDatabaseSchemaVersion, true, openError);
  if (db == nullptr) {
    std::cerr << "Can't open music playlist database: " << openError << "\n";
    return nullptr;
  }

  if (const auto pragmaError =
          applySqlitePragmas(db, {"PRAGMA synchronous=NORMAL"})) {
    std::cerr << "Could not configure music playlist database: "
              << *pragmaError << "\n";
    closeSqliteDatabase(db);
    return nullptr;
  }
  registerMusicPlaylistSqliteFunctions(db);
  if (!attachChartDatabase(db, chartDatabasePath)) {
    closeSqliteDatabase(db);
    return nullptr;
  }
  return db;
}

void MusicPlaylistRepository::Impl::Close(sqlite3 *db) {
  if (schemaDatabase == db) {
    schemaDatabase = nullptr;
  }
  closeSqliteDatabase(db);
}

bool MusicPlaylistRepository::Impl::CreateTables(sqlite3 *db) {
  if (db == nullptr) {
    return false;
  }
  if (schemaDatabase == db) {
    return true;
  }

  const bool callerOwnsTransaction = sqlite3_get_autocommit(db) == 0;
  const char *beginQuery = callerOwnsTransaction
                               ? "SAVEPOINT asobmashow_music_schema"
                               : "BEGIN";
  const char *commitQuery = callerOwnsTransaction
                                ? "RELEASE asobmashow_music_schema"
                                : "COMMIT";
  const char *rollbackQuery =
      callerOwnsTransaction
          ? "ROLLBACK TO asobmashow_music_schema; RELEASE "
            "asobmashow_music_schema"
          : "ROLLBACK";
  std::string transactionError;
  SqliteTransactionHandle transaction(db, beginQuery, transactionError,
                                      commitQuery, rollbackQuery);
  if (!transaction.active()) {
    logSqlErrorText("beginning music playlist schema setup", transactionError);
    return false;
  }

  struct SchemaStatement {
    const char *query;
    const char *context;
  };
  const SchemaStatement tables[] = {
      {"CREATE TABLE IF NOT EXISTS music_playlists ("
       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
       "name TEXT NOT NULL UNIQUE,"
       "created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
       "updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
       ")",
       "creating music playlist table"},
      {"CREATE TABLE IF NOT EXISTS music_playlist_items ("
       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
       "playlist_id INTEGER NOT NULL,"
       "position INTEGER NOT NULL,"
       "music_key_type TEXT NOT NULL,"
       "music_key TEXT NOT NULL,"
       "chart_path TEXT NOT NULL DEFAULT '',"
       "chart_md5 TEXT NOT NULL DEFAULT '',"
       "chart_sha256 TEXT NOT NULL DEFAULT '',"
       "added_at TEXT DEFAULT CURRENT_TIMESTAMP,"
       "FOREIGN KEY(playlist_id) REFERENCES music_playlists(id) "
       "ON DELETE CASCADE,"
       "UNIQUE(playlist_id, music_key_type, music_key)"
       ")",
       "creating music playlist item table"},
      {"CREATE TABLE IF NOT EXISTS music_player_state ("
       "key TEXT PRIMARY KEY,"
       "value TEXT NOT NULL,"
       "updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
       ")",
       "creating music player state table"},
      {"CREATE TABLE IF NOT EXISTS music_now_playing_items ("
       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
       "position INTEGER NOT NULL,"
       "music_key_type TEXT NOT NULL,"
       "music_key TEXT NOT NULL,"
       "chart_path TEXT NOT NULL DEFAULT '',"
       "chart_md5 TEXT NOT NULL DEFAULT '',"
       "chart_sha256 TEXT NOT NULL DEFAULT '',"
       "added_at TEXT DEFAULT CURRENT_TIMESTAMP"
       ")",
       "creating now playing table"},
  };
  for (const SchemaStatement &table : tables) {
    if (!execSql(db, table.query, table.context)) {
      return false;
    }
  }
  if (!ensureStoredMusicTrackIdentityColumns(db, "music_playlist_items") ||
      !ensureStoredMusicTrackIdentityColumns(db, "music_now_playing_items")) {
    return false;
  }

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_music_playlist_items_playlist_position "
      "ON music_playlist_items(playlist_id, position)",
      "CREATE INDEX IF NOT EXISTS idx_music_playlist_items_music_key "
      "ON music_playlist_items(music_key_type, music_key)",
      "CREATE INDEX IF NOT EXISTS idx_music_playlist_items_chart_path "
      "ON music_playlist_items(playlist_id, chart_path)",
      "CREATE INDEX IF NOT EXISTS idx_music_playlist_items_chart_md5 "
      "ON music_playlist_items(playlist_id, chart_md5)",
      "CREATE INDEX IF NOT EXISTS idx_music_playlist_items_chart_sha256 "
      "ON music_playlist_items(playlist_id, chart_sha256)",
      "CREATE INDEX IF NOT EXISTS idx_music_now_playing_position "
      "ON music_now_playing_items(position, id)",
      "CREATE INDEX IF NOT EXISTS idx_music_now_playing_music_key "
      "ON music_now_playing_items(music_key_type, music_key)",
      "CREATE INDEX IF NOT EXISTS idx_music_now_playing_chart_path "
      "ON music_now_playing_items(chart_path)",
      "CREATE INDEX IF NOT EXISTS idx_music_now_playing_chart_md5 "
      "ON music_now_playing_items(chart_md5)",
      "CREATE INDEX IF NOT EXISTS idx_music_now_playing_chart_sha256 "
      "ON music_now_playing_items(chart_sha256)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating music playlist index")) {
      return false;
    }
  }
  if (!migrateMusicPlaylistDatabaseSchema(db)) {
    return false;
  }
  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing music playlist schema setup", transactionError);
    return false;
  }
  if (!callerOwnsTransaction) {
    schemaDatabase = db;
  }
  return true;
}

int MusicPlaylistRepository::Impl::EnsurePlaylist(sqlite3 *db, const std::string &name) {
  if (!CreateTables(db)) {
    return 0;
  }

  std::string playlistName = trimCopy(name);
  if (playlistName.empty()) {
    playlistName = "My Playlist";
  }
  const char *insertQuery =
      "INSERT OR IGNORE INTO music_playlists (name) VALUES (@name)";
  SqliteStatementHandle insertStmt;
  if (!prepareSqliteStatementLogged(db, insertQuery, insertStmt,
                                    "preparing music playlist insert",
                                    logSqlErrorText)) {
    return 0;
  }
  bindSqliteText(insertStmt, 1, playlistName);
  int rc = sqlite3_step(insertStmt);
  if (rc != SQLITE_DONE) {
    logSqlError("inserting music playlist", db);
    return 0;
  }

  const char *selectQuery = "SELECT id FROM music_playlists WHERE name = @name";
  SqliteStatementHandle selectStmt;
  if (!prepareSqliteStatementLogged(db, selectQuery, selectStmt,
                                    "preparing music playlist select",
                                    logSqlErrorText)) {
    return 0;
  }
  bindSqliteText(selectStmt, 1, playlistName);
  if (sqlite3_step(selectStmt) == SQLITE_ROW) {
    return sqlite3_column_int(selectStmt, 0);
  }
  return 0;
}

bool MusicPlaylistRepository::Impl::RenamePlaylist(sqlite3 *db, int playlistId,
                                     const std::string &name) {
  if (playlistId <= 0 || !CreateTables(db)) {
    return false;
  }

  const std::string playlistName = trimCopy(name);
  if (playlistName.empty()) {
    return false;
  }

  const char *selectQuery = "SELECT name FROM music_playlists WHERE id = ?1";
  SqliteStatementHandle selectStmt;
  if (!prepareSqliteStatementLogged(
          db, selectQuery, selectStmt,
          "preparing music playlist rename select", logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(selectStmt, 1, playlistId);
  if (sqlite3_step(selectStmt) != SQLITE_ROW) {
    return false;
  }
  if (columnString(selectStmt, 0) == playlistName) {
    return true;
  }

  const char *updateQuery =
      "UPDATE music_playlists SET name = ?1, updated_at = CURRENT_TIMESTAMP "
      "WHERE id = ?2";
  SqliteStatementHandle updateStmt;
  if (!prepareSqliteStatementLogged(db, updateQuery, updateStmt,
                                    "preparing music playlist rename",
                                    logSqlErrorText)) {
    return false;
  }
  bindSqliteText(updateStmt, 1, playlistName);
  sqlite3_bind_int(updateStmt, 2, playlistId);
  int rc = sqlite3_step(updateStmt);
  if (rc != SQLITE_DONE) {
    logSqlError("renaming music playlist", db);
    return false;
  }
  return sqlite3_changes(db) > 0;
}

std::vector<MusicPlaylistInfo> MusicPlaylistRepository::Impl::SelectPlaylists(sqlite3 *db) {
  std::vector<MusicPlaylistInfo> playlists;
  if (!CreateTables(db)) {
    return playlists;
  }

  const char *query =
      "SELECT mp.id, mp.name, COUNT(mpi.id) AS track_count "
      "FROM music_playlists mp "
      "LEFT JOIN music_playlist_items mpi ON mpi.playlist_id = mp.id "
      "GROUP BY mp.id, mp.name "
      "ORDER BY mp.id";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting music playlists",
                                    logSqlErrorText)) {
    return playlists;
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    MusicPlaylistInfo playlist;
    playlist.id = sqlite3_column_int(stmt, 0);
    playlist.name = columnString(stmt, 1);
    playlist.trackCount = sqlite3_column_int(stmt, 2);
    playlists.push_back(std::move(playlist));
  }
  return playlists;
}

bool MusicPlaylistRepository::Impl::InsertTrack(sqlite3 *db, int playlistId,
                                  const bms_parser::ChartMeta &chartMeta) {
  if (playlistId <= 0 || !CreateTables(db)) {
    return false;
  }

  const auto identity = storedMusicTrackIdentity(chartMeta);
  if (identity.musicKey.empty()) {
    std::cerr << "Cannot insert music playlist track without a music key.\n";
    return false;
  }

  const int existingRowId =
      selectStoredMusicTrackRowId(db, playlistId, identity);
  if (existingRowId > 0) {
    if (!updateStoredMusicTrackRowIdentity(db, existingRowId, identity) ||
        !deleteDuplicateStoredMusicTrackRows(db, playlistId, existingRowId,
                                             identity)) {
      return false;
    }
    compactPlaylistPositions(db, playlistId);
    touchPlaylistUpdatedAt(db, playlistId);
    return true;
  }

  const char *query =
      "INSERT INTO music_playlist_items "
      "(playlist_id, position, music_key_type, music_key, chart_path, "
      "chart_md5, chart_sha256) "
      "VALUES (?1, (SELECT COALESCE(MAX(position) + 1, 0) "
      "FROM music_playlist_items WHERE playlist_id = ?1), ?2, ?3, ?4, ?5, ?6) "
      "ON CONFLICT(playlist_id, music_key_type, music_key) DO UPDATE SET "
      "chart_path = excluded.chart_path,"
      "chart_md5 = excluded.chart_md5,"
      "chart_sha256 = excluded.chart_sha256,"
      "added_at = CURRENT_TIMESTAMP";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, query, stmt, "preparing music playlist track insert",
          logSqlErrorText)) {
    return false;
  }

  sqlite3_bind_int(stmt, 1, playlistId);
  bindStoredMusicTrackIdentity(stmt, 2, identity);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    logSqlError("inserting music playlist track", db);
    return false;
  }

  touchPlaylistUpdatedAt(db, playlistId);
  return true;
}

bool MusicPlaylistRepository::Impl::DeleteTrack(sqlite3 *db, int playlistId,
                                  const bms_parser::ChartMeta &chartMeta,
                                  int storedItemId) {
  if (playlistId <= 0 || !CreateTables(db)) {
    return false;
  }

  std::string query = "DELETE FROM music_playlist_items "
                      "WHERE playlist_id = ?1 AND ";
  StoredMusicTrackIdentity identity;
  if (storedItemId > 0) {
    query += "id = ?2";
  } else {
    identity = storedMusicTrackIdentity(chartMeta);
    if (identity.musicKey.empty()) {
      std::cerr << "Cannot delete music playlist track without a music key.\n";
      return false;
    }
    query += storedMusicTrackRowPredicate(2);
  }

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, query, stmt, "preparing music playlist track delete",
          logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(stmt, 1, playlistId);
  if (storedItemId > 0) {
    sqlite3_bind_int(stmt, 2, storedItemId);
  } else {
    bindStoredMusicTrackIdentity(stmt, 2, identity);
  }
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    logSqlError("deleting music playlist track", db);
    return false;
  }

  const bool deleted = sqlite3_changes(db) > 0;
  if (deleted) {
    compactPlaylistPositions(db, playlistId);
    touchPlaylistUpdatedAt(db, playlistId);
  }
  return deleted;
}

bool MusicPlaylistRepository::Impl::MoveTrack(sqlite3 *db, int playlistId,
                                const bms_parser::ChartMeta &chartMeta,
                                int delta, int storedItemId) {
  if (playlistId <= 0 || delta == 0 || !CreateTables(db)) {
    return false;
  }

  std::string selectCurrentQuery =
      "SELECT id, position FROM music_playlist_items "
      "WHERE playlist_id = ?1 AND ";
  StoredMusicTrackIdentity identity;
  if (storedItemId > 0) {
    selectCurrentQuery += "id = ?2";
  } else {
    identity = storedMusicTrackIdentity(chartMeta);
    if (identity.musicKey.empty()) {
      std::cerr << "Cannot move music playlist track without a music key.\n";
      return false;
    }
    selectCurrentQuery += storedMusicTrackRowPredicate(2);
    selectCurrentQuery += " ORDER BY ";
    selectCurrentQuery += storedMusicTrackRowPreferenceOrderBy(2);
    selectCurrentQuery += ", position, id";
  }
  selectCurrentQuery += " LIMIT 1";
  SqliteStatementHandle currentStmt;
  if (!prepareSqliteStatementLogged(
          db, selectCurrentQuery, currentStmt,
          "preparing music playlist move select", logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(currentStmt, 1, playlistId);
  if (storedItemId > 0) {
    sqlite3_bind_int(currentStmt, 2, storedItemId);
  } else {
    bindStoredMusicTrackIdentity(currentStmt, 2, identity);
  }
  if (sqlite3_step(currentStmt) != SQLITE_ROW) {
    return false;
  }
  const int currentId = sqlite3_column_int(currentStmt, 0);
  const int currentPosition = sqlite3_column_int(currentStmt, 1);

  const char *selectNeighborQuery =
      delta < 0
          ? "SELECT id, position FROM music_playlist_items "
            "WHERE playlist_id = ?1 AND position < ?2 "
            "ORDER BY position DESC, id DESC LIMIT 1"
          : "SELECT id, position FROM music_playlist_items "
            "WHERE playlist_id = ?1 AND position > ?2 "
            "ORDER BY position ASC, id ASC LIMIT 1";
  SqliteStatementHandle neighborStmt;
  if (!prepareSqliteStatementLogged(
          db, selectNeighborQuery, neighborStmt,
          "preparing music playlist move neighbor select", logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(neighborStmt, 1, playlistId);
  sqlite3_bind_int(neighborStmt, 2, currentPosition);
  if (sqlite3_step(neighborStmt) != SQLITE_ROW) {
    return false;
  }
  const int neighborId = sqlite3_column_int(neighborStmt, 0);
  const int neighborPosition = sqlite3_column_int(neighborStmt, 1);

  const char *swapQuery =
      "UPDATE music_playlist_items SET position = "
      "CASE id WHEN ?1 THEN ?2 WHEN ?3 THEN ?4 ELSE position END "
      "WHERE id IN (?1, ?3)";
  SqliteStatementHandle swapStmt;
  if (!prepareSqliteStatementLogged(db, swapQuery, swapStmt,
                                    "preparing music playlist move swap",
                                    logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(swapStmt, 1, currentId);
  sqlite3_bind_int(swapStmt, 2, neighborPosition);
  sqlite3_bind_int(swapStmt, 3, neighborId);
  sqlite3_bind_int(swapStmt, 4, currentPosition);
  int rc = sqlite3_step(swapStmt);
  if (rc != SQLITE_DONE) {
    logSqlError("moving music playlist track", db);
    return false;
  }

  const bool moved = sqlite3_changes(db) > 0;
  if (moved) {
    touchPlaylistUpdatedAt(db, playlistId);
  }
  return moved;
}

bool MusicPlaylistRepository::Impl::ClearPlaylist(sqlite3 *db, int playlistId) {
  if (playlistId <= 0 || !CreateTables(db)) {
    return false;
  }

  const char *query = "DELETE FROM music_playlist_items WHERE playlist_id = ?1";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing music playlist clear",
                                    logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(stmt, 1, playlistId);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    logSqlError("clearing music playlist", db);
    return false;
  }

  touchPlaylistUpdatedAt(db, playlistId);
  return true;
}

bool MusicPlaylistRepository::Impl::DeletePlaylist(sqlite3 *db, int playlistId) {
  if (playlistId <= 0 || !CreateTables(db)) {
    return false;
  }

  const char *deleteItemsQuery =
      "DELETE FROM music_playlist_items WHERE playlist_id = ?1";
  SqliteStatementHandle deleteItemsStmt;
  if (!prepareSqliteStatementLogged(
          db, deleteItemsQuery, deleteItemsStmt,
          "preparing music playlist item delete", logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(deleteItemsStmt, 1, playlistId);
  int rc = sqlite3_step(deleteItemsStmt);
  if (rc != SQLITE_DONE) {
    logSqlError("deleting music playlist items", db);
    return false;
  }

  const char *deletePlaylistQuery =
      "DELETE FROM music_playlists WHERE id = ?1";
  SqliteStatementHandle deletePlaylistStmt;
  if (!prepareSqliteStatementLogged(
          db, deletePlaylistQuery, deletePlaylistStmt,
          "preparing music playlist delete", logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(deletePlaylistStmt, 1, playlistId);
  rc = sqlite3_step(deletePlaylistStmt);
  if (rc != SQLITE_DONE) {
    logSqlError("deleting music playlist", db);
    return false;
  }
  return sqlite3_changes(db) > 0;
}

MusicPlayerStateRecord MusicPlaylistRepository::Impl::SelectPlayerState(sqlite3 *db) {
  MusicPlayerStateRecord state;
  if (db == nullptr || !CreateTables(db)) {
    return state;
  }

  const char *query = "SELECT key, value FROM music_player_state";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting music player state",
                                    logSqlErrorText)) {
    return state;
  }

  std::unordered_map<std::string, std::string> values;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    values[columnString(stmt, 0)] = columnString(stmt, 1);
  }

  state.selectedPlaylistId = stateInt(values, "selected_playlist_id", 0);
  state.playlistCursorIndex = stateInt(values, "playlist_cursor_index", -1);
  state.queueCursorIndex = stateInt(values, "queue_cursor_index", -1);
  state.repeatMode = stateInt(values, "queue_repeat_mode", 2);
  state.queueDisplayName = stateString(values, "queue_display_name");
  return state;
}

bool MusicPlaylistRepository::Impl::SavePlayerState(sqlite3 *db,
                                      const MusicPlayerStateRecord &state) {
  if (db == nullptr || !CreateTables(db)) {
    return false;
  }

  const bool callerOwnsTransaction = sqlite3_get_autocommit(db) == 0;
  const char *beginQuery = callerOwnsTransaction
                               ? "SAVEPOINT asobmashow_music_state"
                               : "BEGIN";
  const char *commitQuery = callerOwnsTransaction
                                ? "RELEASE asobmashow_music_state"
                                : "COMMIT";
  const char *rollbackQuery =
      callerOwnsTransaction
          ? "ROLLBACK TO asobmashow_music_state; RELEASE "
            "asobmashow_music_state"
          : "ROLLBACK";
  std::string transactionError;
  SqliteTransactionHandle transaction(db, beginQuery, transactionError,
                                      commitQuery, rollbackQuery);
  if (!transaction.active()) {
    logSqlErrorText("beginning music player state save", transactionError);
    return false;
  }

  const char *query =
      "INSERT INTO music_player_state (key, value, updated_at) "
      "VALUES (?1, ?2, CURRENT_TIMESTAMP) "
      "ON CONFLICT(key) DO UPDATE SET "
      "value = excluded.value, updated_at = CURRENT_TIMESTAMP";
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "preparing music player state save",
                                    logSqlErrorText)) {
    return false;
  }

  const auto saveValue = [&](const char *key, const std::string &value) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bindSqliteText(stmt, 1, key);
    bindSqliteText(stmt, 2, value);
    const int stepRc = sqlite3_step(stmt);
    if (stepRc != SQLITE_DONE) {
      logSqlError("saving music player state", db);
      return false;
    }
    return true;
  };

  const bool saved =
      saveValue("selected_playlist_id",
                std::to_string(state.selectedPlaylistId)) &&
      saveValue("playlist_cursor_index",
                std::to_string(state.playlistCursorIndex)) &&
      saveValue("queue_cursor_index",
                std::to_string(state.queueCursorIndex)) &&
      saveValue("queue_repeat_mode", std::to_string(state.repeatMode)) &&
      saveValue("queue_display_name", state.queueDisplayName);
  if (!saved) {
    return false;
  }
  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing music player state save", transactionError);
    return false;
  }
  return true;
}

bool MusicPlaylistRepository::Impl::ReplaceNowPlayingTracks(
    sqlite3 *db, const std::vector<bms_parser::ChartMeta> &tracks) {
  if (db == nullptr || !CreateTables(db)) {
    return false;
  }

  const bool callerOwnsTransaction = sqlite3_get_autocommit(db) == 0;
  const char *beginQuery = callerOwnsTransaction
                               ? "SAVEPOINT asobmashow_now_playing"
                               : "BEGIN";
  const char *commitQuery = callerOwnsTransaction
                                ? "RELEASE asobmashow_now_playing"
                                : "COMMIT";
  const char *rollbackQuery =
      callerOwnsTransaction
          ? "ROLLBACK TO asobmashow_now_playing; RELEASE "
            "asobmashow_now_playing"
          : "ROLLBACK";
  std::string transactionError;
  SqliteTransactionHandle transaction(db, beginQuery, transactionError,
                                      commitQuery, rollbackQuery);
  if (!transaction.active()) {
    logSqlErrorText("beginning now playing save", transactionError);
    return false;
  }

  bool ok = execSql(db, "DELETE FROM music_now_playing_items",
                    "clearing now playing tracks");
  const char *insertQuery =
      "INSERT INTO music_now_playing_items "
      "(position, music_key_type, music_key, chart_path, chart_md5, "
      "chart_sha256) "
      "VALUES (?1, ?2, ?3, ?4, ?5, ?6)";
  SqliteStatementHandle insertStmt;
  if (ok) {
    if (!prepareSqliteStatementLogged(db, insertQuery, insertStmt,
                                      "preparing now playing insert",
                                      logSqlErrorText)) {
      ok = false;
    }
  }

  for (std::size_t i = 0; ok && i < tracks.size(); ++i) {
    const auto identity = storedMusicTrackIdentity(tracks[i]);
    if (identity.musicKey.empty()) {
      std::cerr << "Cannot save now playing track without a music key.\n";
      ok = false;
      break;
    }

    sqlite3_reset(insertStmt);
    sqlite3_clear_bindings(insertStmt);
    sqlite3_bind_int(insertStmt, 1, static_cast<int>(i));
    bindStoredMusicTrackIdentity(insertStmt, 2, identity);
    const int rc = sqlite3_step(insertStmt);
    if (rc != SQLITE_DONE) {
      logSqlError("saving now playing track", db);
      ok = false;
    }
  }

  if (!ok) {
    return false;
  }

  if (!transaction.commit(transactionError)) {
    logSqlErrorText("committing now playing save", transactionError);
    return false;
  }
  return true;
}

void MusicPlaylistRepository::Impl::SelectLibraryTracks(
    sqlite3 *db, std::vector<MusicTrackRecord> &tracks) {
  if (db == nullptr || !CreateTables(db)) {
    return;
  }

  const std::string preferredChart =
      asobmshow::chart_sql::preferredChartPredicate("cm", kChartMetaTable);
  const std::string preferredRepresentative =
      asobmshow::chart_sql::preferredChartPredicate("rep", kChartMetaTable);
  std::string representativeOrder = musicRepresentativeOrderBy("rep");
  representativeOrder += ", ";
  representativeOrder += chartArtworkOrderBy("rep");
  representativeOrder +=
      ", rep.total_notes DESC, rep.length DESC, ";
  representativeOrder += chartSourceOrderBy("rep");
  representativeOrder += ", rep.title COLLATE NOCASE, rep.path";

  std::string query =
      "WITH folder_groups AS ("
      "SELECT cm.folder AS music_key, COUNT(*) AS music_chart_count, "
      "(SELECT rep.path FROM ";
  query += kChartMetaTable;
  query += " rep WHERE rep.folder = cm.folder AND ";
  query += preferredRepresentative;
  query += " ORDER BY ";
  query += representativeOrder;
  query += " LIMIT 1) AS representative_path FROM ";
  query += kChartMetaTable;
  query += " cm WHERE cm.folder IS NOT NULL AND cm.folder != '' AND ";
  query += preferredChart;
  query += " GROUP BY cm.folder), path_groups AS ("
           "SELECT cm.path AS music_key, 1 AS music_chart_count, "
           "cm.path AS representative_path FROM ";
  query += kChartMetaTable;
  query += " cm WHERE (cm.folder IS NULL OR cm.folder = '') AND ";
  query += preferredChart;
  query += "), music_groups AS ("
           "SELECT music_key, music_chart_count, representative_path "
           "FROM folder_groups UNION ALL "
           "SELECT music_key, music_chart_count, representative_path "
           "FROM path_groups) SELECT ";
  query += kChartMetaSelectColumns;
  query += ", mg.music_chart_count FROM music_groups mg JOIN ";
  query += kChartMetaTable;
  query +=
      " cm ON cm.path = mg.representative_path "
      "ORDER BY cm.title COLLATE NOCASE, cm.path";

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting music library tracks",
                                    logSqlErrorText)) {
    return;
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    MusicTrackRecord record;
    record.representativeChart = readChartMeta(stmt);
    record.chartCount =
        std::max(1, sqlite3_column_int(stmt, kChartMetaColumnCount));
    record.useChartPathIdentity = false;
    tracks.push_back(std::move(record));
  }
}

void MusicPlaylistRepository::Impl::SelectLibraryGroupTracks(
    sqlite3 *db, const bms_parser::ChartMeta &chartMeta,
    std::vector<MusicTrackRecord> &tracks) {
  if (db == nullptr || !CreateTables(db)) {
    return;
  }

  std::filesystem::path groupPath = chartMeta.Folder;
  const bool useFolder = !groupPath.empty();
  if (!useFolder) {
    groupPath = chartMeta.BmsPath;
  }
  const std::string groupKey =
      chart_storage_identity::StoredPathText(groupPath);
  if (groupKey.empty()) {
    return;
  }

  std::string query = "SELECT ";
  query += kChartMetaSelectColumns;
  query += " FROM ";
  query += kChartMetaTable;
  query += " cm WHERE ";
  query += useFolder ? "cm.folder = ?1 AND " : "cm.path = ?1 AND ";
  query += asobmshow::chart_sql::preferredChartPredicate("cm",
                                                         kChartMetaTable);
  query += " ORDER BY ";
  query += musicRepresentativeOrderBy("cm");
  query += ", cm.title COLLATE NOCASE, cm.subtitle COLLATE NOCASE, "
           "cm.difficulty, cm.level, cm.total_notes DESC, cm.length DESC, ";
  query += chartSourceOrderBy("cm");

  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting music library group tracks",
                                    logSqlErrorText)) {
    return;
  }
  bindSqliteText(stmt, 1, groupKey);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    MusicTrackRecord record;
    record.representativeChart = readChartMeta(stmt);
    record.chartCount = 1;
    record.useChartPathIdentity = true;
    tracks.push_back(std::move(record));
  }
}

void MusicPlaylistRepository::Impl::SelectNowPlayingTracks(
    sqlite3 *db, std::vector<MusicTrackRecord> &tracks) {
  if (db == nullptr || !CreateTables(db)) {
    return;
  }

  const std::string query = storedMusicTrackSelectQuery(
      {"music_now_playing_items", "mnp", "queue_position",
       "queue_music_key_type", ""});
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting now playing tracks",
                                    logSqlErrorText)) {
    return;
  }
  readStoredMusicTrackRows(stmt, tracks);
}

void MusicPlaylistRepository::Impl::SelectTracks(sqlite3 *db, int playlistId,
                                   std::vector<MusicTrackRecord> &tracks) {
  if (playlistId <= 0 || !CreateTables(db)) {
    return;
  }

  const std::string query = storedMusicTrackSelectQuery(
      {"music_playlist_items", "mpi", "playlist_position",
       "playlist_music_key_type", "mpi.playlist_id = ?1 AND "});
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(db, query, stmt,
                                    "selecting music playlist tracks",
                                    logSqlErrorText)) {
    return;
  }
  sqlite3_bind_int(stmt, 1, playlistId);
  readStoredMusicTrackRows(stmt, tracks);
}

MusicPlaylistRepository::MusicPlaylistRepository()
    : MusicPlaylistRepository(
          Utils::GetDocumentsPath("db") / kPlaylistDatabaseFileName,
          Utils::GetDocumentsPath("db") / kChartDatabaseFileName) {}

MusicPlaylistRepository::MusicPlaylistRepository(
    std::filesystem::path databasePath,
    std::filesystem::path chartDatabasePath)
    : impl_(std::make_unique<Impl>(std::move(databasePath),
                                   std::move(chartDatabasePath))) {}

MusicPlaylistRepository::~MusicPlaylistRepository() = default;

bool MusicPlaylistRepository::EnsureReady() {
  std::lock_guard lock(impl_->mutex);
  return impl_->EnsureDatabase();
}

void MusicPlaylistRepository::Shutdown() {
  std::lock_guard lock(impl_->mutex);
  impl_->Shutdown();
}

int MusicPlaylistRepository::EnsurePlaylist(const std::string &name) {
  std::lock_guard lock(impl_->mutex);
  return impl_->EnsureDatabase()
             ? impl_->EnsurePlaylist(impl_->database, name)
             : 0;
}

int MusicPlaylistRepository::EnsurePlaylistWithTracks(
    const std::string &name,
    const std::vector<bms_parser::ChartMeta> &tracks) {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->EnsureDatabase()) {
    return 0;
  }

  std::string transactionError;
  SqliteTransactionHandle transaction(impl_->database, "BEGIN",
                                      transactionError);
  if (!transaction.active()) {
    return 0;
  }
  const int playlistId = impl_->EnsurePlaylist(impl_->database, name);
  if (playlistId <= 0) {
    return 0;
  }
  for (const auto &track : tracks) {
    if (!impl_->InsertTrack(impl_->database, playlistId, track)) {
      return 0;
    }
  }
  return transaction.commit(transactionError) ? playlistId : 0;
}

bool MusicPlaylistRepository::RenamePlaylist(int playlistId,
                                             const std::string &name) {
  std::lock_guard lock(impl_->mutex);
  return impl_->EnsureDatabase() &&
         impl_->RenamePlaylist(impl_->database, playlistId, name);
}

std::vector<MusicPlaylistInfo> MusicPlaylistRepository::SelectPlaylists() {
  std::lock_guard lock(impl_->mutex);
  return impl_->EnsureDatabase()
             ? impl_->SelectPlaylists(impl_->database)
             : std::vector<MusicPlaylistInfo>{};
}

bool MusicPlaylistRepository::InsertTrack(
    int playlistId, const bms_parser::ChartMeta &chartMeta) {
  std::lock_guard lock(impl_->mutex);
  return impl_->EnsureDatabase() &&
         impl_->InsertTrack(impl_->database, playlistId, chartMeta);
}

bool MusicPlaylistRepository::DeleteTrack(
    int playlistId, const bms_parser::ChartMeta &chartMeta, int storedItemId) {
  std::lock_guard lock(impl_->mutex);
  return impl_->EnsureDatabase() &&
         impl_->DeleteTrack(impl_->database, playlistId, chartMeta,
                            storedItemId);
}

bool MusicPlaylistRepository::MoveTrack(
    int playlistId, const bms_parser::ChartMeta &chartMeta, int delta,
    int storedItemId) {
  std::lock_guard lock(impl_->mutex);
  return impl_->EnsureDatabase() &&
         impl_->MoveTrack(impl_->database, playlistId, chartMeta, delta,
                          storedItemId);
}

bool MusicPlaylistRepository::ClearPlaylist(int playlistId) {
  std::lock_guard lock(impl_->mutex);
  return impl_->EnsureDatabase() &&
         impl_->ClearPlaylist(impl_->database, playlistId);
}

bool MusicPlaylistRepository::DeletePlaylist(int playlistId) {
  std::lock_guard lock(impl_->mutex);
  return impl_->EnsureDatabase() &&
         impl_->DeletePlaylist(impl_->database, playlistId);
}

MusicPlayerStateRecord MusicPlaylistRepository::SelectPlayerState() {
  std::lock_guard lock(impl_->mutex);
  return impl_->EnsureDatabase()
             ? impl_->SelectPlayerState(impl_->database)
             : MusicPlayerStateRecord{};
}

bool MusicPlaylistRepository::SavePlayerState(
    const MusicPlayerStateRecord &state) {
  std::lock_guard lock(impl_->mutex);
  return impl_->EnsureDatabase() &&
         impl_->SavePlayerState(impl_->database, state);
}

bool MusicPlaylistRepository::ReplaceNowPlayingTracks(
    const std::vector<bms_parser::ChartMeta> &tracks) {
  std::lock_guard lock(impl_->mutex);
  return impl_->EnsureDatabase() &&
         impl_->ReplaceNowPlayingTracks(impl_->database, tracks);
}

bool MusicPlaylistRepository::SaveNowPlayingState(
    const std::vector<bms_parser::ChartMeta> &tracks,
    const MusicPlayerStateRecord &state) {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->EnsureDatabase()) {
    return false;
  }

  std::string transactionError;
  SqliteTransactionHandle transaction(impl_->database, "BEGIN",
                                      transactionError);
  if (!transaction.active()) {
    return false;
  }
  if (!impl_->ReplaceNowPlayingTracks(impl_->database, tracks) ||
      !impl_->SavePlayerState(impl_->database, state)) {
    return false;
  }
  return transaction.commit(transactionError);
}

std::vector<MusicTrackRecord>
MusicPlaylistRepository::SelectLibraryTracks() {
  std::lock_guard lock(impl_->mutex);
  std::vector<MusicTrackRecord> tracks;
  if (impl_->EnsureDatabase()) {
    impl_->SelectLibraryTracks(impl_->database, tracks);
  }
  return tracks;
}

std::vector<MusicTrackRecord>
MusicPlaylistRepository::SelectLibraryGroupTracks(
    const bms_parser::ChartMeta &chartMeta) {
  std::lock_guard lock(impl_->mutex);
  std::vector<MusicTrackRecord> tracks;
  if (impl_->EnsureDatabase()) {
    impl_->SelectLibraryGroupTracks(impl_->database, chartMeta, tracks);
  }
  return tracks;
}

std::vector<MusicTrackRecord>
MusicPlaylistRepository::SelectNowPlayingTracks() {
  std::lock_guard lock(impl_->mutex);
  std::vector<MusicTrackRecord> tracks;
  if (impl_->EnsureDatabase()) {
    impl_->SelectNowPlayingTracks(impl_->database, tracks);
  }
  return tracks;
}

std::vector<MusicTrackRecord>
MusicPlaylistRepository::SelectTracks(int playlistId) {
  std::lock_guard lock(impl_->mutex);
  std::vector<MusicTrackRecord> tracks;
  if (impl_->EnsureDatabase()) {
    impl_->SelectTracks(impl_->database, playlistId, tracks);
  }
  return tracks;
}
