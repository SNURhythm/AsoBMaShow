#pragma once

#include "../repositories/ChartRepository.h"
#include "NativeMusicPlayer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace music_playlist {

inline constexpr const char *kNowPlayingDisplayName = "Now Playing";

enum class QueueRepeatMode {
  None,
  One,
  All,
};

struct MusicTrack {
  std::string trackId;
  int storedItemId = 0;
  std::string chartId;
  std::string groupId;
  bms_parser::ChartMeta representativeChart;
  std::string title;
  std::string subtitle;
  std::string artist;
  std::string subArtist;
  std::string genre;
  std::filesystem::path artworkPath;
  int chartCount = 1;
  long long durationMicros = 0;
  bool groupRepresentative = false;
  bool expandedChart = false;
};

struct MusicQueueSnapshot {
  QueueRepeatMode repeatMode = QueueRepeatMode::All;
  std::string displayName;
  std::vector<MusicTrack> tracks;
  std::optional<std::size_t> currentIndex;
  std::optional<std::size_t> detachedCurrentNextIndex;
};

std::string TrackIdForChart(const bms_parser::ChartMeta &meta);
std::string ChartTrackIdForChart(const bms_parser::ChartMeta &meta);
std::string ChartIdForChart(const bms_parser::ChartMeta &meta);
std::filesystem::path ArtworkPathForChart(const bms_parser::ChartMeta &meta);
MusicTrack MakeTrack(const MusicTrackRecord &record);
std::vector<MusicTrack>
MakeTracks(const std::vector<MusicTrackRecord> &records);
bool SameTrackIdentity(const MusicTrack &a, const MusicTrack &b);
std::optional<std::size_t> FindTrackIndex(
    const std::vector<MusicTrack> &tracks, const MusicTrack &track);
std::vector<MusicTrack>
ShuffledTracks(std::vector<MusicTrack> tracks,
               std::optional<std::uint64_t> seed = std::nullopt);
native_music_player::TrackMetadata MakeNativeMetadata(const MusicTrack &track);
native_music_player::TrackMetadata MakeNativeMetadata(const MusicTrack &track,
                                                      bool includeArtwork);

class MusicQueue {
public:
  void SetPlaylist(std::vector<MusicTrack> tracks, std::size_t startIndex = 0,
                   std::string displayName = {});
  void SetPlaylistAfterCurrentRemoved(std::vector<MusicTrack> tracks,
                                      std::size_t nextIndex,
                                      std::string displayName = {});
  void Clear();
  void SetRepeatMode(QueueRepeatMode mode) { repeatMode = mode; }

  [[nodiscard]] QueueRepeatMode RepeatMode() const { return repeatMode; }
  [[nodiscard]] bool Empty() const { return tracks.empty(); }
  [[nodiscard]] std::size_t Size() const { return tracks.size(); }
  [[nodiscard]] const std::vector<MusicTrack> &Tracks() const { return tracks; }
  [[nodiscard]] std::optional<std::size_t> CurrentIndex() const {
    return currentIndex;
  }
  [[nodiscard]] const MusicTrack *Current() const;
  [[nodiscard]] const MusicTrack *PeekNext() const;
  [[nodiscard]] const MusicTrack *PeekPrevious() const;
  [[nodiscard]] MusicQueueSnapshot Snapshot() const;

  const MusicTrack *Next();
  const MusicTrack *Previous();

private:
  [[nodiscard]] const MusicTrack *TrackAt(std::size_t index) const;
  const MusicTrack *SelectIndex(std::size_t index);

  QueueRepeatMode repeatMode = QueueRepeatMode::All;
  std::string displayName;
  std::vector<MusicTrack> tracks;
  std::optional<std::size_t> currentIndex;
  std::optional<std::size_t> detachedCurrentNextIndex;
};

} // namespace music_playlist
