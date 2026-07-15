#pragma once

#include "ChartRepository.h"
#include "../sqlite3.h"

#include <string>
#include <vector>

struct MusicPlaylistInfo {
  int id = 0;
  std::string name;
  int trackCount = 0;
};

struct MusicPlayerStateRecord {
  int selectedPlaylistId = 0;
  int playlistCursorIndex = -1;
  int queueCursorIndex = -1;
  int repeatMode = 2;
  std::string queueDisplayName;
};

class MusicPlaylistRepository {
public:
  MusicPlaylistRepository() = default;
  MusicPlaylistRepository(const MusicPlaylistRepository &) = delete;
  MusicPlaylistRepository &operator=(const MusicPlaylistRepository &) = delete;
  MusicPlaylistRepository(MusicPlaylistRepository &&) = delete;
  MusicPlaylistRepository &operator=(MusicPlaylistRepository &&) = delete;

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
                   int storedItemId = 0);
  bool MoveTrack(sqlite3 *db, int playlistId,
                 const bms_parser::ChartMeta &chartMeta, int delta,
                 int storedItemId = 0);
  bool ClearPlaylist(sqlite3 *db, int playlistId);
  bool DeletePlaylist(sqlite3 *db, int playlistId);
  MusicPlayerStateRecord SelectPlayerState(sqlite3 *db);
  bool SavePlayerState(sqlite3 *db, const MusicPlayerStateRecord &state);
  bool ReplaceNowPlayingTracks(
      sqlite3 *db, const std::vector<bms_parser::ChartMeta> &tracks);
  void SelectLibraryTracks(sqlite3 *db, std::vector<MusicTrackRecord> &tracks);
  void SelectLibraryGroupTracks(sqlite3 *db,
                                const bms_parser::ChartMeta &chartMeta,
                                std::vector<MusicTrackRecord> &tracks);
  void SelectNowPlayingTracks(sqlite3 *db,
                              std::vector<MusicTrackRecord> &tracks);
  void SelectTracks(sqlite3 *db, int playlistId,
                    std::vector<MusicTrackRecord> &tracks);

private:
  sqlite3 *schemaDatabase = nullptr;
};
