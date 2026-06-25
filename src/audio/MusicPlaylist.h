#pragma once

#include "../ChartDBHelper.h"
#include "NativeMusicPlayer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace music_playlist {

enum class QueueMode {
  Playlist,
  RandomAll,
};

struct MusicTrack {
  std::string trackId;
  std::string chartId;
  bms_parser::ChartMeta representativeChart;
  std::string title;
  std::string subtitle;
  std::string artist;
  std::string subArtist;
  std::string genre;
  int chartCount = 1;
  long long durationMicros = 0;
};

std::string TrackIdForChart(const bms_parser::ChartMeta &meta);
std::string ChartIdForChart(const bms_parser::ChartMeta &meta);
MusicTrack MakeTrack(const MusicTrackRecord &record);
std::vector<MusicTrack>
MakeTracks(const std::vector<MusicTrackRecord> &records);
native_music_player::TrackMetadata MakeNativeMetadata(const MusicTrack &track);

class MusicQueue {
public:
  void SetPlaylist(std::vector<MusicTrack> tracks, std::size_t startIndex = 0);
  void SetRandomAll(std::vector<MusicTrack> tracks,
                    std::optional<std::uint64_t> seed = std::nullopt);
  void Clear();

  [[nodiscard]] QueueMode Mode() const { return mode; }
  [[nodiscard]] bool Empty() const { return tracks.empty(); }
  [[nodiscard]] std::size_t Size() const { return tracks.size(); }
  [[nodiscard]] const std::vector<MusicTrack> &Tracks() const { return tracks; }
  [[nodiscard]] std::optional<std::size_t> CurrentIndex() const {
    return currentIndex;
  }
  [[nodiscard]] std::optional<std::uint64_t> RandomSeed() const {
    return randomSeed;
  }
  [[nodiscard]] const MusicTrack *Current() const;

  const MusicTrack *Next();
  const MusicTrack *Previous();

private:
  const MusicTrack *SelectIndex(std::size_t index);
  void PushRandomHistory(std::size_t index);
  void RefillRandomBag();
  std::optional<std::size_t> DrawRandomIndex();

  QueueMode mode = QueueMode::Playlist;
  std::vector<MusicTrack> tracks;
  std::optional<std::size_t> currentIndex;
  std::optional<std::uint64_t> randomSeed;
  std::mt19937_64 shuffleEngine;
  std::vector<std::size_t> randomBag;
  std::vector<std::size_t> randomHistory;
  std::size_t randomHistoryCursor = 0;
};

} // namespace music_playlist
