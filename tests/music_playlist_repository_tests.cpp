#include "../src/repositories/MusicPlaylistRepository.h"
#include "../src/repositories/SqliteRAII.h"
#include "RepositorySqliteTestSupport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kChartMd5 = "0123456789abcdef0123456789abcdef";
constexpr std::string_view kChartSha256 =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic<unsigned long long> sequence{0};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-music-repository-" + std::to_string(nonce) + "-" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void execOrAbort(sqlite3 *database, const std::string &sql) {
  char *error = nullptr;
  const int result = sqlite3_exec(database, sql.c_str(), nullptr, nullptr,
                                  &error);
  if (result != SQLITE_OK) {
    sqlite3_free(error);
    std::abort();
  }
}

SqliteConnectionHandle openDatabase(const std::filesystem::path &path) {
  std::filesystem::create_directories(path.parent_path());
  sqlite3 *database = nullptr;
  assert(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK);
  return SqliteConnectionHandle(database);
}

int queryInt(sqlite3 *database, std::string_view sql) {
  SqliteStatementHandle statement;
  assert(prepareSqliteStatement(database, std::string(sql), statement) ==
         SQLITE_OK);
  assert(sqlite3_step(statement.get()) == SQLITE_ROW);
  return sqlite3_column_int(statement.get(), 0);
}

bool columnExists(sqlite3 *database, std::string_view table,
                  std::string_view column) {
  SqliteStatementHandle statement;
  const std::string sql = "PRAGMA table_info(\"" + std::string(table) +
                          "\")";
  assert(prepareSqliteStatement(database, sql, statement) == SQLITE_OK);
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    if (sqliteColumnString(statement.get(), 1) == column) {
      return true;
    }
  }
  return false;
}

bool indexExists(sqlite3 *database, std::string_view name) {
  SqliteStatementHandle statement;
  assert(prepareSqliteStatement(
             database,
             "SELECT 1 FROM sqlite_master WHERE type='index' AND name=?1",
             statement) == SQLITE_OK);
  bindSqliteText(statement.get(), 1, std::string(name));
  return sqlite3_step(statement.get()) == SQLITE_ROW;
}

bms_parser::ChartMeta
createChartDatabase(const std::filesystem::path &databasePath) {
  auto database = openDatabase(databasePath);
  execOrAbort(
      database.get(),
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
      "has_document INTEGER NOT NULL DEFAULT 0,"
      "has_bpm_stop INTEGER NOT NULL DEFAULT 0,"
      "has_scroll_change INTEGER NOT NULL DEFAULT 0,"
      "add_date INTEGER NOT NULL DEFAULT 0,"
      "total_landmine_notes INTEGER NOT NULL DEFAULT 0,"
      "has_random_sequence INTEGER NOT NULL DEFAULT 0,"
      "most_prevalent_bpm REAL NOT NULL DEFAULT 0,"
      "has_bga INTEGER NOT NULL DEFAULT 0,"
      "rank_type INTEGER NOT NULL DEFAULT 0,"
      "source_priority INTEGER,"
      "source_archive_size INTEGER"
      ")");

  bms_parser::ChartMeta chartMeta;
  chartMeta.BmsPath = databasePath.parent_path() / "chart.bms";
  chartMeta.Folder = databasePath.parent_path();
  chartMeta.MD5 = kChartMd5;
  chartMeta.SHA256 = kChartSha256;
  chartMeta.Title = "Test Track";
  chartMeta.Artist = "Test Artist";
  chartMeta.TotalNotes = 100;

  SqliteStatementHandle insert;
  assert(prepareSqliteStatement(
             database.get(),
             "INSERT INTO chart_meta("
             "path,md5,sha256,title,subtitle,genre,artist,sub_artist,folder,"
             "stage_file,banner,back_bmp,preview,level,difficulty,total,"
             "has_total,bpm,max_bpm,min_bpm,length,rank,player,keys,"
             "total_notes,total_long_notes,total_scratch_notes,"
             "total_backspin_notes,ln_mode,source_priority,"
             "source_archive_size) VALUES("
             "?1,?2,?3,'Test Track','','Genre','Test Artist','',?4,"
             "'','','','',7,2,100,1,120,120,120,60000000,2,1,7,"
             "100,0,0,0,0,0,0)",
             insert) == SQLITE_OK);
  bindSqliteText(insert.get(), 1, chartMeta.BmsPath.string());
  bindSqliteText(insert.get(), 2, chartMeta.MD5);
  bindSqliteText(insert.get(), 3, chartMeta.SHA256);
  bindSqliteText(insert.get(), 4, chartMeta.Folder.string());
  assert(sqlite3_step(insert.get()) == SQLITE_DONE);
  return chartMeta;
}

std::atomic<int> *connectionOpenCount = nullptr;
sqlite3 *lastOpenedConnection = nullptr;

int countConnectionOpen(sqlite3 *database, char **, const sqlite3_api_routines *) {
  assert(connectionOpenCount != nullptr);
  connectionOpenCount->fetch_add(1, std::memory_order_relaxed);
  lastOpenedConnection = database;
  return SQLITE_OK;
}

class ScopedConnectionOpenCounter {
public:
  explicit ScopedConnectionOpenCounter(std::atomic<int> &counter) {
    assert(connectionOpenCount == nullptr);
    connectionOpenCount = &counter;
    sqlite3_reset_auto_extension();
    assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(
               countConnectionOpen)) == SQLITE_OK);
  }

  ~ScopedConnectionOpenCounter() {
    sqlite3_reset_auto_extension();
    connectionOpenCount = nullptr;
  }

private:
  ScopedConnectionOpenCounter(const ScopedConnectionOpenCounter &) = delete;
  ScopedConnectionOpenCounter &
  operator=(const ScopedConnectionOpenCounter &) = delete;
};

void testRoundTripAndConnectionReuse() {
  TempDirectory temporary;
  const auto playlistPath = temporary.path() / "music_playlist.db";
  const auto chartPath = temporary.path() / "chart.db";
  const bms_parser::ChartMeta chartMeta = createChartDatabase(chartPath);

  std::atomic<int> connectionCount{0};
  ScopedConnectionOpenCounter countConnections(connectionCount);
  MusicPlaylistRepository repository(playlistPath, chartPath);
  assert(repository.EnsureReady());
  const int readyConnectionCount = connectionCount.load();
  assert(readyConnectionCount > 0);
  assert(repository.EnsureReady());

  const int playlistId = repository.EnsurePlaylist("My Playlist");
  assert(playlistId > 0);
  assert(repository.InsertTrack(playlistId, chartMeta));
  const auto playlists = repository.SelectPlaylists();
  assert(playlists.size() == 1);
  assert(playlists.front().trackCount == 1);
  assert(repository.RenamePlaylist(playlistId, "Renamed Playlist"));

  const auto tracks = repository.SelectTracks(playlistId);
  assert(tracks.size() == 1);
  assert(tracks.front().representativeChart.Title == "Test Track");
  assert(tracks.front().representativeChart.Rank == 2);
  assert(tracks.front().representativeChart.RankType ==
         bms_parser::JudgeRankType::BmsRank);
  assert(repository.SelectLibraryTracks().size() == 1);
  assert(repository.SelectLibraryGroupTracks(chartMeta).size() == 1);
  const int batchPlaylistId =
      repository.EnsurePlaylistWithTracks("Batch Playlist", {chartMeta});
  assert(batchPlaylistId > 0);
  assert(repository.SelectTracks(batchPlaylistId).size() == 1);

  const MusicPlayerStateRecord state{.selectedPlaylistId = playlistId,
                                     .playlistCursorIndex = 3,
                                     .queueCursorIndex = 2,
                                     .repeatMode = 1,
                                     .queueDisplayName = "Queue"};
  assert(repository.SavePlayerState(state));
  const MusicPlayerStateRecord loadedState = repository.SelectPlayerState();
  assert(loadedState.selectedPlaylistId == state.selectedPlaylistId);
  assert(loadedState.playlistCursorIndex == state.playlistCursorIndex);
  assert(loadedState.queueCursorIndex == state.queueCursorIndex);
  assert(loadedState.repeatMode == state.repeatMode);
  assert(loadedState.queueDisplayName == state.queueDisplayName);

  assert(repository.ReplaceNowPlayingTracks({chartMeta}));
  assert(repository.SaveNowPlayingState({chartMeta}, state));
  assert(repository.SelectNowPlayingTracks().size() == 1);
  assert(!repository.MoveTrack(playlistId, chartMeta, -1));
  assert(repository.DeleteTrack(playlistId, chartMeta));
  assert(repository.InsertTrack(playlistId, chartMeta));
  assert(repository.ClearPlaylist(playlistId));
  assert(repository.DeletePlaylist(playlistId));
  assert(repository.DeletePlaylist(batchPlaylistId));
  assert(connectionCount.load() == readyConnectionCount);

  repository.Shutdown();
  assert(repository.EnsureReady());
  assert(connectionCount.load() > readyConnectionCount);
  repository.Shutdown();
}

void createVersionZeroPlaylistDatabase(
    const std::filesystem::path &databasePath,
    const bms_parser::ChartMeta &chartMeta) {
  auto database = openDatabase(databasePath);
  execOrAbort(
      database.get(),
      "CREATE TABLE music_playlists("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL UNIQUE,"
      "created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
      "updated_at TEXT DEFAULT CURRENT_TIMESTAMP);"
      "CREATE TABLE music_playlist_items("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,playlist_id INTEGER NOT NULL,"
      "position INTEGER NOT NULL,music_key_type TEXT NOT NULL,"
      "music_key TEXT NOT NULL,added_at TEXT DEFAULT CURRENT_TIMESTAMP,"
      "UNIQUE(playlist_id,music_key_type,music_key));"
      "CREATE TABLE music_player_state("
      "key TEXT PRIMARY KEY,value TEXT NOT NULL,"
      "updated_at TEXT DEFAULT CURRENT_TIMESTAMP);"
      "CREATE TABLE music_now_playing_items("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,position INTEGER NOT NULL,"
      "music_key_type TEXT NOT NULL,music_key TEXT NOT NULL,"
      "added_at TEXT DEFAULT CURRENT_TIMESTAMP);"
      "INSERT INTO music_playlists(id,name) VALUES(7,'Legacy Playlist');"
      "INSERT INTO music_player_state(key,value) VALUES"
      "('selected_playlist_id','7'),('queue_display_name','Legacy Queue');"
      "PRAGMA user_version=0");

  SqliteStatementHandle playlistInsert;
  assert(prepareSqliteStatement(
             database.get(),
             "INSERT INTO music_playlist_items("
             "playlist_id,position,music_key_type,music_key) "
             "VALUES(7,0,'path',?1)",
             playlistInsert) == SQLITE_OK);
  bindSqliteText(playlistInsert.get(), 1, chartMeta.BmsPath.string());
  assert(sqlite3_step(playlistInsert.get()) == SQLITE_DONE);

  SqliteStatementHandle queueInsert;
  assert(prepareSqliteStatement(
             database.get(),
             "INSERT INTO music_now_playing_items("
             "position,music_key_type,music_key) VALUES(0,'path',?1)",
             queueInsert) == SQLITE_OK);
  bindSqliteText(queueInsert.get(), 1, chartMeta.BmsPath.string());
  assert(sqlite3_step(queueInsert.get()) == SQLITE_DONE);
}

void testVersionZeroMigration() {
  TempDirectory temporary;
  const auto playlistPath = temporary.path() / "music_playlist.db";
  const auto chartPath = temporary.path() / "chart.db";
  const bms_parser::ChartMeta chartMeta = createChartDatabase(chartPath);
  createVersionZeroPlaylistDatabase(playlistPath, chartMeta);

  MusicPlaylistRepository repository(playlistPath, chartPath);
  assert(repository.EnsureReady());
  const auto playlists = repository.SelectPlaylists();
  assert(playlists.size() == 1);
  assert(playlists.front().id == 7);
  assert(playlists.front().trackCount == 1);
  const auto state = repository.SelectPlayerState();
  assert(state.selectedPlaylistId == 7);
  assert(state.queueDisplayName == "Legacy Queue");
  assert(repository.SelectTracks(7).size() == 1);
  assert(repository.SelectNowPlayingTracks().size() == 1);
  repository.Shutdown();

  auto database = openDatabase(playlistPath);
  assert(queryInt(database.get(), "PRAGMA user_version") == 1);
  constexpr std::array tables{"music_playlist_items",
                              "music_now_playing_items"};
  constexpr std::array columns{"chart_path", "chart_md5", "chart_sha256"};
  for (const auto *table : tables) {
    for (const auto *column : columns) {
      assert(columnExists(database.get(), table, column));
    }
  }
  constexpr std::array indexes{
      "idx_music_playlist_items_playlist_position",
      "idx_music_playlist_items_music_key",
      "idx_music_playlist_items_chart_path",
      "idx_music_playlist_items_chart_md5",
      "idx_music_playlist_items_chart_sha256",
      "idx_music_now_playing_position",
      "idx_music_now_playing_music_key",
      "idx_music_now_playing_chart_path",
      "idx_music_now_playing_chart_md5",
      "idx_music_now_playing_chart_sha256"};
  for (const auto *index : indexes) {
    assert(indexExists(database.get(), index));
  }
  assert(queryInt(database.get(), "SELECT COUNT(*) FROM music_playlists") ==
         1);
  assert(queryInt(database.get(),
                  "SELECT COUNT(*) FROM music_player_state") == 2);
}

void writeHeaderShapedCorruptDatabase(const std::filesystem::path &path) {
  std::filesystem::create_directories(path.parent_path());
  std::string bytes(4096, '\0');
  constexpr std::string_view magic("SQLite format 3\0", 16);
  std::copy(magic.begin(), magic.end(), bytes.begin());
  bytes[16] = 0x10;
  bytes[17] = 0;
  bytes[18] = 1;
  bytes[19] = 1;
  std::ofstream output(path, std::ios::binary);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  assert(output);
}

void testPlaylistNamesAreBoundAsData() {
  TempDirectory temporary;
  const auto playlistPath = temporary.path() / "music_playlist.db";
  const auto chartPath = temporary.path() / "chart.db";
  createChartDatabase(chartPath);

  MusicPlaylistRepository repository(playlistPath, chartPath);
  assert(repository.EnsureReady());

  const std::string createPayload =
      "Robert'); DROP TABLE music_playlists; --";
  const int playlistId = repository.EnsurePlaylist(createPayload);
  assert(playlistId > 0);

  auto playlists = repository.SelectPlaylists();
  assert(playlists.size() == 1);
  assert(playlists.front().name == createPayload);

  const std::string renamePayload =
      "' UNION SELECT 1, 'injected', 999; --";
  assert(repository.RenamePlaylist(playlistId, renamePayload));
  playlists = repository.SelectPlaylists();
  assert(playlists.size() == 1);
  assert(playlists.front().id == playlistId);
  assert(playlists.front().name == renamePayload);

  repository.Shutdown();
  auto database = openDatabase(playlistPath);
  assert(queryInt(database.get(), "SELECT COUNT(*) FROM music_playlists") ==
         1);
}

void testInvalidInputsDoNotMutatePlaylistFamily() {
  TempDirectory temporary;
  const auto chartPath = temporary.path() / "chart.db";
  createChartDatabase(chartPath);

  {
    const auto path = temporary.path() / "future" / "music_playlist.db";
    auto database = openDatabase(path);
    execOrAbort(database.get(),
                "CREATE TABLE sentinel(value TEXT);"
                "INSERT INTO sentinel VALUES('unchanged');"
                "PRAGMA user_version=2");
    database.reset();
    const auto before = repository_test::rawDatabaseFamilySnapshot(path);
    MusicPlaylistRepository repository(path, chartPath);
    assert(!repository.EnsureReady());
    assert(repository_test::rawDatabaseFamilySnapshot(path) == before);
  }

  {
    const auto path = temporary.path() / "corrupt" / "music_playlist.db";
    writeHeaderShapedCorruptDatabase(path);
    const auto before = repository_test::rawDatabaseFamilySnapshot(path);
    MusicPlaylistRepository repository(path, chartPath);
    assert(!repository.EnsureReady());
    assert(repository_test::rawDatabaseFamilySnapshot(path) == before);
  }

  {
    const auto path = temporary.path() / "invalid-chart" / "music_playlist.db";
    const auto before = repository_test::rawDatabaseFamilySnapshot(path);
    MusicPlaylistRepository repository(
        path, temporary.path() / "missing-chart.db");
    assert(!repository.EnsureReady());
    assert(repository_test::rawDatabaseFamilySnapshot(path) == before);
  }
}

void testBatchedGroupsUseIndexedReusableStatements() {
  TempDirectory temporary;
  const auto chartPath = temporary.path() / "chart.db";
  const auto original = createChartDatabase(chartPath);
  auto chartDb = openDatabase(chartPath);
  execOrAbort(chartDb.get(), "CREATE INDEX idx_chart_meta_folder ON chart_meta(folder);"
      "CREATE INDEX idx_chart_meta_sha256 ON chart_meta(sha256);"
      "CREATE INDEX idx_chart_meta_md5 ON chart_meta(md5);");
  execOrAbort(chartDb.get(), "WITH RECURSIVE seq(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM seq WHERE n<3000) "
      "INSERT INTO chart_meta(path,folder,md5,sha256,title,source_priority,source_archive_size) "
      "SELECT '/library/'||n||'/chart.bms','/library/'||n,'','hash-'||n,'Song '||n,0,0 FROM seq");
  std::vector<bms_parser::ChartMeta> groups;
  for (int i = 1; i <= 80; ++i) {
    bms_parser::ChartMeta group; group.Folder = "/library/" + std::to_string(i); groups.push_back(group);
  }
  auto fallback = original; fallback.Folder.clear(); groups.push_back(fallback);
  groups.push_back(groups.front()); // Repeated input identities preserve input positions.
  std::atomic<int> connections{0}; ScopedConnectionOpenCounter connectionCapture(connections);
  MusicPlaylistRepository repository(temporary.path() / "music.db", chartPath);
  assert(repository.EnsureReady());
  auto *owner = lastOpenedConnection;
  assert(std::filesystem::equivalent(std::filesystem::path(sqlite3_db_filename(owner, "main")), temporary.path() / "music.db"));
  repository_test::StatementTrace trace;
  auto traceStatements = std::make_unique<repository_test::ScopedStatementTrace>(owner, trace);
  MusicLibraryQueryMetrics first;
  const auto rows = repository.SelectLibraryGroups(groups, &first);
  assert(rows.size() == groups.size());
  for (const auto &row : rows) assert(row.size() == 1 && row.front().useChartPathIdentity);
  assert(rows.front().front().representativeChart.Folder == groups.front().Folder);
  assert(rows[80].front().representativeChart.BmsPath == original.BmsPath);
  assert(first.prepared == 3 && first.selects == 1); // One indexed selection, regardless of expanded group count.
  assert(first.vmSteps < 50000 && first.fullScanSteps < 500);
  const auto select = std::ranges::find_if(trace.sql, [](const auto &sql) { return sql.starts_with("WITH requested AS"); });
  assert(select != trace.sql.end());
  const auto plan = repository_test::explainPlan(owner, *select);
  assert(repository_test::planContains(plan, "idx_chart_meta_folder"));
  assert(repository_test::planContains(plan, "sqlite_autoindex_chart_meta_1"));
  assert(repository_test::planContains(plan, "idx_chart_meta_sha256"));
  std::cout << "Batched group query: " << first.prepared << " preparations, " << first.selects
            << " select, " << first.vmSteps << " VM steps, " << first.fullScanSteps << " full-scan steps\n";
  for (const auto &line : plan) std::cout << "  " << line << '\n';
  MusicLibraryQueryMetrics next;
  assert(repository.SelectLibraryGroups({groups.front()}, &next).front().size() == 1);
  assert(next.prepared == 0 && next.selects == 1 && next.vmSteps < 1000 && next.fullScanSteps < 10);
  execOrAbort(chartDb.get(), "DELETE FROM chart_meta WHERE folder='/library/1'");
  assert(repository.SelectLibraryGroups({groups.front()}).front().empty()); // No stale result cache.
  traceStatements.reset();
  repository.Shutdown();
  assert(repository.SelectLibraryGroups({groups[1]}).front().size() == 1); // Statements follow connection lifetime.
}

} // namespace

int main() {
  testBatchedGroupsUseIndexedReusableStatements();
  testRoundTripAndConnectionReuse();
  testVersionZeroMigration();
  testPlaylistNamesAreBoundAsData();
  testInvalidInputsDoNotMutatePlaylistFamily();
  return 0;
}
