#include "MusicPlayerService.h"
#include "../StableHash.h"
#include "../RAII.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <thread>
#include <utility>

namespace music_player {
namespace {

constexpr const char *kDefaultPlaylistName = "My Playlist";

void setEmptyLibraryError(std::string &errorMessage) {
  errorMessage = "No music tracks are available in the chart library.";
}

void setEmptyPlaylistError(std::string &errorMessage) {
  errorMessage = "The music playlist is empty.";
}

std::vector<music_playlist::MusicTrack>
loadPlaylistTracks(MusicPlaylistRepository &repository, int playlistId) {
  return music_playlist::MakeTracks(repository.SelectTracks(playlistId));
}

std::vector<music_playlist::MusicTrack>
loadFavoriteTracks(ChartRepository &charts) {
  std::vector<MusicTrackRecord> records;
  auto session = charts.OpenSession();
  if (!session.has_value()) {
    return {};
  }
  session->SelectFavoriteMusicTracks(records);
  return music_playlist::MakeTracks(records);
}

std::vector<music_playlist::MusicTrack>
loadNowPlayingTracks(MusicPlaylistRepository &repository) {
  return music_playlist::MakeTracks(repository.SelectNowPlayingTracks());
}

int repeatModeToRecordValue(music_playlist::QueueRepeatMode mode) {
  switch (mode) {
  case music_playlist::QueueRepeatMode::None:
    return 0;
  case music_playlist::QueueRepeatMode::One:
    return 1;
  case music_playlist::QueueRepeatMode::All:
  default:
    return 2;
  }
}

music_playlist::QueueRepeatMode repeatModeFromRecordValue(int value) {
  switch (value) {
  case 0:
    return music_playlist::QueueRepeatMode::None;
  case 1:
    return music_playlist::QueueRepeatMode::One;
  case 2:
  default:
    return music_playlist::QueueRepeatMode::All;
  }
}

bool hasPlaylistId(const std::vector<MusicPlaylistInfo> &playlists,
                   int playlistId) {
  return std::any_of(playlists.begin(), playlists.end(),
                     [playlistId](const MusicPlaylistInfo &playlist) {
                       return playlist.id == playlistId;
                     });
}

std::optional<MusicPlaylistInfo>
findPlaylistById(const std::vector<MusicPlaylistInfo> &playlists,
                 int playlistId) {
  const auto it =
      std::find_if(playlists.begin(), playlists.end(),
                   [playlistId](const MusicPlaylistInfo &playlist) {
                     return playlist.id == playlistId;
                   });
  if (it == playlists.end()) {
    return std::nullopt;
  }
  return *it;
}

long long nativeQueueItemIdForTrack(
    const music_playlist::MusicTrack &track, std::size_t index) {
  const std::string identity = !track.trackId.empty() ? track.trackId
                               : !track.chartId.empty() ? track.chartId
                                                        : track.title;
  const std::string key = identity + "\n" + std::to_string(index);
  std::uint64_t value =
      stable_hash::fnv1a64(key) &
      static_cast<std::uint64_t>(std::numeric_limits<long long>::max());
  if (value == 0) {
    value = 1;
  }
  return static_cast<long long>(value);
}

native_music_player::QueueMetadata nativeQueueMetadataForSnapshot(
    const music_playlist::MusicQueueSnapshot &snapshot,
    const std::optional<music_playlist::MusicTrack> &loadedTrack) {
  native_music_player::QueueMetadata queueMetadata;
  queueMetadata.title =
      snapshot.displayName.empty() ? music_playlist::kNowPlayingDisplayName
                                   : snapshot.displayName;
  if (snapshot.currentIndex &&
      *snapshot.currentIndex < snapshot.tracks.size()) {
    queueMetadata.currentIndex = static_cast<int>(*snapshot.currentIndex);
  } else if (loadedTrack) {
    if (const auto loadedIndex =
            music_playlist::FindTrackIndex(snapshot.tracks, *loadedTrack)) {
      queueMetadata.currentIndex = static_cast<int>(*loadedIndex);
    }
  }

  queueMetadata.items.reserve(snapshot.tracks.size());
  for (std::size_t i = 0; i < snapshot.tracks.size(); ++i) {
    music_playlist::MusicTrack track = snapshot.tracks[i];
    if (loadedTrack &&
        music_playlist::SameTrackIdentity(track, *loadedTrack)) {
      track.durationMicros = loadedTrack->durationMicros;
    }
    queueMetadata.items.push_back(
        {.metadata = music_playlist::MakeNativeMetadata(track, false),
         .itemId = nativeQueueItemIdForTrack(track, i)});
  }
  return queueMetadata;
}

} // namespace

MusicPlayerService::MusicPlayerService(MusicPlaylistRepository &repository,
                                       ChartRepository &chartRepository)
    : repository(repository), charts(chartRepository) {}

MusicPlayerService::~MusicPlayerService() {
  StopSleepTimerWorker();
  StopPlaybackWorker();
  StopNativeControlEventPump();
  std::lock_guard<std::mutex> lock(stateMutex);
  repository.Shutdown();
}

bool MusicPlayerService::EnsureRepositoryReadyLocked(
    std::string &errorMessage) {
  if (!repository.EnsureReady()) {
    errorMessage = "Could not open music playlist database.";
    return false;
  }
  return true;
}

bool MusicPlayerService::ReloadLibrary(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }

  const std::vector<MusicTrackRecord> records = repository.SelectLibraryTracks();
  favoriteTracks = loadFavoriteTracks(charts);

  libraryTracks = music_playlist::MakeTracks(records);
  contentRevision.fetch_add(1, std::memory_order_release);
  return true;
}

bool MusicPlayerService::ReloadLibraryAndPlaylists(
    std::string &errorMessage, int preferredSelectedPlaylistId) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }

  const std::vector<MusicTrackRecord> records = repository.SelectLibraryTracks();
  libraryTracks = music_playlist::MakeTracks(records);
  favoriteTracks = loadFavoriteTracks(charts);
  persistedState = repository.SelectPlayerState();

  RefreshPlaylistCachesLocked(preferredSelectedPlaylistId);
  if (defaultPlaylistId <= 0) {
    errorMessage = "Could not create music playlist.";
    return false;
  }
  RestoreQueueFromPersistedStateLocked();
  return true;
}

std::size_t MusicPlayerService::LibraryTrackCount() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return libraryTracks.size();
}

std::vector<music_playlist::MusicTrack>
MusicPlayerService::LibraryTracksSnapshot() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return libraryTracks;
}

std::vector<music_playlist::MusicTrack>
MusicPlayerService::FavoriteTracksSnapshot() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return favoriteTracks;
}

bool MusicPlayerService::LoadLibraryGroupTracks(
    const music_playlist::MusicTrack &groupTrack,
    std::vector<music_playlist::MusicTrack> &tracks,
    std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();
  tracks.clear();

  if (groupTrack.groupId.empty()) {
    errorMessage = "Selected track does not belong to a chart group.";
    return false;
  }

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }

  const std::vector<MusicTrackRecord> records =
      repository.SelectLibraryGroupTracks(groupTrack.representativeChart);

  tracks = music_playlist::MakeTracks(records);
  if (tracks.empty()) {
    errorMessage = "No charts were found in the selected group.";
    return false;
  }
  return true;
}

bool MusicPlayerService::SetFavorite(const bms_parser::ChartMeta &chartMeta,
                                     bool favorite,
                                     std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  auto chartSession = charts.OpenSession();
  if (!chartSession.has_value()) {
    errorMessage = "Could not open chart database.";
    return false;
  }
  const bool updated = chartSession->SetFavorite(chartMeta, favorite);
  if (!updated) {
    errorMessage = favorite ? "Could not add favorite."
                            : "Could not remove favorite.";
    return false;
  }
  favoriteTracks = loadFavoriteTracks(charts);
  contentRevision.fetch_add(1, std::memory_order_release);
  return true;
}

void MusicPlayerService::RefreshPlaylistCachesLocked(
    int preferredSelectedPlaylistId) {
  defaultPlaylistId = repository.EnsurePlaylist(kDefaultPlaylistName);
  playlists = repository.SelectPlaylists();

  if (preferredSelectedPlaylistId > 0 &&
      hasPlaylistId(playlists, preferredSelectedPlaylistId)) {
    selectedPlaylistId = preferredSelectedPlaylistId;
  } else if (selectedPlaylistId > 0 &&
             hasPlaylistId(playlists, selectedPlaylistId)) {
    // Keep the current selection.
  } else if (persistedState.selectedPlaylistId > 0 &&
             hasPlaylistId(playlists, persistedState.selectedPlaylistId)) {
    selectedPlaylistId = persistedState.selectedPlaylistId;
  } else if (defaultPlaylistId > 0 && hasPlaylistId(playlists, defaultPlaylistId)) {
    selectedPlaylistId = defaultPlaylistId;
  } else if (!playlists.empty()) {
    selectedPlaylistId = playlists.front().id;
  } else {
    selectedPlaylistId = 0;
  }

  defaultPlaylistTracks =
      defaultPlaylistId > 0 ? loadPlaylistTracks(repository, defaultPlaylistId)
                            : std::vector<music_playlist::MusicTrack>{};
  selectedPlaylistTracks =
      selectedPlaylistId == defaultPlaylistId
          ? defaultPlaylistTracks
          : (selectedPlaylistId > 0
                 ? loadPlaylistTracks(repository, selectedPlaylistId)
                 : std::vector<music_playlist::MusicTrack>{});
  contentRevision.fetch_add(1, std::memory_order_release);
}

void MusicPlayerService::RestoreQueueFromPersistedStateLocked() {
  queue.SetRepeatMode(repeatModeFromRecordValue(persistedState.repeatMode));
  if (!queue.Empty()) {
    SyncNativeQueueLocked();
    return;
  }

  auto nowPlayingTracks = loadNowPlayingTracks(repository);
  if (nowPlayingTracks.empty()) {
    SyncNativeQueueLocked();
    return;
  }

  const int cursor =
      std::clamp(persistedState.queueCursorIndex, 0,
                 static_cast<int>(nowPlayingTracks.size()) - 1);
  std::string displayName = persistedState.queueDisplayName;
  if (displayName.empty()) {
    displayName = music_playlist::kNowPlayingDisplayName;
  }
  queue.SetPlaylist(std::move(nowPlayingTracks),
                    static_cast<std::size_t>(cursor), std::move(displayName));
  SyncNativeQueueLocked();
}

void MusicPlayerService::PersistPlayerStateLocked() {
  repository.SavePlayerState(persistedState);
}

void MusicPlayerService::PersistQueueTracksLocked() {
  sessionOnlyQueueOrder = false;
  sessionPersistentQueueTracks.clear();
  PersistQueueTracksLocked(queue.Snapshot().tracks, false);
}

void MusicPlayerService::PersistQueueTracksLocked(
    const std::vector<music_playlist::MusicTrack> &tracks,
    bool preserveCursor) {
  const auto snapshot = queue.Snapshot();
  std::string databaseError;
  if (!EnsureRepositoryReadyLocked(databaseError)) {
    SyncNativeQueueLocked();
    return;
  }

  MusicPlayerStateRecord nextState = persistedState;
  nextState.repeatMode = repeatModeToRecordValue(snapshot.repeatMode);
  nextState.queueDisplayName = snapshot.displayName;
  if (preserveCursor) {
    nextState.queueCursorIndex = persistedState.queueCursorIndex;
  } else if (snapshot.currentIndex) {
    nextState.queueCursorIndex = static_cast<int>(*snapshot.currentIndex);
  } else if (snapshot.detachedCurrentNextIndex) {
    nextState.queueCursorIndex =
        static_cast<int>(*snapshot.detachedCurrentNextIndex);
  } else {
    nextState.queueCursorIndex = -1;
  }

  std::vector<bms_parser::ChartMeta> chartMetas;
  chartMetas.reserve(tracks.size());
  for (const auto &track : tracks) {
    chartMetas.push_back(track.representativeChart);
  }

  if (repository.SaveNowPlayingState(chartMetas, nextState)) {
    persistedState = std::move(nextState);
  }
  SyncNativeQueueLocked();
}

void MusicPlayerService::PersistQueueCursorLocked() {
  const auto snapshot = queue.Snapshot();
  std::string databaseError;
  if (!EnsureRepositoryReadyLocked(databaseError)) {
    SyncNativeQueueLocked();
    return;
  }

  MusicPlayerStateRecord nextState = persistedState;
  nextState.repeatMode = repeatModeToRecordValue(snapshot.repeatMode);
  nextState.queueDisplayName = snapshot.displayName;
  if (sessionOnlyQueueOrder) {
    nextState.queueCursorIndex = persistedState.queueCursorIndex;
  } else if (snapshot.currentIndex) {
    nextState.queueCursorIndex = static_cast<int>(*snapshot.currentIndex);
  } else if (snapshot.detachedCurrentNextIndex) {
    nextState.queueCursorIndex =
        static_cast<int>(*snapshot.detachedCurrentNextIndex);
  } else {
    nextState.queueCursorIndex = -1;
  }

  const bool saved = repository.SavePlayerState(nextState);
  if (saved) {
    persistedState = std::move(nextState);
  }
  SyncNativeQueueLocked();
}

void MusicPlayerService::SyncNativeQueueLocked() {
  std::string ignored;
  native_music_player::SetQueueMetadata(
      nativeQueueMetadataForSnapshot(queue.Snapshot(), loadedTrack), ignored);
}

bool MusicPlayerService::ReloadPlaylists(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }

  persistedState = repository.SelectPlayerState();
  RefreshPlaylistCachesLocked(selectedPlaylistId);
  if (defaultPlaylistId <= 0) {
    errorMessage = "Could not create music playlist.";
    return false;
  }
  return true;
}

std::vector<MusicPlaylistInfo> MusicPlayerService::PlaylistsSnapshot() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return playlists;
}

int MusicPlayerService::SelectedPlaylistId() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return selectedPlaylistId;
}

std::vector<music_playlist::MusicTrack>
MusicPlayerService::SelectedPlaylistTracksSnapshot() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return selectedPlaylistTracks;
}

MusicPlayerStateRecord MusicPlayerService::PlayerStateSnapshot() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return persistedState;
}

int MusicPlayerService::CreatePlaylist(const std::string &name,
                                       std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return 0;
  }

  const int playlistId = repository.EnsurePlaylist(name);
  if (playlistId <= 0) {
    errorMessage = "Could not create music playlist.";
    return 0;
  }
  RefreshPlaylistCachesLocked(playlistId);
  persistedState.selectedPlaylistId = selectedPlaylistId;
  persistedState.playlistCursorIndex = -1;
  PersistPlayerStateLocked();
  return playlistId;
}

int MusicPlayerService::CreatePlaylistFromTracks(
    const std::string &name,
    const std::vector<music_playlist::MusicTrack> &tracks,
    std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (tracks.empty()) {
    errorMessage = "Now Playing is empty.";
    return 0;
  }

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return 0;
  }

  std::vector<bms_parser::ChartMeta> chartMetas;
  chartMetas.reserve(tracks.size());
  for (const auto &track : tracks) {
    chartMetas.push_back(track.representativeChart);
  }
  const int playlistId =
      repository.EnsurePlaylistWithTracks(name, chartMetas);
  if (playlistId <= 0) {
    errorMessage = "Could not save every Now Playing track.";
    return 0;
  }

  RefreshPlaylistCachesLocked(playlistId);
  persistedState.selectedPlaylistId = selectedPlaylistId;
  persistedState.playlistCursorIndex = -1;
  PersistPlayerStateLocked();
  return playlistId;
}

bool MusicPlayerService::RenameSelectedPlaylist(const std::string &name,
                                                std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (selectedPlaylistId <= 0) {
    errorMessage = "Select a playlist first.";
    return false;
  }

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }

  const int playlistId = selectedPlaylistId;
  const bool renamed = repository.RenamePlaylist(playlistId, name);
  RefreshPlaylistCachesLocked(playlistId);
  if (!renamed) {
    errorMessage = "Could not rename playlist. The name may already exist.";
    return false;
  }
  return true;
}

bool MusicPlayerService::SelectPlaylist(int playlistId,
                                        std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (playlistId <= 0) {
    errorMessage = "Select a playlist first.";
    return false;
  }

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }

  RefreshPlaylistCachesLocked(playlistId);
  persistedState.selectedPlaylistId = selectedPlaylistId;
  persistedState.playlistCursorIndex = -1;
  PersistPlayerStateLocked();
  if (selectedPlaylistId != playlistId) {
    errorMessage = "Selected playlist no longer exists.";
    return false;
  }
  return true;
}

bool MusicPlayerService::AddChartToPlaylist(
    int targetPlaylistId, const bms_parser::ChartMeta &chartMeta,
    std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }

  RefreshPlaylistCachesLocked(selectedPlaylistId);
  const int playlistId = targetPlaylistId;
  if (!hasPlaylistId(playlists, playlistId)) {
    errorMessage = "Select a playlist first.";
    return false;
  }
  const bool inserted =
      playlistId > 0 && repository.InsertTrack(playlistId, chartMeta);
  RefreshPlaylistCachesLocked(selectedPlaylistId);
  if (!inserted) {
    errorMessage = "Could not add selected chart to the playlist.";
    return false;
  }
  return true;
}

bool MusicPlayerService::RemoveChartFromSelectedPlaylist(
    const bms_parser::ChartMeta &chartMeta, std::string &errorMessage,
    int storedItemId) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }

  RefreshPlaylistCachesLocked(selectedPlaylistId);
  const int playlistId = selectedPlaylistId;
  const bool deleted = playlistId > 0 &&
                       repository.DeleteTrack(playlistId, chartMeta,
                                              storedItemId);
  RefreshPlaylistCachesLocked(playlistId);
  if (!deleted) {
    errorMessage = "Selected chart is not in this playlist.";
    return false;
  }
  return true;
}

bool MusicPlayerService::MoveChartInSelectedPlaylist(
    const bms_parser::ChartMeta &chartMeta, int delta,
    std::string &errorMessage, int storedItemId) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }

  RefreshPlaylistCachesLocked(selectedPlaylistId);
  const int playlistId = selectedPlaylistId;
  const bool moved = playlistId > 0 &&
                     repository.MoveTrack(playlistId, chartMeta, delta,
                                          storedItemId);
  RefreshPlaylistCachesLocked(playlistId);
  if (!moved) {
    errorMessage = delta < 0 ? "Selected track is already at the top."
                             : "Selected track is already at the bottom.";
    return false;
  }
  return true;
}

bool MusicPlayerService::ClearSelectedPlaylist(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }

  RefreshPlaylistCachesLocked(selectedPlaylistId);
  const int playlistId = selectedPlaylistId;
  const bool cleared = playlistId > 0 && repository.ClearPlaylist(playlistId);
  RefreshPlaylistCachesLocked(playlistId);
  if (!cleared) {
    errorMessage = "Could not clear the playlist.";
    return false;
  }
  return true;
}

bool MusicPlayerService::DeleteSelectedPlaylist(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }

  RefreshPlaylistCachesLocked(selectedPlaylistId);
  const int playlistId = selectedPlaylistId;
  if (playlistId <= 0) {
    errorMessage = "Select a playlist first.";
    return false;
  }
  if (playlistId == defaultPlaylistId) {
    errorMessage = "My Playlist cannot be deleted.";
    return false;
  }

  const bool deleted = repository.DeletePlaylist(playlistId);
  RefreshPlaylistCachesLocked(0);
  if (persistedState.selectedPlaylistId == playlistId) {
    persistedState.selectedPlaylistId = selectedPlaylistId;
    persistedState.playlistCursorIndex = -1;
    PersistPlayerStateLocked();
  }
  if (!deleted) {
    errorMessage = "Could not delete the playlist.";
    return false;
  }
  return true;
}

bool MusicPlayerService::SavePlaylistCursor(int playlistId, int cursorIndex,
                                            std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }

  persistedState.selectedPlaylistId = playlistId;
  persistedState.playlistCursorIndex = cursorIndex;
  const bool saved = repository.SavePlayerState(persistedState);
  if (!saved) {
    errorMessage = "Could not save music player selection.";
    return false;
  }
  return true;
}

std::optional<MusicPlaylistInfo>
MusicPlayerService::DefaultPlaylistSnapshot() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  for (const auto &playlist : playlists) {
    if (playlist.id == defaultPlaylistId) {
      return playlist;
    }
  }
  return std::nullopt;
}

std::vector<music_playlist::MusicTrack>
MusicPlayerService::DefaultPlaylistTracksSnapshot() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return defaultPlaylistTracks;
}

bool MusicPlayerService::AddChartToDefaultPlaylist(
    const bms_parser::ChartMeta &chartMeta, std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }

  RefreshPlaylistCachesLocked(selectedPlaylistId);
  if (defaultPlaylistId <= 0) {
    errorMessage = "Could not create music playlist.";
    return false;
  }
  const bool inserted = repository.InsertTrack(defaultPlaylistId, chartMeta);
  RefreshPlaylistCachesLocked(selectedPlaylistId);
  if (!inserted) {
    errorMessage = "Could not add selected chart to the music playlist.";
    return false;
  }
  return true;
}

bool MusicPlayerService::RemoveChartFromDefaultPlaylist(
    const bms_parser::ChartMeta &chartMeta, std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }

  RefreshPlaylistCachesLocked(selectedPlaylistId);
  if (defaultPlaylistId <= 0) {
    errorMessage = "Could not create music playlist.";
    return false;
  }
  const bool deleted = repository.DeleteTrack(defaultPlaylistId, chartMeta);
  RefreshPlaylistCachesLocked(selectedPlaylistId);
  if (!deleted) {
    errorMessage = "Selected chart is not in My Playlist.";
    return false;
  }
  return true;
}

bool MusicPlayerService::ClearDefaultPlaylist(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }

  RefreshPlaylistCachesLocked(selectedPlaylistId);
  if (defaultPlaylistId <= 0) {
    errorMessage = "Could not create music playlist.";
    return false;
  }
  const bool cleared = repository.ClearPlaylist(defaultPlaylistId);
  RefreshPlaylistCachesLocked(selectedPlaylistId);
  if (!cleared) {
    errorMessage = "Could not clear the music playlist.";
    return false;
  }
  return true;
}

bool MusicPlayerService::StartRandomLibrary(std::string &errorMessage,
                                            std::optional<std::uint64_t> seed) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();
  if (libraryTracks.empty()) {
    setEmptyLibraryError(errorMessage);
    queue.Clear();
    PersistQueueTracksLocked();
    return false;
  }
  queue.SetPlaylist(music_playlist::ShuffledTracks(libraryTracks, seed), 0,
                    music_playlist::kNowPlayingDisplayName);
  PersistQueueTracksLocked();
  return true;
}

bool MusicPlayerService::StartSelectedPlaylist(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }
  RefreshPlaylistCachesLocked(selectedPlaylistId);

  if (selectedPlaylistTracks.empty()) {
    setEmptyPlaylistError(errorMessage);
    queue.Clear();
    PersistQueueTracksLocked();
    return false;
  }
  const auto selectedPlaylist = findPlaylistById(playlists, selectedPlaylistId);
  queue.SetPlaylist(selectedPlaylistTracks, 0,
                    selectedPlaylist ? selectedPlaylist->name : "");
  PersistQueueTracksLocked();
  return true;
}

bool MusicPlayerService::StartDefaultPlaylist(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  bool hasDefaultPlaylist = false;
  if (!EnsureRepositoryReadyLocked(errorMessage)) {
    return false;
  }
  RefreshPlaylistCachesLocked(selectedPlaylistId);
  hasDefaultPlaylist = defaultPlaylistId > 0;

  if (!hasDefaultPlaylist) {
    errorMessage = "Could not create music playlist.";
    queue.Clear();
    PersistQueueTracksLocked();
    return false;
  }

  if (defaultPlaylistTracks.empty()) {
    setEmptyPlaylistError(errorMessage);
    queue.Clear();
    PersistQueueTracksLocked();
    return false;
  }
  queue.SetPlaylist(defaultPlaylistTracks, 0, kDefaultPlaylistName);
  PersistQueueTracksLocked();
  return true;
}

void MusicPlayerService::SetNowPlaying(
    std::vector<music_playlist::MusicTrack> tracks, std::size_t startIndex) {
  std::lock_guard<std::mutex> lock(stateMutex);
  queue.SetPlaylist(std::move(tracks), startIndex,
                    music_playlist::kNowPlayingDisplayName);
  PersistQueueTracksLocked();
}

void MusicPlayerService::SetPlaylist(
    std::vector<music_playlist::MusicTrack> tracks, std::size_t startIndex,
    std::string displayName) {
  std::lock_guard<std::mutex> lock(stateMutex);
  queue.SetPlaylist(std::move(tracks), startIndex, std::move(displayName));
  PersistQueueTracksLocked();
}

void MusicPlayerService::SetPlaylistAfterCurrentRemoved(
    std::vector<music_playlist::MusicTrack> tracks, std::size_t nextIndex,
    std::string displayName) {
  std::lock_guard<std::mutex> lock(stateMutex);
  queue.SetPlaylistAfterCurrentRemoved(std::move(tracks), nextIndex,
                                       std::move(displayName));
  PersistQueueTracksLocked();
}

bool MusicPlayerService::AppendToQueue(
    const music_playlist::MusicTrack &track, std::size_t preferredIndex,
    std::string displayName, std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  const auto snapshot = queue.Snapshot();
  const std::string nextDisplayName =
      displayName.empty() ? snapshot.displayName : std::move(displayName);
  std::vector<music_playlist::MusicTrack> visibleTracks = snapshot.tracks;
  std::vector<music_playlist::MusicTrack> persistentTracks =
      sessionOnlyQueueOrder && !sessionPersistentQueueTracks.empty()
          ? sessionPersistentQueueTracks
          : snapshot.tracks;
  visibleTracks.push_back(track);
  persistentTracks.push_back(track);

  const auto loadedIndex =
      loadedTrack ? music_playlist::FindTrackIndex(visibleTracks, *loadedTrack)
                  : std::nullopt;
  if (loadedTrack && !loadedIndex) {
    const std::size_t nextIndex =
        std::min(preferredIndex, visibleTracks.size());
    queue.SetPlaylistAfterCurrentRemoved(
        std::move(visibleTracks), nextIndex, nextDisplayName);
  } else {
    std::size_t startIndex = preferredIndex;
    if (loadedIndex) {
      startIndex = *loadedIndex;
    } else if (snapshot.currentIndex &&
               *snapshot.currentIndex < visibleTracks.size()) {
      startIndex = *snapshot.currentIndex;
    } else if (!visibleTracks.empty()) {
      startIndex = std::min(preferredIndex, visibleTracks.size() - 1);
    }
    queue.SetPlaylist(std::move(visibleTracks), startIndex, nextDisplayName);
  }

  if (sessionOnlyQueueOrder) {
    sessionPersistentQueueTracks = std::move(persistentTracks);
    PersistQueueTracksLocked(sessionPersistentQueueTracks, true);
  } else {
    PersistQueueTracksLocked();
  }

  return true;
}

bool MusicPlayerService::ShuffleQueue(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  const auto snapshot = queue.Snapshot();
  if (snapshot.tracks.size() < 2) {
    errorMessage = "Need at least two tracks to shuffle.";
    return false;
  }

  std::vector<music_playlist::MusicTrack> tracks = snapshot.tracks;
  if (!sessionOnlyQueueOrder || sessionPersistentQueueTracks.empty()) {
    sessionPersistentQueueTracks = snapshot.tracks;
  }
  std::optional<music_playlist::MusicTrack> anchorTrack;
  std::optional<std::size_t> anchorIndex;
  if (loadedTrack) {
    if (const auto index =
            music_playlist::FindTrackIndex(tracks, *loadedTrack)) {
      anchorTrack = *loadedTrack;
      anchorIndex = *index;
    }
  }
  if (!anchorTrack && snapshot.currentIndex &&
      *snapshot.currentIndex < tracks.size()) {
    anchorTrack = tracks[*snapshot.currentIndex];
    anchorIndex = *snapshot.currentIndex;
  }

  if (anchorTrack && anchorIndex) {
    const std::size_t requestedIndex = *anchorIndex;
    tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(*anchorIndex));
    tracks = music_playlist::ShuffledTracks(std::move(tracks));
    const std::size_t insertIndex = std::min(requestedIndex, tracks.size());
    tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(insertIndex),
                  *anchorTrack);
    queue.SetPlaylist(std::move(tracks), insertIndex, snapshot.displayName);
  } else {
    tracks = music_playlist::ShuffledTracks(std::move(tracks));
    if (loadedTrack && snapshot.detachedCurrentNextIndex) {
      const std::size_t nextIndex =
          std::min(*snapshot.detachedCurrentNextIndex, tracks.size());
      queue.SetPlaylistAfterCurrentRemoved(
          std::move(tracks), nextIndex, snapshot.displayName);
    } else {
      queue.SetPlaylist(std::move(tracks), 0, snapshot.displayName);
    }
  }

  sessionOnlyQueueOrder = true;
  SyncNativeQueueLocked();
  return true;
}

bool MusicPlayerService::SetStorageMaintenance(bool active, std::string &errorMessage) {
  {
    std::lock_guard admission(playbackAdmissionMutex);
    storageMaintenance = active;
    native_music_player::SetControlEventsEnabled(!active);
  }
  if (!active) return true;
  // Do not hold admission while joining the native event pump: it may be
  // finishing a Next/Previous request which must now observe the closed gate.
  Stop(errorMessage);
  errorMessage.clear();
  return !native_music_player::IsSupported() || native_music_player::Unload(errorMessage);
}

bool MusicPlayerService::PlayCurrent(std::string &errorMessage) {
  std::lock_guard admission(playbackAdmissionMutex);
  if (storageMaintenance) { errorMessage = "Storage maintenance is active."; return false; }
  std::lock_guard<std::mutex> lock(stateMutex);
  return PlayCurrentLocked(errorMessage);
}

bool MusicPlayerService::PlayCurrentLocked(std::string &errorMessage) {
  errorMessage.clear();
  const auto *track = queue.Current();
  if (track == nullptr) {
    errorMessage = "No music track is selected.";
    return false;
  }
  return PlayTrackLocked(*track, errorMessage);
}

bool MusicPlayerService::PlayNext(std::string &errorMessage) {
  std::lock_guard admission(playbackAdmissionMutex);
  if (storageMaintenance) { errorMessage = "Storage maintenance is active."; return false; }
  std::lock_guard<std::mutex> lock(stateMutex);
  return PlayNextLocked(errorMessage);
}

bool MusicPlayerService::PlayNextLocked(std::string &errorMessage) {
  errorMessage.clear();
  const auto *track = queue.Next();
  if (track == nullptr) {
    errorMessage = "No next music track is available.";
    return false;
  }
  PersistQueueCursorLocked();
  return PlayTrackLocked(*track, errorMessage);
}

bool MusicPlayerService::PlayPrevious(std::string &errorMessage) {
  std::lock_guard admission(playbackAdmissionMutex);
  if (storageMaintenance) { errorMessage = "Storage maintenance is active."; return false; }
  std::lock_guard<std::mutex> lock(stateMutex);
  return PlayPreviousLocked(errorMessage);
}

bool MusicPlayerService::PlayPreviousLocked(std::string &errorMessage) {
  errorMessage.clear();
  const auto *track = queue.Previous();
  if (track == nullptr) {
    errorMessage = "No previous music track is available.";
    return false;
  }
  PersistQueueCursorLocked();
  return PlayTrackLocked(*track, errorMessage);
}

bool MusicPlayerService::PlayCurrentAsync(i18n::Text &statusMessage,
                                          i18n::Text successMessage) {
  return StartPlaybackAsync(PlaybackRequest::Current, statusMessage,
                            std::move(successMessage));
}

bool MusicPlayerService::PlayNextAsync(i18n::Text &statusMessage,
                                       i18n::Text successMessage) {
  return StartPlaybackAsync(PlaybackRequest::Next, statusMessage,
                            std::move(successMessage));
}

bool MusicPlayerService::PlayPreviousAsync(i18n::Text &statusMessage,
                                           i18n::Text successMessage) {
  return StartPlaybackAsync(PlaybackRequest::Previous, statusMessage,
                            std::move(successMessage));
}

bool MusicPlayerService::PlayCurrentAsync(std::string &statusMessage,
                                         i18n::Text successMessage) {
  i18n::Text status;
  const bool started = PlayCurrentAsync(status, std::move(successMessage));
  statusMessage = status.resolve();
  return started;
}

bool MusicPlayerService::PlayNextAsync(std::string &statusMessage,
                                         i18n::Text successMessage) {
  i18n::Text status;
  const bool started = PlayNextAsync(status, std::move(successMessage));
  statusMessage = status.resolve();
  return started;
}

bool MusicPlayerService::PlayPreviousAsync(std::string &statusMessage,
                                         i18n::Text successMessage) {
  i18n::Text status;
  const bool started = PlayPreviousAsync(status, std::move(successMessage));
  statusMessage = status.resolve();
  return started;
}

bool MusicPlayerService::Resume(std::string &errorMessage) {
  std::lock_guard admission(playbackAdmissionMutex);
  if (storageMaintenance) { errorMessage = "Storage maintenance is active."; return false; }
  std::lock_guard<std::mutex> lock(stateMutex);
  return native_music_player::Play(errorMessage);
}

bool MusicPlayerService::Pause(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  return native_music_player::Pause(errorMessage);
}

bool MusicPlayerService::Stop(std::string &errorMessage) {
  ClearSleepTimer();
  return StopPlaybackInternal(errorMessage);
}

bool MusicPlayerService::StopPlaybackInternal(std::string &errorMessage) {
  StopPlaybackWorker();
  bool stopped = false;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    stopped = native_music_player::Stop(errorMessage);
    loadedTrack.reset();
  }
  StopNativeControlEventPump();
  return stopped;
}

bool MusicPlayerService::Seek(long long positionMicros,
                              std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  return native_music_player::Seek(positionMicros, errorMessage);
}

bool MusicPlayerService::SetPlaybackRate(audio::PlaybackRate rate,
                                         std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  if (!native_music_player::SetPlaybackRate(rate, errorMessage)) {
    return false;
  }
  playbackRate = rate;
  return true;
}

audio::PlaybackRate MusicPlayerService::PlaybackRate() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return playbackRate;
}

bool MusicPlayerService::SetClubMode(bool enabled,
                                     std::string &statusMessage) {
  std::lock_guard admission(playbackAdmissionMutex);
  if (storageMaintenance) { statusMessage = "Storage maintenance is active."; return false; }
  std::optional<music_playlist::MusicTrack> track;
  bool appliedImmediately = false;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (clubMode == enabled) {
      statusMessage = enabled ? "Club Beat is on." : "Club Beat is off.";
      return true;
    }
    track = loadedTrack;
    if (!track) {
      clubMode = enabled;
      appliedImmediately = true;
    }
  }

  StopPlaybackWorker();
  if (appliedImmediately) {
    statusMessage = enabled ? "Club Beat on." : "Club Beat off.";
    return true;
  }

  const std::uint64_t requestRevision =
      playbackRequestRevision.fetch_add(1, std::memory_order_acq_rel) + 1;
  renderCancelled.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(playbackThreadMutex);
    playbackThread = std::jthread(
        [this, track = std::move(*track), requestRevision,
         enabled](const std::stop_token &stopToken) mutable {
          ClubModeSwitchWorker(std::move(track), requestRevision, enabled,
                               stopToken);
        });
  }
  statusMessage = enabled ? "Preparing Club Beat..."
                          : "Preparing original mix...";
  return true;
}

bool MusicPlayerService::ClubMode() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return clubMode;
}

bool MusicPlayerService::SetSleepTimer(long long durationMicros,
                                       std::string &statusMessage) {
  statusMessage.clear();
  if (durationMicros <= 0) {
    ClearSleepTimer();
    statusMessage = "Sleep timer off.";
    return true;
  }

  const auto duration = std::chrono::microseconds(durationMicros);
  {
    std::lock_guard<std::mutex> lock(sleepTimerMutex);
    sleepTimerDeadline = std::chrono::steady_clock::now() + duration;
  }
  EnsureSleepTimerWorker();
  sleepTimerCv.notify_all();
  statusMessage = "Sleep timer set.";
  return true;
}

void MusicPlayerService::ClearSleepTimer() {
  {
    std::lock_guard<std::mutex> lock(sleepTimerMutex);
    sleepTimerDeadline.reset();
  }
  sleepTimerCv.notify_all();
}

long long MusicPlayerService::SleepTimerRemainingMicros() const {
  std::lock_guard<std::mutex> lock(sleepTimerMutex);
  if (!sleepTimerDeadline) {
    return 0;
  }
  const auto now = std::chrono::steady_clock::now();
  if (*sleepTimerDeadline <= now) {
    return 0;
  }
  return std::chrono::duration_cast<std::chrono::microseconds>(
             *sleepTimerDeadline - now)
      .count();
}

bool MusicPlayerService::ProcessNativeControlEvents(
    i18n::Text &statusMessage) {
  statusMessage = {};
  bool handled = false;
  const auto restoreIdleSession = [this] {
    std::lock_guard lock(stateMutex);
    const auto state = native_music_player::GetState();
    if (!state.playing) {
      std::string ignored;
      if (state.loaded) {
        native_music_player::Pause(ignored);
      } else {
        native_music_player::Stop(ignored);
      }
    }
  };
  for (auto &error : native_music_player::DrainPlaybackErrors()) {
    handled = true;
    statusMessage = std::move(error);
    std::lock_guard lock(stateMutex);
    // A queued failure may belong to a track replaced before this drain.
    if (!native_music_player::GetState().loaded) {
      loadedTrack.reset();
    }
  }
  for (const auto event : native_music_player::DrainControlEvents()) {
    handled = true;
    switch (event) {
    case native_music_player::ControlEvent::Previous:
      if (!PlayPreviousAsync(statusMessage)) {
        restoreIdleSession();
      }
      break;
    case native_music_player::ControlEvent::Next:
      if (!PlayNextAsync(statusMessage)) {
        restoreIdleSession();
      }
      break;
    case native_music_player::ControlEvent::Finished:
      if (!PlayNextAsync(statusMessage, i18n::message(
              "music_player.service.finished_next.message"))) {
        std::lock_guard lock(stateMutex);
        std::string ignored;
        native_music_player::Stop(ignored);
        loadedTrack.reset();
      }
      break;
    }
  }
  return handled;
}

bool MusicPlayerService::ConsumeNativeControlStatus(
    i18n::Text &statusMessage) {
  std::lock_guard<std::mutex> lock(nativeControlStatusMutex);
  if (consumedNativeControlStatusRevision == nativeControlStatusRevision) {
    return false;
  }
  consumedNativeControlStatusRevision = nativeControlStatusRevision;
  statusMessage = nativeControlStatusMessage;
  return true;
}

bool MusicPlayerService::ProcessNativeControlEvents(
    std::string &statusMessage) {
  i18n::Text status;
  const bool handled = ProcessNativeControlEvents(status);
  statusMessage = status.resolve();
  return handled;
}

bool MusicPlayerService::ConsumeNativeControlStatus(
    std::string &statusMessage) {
  i18n::Text status;
  if (!ConsumeNativeControlStatus(status)) {
    return false;
  }
  statusMessage = status.resolve();
  return true;
}

void MusicPlayerService::CancelRender() {
  renderCancelled.store(true, std::memory_order_release);
}

std::optional<music_playlist::MusicTrack>
MusicPlayerService::CurrentTrackSnapshot() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  if (loadedTrack) {
    return loadedTrack;
  }
  const auto *track = queue.Current();
  if (track == nullptr) {
    return std::nullopt;
  }
  return *track;
}

music_playlist::MusicQueueSnapshot MusicPlayerService::QueueSnapshot() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return queue.Snapshot();
}

music_playlist::QueueRepeatMode MusicPlayerService::RepeatMode() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return queue.RepeatMode();
}

void MusicPlayerService::SetRepeatMode(
    music_playlist::QueueRepeatMode mode) {
  std::lock_guard<std::mutex> lock(stateMutex);
  queue.SetRepeatMode(mode);
  PersistQueueCursorLocked();
}

bool MusicPlayerService::PlayTrackLocked(
    const music_playlist::MusicTrack &track, std::string &errorMessage) {
  errorMessage.clear();

  if (!native_music_player::IsSupported()) {
    errorMessage = "Native music playback is not supported on this platform.";
    return false;
  }

  bool playing = false;
  ScopeExit releaseFailedSession([this, &playing] {
    if (!playing && !native_music_player::GetState().playing) {
      std::string ignored;
      native_music_player::Stop(ignored);
      loadedTrack.reset();
    }
  });
  renderCancelled.store(false, std::memory_order_release);
  auto prepared = music_preparation::PrepareMusicForPlayback(
      track.representativeChart, renderCancelled, clubMode);
  if (!prepared.success) {
    errorMessage = prepared.message.empty()
                       ? "Could not render music track."
                       : prepared.message;
    return false;
  }

  auto metadata = music_playlist::MakeNativeMetadata(track);
  metadata.durationMicros = 0;
  if (prepared.durationMicros > 0) {
    metadata.durationMicros = prepared.durationMicros;
  }

  const bool loaded = native_music_player::LoadStream(prepared.stream, metadata, errorMessage);
  if (!loaded) {
    return false;
  }
  if (!native_music_player::SetPlaybackRate(playbackRate, errorMessage)) {
    return false;
  }
  playing = native_music_player::Play(errorMessage);
  if (playing) {
    loadedTrack = track;
    if (prepared.durationMicros > 0) {
      loadedTrack->durationMicros = prepared.durationMicros;
    }
    SyncNativeQueueLocked();
    EnsureNativeControlEventPump();
  }
  return playing;
}

bool MusicPlayerService::StartPlaybackAsync(PlaybackRequest request,
                                            i18n::Text &statusMessage,
                                            i18n::Text successMessage) {
  std::lock_guard admission(playbackAdmissionMutex);
  if (storageMaintenance) { statusMessage = "Storage maintenance is active."; return false; }
  statusMessage = {};
  if (!native_music_player::IsSupported()) {
    statusMessage = i18n::message("music_player.service.native_unavailable.message");
    return false;
  }

  music_playlist::MusicTrack track;
  bool requestedClubMode = false;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    const music_playlist::MusicTrack *selectedTrack = nullptr;
    switch (request) {
    case PlaybackRequest::Current:
      selectedTrack = queue.Current();
      break;
    case PlaybackRequest::Next:
      selectedTrack = queue.Next();
      break;
    case PlaybackRequest::Previous:
      selectedTrack = queue.Previous();
      break;
    }
    if (selectedTrack == nullptr) {
      switch (request) {
      case PlaybackRequest::Current:
        statusMessage = i18n::message("music_player.service.no_track_selected.message");
        break;
      case PlaybackRequest::Next:
        statusMessage = i18n::message("music_player.service.no_next_track.message");
        break;
      case PlaybackRequest::Previous:
        statusMessage = i18n::message("music_player.service.no_previous_track.message");
        break;
      }
      return false;
    }
    if (request != PlaybackRequest::Current) {
      PersistQueueCursorLocked();
    }
    track = *selectedTrack;
    requestedClubMode = clubMode;
  }

  StopPlaybackWorker();

  const std::uint64_t requestRevision =
      playbackRequestRevision.fetch_add(1, std::memory_order_acq_rel) + 1;
  renderCancelled.store(false, std::memory_order_release);

  {
    std::lock_guard<std::mutex> lock(playbackThreadMutex);
    playbackThread = std::jthread(
        [this, track = std::move(track), requestRevision, requestedClubMode,
         successMessage = std::move(successMessage)](
            const std::stop_token &stopToken) mutable {
          PlaybackWorker(std::move(track), requestRevision, requestedClubMode,
                         std::move(successMessage), stopToken);
        });
  }

  statusMessage = i18n::message("music_player.service.preparing.progress");
  return true;
}

void MusicPlayerService::PlaybackWorker(
    music_playlist::MusicTrack track, std::uint64_t requestRevision,
    bool requestedClubMode, i18n::Text successMessage,
    const std::stop_token &stopToken) {
  const auto isCurrentRequest = [this, requestRevision, &stopToken]() {
    return !stopToken.stop_requested() &&
           playbackRequestRevision.load(std::memory_order_acquire) ==
               requestRevision;
  };

  if (!isCurrentRequest()) {
    return;
  }

  music_preparation::PreparedMusic prepared;
  try {
    prepared = music_preparation::PrepareMusicForPlayback(
        track.representativeChart, renderCancelled, requestedClubMode);
  } catch (const std::exception &e) {
    prepared = {.message = std::string("Could not render music track: ") + e.what()};
  } catch (...) {
    prepared = {.message = "Could not render music track."};
  }
  if (!isCurrentRequest()) {
    return;
  }

  i18n::Text statusMessage;
  bool playing = false;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (!isCurrentRequest()) {
      return;
    }
    if (!prepared.success) {
      statusMessage = prepared.message.empty()
                          ? i18n::message("music_player.service.render_failed.message")
                          : prepared.message;
    } else {
      auto metadata = music_playlist::MakeNativeMetadata(track);
      metadata.durationMicros = 0;
      if (prepared.durationMicros > 0) {
        metadata.durationMicros = prepared.durationMicros;
      }

      std::string errorMessage;
      const bool loaded = native_music_player::LoadStream(prepared.stream, metadata, errorMessage);
      if (!loaded) {
        statusMessage = errorMessage;
      } else if (!native_music_player::SetPlaybackRate(playbackRate,
                                                       errorMessage)) {
        statusMessage = errorMessage;
      } else if (!native_music_player::Play(errorMessage)) {
        statusMessage = errorMessage;
      } else {
        loadedTrack = track;
        if (prepared.durationMicros > 0) {
          loadedTrack->durationMicros = prepared.durationMicros;
        }
        SyncNativeQueueLocked();
        playing = true;
        statusMessage =
            successMessage.empty() ? i18n::message("music_player.service.playing.message") : successMessage;
      }
    }
    // Android keeps focus/foreground eligibility during a queue handoff.
    // Release it when preparation/loading fails with no prior track playing.
    if (!playing && !native_music_player::GetState().playing) {
      std::string ignored;
      native_music_player::Stop(ignored);
      loadedTrack.reset();
    }
  }

  if (playing) {
    EnsureNativeControlEventPump();
  }
  if (isCurrentRequest() && !statusMessage.empty()) {
    PublishNativeControlStatus(statusMessage);
  }
}

void MusicPlayerService::ClubModeSwitchWorker(
    music_playlist::MusicTrack track, std::uint64_t requestRevision,
    bool requestedClubMode, const std::stop_token &stopToken) {
  const auto isCurrentRequest = [this, requestRevision, &stopToken]() {
    return !stopToken.stop_requested() &&
           playbackRequestRevision.load(std::memory_order_acquire) ==
               requestRevision;
  };

  music_preparation::PreparedMusic prepared;
  try {
    prepared = music_preparation::PrepareMusicForPlayback(
        track.representativeChart, renderCancelled, requestedClubMode);
  } catch (const std::exception &e) {
    prepared = {.message = std::string("Could not switch Club Beat: ") + e.what()};
  } catch (...) {
    prepared = {.message = "Could not switch Club Beat."};
  }
  if (!isCurrentRequest()) {
    return;
  }

  i18n::Text statusMessage;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (!isCurrentRequest() || !loadedTrack ||
        !music_playlist::SameTrackIdentity(*loadedTrack, track)) {
      return;
    }
    if (!prepared.success) {
      statusMessage = prepared.message.empty()
                          ? i18n::message("music_player.service.club_switch_failed.message")
                          : prepared.message;
    } else {
      const auto previousState = native_music_player::GetState();
      auto metadata = music_playlist::MakeNativeMetadata(track);
      metadata.durationMicros = prepared.durationMicros;
      std::string errorMessage;
      const bool loaded = native_music_player::LoadStream(prepared.stream, metadata, errorMessage);
      if (!loaded ||
          !native_music_player::SetPlaybackRate(playbackRate, errorMessage) ||
          (previousState.positionMicros > 0 &&
           !native_music_player::Seek(previousState.positionMicros,
                                      errorMessage)) ||
          (previousState.playing && !native_music_player::Play(errorMessage))) {
        statusMessage = errorMessage;
        if (!native_music_player::GetState().playing) {
          std::string ignored;
          native_music_player::Stop(ignored);
          loadedTrack.reset();
        }
      } else {
        clubMode = requestedClubMode;
        if (prepared.durationMicros > 0) {
          loadedTrack->durationMicros = prepared.durationMicros;
        }
        SyncNativeQueueLocked();
        statusMessage = requestedClubMode ? i18n::message("music_player.service.club_on.message")
                                          : i18n::message("music_player.service.club_off.message");
      }
    }
  }

  if (isCurrentRequest() && !statusMessage.empty()) {
    PublishNativeControlStatus(statusMessage);
  }
}

void MusicPlayerService::StopPlaybackWorker() {
  renderCancelled.store(true, std::memory_order_release);
  playbackRequestRevision.fetch_add(1, std::memory_order_acq_rel);

  std::jthread threadToStop;
  {
    std::lock_guard<std::mutex> lock(playbackThreadMutex);
    if (!playbackThread.joinable()) {
      return;
    }
    playbackThread.request_stop();
    threadToStop = std::move(playbackThread);
  }

  if (threadToStop.joinable()) {
    threadToStop.join();
  }
}

void MusicPlayerService::EnsureSleepTimerWorker() {
  std::lock_guard<std::mutex> lock(sleepTimerThreadMutex);
  if (sleepTimerThread.joinable()) {
    return;
  }
  sleepTimerThread = std::jthread([this](const std::stop_token &stopToken) {
    SleepTimerWorker(stopToken);
  });
}

void MusicPlayerService::StopSleepTimerWorker() {
  {
    std::lock_guard<std::mutex> lock(sleepTimerMutex);
    sleepTimerDeadline.reset();
  }
  sleepTimerCv.notify_all();

  std::jthread threadToStop;
  {
    std::lock_guard<std::mutex> lock(sleepTimerThreadMutex);
    if (!sleepTimerThread.joinable()) {
      return;
    }
    {
      // Serialize the stop predicate with entry into the timer's wait.
      std::lock_guard<std::mutex> timerLock(sleepTimerMutex);
      sleepTimerThread.request_stop();
    }
    threadToStop = std::move(sleepTimerThread);
  }

  sleepTimerCv.notify_all();
  if (threadToStop.joinable()) {
    threadToStop.join();
  }
}

void MusicPlayerService::SleepTimerWorker(const std::stop_token &stopToken) {
  std::unique_lock<std::mutex> lock(sleepTimerMutex);
  while (!stopToken.stop_requested()) {
    if (!sleepTimerDeadline) {
      sleepTimerCv.wait(lock, [this, &stopToken] {
        return stopToken.stop_requested() || sleepTimerDeadline.has_value();
      });
      continue;
    }

    const auto deadline = *sleepTimerDeadline;
    const auto now = std::chrono::steady_clock::now();
    if (now < deadline) {
      sleepTimerCv.wait_until(lock, deadline, [this, &stopToken, deadline] {
        return stopToken.stop_requested() || !sleepTimerDeadline ||
               *sleepTimerDeadline != deadline;
      });
      continue;
    }

    sleepTimerDeadline.reset();
    lock.unlock();
    std::string stopMessage;
    if (StopPlaybackInternal(stopMessage)) {
      PublishNativeControlStatus(i18n::message(
          "music_player.service.sleep_stopped.message"));
    } else {
      PublishNativeControlStatus(stopMessage.empty()
                                     ? i18n::message("music_player.service.sleep_expired.message")
                                     : stopMessage);
    }
    lock.lock();
  }
}

void MusicPlayerService::EnsureNativeControlEventPump() {
  if (!native_music_player::IsSupported()) {
    return;
  }

  std::lock_guard<std::mutex> lock(nativeControlThreadMutex);
  if (nativeControlEventThreadStopping) {
    return;
  }
  if (nativeControlEventThread.joinable()) {
    return;
  }
  nativeControlEventThread =
      std::jthread([this](const std::stop_token &stopToken) {
        NativeControlEventLoop(stopToken);
      });
}

void MusicPlayerService::StopNativeControlEventPump() {
  std::jthread threadToStop;
  {
    std::lock_guard<std::mutex> lock(nativeControlThreadMutex);
    if (!nativeControlEventThread.joinable()) {
      return;
    }
    nativeControlEventThreadStopping = true;
    nativeControlEventThread.request_stop();
    native_music_player::DrainControlEvents();
    native_music_player::DrainPlaybackErrors();
    threadToStop = std::move(nativeControlEventThread);
  }

  if (threadToStop.joinable()) {
    threadToStop.join();
  }

  std::lock_guard<std::mutex> lock(nativeControlThreadMutex);
  nativeControlEventThreadStopping = false;
}

void MusicPlayerService::NativeControlEventLoop(
    const std::stop_token &stopToken) {
  while (!stopToken.stop_requested()) {
    i18n::Text statusMessage;
    if (ProcessNativeControlEvents(statusMessage) && !statusMessage.empty()) {
      PublishNativeControlStatus(statusMessage);
    }

    for (int i = 0; i < 10 && !stopToken.stop_requested(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }
}

void MusicPlayerService::PublishNativeControlStatus(
    const i18n::Text &statusMessage) {
  std::lock_guard<std::mutex> lock(nativeControlStatusMutex);
  nativeControlStatusMessage = statusMessage;
  ++nativeControlStatusRevision;
}

} // namespace music_player
