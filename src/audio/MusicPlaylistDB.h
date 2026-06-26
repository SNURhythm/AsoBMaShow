#pragma once

#include "../ChartDBHelper.h"
#include "../sqlite3.h"

#include <string>
#include <vector>

struct MusicPlaylistInfo {
  int id = 0;
  std::string name;
  int trackCount = 0;
};

class MusicPlaylistDB {
public:
  sqlite3 *Connect();
  void Close(sqlite3 *db);

  bool CreateTables(sqlite3 *db);
  int EnsurePlaylist(sqlite3 *db, const std::string &name);
  bool RenamePlaylist(sqlite3 *db, int playlistId, const std::string &name);
  std::vector<MusicPlaylistInfo> SelectPlaylists(sqlite3 *db);
  bool InsertTrack(sqlite3 *db, int playlistId,
                   const bms_parser::ChartMeta &chartMeta);
  bool DeleteTrack(sqlite3 *db, int playlistId,
                   const bms_parser::ChartMeta &chartMeta);
  bool MoveTrack(sqlite3 *db, int playlistId,
                 const bms_parser::ChartMeta &chartMeta, int delta);
  bool ClearPlaylist(sqlite3 *db, int playlistId);
  void SelectLibraryTracks(sqlite3 *db, std::vector<MusicTrackRecord> &tracks);
  void SelectTracks(sqlite3 *db, int playlistId,
                    std::vector<MusicTrackRecord> &tracks);
};
