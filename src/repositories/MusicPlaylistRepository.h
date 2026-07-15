#pragma once

#include "ChartRepository.h"

#include <filesystem>
#include <memory>
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
  MusicPlaylistRepository();
  MusicPlaylistRepository(std::filesystem::path databasePath,
                          std::filesystem::path chartDatabasePath);
  ~MusicPlaylistRepository();

  MusicPlaylistRepository(const MusicPlaylistRepository &) = delete;
  MusicPlaylistRepository &
  operator=(const MusicPlaylistRepository &) = delete;

  bool EnsureReady();
  void Shutdown();
  int EnsurePlaylist(const std::string &name);
  int EnsurePlaylistWithTracks(
      const std::string &name,
      const std::vector<bms_parser::ChartMeta> &tracks);
  bool RenamePlaylist(int playlistId, const std::string &name);
  std::vector<MusicPlaylistInfo> SelectPlaylists();
  bool InsertTrack(int playlistId,
                   const bms_parser::ChartMeta &chartMeta);
  bool DeleteTrack(int playlistId,
                   const bms_parser::ChartMeta &chartMeta,
                   int storedItemId = 0);
  bool MoveTrack(int playlistId,
                 const bms_parser::ChartMeta &chartMeta,
                 int delta, int storedItemId = 0);
  bool ClearPlaylist(int playlistId);
  bool DeletePlaylist(int playlistId);
  MusicPlayerStateRecord SelectPlayerState();
  bool SavePlayerState(const MusicPlayerStateRecord &state);
  bool ReplaceNowPlayingTracks(
      const std::vector<bms_parser::ChartMeta> &tracks);
  bool SaveNowPlayingState(
      const std::vector<bms_parser::ChartMeta> &tracks,
      const MusicPlayerStateRecord &state);
  std::vector<MusicTrackRecord> SelectLibraryTracks();
  std::vector<MusicTrackRecord> SelectLibraryGroupTracks(
      const bms_parser::ChartMeta &chartMeta);
  std::vector<MusicTrackRecord> SelectNowPlayingTracks();
  std::vector<MusicTrackRecord> SelectTracks(int playlistId);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
