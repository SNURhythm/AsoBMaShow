#include "MusicPlaylistDB.h"

#include "../BmsMetadataText.h"
#include "../ChartMetaSql.h"
#include "../ChartSqlExpressions.h"
#include "../SqliteRAII.h"
#include "../Utils.h"
#include "../path.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace {

constexpr const char *kPlaylistDatabaseFileName = "music_playlist.db";
constexpr const char *kChartDatabaseFileName = "chart.db";
constexpr const char *kChartDatabaseSchema = "chart_library";
constexpr const char *kChartMetaTable = "chart_library.chart_meta";

using asobmshow::bms_metadata::normalizedHash;
using asobmshow::bms_metadata::trimCopy;
using asobmshow::chart_sql::chartArtworkOrderBy;
using asobmshow::chart_sql::chartIdentityMatchPredicate;
using asobmshow::chart_sql::chartIdentityPreferenceOrderBy;
using asobmshow::chart_sql::chartSourceOrderBy;
using asobmshow::chart_sql::kChartMetaColumnCount;
using asobmshow::chart_sql::kChartMetaSelectColumns;
using asobmshow::chart_sql::kMaxSqlIntegerText;

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

bool attachChartDatabase(sqlite3 *db, const std::filesystem::path &path) {
  if (const auto error = attachSqliteDatabase(db, path, kChartDatabaseSchema)) {
    logSqlErrorText("attaching chart database", *error);
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
    ChartDBHelper::ToAbsolutePath(path);
  }
  return path;
}

std::filesystem::path relativePathFromColumn(sqlite3_stmt *stmt, int idx) {
  return pathFromDbText(columnString(stmt, idx));
}

std::string storedPathText(std::filesystem::path path) {
  if (path.empty()) {
    return "";
  }
  ChartDBHelper::ToRelativePath(path);
  path = path.lexically_normal();
  return fspath_to_utf8(path);
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
  identity.chartPath = storedPathText(chartMeta.BmsPath);
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

std::string storedMusicTrackRowPredicate(int firstIndex) {
  const std::string keyTypeParam = sqlParam(firstIndex);
  const std::string musicKeyParam = sqlParam(firstIndex + 1);
  const std::string chartPathParam = sqlParam(firstIndex + 2);
  const std::string md5Param = sqlParam(firstIndex + 3);
  const std::string sha256Param = sqlParam(firstIndex + 4);
  return "((music_key_type = " + keyTypeParam + " AND music_key = " +
         musicKeyParam + ") OR (" + sha256Param +
         " != '' AND lower(trim(chart_sha256)) = " + sha256Param + ") OR (" +
         md5Param + " != '' AND lower(trim(chart_md5)) = " + md5Param +
         ") OR (" + chartPathParam + " != '' AND chart_path = " +
         chartPathParam + "))";
}

std::string storedMusicTrackRowPreferenceOrderBy(int firstIndex) {
  const std::string keyTypeParam = sqlParam(firstIndex);
  const std::string musicKeyParam = sqlParam(firstIndex + 1);
  const std::string chartPathParam = sqlParam(firstIndex + 2);
  const std::string md5Param = sqlParam(firstIndex + 3);
  const std::string sha256Param = sqlParam(firstIndex + 4);
  return "CASE WHEN music_key_type = " + keyTypeParam + " AND music_key = " +
         musicKeyParam + " THEN 0 WHEN " + sha256Param +
         " != '' AND lower(trim(chart_sha256)) = " + sha256Param +
         " THEN 1 WHEN " + md5Param +
         " != '' AND lower(trim(chart_md5)) = " + md5Param + " THEN 2 WHEN " +
         chartPathParam + " != '' AND chart_path = " + chartPathParam +
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

std::string
storedMusicTrackSelectQuery(const StoredMusicTrackSelectQuery &config) {
  std::string query = "SELECT ";
  query += kChartMetaSelectColumns;
  query += ", cm.music_chart_count, cm.";
  query += config.keyTypeAlias;
  query += " FROM (SELECT cm.*, ";
  query += config.itemAlias;
  query += ".position AS ";
  query += config.positionAlias;
  query += ", COUNT(*) OVER (PARTITION BY ";
  query += config.itemAlias;
  query += ".id) AS music_chart_count, ";
  query += config.itemAlias;
  query += ".music_key_type AS ";
  query += config.keyTypeAlias;
  query += ", ROW_NUMBER() OVER (PARTITION BY ";
  query += config.itemAlias;
  query += ".id ORDER BY ";
  query += chartArtworkOrderBy("cm");
  query += ", ";
  query += chartIdentityPreferenceOrderBy(config.itemAlias, "cm");
  query += ", total_notes DESC, length DESC, ";
  query += chartSourceOrderBy("cm");
  query += ", title COLLATE NOCASE, path) AS music_rank FROM ";
  query += config.itemTable;
  query += " ";
  query += config.itemAlias;
  query += " JOIN ";
  query += kChartMetaTable;
  query += " cm ON ";
  query += config.itemAlias;
  query += ".music_key_type = 'path' AND ";
  query += chartIdentityMatchPredicate(config.itemAlias, "cm");
  query += " WHERE ";
  if (config.wherePrefix != nullptr) {
    query += config.wherePrefix;
  }
  query += asobmshow::chart_sql::preferredChartPredicate("cm",
                                                         kChartMetaTable);
  query += ") cm WHERE cm.music_rank = 1 ORDER BY cm.";
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
    record.useChartPathIdentity =
        columnString(stmt, kChartMetaColumnCount + 1) == "path";
    tracks.push_back(std::move(record));
  }
}

} // namespace

sqlite3 *MusicPlaylistDB::Connect() {
  std::filesystem::path directory = Utils::GetDocumentsPath("db");
  std::error_code directoryError;
  if (!Utils::EnsureDirectoryExists(directory, directoryError)) {
    std::cerr << "Can't create music playlist database directory "
              << fspath_to_utf8(directory) << ": " << directoryError.message()
              << "\n";
    return nullptr;
  }
  const std::filesystem::path playlistPath =
      directory / kPlaylistDatabaseFileName;
  const std::filesystem::path chartPath = directory / kChartDatabaseFileName;

  std::string openError;
  sqlite3 *db = openSqliteDatabase(playlistPath, openError);
  if (db == nullptr) {
    std::cerr << "Can't open music playlist database: " << openError << "\n";
    return nullptr;
  }

  if (const auto pragmaError = applySqlitePragmas(
          db, {"PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL"})) {
    std::cerr << "Could not configure music playlist database: "
              << *pragmaError << "\n";
  }
  registerMusicPlaylistSqliteFunctions(db);
  attachChartDatabase(db, chartPath);
  return db;
}

void MusicPlaylistDB::Close(sqlite3 *db) {
  closeSqliteDatabase(db);
}

bool MusicPlaylistDB::CreateTables(sqlite3 *db) {
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

  const char *indexes[] = {
      "CREATE INDEX IF NOT EXISTS idx_music_playlist_items_playlist_position "
      "ON music_playlist_items(playlist_id, position)",
      "CREATE INDEX IF NOT EXISTS idx_music_playlist_items_music_key "
      "ON music_playlist_items(music_key_type, music_key)",
      "CREATE INDEX IF NOT EXISTS idx_music_now_playing_position "
      "ON music_now_playing_items(position, id)",
      "CREATE INDEX IF NOT EXISTS idx_music_now_playing_music_key "
      "ON music_now_playing_items(music_key_type, music_key)",
  };
  for (const auto *indexQuery : indexes) {
    if (!execSql(db, indexQuery, "creating music playlist index")) {
      return false;
    }
  }
  return true;
}

int MusicPlaylistDB::EnsurePlaylist(sqlite3 *db, const std::string &name) {
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

bool MusicPlaylistDB::RenamePlaylist(sqlite3 *db, int playlistId,
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

std::vector<MusicPlaylistInfo> MusicPlaylistDB::SelectPlaylists(sqlite3 *db) {
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

bool MusicPlaylistDB::InsertTrack(sqlite3 *db, int playlistId,
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

bool MusicPlaylistDB::DeleteTrack(sqlite3 *db, int playlistId,
                                  const bms_parser::ChartMeta &chartMeta) {
  if (playlistId <= 0 || !CreateTables(db)) {
    return false;
  }

  const auto identity = storedMusicTrackIdentity(chartMeta);
  if (identity.musicKey.empty()) {
    std::cerr << "Cannot delete music playlist track without a music key.\n";
    return false;
  }

  std::string query =
      "DELETE FROM music_playlist_items WHERE playlist_id = ?1 AND ";
  query += storedMusicTrackRowPredicate(2);
  SqliteStatementHandle stmt;
  if (!prepareSqliteStatementLogged(
          db, query, stmt, "preparing music playlist track delete",
          logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(stmt, 1, playlistId);
  bindStoredMusicTrackIdentity(stmt, 2, identity);
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

bool MusicPlaylistDB::MoveTrack(sqlite3 *db, int playlistId,
                                const bms_parser::ChartMeta &chartMeta,
                                int delta) {
  if (playlistId <= 0 || delta == 0 || !CreateTables(db)) {
    return false;
  }

  const auto identity = storedMusicTrackIdentity(chartMeta);
  if (identity.musicKey.empty()) {
    std::cerr << "Cannot move music playlist track without a music key.\n";
    return false;
  }

  std::string selectCurrentQuery =
      "SELECT id, position FROM music_playlist_items WHERE playlist_id = ?1 "
      "AND ";
  selectCurrentQuery += storedMusicTrackRowPredicate(2);
  selectCurrentQuery += " ORDER BY ";
  selectCurrentQuery += storedMusicTrackRowPreferenceOrderBy(2);
  selectCurrentQuery += ", position, id LIMIT 1";
  SqliteStatementHandle currentStmt;
  if (!prepareSqliteStatementLogged(
          db, selectCurrentQuery, currentStmt,
          "preparing music playlist move select", logSqlErrorText)) {
    return false;
  }
  sqlite3_bind_int(currentStmt, 1, playlistId);
  bindStoredMusicTrackIdentity(currentStmt, 2, identity);
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

bool MusicPlaylistDB::ClearPlaylist(sqlite3 *db, int playlistId) {
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

bool MusicPlaylistDB::DeletePlaylist(sqlite3 *db, int playlistId) {
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

MusicPlayerStateRecord MusicPlaylistDB::SelectPlayerState(sqlite3 *db) {
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

bool MusicPlaylistDB::SavePlayerState(sqlite3 *db,
                                      const MusicPlayerStateRecord &state) {
  if (db == nullptr || !CreateTables(db)) {
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

  return saveValue("selected_playlist_id",
                   std::to_string(state.selectedPlaylistId)) &&
         saveValue("playlist_cursor_index",
                   std::to_string(state.playlistCursorIndex)) &&
         saveValue("queue_cursor_index",
                   std::to_string(state.queueCursorIndex)) &&
         saveValue("queue_repeat_mode", std::to_string(state.repeatMode)) &&
         saveValue("queue_display_name", state.queueDisplayName);
}

bool MusicPlaylistDB::ReplaceNowPlayingTracks(
    sqlite3 *db, const std::vector<bms_parser::ChartMeta> &tracks) {
  if (db == nullptr || !CreateTables(db)) {
    return false;
  }

  std::string transactionError;
  SqliteTransactionHandle transaction(db, "BEGIN IMMEDIATE", transactionError);
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

void MusicPlaylistDB::SelectLibraryTracks(
    sqlite3 *db, std::vector<MusicTrackRecord> &tracks) {
  if (db == nullptr || !CreateTables(db)) {
    return;
  }

  const char *musicKey = "COALESCE(NULLIF(cm.folder, ''), cm.path)";
  std::string query = "SELECT ";
  query += kChartMetaSelectColumns;
  query += ", cm.music_chart_count FROM (SELECT cm.*, "
           "COUNT(*) OVER (PARTITION BY ";
  query += musicKey;
  query += ") AS music_chart_count, "
           "ROW_NUMBER() OVER (PARTITION BY ";
  query += musicKey;
  query += " ORDER BY ";
  query += musicRepresentativeOrderBy("cm");
  query += ", ";
  query += chartArtworkOrderBy("cm");
  query += ", total_notes DESC, length DESC, ";
  query += chartSourceOrderBy("cm");
  query += ", title COLLATE NOCASE, path) AS music_rank FROM ";
  query += kChartMetaTable;
  query += " cm WHERE ";
  query += asobmshow::chart_sql::preferredChartPredicate("cm",
                                                         kChartMetaTable);
  query += ") cm WHERE cm.music_rank = 1 "
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

void MusicPlaylistDB::SelectLibraryGroupTracks(
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
  const std::string groupKey = storedPathText(groupPath);
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

void MusicPlaylistDB::SelectNowPlayingTracks(
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

void MusicPlaylistDB::SelectTracks(sqlite3 *db, int playlistId,
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
