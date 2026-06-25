#include "MusicPlayerService.h"

#include "../ChartDBHelper.h"

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

bool MusicPlayerService::ReloadPlaylists(std::string &errorMessage) {
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

const MusicPlaylistInfo *MusicPlayerService::DefaultPlaylist() const {
  for (const auto &playlist : playlists) {
    if (playlist.id == defaultPlaylistId) {
      return &playlist;
    }
  }
  return nullptr;
}

bool MusicPlayerService::AddChartToDefaultPlaylist(
    const bms_parser::ChartMeta &chartMeta, std::string &errorMessage) {
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

bool MusicPlayerService::StartDefaultPlaylist(std::string &errorMessage) {
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

bool MusicPlayerService::ProcessNativeControlEvents(
    std::string &statusMessage) {
  bool handled = false;
  for (const auto event : native_music_player::DrainControlEvents()) {
    handled = true;
    std::string errorMessage;
    switch (event) {
    case native_music_player::ControlEvent::Previous:
      if (PlayPrevious(errorMessage)) {
        statusMessage = "Playing previous track.";
      } else {
        statusMessage = errorMessage;
      }
      break;
    case native_music_player::ControlEvent::Next:
      if (PlayNext(errorMessage)) {
        statusMessage = "Playing next track.";
      } else {
        statusMessage = errorMessage;
      }
      break;
    case native_music_player::ControlEvent::Finished:
      if (PlayNext(errorMessage)) {
        statusMessage = "Track finished. Playing next track.";
      } else {
        statusMessage = errorMessage;
      }
      break;
    }
  }
  return handled;
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
