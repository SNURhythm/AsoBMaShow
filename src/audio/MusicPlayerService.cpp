#include "MusicPlayerService.h"

#include <algorithm>
#include <chrono>
#include <exception>
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
loadPlaylistTracks(MusicPlaylistDB &playlistDb, sqlite3 *db, int playlistId) {
  std::vector<MusicTrackRecord> records;
  playlistDb.SelectTracks(db, playlistId, records);
  return music_playlist::MakeTracks(records);
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

} // namespace

MusicPlayerService::~MusicPlayerService() {
  StopPlaybackWorker();
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
  playlistDb.Close(db);

  libraryTracks = music_playlist::MakeTracks(records);
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

bool MusicPlayerService::ReloadPlaylists(std::string &errorMessage) {
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

std::optional<MusicPlaylistInfo>
MusicPlayerService::SelectedPlaylistSnapshot() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return findPlaylistById(playlists, selectedPlaylistId);
}

std::vector<music_playlist::MusicTrack>
MusicPlayerService::SelectedPlaylistTracksSnapshot() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return selectedPlaylistTracks;
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
  playlistDb.Close(db);
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
  playlistDb.Close(db);
  if (selectedPlaylistId != playlistId) {
    errorMessage = "Selected playlist no longer exists.";
    return false;
  }
  return true;
}

bool MusicPlayerService::AddChartToSelectedPlaylist(
    const bms_parser::ChartMeta &chartMeta, std::string &errorMessage) {
  int targetPlaylistId = 0;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    targetPlaylistId = selectedPlaylistId;
  }
  return AddChartToPlaylist(targetPlaylistId, chartMeta, errorMessage);
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

bool MusicPlayerService::MoveChartInDefaultPlaylist(
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
  if (defaultPlaylistId <= 0) {
    playlistDb.Close(db);
    errorMessage = "Could not create music playlist.";
    return false;
  }
  const bool moved =
      playlistDb.MoveTrack(db, defaultPlaylistId, chartMeta, delta);
  RefreshPlaylistCachesLocked(playlistDb, db, selectedPlaylistId);
  playlistDb.Close(db);
  if (!moved) {
    errorMessage = delta < 0 ? "Selected track is already at the top."
                             : "Selected track is already at the bottom.";
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

bool MusicPlayerService::StartLibraryPlaylist(std::string &errorMessage,
                                              std::size_t startIndex) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();
  if (libraryTracks.empty()) {
    setEmptyLibraryError(errorMessage);
    queue.Clear();
    return false;
  }
  queue.SetPlaylist(libraryTracks, startIndex, "Library");
  return true;
}

bool MusicPlayerService::StartRandomLibrary(std::string &errorMessage,
                                            std::optional<std::uint64_t> seed) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();
  if (libraryTracks.empty()) {
    setEmptyLibraryError(errorMessage);
    queue.Clear();
    return false;
  }
  queue.SetPlaylist(music_playlist::ShuffledTracks(libraryTracks, seed), 0,
                    music_playlist::kNowPlayingDisplayName);
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
    return false;
  }
  const auto selectedPlaylist = findPlaylistById(playlists, selectedPlaylistId);
  queue.SetPlaylist(selectedPlaylistTracks, 0,
                    selectedPlaylist ? selectedPlaylist->name : "");
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
    return false;
  }

  playlistDb.Close(db);

  if (defaultPlaylistTracks.empty()) {
    setEmptyPlaylistError(errorMessage);
    queue.Clear();
    return false;
  }
  queue.SetPlaylist(defaultPlaylistTracks, 0, kDefaultPlaylistName);
  return true;
}

void MusicPlayerService::SetNowPlaying(
    std::vector<music_playlist::MusicTrack> tracks, std::size_t startIndex) {
  std::lock_guard<std::mutex> lock(stateMutex);
  queue.SetPlaylist(std::move(tracks), startIndex,
                    music_playlist::kNowPlayingDisplayName);
}

void MusicPlayerService::SetPlaylist(
    std::vector<music_playlist::MusicTrack> tracks, std::size_t startIndex,
    std::string displayName) {
  std::lock_guard<std::mutex> lock(stateMutex);
  queue.SetPlaylist(std::move(tracks), startIndex, std::move(displayName));
}

void MusicPlayerService::SetPlaylistAfterCurrentRemoved(
    std::vector<music_playlist::MusicTrack> tracks, std::size_t nextIndex,
    std::string displayName) {
  std::lock_guard<std::mutex> lock(stateMutex);
  queue.SetPlaylistAfterCurrentRemoved(std::move(tracks), nextIndex,
                                       std::move(displayName));
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

bool MusicPlayerService::PlayLibraryTrack(std::size_t index,
                                          std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();
  if (index >= libraryTracks.size()) {
    errorMessage = "Music track index is out of range.";
    return false;
  }
  queue.SetPlaylist(libraryTracks, index, "Library");
  return PlayCurrentLocked(errorMessage);
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
}

chart_music_cache::CacheResult MusicPlayerService::LastCacheResult() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return lastCacheResult;
}

bool MusicPlayerService::PlayTrackLocked(
    const music_playlist::MusicTrack &track, std::string &errorMessage) {
  errorMessage.clear();
  lastCacheResult = {};

  if (!native_music_player::IsSupported()) {
    errorMessage = "Native music playback is not supported on this platform.";
    return false;
  }

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
  if (metadata.durationMicros <= 0 && lastCacheResult.durationMicros > 0) {
    metadata.durationMicros = lastCacheResult.durationMicros;
  }

  if (!native_music_player::Load(lastCacheResult.audioPath, metadata,
                                 errorMessage)) {
    return false;
  }
  const bool playing = native_music_player::Play(errorMessage);
  if (playing) {
    loadedTrack = track;
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
    track = *selectedTrack;
    lastCacheResult = {};
  }

  StopPlaybackWorker();

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
      if (metadata.durationMicros <= 0 && cacheResult.durationMicros > 0) {
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
        playing = true;
        statusMessage =
            successMessage.empty() ? "Playing music." : successMessage;
      }
    }
  }

  if (playing) {
    EnsureNativeControlEventPump();
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
