#include "MusicPlaylist.h"

#include "../path.h"

#include <algorithm>
#include <cctype>
#include <numeric>
#include <random>
#include <utility>

namespace music_playlist {
namespace {

std::string lowerTrimmed(std::string value) {
  value.erase(value.begin(), std::find_if_not(value.begin(), value.end(),
                                              [](unsigned char ch) {
                                                return std::isspace(ch) != 0;
                                              }));
  value.erase(
      std::find_if_not(value.rbegin(), value.rend(),
                       [](unsigned char ch) { return std::isspace(ch) != 0; })
          .base(),
      value.end());
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::string normalizedPathString(std::filesystem::path path) {
  if (path.empty()) {
    return {};
  }
  path = path.lexically_normal();
  return path_t_to_utf8(fspath_to_path_t(path));
}

std::string fallbackTitle(const bms_parser::ChartMeta &meta) {
  if (!meta.Title.empty()) {
    return meta.Title;
  }
  if (!meta.BmsPath.empty()) {
    return path_t_to_utf8(fspath_to_path_t(meta.BmsPath.stem()));
  }
  return "Untitled";
}

std::string combineArtists(const std::string &artist,
                           const std::string &subArtist) {
  if (artist.empty()) {
    return subArtist;
  }
  if (subArtist.empty() || lowerTrimmed(artist) == lowerTrimmed(subArtist)) {
    return artist;
  }
  return artist + " / " + subArtist;
}

long long durationForChart(const bms_parser::ChartMeta &meta) {
  if (meta.TotalLength > 0) {
    return meta.TotalLength;
  }
  return std::max(0LL, meta.PlayLength);
}

std::uint64_t generateSeed() {
  static std::random_device device;
  const auto high = static_cast<std::uint64_t>(device()) << 32u;
  return high ^ static_cast<std::uint64_t>(device());
}

} // namespace

std::string TrackIdForChart(const bms_parser::ChartMeta &meta) {
  const std::string folder = normalizedPathString(meta.Folder);
  if (!folder.empty()) {
    return "folder:" + folder;
  }
  return "path:" + normalizedPathString(meta.BmsPath);
}

std::string ChartIdForChart(const bms_parser::ChartMeta &meta) {
  const std::string sha256 = lowerTrimmed(meta.SHA256);
  if (!sha256.empty()) {
    return "sha256:" + sha256;
  }
  const std::string md5 = lowerTrimmed(meta.MD5);
  if (!md5.empty()) {
    return "md5:" + md5;
  }
  return "path:" + normalizedPathString(meta.BmsPath);
}

MusicTrack MakeTrack(const MusicTrackRecord &record) {
  const auto &meta = record.representativeChart;
  return {
      .trackId = TrackIdForChart(meta),
      .chartId = ChartIdForChart(meta),
      .representativeChart = meta,
      .title = fallbackTitle(meta),
      .subtitle = meta.SubTitle,
      .artist = meta.Artist,
      .subArtist = meta.SubArtist,
      .genre = meta.Genre,
      .chartCount = std::max(1, record.chartCount),
      .durationMicros = durationForChart(meta),
  };
}

std::vector<MusicTrack>
MakeTracks(const std::vector<MusicTrackRecord> &records) {
  std::vector<MusicTrack> tracks;
  tracks.reserve(records.size());
  for (const auto &record : records) {
    tracks.push_back(MakeTrack(record));
  }
  return tracks;
}

native_music_player::TrackMetadata MakeNativeMetadata(const MusicTrack &track) {
  std::string title = track.title;
  if (!track.subtitle.empty()) {
    title += " " + track.subtitle;
  }
  return {
      .title = std::move(title),
      .artist = combineArtists(track.artist, track.subArtist),
      .album = track.genre,
      .durationMicros = track.durationMicros,
  };
}

void MusicQueue::SetPlaylist(std::vector<MusicTrack> newTracks,
                             std::size_t startIndex) {
  mode = QueueMode::Playlist;
  tracks = std::move(newTracks);
  randomSeed.reset();
  randomBag.clear();
  randomHistory.clear();
  randomHistoryCursor = 0;
  currentIndex.reset();
  if (!tracks.empty()) {
    currentIndex = std::min(startIndex, tracks.size() - 1);
  }
}

void MusicQueue::SetRandomAll(std::vector<MusicTrack> newTracks,
                              std::optional<std::uint64_t> seed) {
  mode = QueueMode::RandomAll;
  tracks = std::move(newTracks);
  randomSeed = seed.value_or(generateSeed());
  shuffleEngine.seed(*randomSeed);
  randomBag.clear();
  randomHistory.clear();
  randomHistoryCursor = 0;
  currentIndex.reset();
  if (!tracks.empty()) {
    RefillRandomBag();
    if (const auto index = DrawRandomIndex()) {
      SelectIndex(*index);
      PushRandomHistory(*index);
    }
  }
}

void MusicQueue::Clear() {
  tracks.clear();
  currentIndex.reset();
  randomSeed.reset();
  randomBag.clear();
  randomHistory.clear();
  randomHistoryCursor = 0;
  mode = QueueMode::Playlist;
}

const MusicTrack *MusicQueue::Current() const {
  if (!currentIndex || *currentIndex >= tracks.size()) {
    return nullptr;
  }
  return &tracks[*currentIndex];
}

const MusicTrack *MusicQueue::Next() {
  if (tracks.empty()) {
    return nullptr;
  }

  if (mode == QueueMode::Playlist) {
    const std::size_t index =
        currentIndex ? (*currentIndex + 1) % tracks.size() : 0;
    return SelectIndex(index);
  }

  if (randomHistoryCursor + 1 < randomHistory.size()) {
    ++randomHistoryCursor;
    return SelectIndex(randomHistory[randomHistoryCursor]);
  }

  if (randomBag.empty()) {
    RefillRandomBag();
  }
  if (const auto index = DrawRandomIndex()) {
    SelectIndex(*index);
    PushRandomHistory(*index);
  }
  return Current();
}

const MusicTrack *MusicQueue::Previous() {
  if (tracks.empty()) {
    return nullptr;
  }

  if (mode == QueueMode::Playlist) {
    const std::size_t index = !currentIndex || *currentIndex == 0
                                  ? tracks.size() - 1
                                  : *currentIndex - 1;
    return SelectIndex(index);
  }

  if (randomHistoryCursor > 0) {
    --randomHistoryCursor;
    return SelectIndex(randomHistory[randomHistoryCursor]);
  }
  return Current();
}

const MusicTrack *MusicQueue::SelectIndex(std::size_t index) {
  if (index >= tracks.size()) {
    currentIndex.reset();
    return nullptr;
  }
  currentIndex = index;
  return &tracks[index];
}

void MusicQueue::PushRandomHistory(std::size_t index) {
  if (randomHistoryCursor + 1 < randomHistory.size()) {
    randomHistory.erase(randomHistory.begin() + static_cast<std::ptrdiff_t>(
                                                    randomHistoryCursor + 1),
                        randomHistory.end());
  }
  randomHistory.push_back(index);
  randomHistoryCursor = randomHistory.size() - 1;
}

void MusicQueue::RefillRandomBag() {
  randomBag.resize(tracks.size());
  std::iota(randomBag.begin(), randomBag.end(), 0);

  std::shuffle(randomBag.begin(), randomBag.end(), shuffleEngine);

  if (tracks.size() > 1 && currentIndex && !randomBag.empty() &&
      randomBag.back() == *currentIndex) {
    std::swap(randomBag.front(), randomBag.back());
  }
}

std::optional<std::size_t> MusicQueue::DrawRandomIndex() {
  if (randomBag.empty()) {
    return std::nullopt;
  }
  const std::size_t index = randomBag.back();
  randomBag.pop_back();
  return index;
}

} // namespace music_playlist
