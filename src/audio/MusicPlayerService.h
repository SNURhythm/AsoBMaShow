#pragma once

#include "../ThreadCompat.h"
#include "ChartMusicCache.h"
#include "MusicPlaylistDB.h"
#include "MusicPlaylist.h"
#include "NativeMusicPlayer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
  bool ReloadLibraryAndPlaylists(std::string &errorMessage,
                                 int preferredSelectedPlaylistId = 0);
  [[nodiscard]] std::size_t LibraryTrackCount() const;
  [[nodiscard]] std::vector<music_playlist::MusicTrack>
  LibraryTracksSnapshot() const;
  [[nodiscard]] std::vector<music_playlist::MusicTrack>
  FavoriteTracksSnapshot() const;
  bool LoadLibraryGroupTracks(const music_playlist::MusicTrack &groupTrack,
                              std::vector<music_playlist::MusicTrack> &tracks,
                              std::string &errorMessage);
  bool SetFavorite(const bms_parser::ChartMeta &chartMeta, bool favorite,
                   std::string &errorMessage);
  bool ReloadPlaylists(std::string &errorMessage);
  [[nodiscard]] std::vector<MusicPlaylistInfo> PlaylistsSnapshot() const;
  [[nodiscard]] int SelectedPlaylistId() const;
  [[nodiscard]] std::optional<MusicPlaylistInfo>
  SelectedPlaylistSnapshot() const;
  [[nodiscard]] std::vector<music_playlist::MusicTrack>
  SelectedPlaylistTracksSnapshot() const;
  int CreatePlaylist(const std::string &name, std::string &errorMessage);
  int CreatePlaylistFromTracks(
      const std::string &name,
      const std::vector<music_playlist::MusicTrack> &tracks,
      std::string &errorMessage);
  bool RenameSelectedPlaylist(const std::string &name,
                              std::string &errorMessage);
  bool SelectPlaylist(int playlistId, std::string &errorMessage);
  bool AddChartToSelectedPlaylist(const bms_parser::ChartMeta &chartMeta,
                                  std::string &errorMessage);
  bool AddChartToPlaylist(int playlistId,
                          const bms_parser::ChartMeta &chartMeta,
                          std::string &errorMessage);
  bool RemoveChartFromSelectedPlaylist(const bms_parser::ChartMeta &chartMeta,
                                       std::string &errorMessage);
  bool MoveChartInSelectedPlaylist(const bms_parser::ChartMeta &chartMeta,
                                   int delta, std::string &errorMessage);
  bool ClearSelectedPlaylist(std::string &errorMessage);
  bool DeleteSelectedPlaylist(std::string &errorMessage);
  [[nodiscard]] std::optional<MusicPlaylistInfo>
  DefaultPlaylistSnapshot() const;
  [[nodiscard]] std::vector<music_playlist::MusicTrack>
  DefaultPlaylistTracksSnapshot() const;
  bool AddChartToDefaultPlaylist(const bms_parser::ChartMeta &chartMeta,
                                 std::string &errorMessage);
  bool RemoveChartFromDefaultPlaylist(const bms_parser::ChartMeta &chartMeta,
                                      std::string &errorMessage);
  bool MoveChartInDefaultPlaylist(const bms_parser::ChartMeta &chartMeta,
                                  int delta, std::string &errorMessage);
  bool ClearDefaultPlaylist(std::string &errorMessage);

  bool StartLibraryPlaylist(std::string &errorMessage,
                            std::size_t startIndex = 0);
  bool StartRandomLibrary(std::string &errorMessage,
                          std::optional<std::uint64_t> seed = std::nullopt);
  bool StartSelectedPlaylist(std::string &errorMessage);
  bool StartDefaultPlaylist(std::string &errorMessage);
  void SetNowPlaying(std::vector<music_playlist::MusicTrack> tracks,
                     std::size_t startIndex = 0);
  void SetPlaylist(std::vector<music_playlist::MusicTrack> tracks,
                   std::size_t startIndex = 0,
                   std::string displayName = {});
  void SetPlaylistAfterCurrentRemoved(
      std::vector<music_playlist::MusicTrack> tracks, std::size_t nextIndex = 0,
      std::string displayName = {});

  [[nodiscard]] std::optional<music_playlist::MusicTrack>
  CurrentTrackSnapshot() const;
  [[nodiscard]] music_playlist::MusicQueueSnapshot QueueSnapshot() const;
  [[nodiscard]] music_playlist::QueueRepeatMode RepeatMode() const;
  void SetRepeatMode(music_playlist::QueueRepeatMode mode);
  [[nodiscard]] chart_music_cache::CacheResult LastCacheResult() const;
  [[nodiscard]] native_music_player::PlaybackState PlaybackState() const {
    return native_music_player::GetState();
  }

  bool PlayCurrent(std::string &errorMessage);
  bool PlayLibraryTrack(std::size_t index, std::string &errorMessage);
  bool PlayNext(std::string &errorMessage);
  bool PlayPrevious(std::string &errorMessage);
  bool PlayCurrentAsync(std::string &statusMessage,
                        std::string successMessage = "Playing music.");
  bool PlayNextAsync(std::string &statusMessage,
                     std::string successMessage = "Playing next track.");
  bool PlayPreviousAsync(std::string &statusMessage,
                         std::string successMessage =
                             "Playing previous track.");
  bool Resume(std::string &errorMessage);
  bool Pause(std::string &errorMessage);
  bool Stop(std::string &errorMessage);
  bool Seek(long long positionMicros, std::string &errorMessage);
  bool ProcessNativeControlEvents(std::string &statusMessage);
  bool ConsumeNativeControlStatus(std::string &statusMessage);
  void CancelRender();

private:
  enum class PlaybackRequest { Current, Next, Previous };

  bool PlayCurrentLocked(std::string &errorMessage);
  bool PlayNextLocked(std::string &errorMessage);
  bool PlayPreviousLocked(std::string &errorMessage);
  bool PlayTrackLocked(const music_playlist::MusicTrack &track,
                       std::string &errorMessage);
  bool StartPlaybackAsync(PlaybackRequest request, std::string &statusMessage,
                          std::string successMessage);
  void PlaybackWorker(music_playlist::MusicTrack track,
                      std::uint64_t requestRevision,
                      std::string successMessage,
                      const std::stop_token &stopToken);
  std::vector<music_playlist::MusicTrack> AdjacentTracksLocked() const;
  std::vector<std::filesystem::path> PlaybackCacheKeepPathsLocked(
      const chart_music_cache::CacheResult &currentResult) const;
  void StartAdjacentPreloadWorker(
      std::vector<music_playlist::MusicTrack> tracks);
  void StopAdjacentPreloadWorker();
  void AdjacentPreloadWorker(std::vector<music_playlist::MusicTrack> tracks,
                             std::uint64_t preloadRevision,
                             const std::stop_token &stopToken);
  void RefreshPlaylistCachesLocked(MusicPlaylistDB &playlistDb, sqlite3 *db,
                                   int preferredSelectedPlaylistId);
  void StopPlaybackWorker();
  void EnsureNativeControlEventPump();
  void StopNativeControlEventPump();
  void NativeControlEventLoop(const std::stop_token &stopToken);
  void PublishNativeControlStatus(const std::string &statusMessage);

  mutable std::mutex stateMutex;
  mutable std::mutex nativeControlStatusMutex;
  std::mutex nativeControlThreadMutex;
  std::mutex playbackThreadMutex;
  std::mutex preloadThreadMutex;
  std::vector<music_playlist::MusicTrack> libraryTracks;
  std::vector<music_playlist::MusicTrack> favoriteTracks;
  std::vector<MusicPlaylistInfo> playlists;
  std::vector<music_playlist::MusicTrack> defaultPlaylistTracks;
  std::vector<music_playlist::MusicTrack> selectedPlaylistTracks;
  std::optional<music_playlist::MusicTrack> loadedTrack;
  int defaultPlaylistId = 0;
  int selectedPlaylistId = 0;
  music_playlist::MusicQueue queue;
  std::jthread playbackThread;
  std::jthread preloadThread;
  std::jthread nativeControlEventThread;
  bool nativeControlEventThreadStopping = false;
  std::string nativeControlStatusMessage;
  std::uint64_t nativeControlStatusRevision = 0;
  std::uint64_t consumedNativeControlStatusRevision = 0;
  std::atomic_bool renderCancelled{false};
  std::atomic_bool preloadCancelled{false};
  std::atomic<std::uint64_t> playbackRequestRevision{0};
  std::atomic<std::uint64_t> preloadRequestRevision{0};
  chart_music_cache::CacheResult lastCacheResult;
};

} // namespace music_player
