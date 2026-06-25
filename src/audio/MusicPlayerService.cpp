#include "MusicPlayerService.h"

#include "../ChartDBHelper.h"

#include <utility>

namespace music_player {
namespace {

void setEmptyLibraryError(std::string &errorMessage) {
  errorMessage = "No music tracks are available in the chart library.";
}

} // namespace

bool MusicPlayerService::ReloadLibrary(std::string &errorMessage) {
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

bool MusicPlayerService::StartLibraryPlaylist(std::string &errorMessage,
                                              std::size_t startIndex) {
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
  errorMessage.clear();
  if (libraryTracks.empty()) {
    setEmptyLibraryError(errorMessage);
    queue.Clear();
    return false;
  }
  queue.SetRandomAll(libraryTracks, seed);
  return true;
}

void MusicPlayerService::SetPlaylist(
    std::vector<music_playlist::MusicTrack> tracks, std::size_t startIndex) {
  queue.SetPlaylist(std::move(tracks), startIndex);
}

void MusicPlayerService::SetRandomAll(
    std::vector<music_playlist::MusicTrack> tracks,
    std::optional<std::uint64_t> seed) {
  queue.SetRandomAll(std::move(tracks), seed);
}

bool MusicPlayerService::PlayCurrent(std::string &errorMessage) {
  errorMessage.clear();
  const auto *track = queue.Current();
  if (track == nullptr) {
    errorMessage = "No music track is selected.";
    return false;
  }
  return PlayTrack(*track, errorMessage);
}

bool MusicPlayerService::PlayLibraryTrack(std::size_t index,
                                          std::string &errorMessage) {
  errorMessage.clear();
  if (index >= libraryTracks.size()) {
    errorMessage = "Music track index is out of range.";
    return false;
  }
  queue.SetPlaylist(libraryTracks, index);
  return PlayCurrent(errorMessage);
}

bool MusicPlayerService::PlayNext(std::string &errorMessage) {
  errorMessage.clear();
  const auto *track = queue.Next();
  if (track == nullptr) {
    errorMessage = "No next music track is available.";
    return false;
  }
  return PlayTrack(*track, errorMessage);
}

bool MusicPlayerService::PlayPrevious(std::string &errorMessage) {
  errorMessage.clear();
  const auto *track = queue.Previous();
  if (track == nullptr) {
    errorMessage = "No previous music track is available.";
    return false;
  }
  return PlayTrack(*track, errorMessage);
}

bool MusicPlayerService::Resume(std::string &errorMessage) {
  return native_music_player::Play(errorMessage);
}

bool MusicPlayerService::Pause(std::string &errorMessage) {
  return native_music_player::Pause(errorMessage);
}

bool MusicPlayerService::Stop(std::string &errorMessage) {
  CancelRender();
  return native_music_player::Stop(errorMessage);
}

bool MusicPlayerService::Seek(long long positionMicros,
                              std::string &errorMessage) {
  return native_music_player::Seek(positionMicros, errorMessage);
}

void MusicPlayerService::CancelRender() {
  renderCancelled.store(true, std::memory_order_release);
}

bool MusicPlayerService::PlayTrack(const music_playlist::MusicTrack &track,
                                   std::string &errorMessage) {
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
  return native_music_player::Play(errorMessage);
}

} // namespace music_player
