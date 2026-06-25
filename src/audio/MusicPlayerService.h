#pragma once

#include "ChartMusicCache.h"
#include "MusicPlaylist.h"
#include "NativeMusicPlayer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace music_player {

class MusicPlayerService {
public:
  MusicPlayerService() = default;
  MusicPlayerService(const MusicPlayerService &) = delete;
  MusicPlayerService &operator=(const MusicPlayerService &) = delete;

  bool ReloadLibrary(std::string &errorMessage);
  [[nodiscard]] const std::vector<music_playlist::MusicTrack> &
  LibraryTracks() const {
    return libraryTracks;
  }
  bool ReloadPlaylists(std::string &errorMessage);
  [[nodiscard]] const std::vector<MusicPlaylistInfo> &Playlists() const {
    return playlists;
  }
  [[nodiscard]] const MusicPlaylistInfo *DefaultPlaylist() const;
  bool AddChartToDefaultPlaylist(const bms_parser::ChartMeta &chartMeta,
                                 std::string &errorMessage);
  bool ClearDefaultPlaylist(std::string &errorMessage);

  bool StartLibraryPlaylist(std::string &errorMessage,
                            std::size_t startIndex = 0);
  bool StartRandomLibrary(std::string &errorMessage,
                          std::optional<std::uint64_t> seed = std::nullopt);
  bool StartDefaultPlaylist(std::string &errorMessage);
  void SetPlaylist(std::vector<music_playlist::MusicTrack> tracks,
                   std::size_t startIndex = 0);
  void SetRandomAll(std::vector<music_playlist::MusicTrack> tracks,
                    std::optional<std::uint64_t> seed = std::nullopt);

  [[nodiscard]] const music_playlist::MusicQueue &Queue() const {
    return queue;
  }
  [[nodiscard]] const music_playlist::MusicTrack *CurrentTrack() const {
    return queue.Current();
  }
  [[nodiscard]] chart_music_cache::CacheResult LastCacheResult() const {
    return lastCacheResult;
  }
  [[nodiscard]] native_music_player::PlaybackState PlaybackState() const {
    return native_music_player::GetState();
  }

  bool PlayCurrent(std::string &errorMessage);
  bool PlayLibraryTrack(std::size_t index, std::string &errorMessage);
  bool PlayNext(std::string &errorMessage);
  bool PlayPrevious(std::string &errorMessage);
  bool Resume(std::string &errorMessage);
  bool Pause(std::string &errorMessage);
  bool Stop(std::string &errorMessage);
  bool Seek(long long positionMicros, std::string &errorMessage);
  void CancelRender();

private:
  bool PlayTrack(const music_playlist::MusicTrack &track,
                 std::string &errorMessage);

  std::vector<music_playlist::MusicTrack> libraryTracks;
  std::vector<MusicPlaylistInfo> playlists;
  int defaultPlaylistId = 0;
  music_playlist::MusicQueue queue;
  std::atomic_bool renderCancelled{false};
  chart_music_cache::CacheResult lastCacheResult;
};

} // namespace music_player
