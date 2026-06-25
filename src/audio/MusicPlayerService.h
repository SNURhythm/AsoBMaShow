#pragma once

#include "../ThreadCompat.h"
#include "ChartMusicCache.h"
#include "MusicPlaylist.h"
#include "NativeMusicPlayer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace music_player {

class MusicPlayerService {
public:
  MusicPlayerService() = default;
  ~MusicPlayerService();
  MusicPlayerService(const MusicPlayerService &) = delete;
  MusicPlayerService &operator=(const MusicPlayerService &) = delete;

  bool ReloadLibrary(std::string &errorMessage);
  [[nodiscard]] std::size_t LibraryTrackCount() const;
  bool ReloadPlaylists(std::string &errorMessage);
  [[nodiscard]] std::optional<MusicPlaylistInfo>
  DefaultPlaylistSnapshot() const;
  [[nodiscard]] std::vector<music_playlist::MusicTrack>
  DefaultPlaylistTracksSnapshot() const;
  bool AddChartToDefaultPlaylist(const bms_parser::ChartMeta &chartMeta,
                                 std::string &errorMessage);
  bool RemoveChartFromDefaultPlaylist(const bms_parser::ChartMeta &chartMeta,
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

  [[nodiscard]] std::optional<music_playlist::MusicTrack>
  CurrentTrackSnapshot() const;
  [[nodiscard]] chart_music_cache::CacheResult LastCacheResult() const;
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
  bool ProcessNativeControlEvents(std::string &statusMessage);
  bool ConsumeNativeControlStatus(std::string &statusMessage);
  void CancelRender();

private:
  bool PlayCurrentLocked(std::string &errorMessage);
  bool PlayNextLocked(std::string &errorMessage);
  bool PlayPreviousLocked(std::string &errorMessage);
  bool PlayTrackLocked(const music_playlist::MusicTrack &track,
                       std::string &errorMessage);
  bool ProcessNativeControlEventsLocked(std::string &statusMessage);
  void EnsureNativeControlEventPump();
  void StopNativeControlEventPump();
  void NativeControlEventLoop(const std::stop_token &stopToken);
  void PublishNativeControlStatus(const std::string &statusMessage);

  mutable std::mutex stateMutex;
  mutable std::mutex nativeControlStatusMutex;
  std::mutex nativeControlThreadMutex;
  std::vector<music_playlist::MusicTrack> libraryTracks;
  std::vector<MusicPlaylistInfo> playlists;
  std::vector<music_playlist::MusicTrack> defaultPlaylistTracks;
  int defaultPlaylistId = 0;
  music_playlist::MusicQueue queue;
  std::jthread nativeControlEventThread;
  bool nativeControlEventThreadStopping = false;
  std::string nativeControlStatusMessage;
  std::uint64_t nativeControlStatusRevision = 0;
  std::uint64_t consumedNativeControlStatusRevision = 0;
  std::atomic_bool renderCancelled{false};
  chart_music_cache::CacheResult lastCacheResult;
};

} // namespace music_player
