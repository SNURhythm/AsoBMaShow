#include "MusicPlayerService.h"

#include "../ChartDBHelper.h"

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
loadPlaylistTracks(ChartDBHelper &dbHelper, sqlite3 *db, int playlistId) {
  std::vector<MusicTrackRecord> records;
  dbHelper.SelectMusicPlaylistTracks(db, playlistId, records);
  return music_playlist::MakeTracks(records);
}

} // namespace

MusicPlayerService::~MusicPlayerService() {
  StopPlaybackWorker();
  StopNativeControlEventPump();
}

bool MusicPlayerService::ReloadLibrary(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  auto &dbHelper = ChartDBHelper::GetInstance();
  sqlite3 *db = dbHelper.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  std::vector<MusicTrackRecord> records;
  dbHelper.SelectMusicTracks(db, records);
  dbHelper.Close(db);

  libraryTracks = music_playlist::MakeTracks(records);
  return true;
}

std::size_t MusicPlayerService::LibraryTrackCount() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return libraryTracks.size();
}

bool MusicPlayerService::ReloadPlaylists(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  auto &dbHelper = ChartDBHelper::GetInstance();
  sqlite3 *db = dbHelper.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  defaultPlaylistId = dbHelper.EnsureMusicPlaylist(db, kDefaultPlaylistName);
  if (defaultPlaylistId <= 0) {
    dbHelper.Close(db);
    errorMessage = "Could not create music playlist.";
    return false;
  }
  playlists = dbHelper.SelectMusicPlaylists(db);
  defaultPlaylistTracks = loadPlaylistTracks(dbHelper, db, defaultPlaylistId);
  dbHelper.Close(db);
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

  auto &dbHelper = ChartDBHelper::GetInstance();
  sqlite3 *db = dbHelper.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  defaultPlaylistId = dbHelper.EnsureMusicPlaylist(db, kDefaultPlaylistName);
  if (defaultPlaylistId <= 0) {
    dbHelper.Close(db);
    errorMessage = "Could not create music playlist.";
    return false;
  }
  const bool inserted =
      dbHelper.InsertMusicPlaylistTrack(db, defaultPlaylistId, chartMeta);
  playlists = dbHelper.SelectMusicPlaylists(db);
  defaultPlaylistTracks = loadPlaylistTracks(dbHelper, db, defaultPlaylistId);
  dbHelper.Close(db);
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

  auto &dbHelper = ChartDBHelper::GetInstance();
  sqlite3 *db = dbHelper.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  defaultPlaylistId = dbHelper.EnsureMusicPlaylist(db, kDefaultPlaylistName);
  if (defaultPlaylistId <= 0) {
    dbHelper.Close(db);
    errorMessage = "Could not create music playlist.";
    return false;
  }
  const bool deleted =
      dbHelper.DeleteMusicPlaylistTrack(db, defaultPlaylistId, chartMeta);
  playlists = dbHelper.SelectMusicPlaylists(db);
  defaultPlaylistTracks = loadPlaylistTracks(dbHelper, db, defaultPlaylistId);
  dbHelper.Close(db);
  if (!deleted) {
    errorMessage = "Selected chart is not in My Playlist.";
    return false;
  }
  return true;
}

bool MusicPlayerService::ClearDefaultPlaylist(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  auto &dbHelper = ChartDBHelper::GetInstance();
  sqlite3 *db = dbHelper.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  defaultPlaylistId = dbHelper.EnsureMusicPlaylist(db, kDefaultPlaylistName);
  if (defaultPlaylistId <= 0) {
    dbHelper.Close(db);
    errorMessage = "Could not create music playlist.";
    return false;
  }
  const bool cleared = dbHelper.ClearMusicPlaylist(db, defaultPlaylistId);
  playlists = dbHelper.SelectMusicPlaylists(db);
  if (cleared) {
    defaultPlaylistTracks.clear();
  } else {
    defaultPlaylistTracks = loadPlaylistTracks(dbHelper, db, defaultPlaylistId);
  }
  dbHelper.Close(db);
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
  queue.SetPlaylist(libraryTracks, startIndex);
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
  queue.SetRandomAll(libraryTracks, seed);
  return true;
}

bool MusicPlayerService::StartDefaultPlaylist(std::string &errorMessage) {
  std::lock_guard<std::mutex> lock(stateMutex);
  errorMessage.clear();

  auto &dbHelper = ChartDBHelper::GetInstance();
  sqlite3 *db = dbHelper.Connect();
  if (db == nullptr) {
    errorMessage = "Could not open chart database.";
    return false;
  }

  defaultPlaylistId = dbHelper.EnsureMusicPlaylist(db, kDefaultPlaylistName);
  if (defaultPlaylistId <= 0) {
    dbHelper.Close(db);
    errorMessage = "Could not create music playlist.";
    queue.Clear();
    return false;
  }

  playlists = dbHelper.SelectMusicPlaylists(db);
  defaultPlaylistTracks = loadPlaylistTracks(dbHelper, db, defaultPlaylistId);
  dbHelper.Close(db);

  if (defaultPlaylistTracks.empty()) {
    setEmptyPlaylistError(errorMessage);
    queue.Clear();
    return false;
  }
  queue.SetPlaylist(defaultPlaylistTracks);
  return true;
}

void MusicPlayerService::SetPlaylist(
    std::vector<music_playlist::MusicTrack> tracks, std::size_t startIndex) {
  std::lock_guard<std::mutex> lock(stateMutex);
  queue.SetPlaylist(std::move(tracks), startIndex);
}

void MusicPlayerService::SetRandomAll(
    std::vector<music_playlist::MusicTrack> tracks,
    std::optional<std::uint64_t> seed) {
  std::lock_guard<std::mutex> lock(stateMutex);
  queue.SetRandomAll(std::move(tracks), seed);
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
  queue.SetPlaylist(libraryTracks, index);
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
  std::lock_guard<std::mutex> lock(stateMutex);
  return ProcessNativeControlEventsLocked(statusMessage);
}

bool MusicPlayerService::ProcessNativeControlEventsLocked(
    std::string &statusMessage) {
  bool handled = false;
  for (const auto event : native_music_player::DrainControlEvents()) {
    handled = true;
    std::string errorMessage;
    switch (event) {
    case native_music_player::ControlEvent::Previous:
      if (PlayPreviousLocked(errorMessage)) {
        statusMessage = "Playing previous track.";
      } else {
        statusMessage = errorMessage;
      }
      break;
    case native_music_player::ControlEvent::Next:
      if (PlayNextLocked(errorMessage)) {
        statusMessage = "Playing next track.";
      } else {
        statusMessage = errorMessage;
      }
      break;
    case native_music_player::ControlEvent::Finished:
      if (PlayNextLocked(errorMessage)) {
        statusMessage = "Track finished. Playing next track.";
      } else {
        statusMessage = errorMessage;
      }
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
  const auto *track = queue.Current();
  if (track == nullptr) {
    return std::nullopt;
  }
  return *track;
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
