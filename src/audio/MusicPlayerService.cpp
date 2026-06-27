#include "MusicPlayerService.h"

#include "../SqliteRAII.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <thread>
#include <unordered_set>
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
loadPlaylistTracks(MusicPlaylistDB &playlistDb, sqlite3 *db, int playlistId) {
  std::vector<MusicTrackRecord> records;
  playlistDb.SelectTracks(db, playlistId, records);
  return music_playlist::MakeTracks(records);
}

std::vector<music_playlist::MusicTrack>
loadFavoriteTracks() {
  std::vector<MusicTrackRecord> records;
  auto &chartDb = ChartDBHelper::GetInstance();
  sqlite3 *db = chartDb.Connect();
  if (db == nullptr) {
    return {};
  }
  chartDb.SelectFavoriteMusicTracks(db, records);
  chartDb.Close(db);
  return music_playlist::MakeTracks(records);
}

std::vector<music_playlist::MusicTrack>
loadNowPlayingTracks(MusicPlaylistDB &playlistDb, sqlite3 *db) {
  std::vector<MusicTrackRecord> records;
  playlistDb.SelectNowPlayingTracks(db, records);
  return music_playlist::MakeTracks(records);
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

std::uint64_t fnv1a64(const std::string &value) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const unsigned char c : value) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

long long nativeQueueItemIdForTrack(
    const music_playlist::MusicTrack &track, std::size_t index) {
  const std::string identity = !track.trackId.empty() ? track.trackId
                               : !track.chartId.empty() ? track.chartId
                                                        : track.title;
  const std::string key = identity + "\n" + std::to_string(index);
  std::uint64_t value =
      fnv1a64(key) & static_cast<std::uint64_t>(
                        std::numeric_limits<long long>::max());
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

MusicPlayerService::~MusicPlayerService() {
  StopSleepTimerWorker();
  StopPlaybackWorker();
  StopAdjacentPreloadWorker();
  StopNativeControlEventPump();
}

bool MusicPlayerService::ReloadLibrary(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  std::vector<MusicTrackRecord> records;
  playlistDb.SelectLibraryTracks(db, records);
  favoriteTracks = loadFavoriteTracks();
  playlistDb.Close(db);

  libraryTracks = music_playlist::MakeTracks(records);
  return true;
}

bool MusicPlayerService::ReloadLibraryAndPlaylists(
    std::string &errorMessage, int preferredSelectedPlaylistId) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  std::vector<MusicTrackRecord> records;
  playlistDb.SelectLibraryTracks(db, records);
  libraryTracks = music_playlist::MakeTracks(records);
  favoriteTracks = loadFavoriteTracks();
  persistedState = playlistDb.SelectPlayerState(db);

  RefreshPlaylistCachesLocked(playlistDb, db, preferredSelectedPlaylistId);
  if (defaultPlaylistId <= 0) {
    playlistDb.Close(db);
    errorMessage = "Could not create music playlist.";
    return false;
  }
  RestoreQueueFromPersistedStateLocked(playlistDb, db);
  playlistDb.Close(db);
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
  errorMessage.clear();
  tracks.clear();

  if (groupTrack.groupId.empty()) {
    errorMessage = "Selected track does not belong to a chart group.";
    return false;
  }

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  std::vector<MusicTrackRecord> records;
  playlistDb.SelectLibraryGroupTracks(db, groupTrack.representativeChart,
                                      records);
  playlistDb.Close(db);

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

  auto &chartDb = ChartDBHelper::GetInstance();
  sqlite3 *chartDbConnection = chartDb.Connect();
  if (chartDbConnection == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }
  const bool updated =
      chartDb.SetFavorite(chartDbConnection, chartMeta, favorite);
  chartDb.Close(chartDbConnection);
  favoriteTracks = loadFavoriteTracks();
  if (!updated) {
    errorMessage = favorite ? "Could not add favorite."
                            : "Could not remove favorite.";
    return false;
  }
  return true;
}

void MusicPlayerService::RefreshPlaylistCachesLocked(
    MusicPlaylistDB &playlistDb, sqlite3 *db,
    int preferredSelectedPlaylistId) {
  defaultPlaylistId = playlistDb.EnsurePlaylist(db, kDefaultPlaylistName);
  playlists = playlistDb.SelectPlaylists(db);

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
      defaultPlaylistId > 0 ? loadPlaylistTracks(playlistDb, db, defaultPlaylistId)
                            : std::vector<music_playlist::MusicTrack>{};
  selectedPlaylistTracks =
      selectedPlaylistId == defaultPlaylistId
          ? defaultPlaylistTracks
          : (selectedPlaylistId > 0
                 ? loadPlaylistTracks(playlistDb, db, selectedPlaylistId)
                 : std::vector<music_playlist::MusicTrack>{});
}

void MusicPlayerService::RestoreQueueFromPersistedStateLocked(
    MusicPlaylistDB &playlistDb, sqlite3 *db) {
  queue.SetRepeatMode(repeatModeFromRecordValue(persistedState.repeatMode));
  if (!queue.Empty()) {
    SyncNativeQueueLocked();
    return;
  }

  auto nowPlayingTracks = loadNowPlayingTracks(playlistDb, db);
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

void MusicPlayerService::PersistPlayerStateLocked(
    MusicPlaylistDB &playlistDb, sqlite3 *db) {
  playlistDb.SavePlayerState(db, persistedState);
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
  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
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

  const bool savedTracks = playlistDb.ReplaceNowPlayingTracks(db, chartMetas);
  const bool savedState =
      savedTracks && playlistDb.SavePlayerState(db, nextState);
  playlistDb.Close(db);
  if (savedTracks && savedState) {
    persistedState = std::move(nextState);
  }
  SyncNativeQueueLocked();
}

void MusicPlayerService::PersistQueueCursorLocked() {
  const auto snapshot = queue.Snapshot();
  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
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

  const bool saved = playlistDb.SavePlayerState(db, nextState);
  playlistDb.Close(db);
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

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  persistedState = playlistDb.SelectPlayerState(db);
  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  if (defaultPlaylistId <= 0) {
    playlistDb.Close(db);
    errorMessage = "Could not create music playlist.";
    return false;
  }
  playlistDb.Close(db);
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

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return 0;
  }

  const int playlistId = playlistDb.EnsurePlaylist(db, name);
  if (playlistId <= 0) {
    playlistDb.Close(db);
    errorMessage = "Could not create music playlist.";
    return 0;
  }
  RefreshPlaylistCachesLocked(playlistDb, db, playlistId);
  persistedState.selectedPlaylistId = selectedPlaylistId;
  persistedState.playlistCursorIndex = -1;
  PersistPlayerStateLocked(playlistDb, db);
  playlistDb.Close(db);
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

  MusicPlaylistDB playlistDb;
  SqliteConnectionHandle dbHandle(playlistDb.Connect());
  sqlite3 *db = dbHandle.get();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return 0;
  }

  std::string transactionError;
  SqliteTransactionHandle transaction(db, "BEGIN IMMEDIATE", transactionError);
  if (!transaction.active()) {
    std::cerr << "SQL error while beginning music playlist save: "
              << transactionError << "\n";
    errorMessage = "Could not start music playlist save.";
    return 0;
  }

  const int playlistId = playlistDb.EnsurePlaylist(db, name);
  if (playlistId <= 0) {
    errorMessage = "Could not create music playlist.";
    return 0;
  }

  bool insertedAll = true;
  for (const auto &track : tracks) {
    if (!playlistDb.InsertTrack(db, playlistId, track.representativeChart)) {
      insertedAll = false;
      break;
    }
  }

  if (!insertedAll) {
    errorMessage = "Could not save every Now Playing track.";
    return 0;
  }

  if (!transaction.commit(transactionError)) {
    std::cerr << "SQL error while finishing music playlist save: "
              << transactionError << "\n";
    errorMessage = "Could not save music playlist.";
    return 0;
  }

  RefreshPlaylistCachesLocked(playlistDb, db, playlistId);
  persistedState.selectedPlaylistId = selectedPlaylistId;
  persistedState.playlistCursorIndex = -1;
  PersistPlayerStateLocked(playlistDb, db);
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

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  const int playlistId = selectedPlaylistId;
  const bool renamed = playlistDb.RenamePlaylist(db, playlistId, name);
  RefreshPlaylistCachesLocked(playlistDb, db, playlistId);
  playlistDb.Close(db);
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

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  RefreshPlaylistCachesLocked(playlistDb, db, playlistId);
  persistedState.selectedPlaylistId = selectedPlaylistId;
  persistedState.playlistCursorIndex = -1;
  PersistPlayerStateLocked(playlistDb, db);
  playlistDb.Close(db);
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

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  const int playlistId = targetPlaylistId;
  if (!hasPlaylistId(playlists, playlistId)) {
    playlistDb.Close(db);
    errorMessage = "Select a playlist first.";
    return false;
  }
  const bool inserted =
      playlistId > 0 &&
      playlistDb.InsertTrack(db, playlistId, chartMeta);
  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  playlistDb.Close(db);
  if (!inserted) {
    errorMessage = "Could not add selected chart to the playlist.";
    return false;
  }
  return true;
}

bool MusicPlayerService::RemoveChartFromSelectedPlaylist(
    const bms_parser::ChartMeta &chartMeta, std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  const int playlistId = selectedPlaylistId;
  const bool deleted =
      playlistId > 0 &&
      playlistDb.DeleteTrack(db, playlistId, chartMeta);
  RefreshPlaylistCachesLocked(playlistDb, db, playlistId);
  playlistDb.Close(db);
  if (!deleted) {
    errorMessage = "Selected chart is not in this playlist.";
    return false;
  }
  return true;
}

bool MusicPlayerService::MoveChartInSelectedPlaylist(
    const bms_parser::ChartMeta &chartMeta, int delta,
    std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  const int playlistId = selectedPlaylistId;
  const bool moved =
      playlistId > 0 &&
      playlistDb.MoveTrack(db, playlistId, chartMeta, delta);
  RefreshPlaylistCachesLocked(playlistDb, db, playlistId);
  playlistDb.Close(db);
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

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  const int playlistId = selectedPlaylistId;
  const bool cleared =
      playlistId > 0 && playlistDb.ClearPlaylist(db, playlistId);
  RefreshPlaylistCachesLocked(playlistDb, db, playlistId);
  playlistDb.Close(db);
  if (!cleared) {
    errorMessage = "Could not clear the playlist.";
    return false;
  }
  return true;
}

bool MusicPlayerService::DeleteSelectedPlaylist(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  const int playlistId = selectedPlaylistId;
  if (playlistId <= 0) {
    playlistDb.Close(db);
    errorMessage = "Select a playlist first.";
    return false;
  }
  if (playlistId == defaultPlaylistId) {
    playlistDb.Close(db);
    errorMessage = "My Playlist cannot be deleted.";
    return false;
  }

  const bool deleted = playlistDb.DeletePlaylist(db, playlistId);
  RefreshPlaylistCachesLocked(playlistDb, db, 0);
  if (persistedState.selectedPlaylistId == playlistId) {
    persistedState.selectedPlaylistId = selectedPlaylistId;
    persistedState.playlistCursorIndex = -1;
    PersistPlayerStateLocked(playlistDb, db);
  }
  playlistDb.Close(db);
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

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  persistedState.selectedPlaylistId = playlistId;
  persistedState.playlistCursorIndex = cursorIndex;
  const bool saved = playlistDb.SavePlayerState(db, persistedState);
  playlistDb.Close(db);
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

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  if (defaultPlaylistId <= 0) {
    playlistDb.Close(db);
    errorMessage = "Could not create music playlist.";
    return false;
  }
  const bool inserted =
      playlistDb.InsertTrack(db, defaultPlaylistId, chartMeta);
  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  playlistDb.Close(db);
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

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  if (defaultPlaylistId <= 0) {
    playlistDb.Close(db);
    errorMessage = "Could not create music playlist.";
    return false;
  }
  const bool deleted =
      playlistDb.DeleteTrack(db, defaultPlaylistId, chartMeta);
  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  playlistDb.Close(db);
  if (!deleted) {
    errorMessage = "Selected chart is not in My Playlist.";
    return false;
  }
  return true;
}

bool MusicPlayerService::ClearDefaultPlaylist(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  if (defaultPlaylistId <= 0) {
    playlistDb.Close(db);
    errorMessage = "Could not create music playlist.";
    return false;
  }
  const bool cleared = playlistDb.ClearPlaylist(db, defaultPlaylistId);
  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  playlistDb.Close(db);
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

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  playlistDb.Close(db);

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

  MusicPlaylistDB playlistDb;
  sqlite3 *db = playlistDb.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  if (defaultPlaylistId <= 0) {
    playlistDb.Close(db);
    errorMessage = "Could not create music playlist.";
    queue.Clear();
    PersistQueueTracksLocked();
    return false;
  }

  playlistDb.Close(db);

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

  if (loadedTrack) {
    StopAdjacentPreloadWorker();
    StartAdjacentPreloadWorker(AdjacentTracksLocked());
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
  if (loadedTrack) {
    StopAdjacentPreloadWorker();
    StartAdjacentPreloadWorker(AdjacentTracksLocked());
  }
  return true;
}

bool MusicPlayerService::PlayCurrent(std::string &errorMessage) {
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

bool MusicPlayerService::PlayCurrentAsync(std::string &statusMessage,
                                          std::string successMessage) {
  return StartPlaybackAsync(PlaybackRequest::Current, statusMessage,
                            std::move(successMessage));
}

bool MusicPlayerService::PlayNextAsync(std::string &statusMessage,
                                       std::string successMessage) {
  return StartPlaybackAsync(PlaybackRequest::Next, statusMessage,
                            std::move(successMessage));
}

bool MusicPlayerService::PlayPreviousAsync(std::string &statusMessage,
                                           std::string successMessage) {
  return StartPlaybackAsync(PlaybackRequest::Previous, statusMessage,
                            std::move(successMessage));
}

bool MusicPlayerService::Resume(std::string &errorMessage) {
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
  StopAdjacentPreloadWorker();
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
    std::string &statusMessage) {
  statusMessage.clear();
  bool handled = false;
  for (const auto event : native_music_player::DrainControlEvents()) {
    handled = true;
    switch (event) {
    case native_music_player::ControlEvent::Previous:
      PlayPreviousAsync(statusMessage, "Playing previous track.");
      break;
    case native_music_player::ControlEvent::Next:
      PlayNextAsync(statusMessage, "Playing next track.");
      break;
    case native_music_player::ControlEvent::Finished:
      PlayNextAsync(statusMessage, "Track finished. Playing next track.");
      break;
    }
  }
  return handled;
}

bool MusicPlayerService::ConsumeNativeControlStatus(
    std::string &statusMessage) {
  std::lock_guard<std::mutex> lock(nativeControlStatusMutex);
  if (consumedNativeControlStatusRevision == nativeControlStatusRevision) {
    return false;
  }
  consumedNativeControlStatusRevision = nativeControlStatusRevision;
  statusMessage = nativeControlStatusMessage;
  return true;
}

void MusicPlayerService::CancelRender() {
  renderCancelled.store(true, std::memory_order_release);
  preloadCancelled.store(true, std::memory_order_release);
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

std::vector<music_playlist::MusicTrack>
MusicPlayerService::AdjacentTracksLocked() const {
  std::vector<music_playlist::MusicTrack> tracks;
  std::unordered_set<std::string> seen;

  const auto keyForTrack = [](const music_playlist::MusicTrack &track) {
    return !track.trackId.empty() ? track.trackId : track.chartId;
  };
  if (const auto *current = queue.Current(); current != nullptr) {
    seen.insert(keyForTrack(*current));
  }
  if (loadedTrack) {
    seen.insert(keyForTrack(*loadedTrack));
  }

  const auto addTrack = [&](const music_playlist::MusicTrack *track) {
    if (track == nullptr) {
      return;
    }
    const std::string key = keyForTrack(*track);
    if (key.empty() || !seen.insert(key).second) {
      return;
    }
    tracks.push_back(*track);
  };

  addTrack(queue.PeekPrevious());
  addTrack(queue.PeekNext());
  return tracks;
}

std::vector<std::filesystem::path>
MusicPlayerService::PlaybackCacheKeepPathsLocked(
    const chart_music_cache::CacheResult &currentResult) const {
  std::vector<std::filesystem::path> paths;
  const auto addPathForTrack = [&](const music_playlist::MusicTrack *track) {
    if (track == nullptr) {
      return;
    }
    paths.push_back(
        chart_music_cache::CachedAudioPathForChart(track->representativeChart));
  };

  if (!currentResult.audioPath.empty()) {
    paths.push_back(currentResult.audioPath);
  }
  if (loadedTrack) {
    addPathForTrack(&*loadedTrack);
  }
  addPathForTrack(queue.Current());
  addPathForTrack(queue.PeekPrevious());
  addPathForTrack(queue.PeekNext());
  return paths;
}

bool MusicPlayerService::PlayTrackLocked(
    const music_playlist::MusicTrack &track, std::string &errorMessage) {
  errorMessage.clear();
  lastCacheResult = {};

  if (!native_music_player::IsSupported()) {
    errorMessage = "Native music playback is not supported on this platform.";
    return false;
  }

  StopAdjacentPreloadWorker();
  renderCancelled.store(false, std::memory_order_release);
  lastCacheResult = chart_music_cache::EnsureRenderedMusicFile(
      track.representativeChart, renderCancelled);
  if (!lastCacheResult.success) {
    errorMessage = lastCacheResult.message.empty()
                       ? "Could not render music track."
                       : lastCacheResult.message;
    return false;
  }

  auto metadata = music_playlist::MakeNativeMetadata(track);
  metadata.durationMicros = 0;
  if (lastCacheResult.durationMicros > 0) {
    metadata.durationMicros = lastCacheResult.durationMicros;
  }

  if (!native_music_player::Load(lastCacheResult.audioPath, metadata,
                                 errorMessage)) {
    return false;
  }
  const bool playing = native_music_player::Play(errorMessage);
  if (playing) {
    loadedTrack = track;
    if (lastCacheResult.durationMicros > 0) {
      loadedTrack->durationMicros = lastCacheResult.durationMicros;
    }
    SyncNativeQueueLocked();
    const auto keepPaths = PlaybackCacheKeepPathsLocked(lastCacheResult);
    auto adjacentTracks = AdjacentTracksLocked();
    chart_music_cache::PruneCacheExcept(keepPaths);
    StartAdjacentPreloadWorker(std::move(adjacentTracks));
    EnsureNativeControlEventPump();
  }
  return playing;
}

bool MusicPlayerService::StartPlaybackAsync(PlaybackRequest request,
                                            std::string &statusMessage,
                                            std::string successMessage) {
  statusMessage.clear();
  if (!native_music_player::IsSupported()) {
    statusMessage = "Native music playback is not supported on this platform.";
    return false;
  }

  music_playlist::MusicTrack track;
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
        statusMessage = "No music track is selected.";
        break;
      case PlaybackRequest::Next:
        statusMessage = "No next music track is available.";
        break;
      case PlaybackRequest::Previous:
        statusMessage = "No previous music track is available.";
        break;
      }
      return false;
    }
    if (request != PlaybackRequest::Current) {
      PersistQueueCursorLocked();
    }
    track = *selectedTrack;
    lastCacheResult = {};
  }

  StopPlaybackWorker();
  StopAdjacentPreloadWorker();

  const std::uint64_t requestRevision =
      playbackRequestRevision.fetch_add(1, std::memory_order_acq_rel) + 1;
  renderCancelled.store(false, std::memory_order_release);

  {
    std::lock_guard<std::mutex> lock(playbackThreadMutex);
    playbackThread = std::jthread(
        [this, track = std::move(track), requestRevision,
         successMessage = std::move(successMessage)](
            const std::stop_token &stopToken) mutable {
          PlaybackWorker(std::move(track), requestRevision,
                         std::move(successMessage), stopToken);
        });
  }

  statusMessage = "Preparing music...";
  return true;
}

void MusicPlayerService::PlaybackWorker(
    music_playlist::MusicTrack track, std::uint64_t requestRevision,
    std::string successMessage, const std::stop_token &stopToken) {
  const auto isCurrentRequest = [this, requestRevision, &stopToken]() {
    return !stopToken.stop_requested() &&
           playbackRequestRevision.load(std::memory_order_acquire) ==
               requestRevision;
  };

  if (!isCurrentRequest()) {
    return;
  }

  chart_music_cache::CacheResult cacheResult;
  try {
    cacheResult = chart_music_cache::EnsureRenderedMusicFile(
        track.representativeChart, renderCancelled);
  } catch (const std::exception &e) {
    cacheResult = {.success = false,
                   .message = std::string("Could not render music track: ") +
                              e.what()};
  } catch (...) {
    cacheResult = {.success = false,
                   .message = "Could not render music track."};
  }
  if (!isCurrentRequest()) {
    return;
  }

  std::string statusMessage;
  bool playing = false;
  std::vector<music_playlist::MusicTrack> adjacentTracks;
  std::vector<std::filesystem::path> keepCachePaths;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (!isCurrentRequest()) {
      return;
    }
    lastCacheResult = cacheResult;
    if (!cacheResult.success) {
      statusMessage = cacheResult.message.empty()
                          ? "Could not render music track."
                          : cacheResult.message;
    } else {
      auto metadata = music_playlist::MakeNativeMetadata(track);
      metadata.durationMicros = 0;
      if (cacheResult.durationMicros > 0) {
        metadata.durationMicros = cacheResult.durationMicros;
      }

      std::string errorMessage;
      if (!native_music_player::Load(cacheResult.audioPath, metadata,
                                     errorMessage)) {
        statusMessage = errorMessage;
      } else if (!native_music_player::Play(errorMessage)) {
        statusMessage = errorMessage;
      } else {
        loadedTrack = track;
        if (cacheResult.durationMicros > 0) {
          loadedTrack->durationMicros = cacheResult.durationMicros;
        }
        SyncNativeQueueLocked();
        keepCachePaths = PlaybackCacheKeepPathsLocked(cacheResult);
        adjacentTracks = AdjacentTracksLocked();
        playing = true;
        statusMessage =
            successMessage.empty() ? "Playing music." : successMessage;
      }
    }
  }

  if (playing) {
    chart_music_cache::PruneCacheExcept(keepCachePaths);
    StartAdjacentPreloadWorker(std::move(adjacentTracks));
    EnsureNativeControlEventPump();
  }
  if (isCurrentRequest() && !statusMessage.empty()) {
    PublishNativeControlStatus(statusMessage);
  }
}

void MusicPlayerService::StartAdjacentPreloadWorker(
    std::vector<music_playlist::MusicTrack> tracks) {
  StopAdjacentPreloadWorker();
  if (tracks.empty()) {
    return;
  }

  const std::uint64_t preloadRevision =
      preloadRequestRevision.fetch_add(1, std::memory_order_acq_rel) + 1;
  preloadCancelled.store(false, std::memory_order_release);

  std::lock_guard<std::mutex> lock(preloadThreadMutex);
  preloadThread = std::jthread(
      [this, tracks = std::move(tracks),
       preloadRevision](const std::stop_token &stopToken) mutable {
        AdjacentPreloadWorker(std::move(tracks), preloadRevision, stopToken);
      });
}

void MusicPlayerService::StopAdjacentPreloadWorker() {
  preloadCancelled.store(true, std::memory_order_release);
  preloadRequestRevision.fetch_add(1, std::memory_order_acq_rel);

  std::jthread threadToStop;
  {
    std::lock_guard<std::mutex> lock(preloadThreadMutex);
    if (!preloadThread.joinable()) {
      return;
    }
    preloadThread.request_stop();
    threadToStop = std::move(preloadThread);
  }

  if (threadToStop.joinable()) {
    threadToStop.join();
  }
}

void MusicPlayerService::AdjacentPreloadWorker(
    std::vector<music_playlist::MusicTrack> tracks,
    std::uint64_t preloadRevision, const std::stop_token &stopToken) {
  const auto isCurrentPreload = [this, preloadRevision, &stopToken]() {
    return !stopToken.stop_requested() &&
           preloadRequestRevision.load(std::memory_order_acquire) ==
               preloadRevision;
  };

  for (const auto &track : tracks) {
    if (!isCurrentPreload()) {
      return;
    }

    try {
      auto result = chart_music_cache::EnsureRenderedMusicFile(
          track.representativeChart, preloadCancelled);
      if (!result.success && isCurrentPreload()) {
        std::cerr << "Could not preload adjacent music track: "
                  << (result.message.empty() ? "Unknown error"
                                             : result.message)
                  << "\n";
      }
    } catch (const std::exception &e) {
      if (isCurrentPreload()) {
        std::cerr << "Could not preload adjacent music track: " << e.what()
                  << "\n";
      }
    } catch (...) {
      if (isCurrentPreload()) {
        std::cerr << "Could not preload adjacent music track.\n";
      }
    }
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
    sleepTimerThread.request_stop();
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
      PublishNativeControlStatus("Sleep timer stopped playback.");
    } else {
      PublishNativeControlStatus(stopMessage.empty()
                                     ? "Sleep timer expired."
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
    std::string statusMessage;
    if (ProcessNativeControlEvents(statusMessage) && !statusMessage.empty()) {
      PublishNativeControlStatus(statusMessage);
    }

    for (int i = 0; i < 10 && !stopToken.stop_requested(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }
}

void MusicPlayerService::PublishNativeControlStatus(
    const std::string &statusMessage) {
  std::lock_guard<std::mutex> lock(nativeControlStatusMutex);
  nativeControlStatusMessage = statusMessage;
  ++nativeControlStatusRevision;
}

} // namespace music_player
